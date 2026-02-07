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
#include "SpectrumBuffer.hpp"
#include "WaterfallBuffer.hpp"
#include "RfFFTAnalyzer.hpp"
#include "UiApp.hpp"


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

    size_t samples_needed = frameCount * 2;

    size_t read = ring->pop(out, samples_needed);               // read 'frameCount' samples
    if (read < samples_needed) {
        std::fill(out + read, out + frameCount, 0.0f);      // If buffer empty, fill rest with silence
        g_underruns.fetch_add(1, std::memory_order_relaxed);
    }
    return paContinue;
}


static double now_seconds() {
    using clock = std::chrono::steady_clock;
    static auto t0 = clock::now();
    auto t = clock::now();
    return std::chrono::duration<double>(t - t0).count();
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
    bool record_mode = false;
    std::ofstream raw_dump;

    for(int i=1; i<argc; i++) {
        if (std::strcmp(argv[i], "--record") == 0) record_mode = true;
        if (std::strcmp(argv[i], "--stream") == 0) live_stream = true;
    }

    // Record mode
    if (record_mode) {
        raw_dump.open("raw_iq_samples.bin", std::ios::binary);
        std::cout << "Recording raw IQ to raw_iq_samples.bin..." << std::endl;
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
    FIRFilter<std::complex<float>> LPF(5, radio_taps);          // First stage LPF decimator
    StereoSeparator stereo(fq);                                 // Separates mono and stereo diff signals
    FIRFilter<float> LPF_mono(10, audio_taps);                  // Second stage anti-aliasing LPF decimator - mono
    FIRFilter<float> LPF_diff(10, audio_taps);                  // Second stage anti-aliasing LPF decimator - stereo diff
    NotchFilter19k pilot_notch(fa);                             // 19kHz notch filter
    FmDemod demod;                                              // demodulator
    DeemphasisBiquad deemph_L(75e-6f, (float)fa);               // 1-pole IIR audio rate - L
    DeemphasisBiquad deemph_R(75e-6f, (float)fa);               // 1-pole IIR audio rate - R
    DcBlocker dc;                                               // audio DC blocker
    IQDcBlocker iq_dc;                                          // IQ DC blocker
    SimpleAgc agc;                                              // automatic gain control

    // IQ ring buffer for rtlsdr_async_read 
    CircularBuffer<uint8_t> iq_ring(1<<20);     // 1MB
    std::atomic<uint64_t> iq_dropped{0};
    AsyncContext actx{&iq_ring, &iq_dropped};   // Declare async context struct for buffer

    // Audio ring buffer
    CircularBuffer<float> audio_ring(65536);      
    std::vector<float> stereo_out_block(1024);          // interleaved (L,R) output block
    int idx = 0;
    PaStream* stream = nullptr;
    unsigned long framesPerBuffer = 1024;
    bool stream_started = false;
    const size_t prime_target = framesPerBuffer * 20;    // ~0.4s

    // Audio file buffer
    const uint64_t target_audio = (uint64_t)fa * 10;    // 10 seconds
    std::vector<float> audio;
    audio.reserve(target_audio * 2);
    int cnt = 0;

    // RF Visualizer buffers
    CircularBuffer<float> fft_ring(1<<20);      // Buffer for RF visualizer
    std::vector<float> rf_block;
    rf_block.reserve(4096 * 2);


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
            2,                // stereo output
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

            // Record raw IQ data samples to file
            if (record_mode && raw_dump.is_open()) {
                raw_dump.write(reinterpret_cast<char*>(iqbuf.data()), n);
            }

            // n_read bytes, interleaved I,Q
            for (int i = 0; i + 1 < n; i += 2) {
                float I = lut[iqbuf[i]];
                float Q = lut[iqbuf[i+1]];
                std::complex<float> x(I, Q);
                std::complex<float> x1;
                iq_dc.process(x);                       // IQ DC blocker

                // Push to FFT ring buffer for visualizer
                rf_block.push_back(x.real());
                rf_block.push_back(x.imag());
                if (rf_block.size() == 4096 * 2) {
                    size_t written = fft_ring.push(rf_block.data(), rf_block.size());
                    rf_block.clear();
                }

                if (!LPF.Filter(x,x1)) continue;        // First stage LPF 

                float fm = demod.push(x1);              // demodulate
                fm = std::clamp(fm, -limit, limit);     // remove bad phase jumps
                fm = dc.push(fm);                       // audio DC blocker 

                auto [raw_mono, raw_diff] = stereo.process(fm);     // stereo separator - 480kS/s

                float mono_out, diff_out;
                bool mono_ready = LPF_mono.Filter(raw_mono, mono_out);  // Second stage LPF - mono - 48kS/s
                bool diff_ready = LPF_diff.Filter(raw_diff, diff_out);  // Second stage LPF - diff - 48kS/s

                if (mono_ready && diff_ready) {
                    mono_out = pilot_notch.push(mono_out);      // Notch filter 19kHz

                    float left = (mono_out + diff_out);         // Matrix L+R
                    float right = (mono_out - diff_out);

                    left = deemph_L.push(left);                 // Deemphasis IIR L+R
                    right = deemph_R.push(right);

                    left = agc.apply(left);             // Automatic gain control
                    right = agc.apply(right);

                    float volume_knob = 1.2f;           // Volume control
                    left *= volume_knob;
                    right *= volume_knob;

                    left = softclip(left);              // soft clip
                    right = softclip(right);

                    if (live_stream) {
                        // Start stream after buffer has been filled to initial target
                        if (!stream_started && audio_ring.read_available() >= prime_target) {
                            Pa_StartStream(stream);
                            stream_started = true;
                        }

                        stereo_out_block[idx++] = left;     // Push to interleaved stereo buffer
                        stereo_out_block[idx++] = right;
                        if (idx == stereo_out_block.size()) {
                            audio_ring.push(stereo_out_block.data(), idx);
                            idx = 0;
                        }
                    }
                    else {
                        audio.push_back(left);
                        audio.push_back(right);
                        if (audio.size() >= target_audio * 2) {
                            running.store(false, std::memory_order_relaxed);
                            break;
                        }
                    }
                }
            }

        }

    });


    // Start RF Analyzer thread
    const int Nfft = 2048;
    const int wf_height = 400;
    RfFFTAnalyzer rf_fft(Nfft, (int)fs);
    SpectrumBuffer rf_spec(Nfft);
    WaterfallBuffer rf_waterfall(wf_height, Nfft);

    std::thread rf_analyzer([&] {
        const int hop_complex = 512;
        const int hop_floats = hop_complex * 2;
        const int frame_floats = Nfft * 2;

        std::vector<float> fifo;
        fifo.reserve(frame_floats * 2);

        std::vector<float> tmp(hop_floats);
        std::vector<float> frame(frame_floats);

        while (running.load(std::memory_order_relaxed) || fft_ring.read_available() > 0) {
            if (fft_ring.pop(tmp.data(), hop_floats) != (size_t)hop_floats) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            fifo.insert(fifo.end(), tmp.begin(), tmp.end());

            while ((int)fifo.size() >= frame_floats) {
                std::copy(fifo.begin(), fifo.begin() + frame_floats, frame.begin());

                float* w = rf_spec.write_ptr();
                rf_fft.compute_db_shifted(frame.data(), w);
                rf_spec.publish(now_seconds());

                rf_waterfall.push_row(w);

                // overlap: drop hop
                fifo.erase(fifo.begin(), fifo.begin() + hop_floats);
            }
        }

    });


    // Instantiate UIApp struct and call run method
    UiAppConfig cfg;
    cfg.fft_size = Nfft;                // 2048 
    cfg.rf_sample_rate = fs;           
    cfg.center_freq_hz = fc;            // 93.3MHz

    UiApp::Run(cfg, rf_spec, rf_waterfall);

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
        std::cout<<"Audio buffer filled. Saving to stereo_out.wav file..."<<std::endl;

        // Initialize .wav file
        AudioFile<float> wav;
        wav.setNumChannels(2);
        wav.setNumSamplesPerChannel((int)audio.size()/2);    // 10 seconds
        wav.setSampleRate(fa);

        for (size_t i = 0; i < audio.size(); i += 2) {
            wav.samples[0][i/2] = audio[i];
            wav.samples[1][i/2] = audio[i+1];
        }
        wav.save("stereo_out.wav");
    }

    return 0;


}