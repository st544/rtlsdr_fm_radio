#include <iostream>
#include <cstdint>
#include <atomic>
#include <complex>
#include <rtl-sdr.h>
#include <portaudio.h>
#include "AudioFile.h"
#include "FIRFilter.hpp"
#include "DspBlocks.hpp"

static std::atomic<uint64_t> g_bytes{0};

static void rtlsdr_cb(unsigned char* buf, uint32_t len, void* /*ctx*/) {
    g_bytes.fetch_add(len, std::memory_order_relaxed);
}


int main(int argc, char* argv[]) { 

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

    // Instantiate filters
    FIRFilter<std::complex<float>> LPF(5, radio_taps);         // First stage LPF decimator
    FIRFilter<float> audio_LPF(10, audio_taps);                  // Second stage anti-aliasing LPF decimator
    FmDemodFast demod;                                              // demodulator
    Deemphasis deemph(75e-6f, (float)fa);                       // 1-pole IIR audio rate
    DcBlocker dc;
    IQDcBlocker iq_dc;
    SimpleAgc agc;

    std::vector<uint8_t> buf(16384); // must be even length
    uint64_t audio_written = 0;
    const uint64_t target_audio = (uint64_t)fa * 10; // 10 seconds

    std::vector<float> audio;
    audio.reserve(target_audio);

    std::cout << "Beginning DSP pipeline" <<std::endl;

    if (live_stream) {
        //todo
    }
    else {
        // Write to 10 second .wav file
        while (audio.size() < target_audio) {
            int n_read = 0;
            int r = rtlsdr_read_sync(dev, buf.data(), (int)buf.size(), &n_read);
            if (r != 0) break;

            // n_read bytes, interleaved I,Q
            for (int i = 0; i + 1 < n_read; i += 2) {
                float I = (buf[i]   - 127.5f) / 128.0f;
                float Q = (buf[i+1] - 127.5f) / 128.0f;
                std::complex<float> x(I, Q);

                std::complex<float> x1;
                iq_dc.process(x);                       // IQ DC blocker
                if (!LPF.Filter(x,x1)) continue;        // First stage LPF - 240kS/s output

                float fm = demod.push(x1);              // demodulate

                fm = std::clamp(fm, -limit, limit);     // remove bad phase jumps

                fm = dc.push(fm);                       // audio DC blocker 

                float a1;
                if (!audio_LPF.Filter(fm, a1)) continue;    // Second stage LPF - 48kS/s output

                float y = deemph.push(a1);                  // IIR filter

                float s = agc.apply(y);         // Automatic gain control

                float volume_knob = 1.0f;       // volume control
                s *= volume_knob;

                s = softclip(s);                // cubic soft clip

                audio.push_back(s);

                if (audio.size() >= target_audio) break;
            }
        }

        rtlsdr_close(dev);

        std::cout<<"Audio buffer filled. Saving to out.wav file..."<<std::endl;

        // Initialize .wav file
        AudioFile<float> wav;
        wav.setNumChannels(1);
        wav.setNumSamplesPerChannel(target_audio);    // 10 seconds
        wav.setSampleRate(fa);

        for (int i = 0; i < audio.size(); i++) {
            wav.samples[0][i] = audio[i];
        }

        wav.save("out.wav");

    }




    return 0;


}