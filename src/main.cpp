#include <iostream>
#include <cstdint>
#include <atomic>
#include <complex>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <rtl-sdr.h>
#include <portaudio.h>
#include "AudioFile.h"
#include "FIRFilter.hpp"
#include "DSPBlocks.hpp"
#include "CircularBuffer.hpp"

static std::atomic<uint64_t> g_underruns{0};
static std::atomic<bool> g_stop_requested{false};
std::atomic<bool> running{true};
std::atomic<bool> reader_finished{false};

// Context struct for rtlsdr async callback
struct AsyncContext {
    CircularBuffer<uint8_t>* iq;        // IQ buffer
    std::atomic<uint64_t>* dropped;     // dropped packets if buffer is full
};

// RTLSDR async callback
static void rtlsdr_async_cb(unsigned char* buf, uint32_t len, void* ctx_void) {
    auto* ctx = reinterpret_cast<AsyncContext*>(ctx_void);

    // Push RTLSDR async data directly into IQ buffer
    size_t written = ctx->iq->push(reinterpret_cast<uint8_t*>(buf), len);

    // Track dropped packets if buffer is full
    if (written < len) {
        ctx->dropped->fetch_add(len - written, std::memory_order_relaxed);
    }
}


// Audiostreaming callback
static int paCallback(const void* input, void* output, 
                        unsigned long frameCount,
                        const PaStreamCallbackTimeInfo* timeInfo,
                        PaStreamCallbackFlags,
                        void* userData) 
{

    float* out = (float*)output;
    auto* ring = reinterpret_cast<CircularBuffer<float>*>(userData);

    size_t read = ring->pop(out, frameCount);               // read 'frameCount' samples
    if (read < frameCount) {
        std::fill(out + read, out + frameCount, 0.0f);      // If buffer empty, fill rest with silence
        g_underruns.fetch_add(1, std::memory_order_relaxed);
    }
    return paContinue;
}


//Ctrl-C signal handler
void ctrlC_Invoked(int s)
{
	std::cout<<"\nStopping loop..."<<std::endl;
    running = false;

    g_stop_requested.store(true, std::memory_order_relaxed);
}



int main(int argc, char* argv[]) { 

    // ctrl+c signal handler
    std::signal(SIGINT, ctrlC_Invoked);

    // Parse arguments
    bool live_stream = false;
    if (argc > 1 && std::strcmp(argv[1], "--stream") == 0) {
        live_stream = true;
    }
    
    ////////////////////////////////////////////////////////
    // Initialization
    ////////////////////////////////////////////////////////

    // Detect RTL-SDR devices and print
    uint32_t n = rtlsdr_get_device_count();
    std::cout << "RTL-SDR devices found: " << n << "\n";

    for (uint32_t i = 0; i < n; i++) {
        const char* name = rtlsdr_get_device_name(i);
        std::cout << "  [" << i << "] " << (name ? name : "(null)") << "\n";
    }

    int r = 0;
    rtlsdr_dev_t* dev = nullptr;

    // Open device
    r = rtlsdr_open(&dev, 0);
    if (r != 0 || !dev) { std::cerr << "rtlsdr_open failed: " << r << "\n"; return 1; }

    // Basic config
    const uint32_t fs = 2'400'000;      // 2.4 MS/s
    const uint32_t fc = 93'300'000;     // 93.3 MHz
    const uint32_t fq = fs / 5;         // decimation after LPF to 480k
    const uint32_t fa = fq / 10;        // decimation to 48k 
    const float max_dev = 75'000.0f;    // max deviation for broadcast FM

    // Thresholds for discriminator after demod
    const float dphi_max = 2.0f * 3.14159265f * (max_dev / fq);     // max change in phase
    const float limit = 1.25f * dphi_max;

    // rtlsdr settings
    r = rtlsdr_set_sample_rate(dev, fs);
    if (r) { std::cerr << "set_sample_rate failed: " << r << "\n"; return 1; }

    r = rtlsdr_set_center_freq(dev, fc);
    if (r) { std::cerr << "set_center_freq failed: " << r << "\n"; return 1; }

    r = rtlsdr_set_tuner_gain_mode(dev, 0);
    if (r) { std::cerr << "set_tuner_gain_mode failed: " << r << "\n"; return 1; }

    r = rtlsdr_set_agc_mode(dev, 0);

    // Reset buffers
    rtlsdr_reset_buffer(dev);

    ////////////////////////////////////////////////////////
    // DSP Pipeline start
    ////////////////////////////////////////////////////////

    // Instantiate dsp blocks
    FIRFilter<std::complex<float>> LPF(5, radio_taps);         // First stage LPF decimator
    FIRFilter<float> audio_LPF(10, audio_taps);                // Second stage anti-aliasing LPF decimator
    NotchFilter19k pilot_notch(fa);                         // 19kHz notch filter
    FmDemod demod;                                         // demodulator
    DeemphasisBiquad deemph(75e-6f, (float)fa);                      // 1-pole IIR audio rate
    DcBlocker dc;
    IQDcBlocker iq_dc;
    SimpleAgc agc;

    // IQ ring buffer for rtlsdr_async_read 
    CircularBuffer<uint8_t> iq_ring(1<<20);     // 1MB
    CircularBuffer<float> fft_ring(32768);      // Buffer for visualizer
    std::atomic<uint64_t> iq_dropped{0};
    AsyncContext actx{&iq_ring, &iq_dropped};   // Declare async context struct for buffer

    // Audio ring buffer
    CircularBuffer<float> audio_ring(32768);      
    std::vector<float> outBlock(512);           // output block for audio ring buffer
    size_t outCount = 0;
    PaStream* stream = nullptr;
    unsigned long framesPerBuffer = 1024;
    bool stream_started = false;
    const size_t prime_target = framesPerBuffer * 20;    // ~0.4s

    // Audio file buffer
    std::vector<uint8_t> buf(16384);      
    uint64_t audio_written = 0;
    const uint64_t target_audio = (uint64_t)fa * 10;    // 10 seconds
    std::vector<float> audio;
    audio.reserve(target_audio);

    // LUT for byte to float conversion
    float lut[256];
    for (int i = 0; i < 256; i++) {
        lut[i] = (i - 127.5f) / 128.0f;
    }

    // Set up PortAudio stream for live mode
    if (live_stream) {
        std::cout<<"Entering live streaming mode. Press Ctrl+C to stop"<<std::endl;
        PaError r = Pa_Initialize();    // open/start stream
        if (r != paNoError) {
            std::cerr<<"PortAudio Init Error: " << Pa_GetErrorText(r) << std::endl;
            return 1;
        }

        PaError err = Pa_OpenDefaultStream(
            &stream,
            0,                // no input
            1,                // mono output
            paFloat32,
            fa,
            framesPerBuffer,
            paCallback,
            &audio_ring        // userData
        );
        if (err != paNoError) {
            std::cerr<<"PortAudio Open Stream Error: " << Pa_GetErrorText(err) << std::endl; 
        }


    }

    ///////////////////////////////////
    // DSP Pipeline Main Loop
    ///////////////////////////////////
    std::cout << "Beginning DSP pipeline" <<std::endl;

    // Start async reader
    std::thread reader([&]{
        rtlsdr_read_async(dev, rtlsdr_async_cb, &actx, 0, 16384);

        reader_finished.store(true, std::memory_order_release);
    });

    // Start thread for DSP pipeline
    std::thread dsp([&] {
        std::vector<uint8_t> iqbuf(16384);
        std::vector<float> outBlock(512);
        size_t outCount = 0;

        while (!reader_finished.load(std::memory_order_acquire) || iq_ring.read_available() > 0) {

            // Force exit if Ctrl+C is used
            if (!running.load(std::memory_order_relaxed) && iq_ring.read_available() == 0) {
                break;
            }

            size_t n = iq_ring.pop(iqbuf.data(), iqbuf.size());
            if (n == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // n_read bytes, interleaved I,Q
            for (int i = 0; i + 1 < n; i += 2) {
                float I = lut[iqbuf[i]];
                float Q = lut[iqbuf[i+1]];
                std::complex<float> x(I, Q);
                std::complex<float> x1;
                iq_dc.process(x);                       // IQ DC blocker
                if (!LPF.Filter(x,x1)) continue;        // First stage LPF 
                float fm = demod.push(x1);              // demodulate
                fm = std::clamp(fm, -limit, limit);     // remove bad phase jumps
                fm = dc.push(fm);                       // audio DC blocker 

                float a1;
                if (!audio_LPF.Filter(fm, a1)) continue;    // Second stage LPF - 48kS/s output
                float y = pilot_notch.push(a1);
                y = deemph.push(y);                         // IIR filter
                float s = agc.apply(y);                     // Automatic gain control
                float volume_knob = 1.2f;                   // volume control
                s *= volume_knob;

                s = softclip(s);                // cubic soft clip

                
                if (live_stream) {
                    // Start stream after buffer has been filled to initial target
                    if (!stream_started && audio_ring.read_available() >= prime_target) {
                        Pa_StartStream(stream);
                        stream_started = true;
                    }

                    outBlock[outCount++] = s;
                    if (outCount == outBlock.size()) {
                        audio_ring.push(outBlock.data(), outCount);
                        outCount = 0;
                    }
                }
                else {
                    audio.push_back(s);
                    if (audio.size() >= target_audio) {
                        running.store(false, std::memory_order_relaxed);
                        break;
                    }
                }
            }

        }

    });

    while (running.load(std::memory_order_relaxed)) {
        if (g_stop_requested.load(std::memory_order_relaxed)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // stop async read
    rtlsdr_cancel_async(dev);
    reader.join();

    // Stop DSP thread
    running.store(false, std::memory_order_relaxed);
    dsp.join();


    rtlsdr_close(dev);      // Close device


    if (live_stream) {
        Pa_StopStream(stream);
        Pa_CloseStream(stream);
        Pa_Terminate();  
    }
    else {
        std::cout<<"Audio buffer filled. Saving to out.wav file..."<<std::endl;

        // Initialize .wav file
        AudioFile<float> wav;
        wav.setNumChannels(1);
        wav.setNumSamplesPerChannel((int)audio.size());    // 10 seconds
        wav.setSampleRate(fa);

        for (int i = 0; i < audio.size(); i++) {
            wav.samples[0][i] = audio[i];
        }
        wav.save("out.wav");
    }

    return 0;


}