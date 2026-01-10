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

static std::atomic<uint64_t> g_bytes{0};
static std::atomic<uint64_t> g_underruns{0};

static void rtlsdr_cb(unsigned char* buf, uint32_t len, void* /*ctx*/) {
    g_bytes.fetch_add(len, std::memory_order_relaxed);
}

static int paCallback(const void* input, void* output, 
                        unsigned long frameCount,
                        const PaStreamCallbackTimeInfo* timeInfo,
                        PaStreamCallbackFlags,
                        void* userData) 
{

    float* out = (float*)output;
    auto* ring = reinterpret_cast<CircularBuffer*>(userData);

    size_t read = ring->pop(out, frameCount);               // read 'frameCount' samples
    if (read < frameCount) {
        std::fill(out + read, out + frameCount, 0.0f);      // If buffer empty, fill rest with silence
        g_underruns.fetch_add(1, std::memory_order_relaxed);
    }
    return paContinue;
}

std::atomic<bool> running{true};

//Ctrl-C signal handler
void ctrlC_Invoked(int s)
{
	std::cout<<"\nStopping loop..."<<std::endl;
    running = false;
	
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
    FmDemodFast demod;                                         // demodulator
    Deemphasis deemph(75e-6f, (float)fa);                      // 1-pole IIR audio rate
    DcBlocker dc;
    IQDcBlocker iq_dc;
    SimpleAgc agc;

    CircularBuffer audio_ring(32768);      // Declare audio ring buffer
    std::vector<float> outBlock(512);       // output block for audio ring buffer
    size_t outCount = 0;
    PaStream* stream = nullptr;
    unsigned long framesPerBuffer = 1024;
    bool stream_started = false;
    const size_t prime_target = framesPerBuffer * 20;    // ~0.4s

    std::vector<uint8_t> buf(16384);        // buffer for wav file
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

    while (running) {
        int n_read = 0;
        int r = rtlsdr_read_sync(dev, buf.data(), (int)buf.size(), &n_read);
        if (r != 0) break;

        // n_read bytes, interleaved I,Q
        for (int i = 0; i + 1 < n_read; i += 2) {
            float I = lut[buf[i]];
            float Q = lut[buf[i+1]];
            std::complex<float> x(I, Q);
            std::complex<float> x1;
            iq_dc.process(x);                       // IQ DC blocker
            if (!LPF.Filter(x,x1)) continue;        // First stage LPF 
            float fm = demod.push(x1);              // demodulate
            fm = std::clamp(fm, -limit, limit);     // remove bad phase jumps
            fm = dc.push(fm);                       // audio DC blocker 

            float a1;
            if (!audio_LPF.Filter(fm, a1)) continue;    // Second stage LPF - 48kS/s output
            float y = deemph.push(a1);                  // IIR filter
            float s = agc.apply(y);                     // Automatic gain control
            float volume_knob = 1.0f;                   // volume control
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

                    if (audio_ring.read_available() > 48000) { // > 1 second buffered
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                }

                static auto last = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                if (now - last > std::chrono::seconds(1)) {
                    last = now;
                    std::cout << "ring_avail=" << audio_ring.read_available()
                            << " underruns=" << g_underruns.load() << "\n";
                }
            
            }
            else {
                audio.push_back(s);
                if (audio.size() >= target_audio) running = false;
            }
        }
    }

    rtlsdr_close(dev);


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