#include <iostream>
#include <cstdint>
#include <atomic>
#include <complex>
#include <rtl-sdr.h>
#include "AudioFile.h"
#include "FIRFilter.hpp"

static std::atomic<uint64_t> g_bytes{0};

static void rtlsdr_cb(unsigned char* buf, uint32_t len, void* /*ctx*/) {
    g_bytes.fetch_add(len, std::memory_order_relaxed);
}

struct FmDemod {
    std::complex<float> prev{1.0f, 0.0f};

    float push(std::complex<float> x) {
        auto d = x * std::conj(prev);
        prev = x;
        return std::atan2(d.imag(), d.real()); // radians/sample
    }
};

struct Deemphasis {
    float y = 0.0f;
    float a;

    Deemphasis(float tau, float fs) {
        float dt = 1.0f / fs;
        a = dt / (tau + dt);
    }
    float push(float x) {
        y += a * (x - y);
        return y;
    }
};

struct DcBlocker {
    float y = 0.0f, x1 = 0.0f;
    float R = 0.995f; // 0.99–0.999, tune if needed
    float push(float x) {
        float out = x - x1 + R * y;
        x1 = x;
        y = out;
        return out;
    }
};

struct IQDcBlocker {
    std::complex<float> avg{0.0f, 0.0f};
    float alpha = 1.0e-4f; // Slow averaging to track drift

    void process(std::complex<float>& x) {
        // Leaky integrator to find the long-term DC average
        avg = avg * (1.0f - alpha) + x * alpha;
        // Subtract DC from signal
        x -= avg;
    }
};

int main() { 
    
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
    const uint32_t fq = fs / 5;         // decimation after LPF 
    const uint32_t fa = fq / 10;        // decimation to 48k 

    r = rtlsdr_set_sample_rate(dev, fs);
    if (r) { std::cerr << "set_sample_rate failed: " << r << "\n"; return 1; }

    r = rtlsdr_set_center_freq(dev, fc);
    if (r) { std::cerr << "set_center_freq failed: " << r << "\n"; return 1; }

    // Set gain 
    r = rtlsdr_set_tuner_gain_mode(dev, 0);
    if (r) { std::cerr << "set_tuner_gain_mode failed: " << r << "\n"; return 1; }

    // Reset buffers
    rtlsdr_reset_buffer(dev);

    ////////////////////////////////////////////////////////
    // DSP Pipeline start
    ////////////////////////////////////////////////////////

    // Instantiate filters
    FIRFilter<std::complex<float>> LPF(5, radio_taps);         // First stage LPF decimator
    FIRFilter<float> audio_LPF(10, audio_taps);                  // Second stage anti-aliasing LPF decimator
    FmDemod demod;                                              // demodulator
    Deemphasis deemph(75e-6f, (float)fa);                       // 1-pole IIR audio rate
    DcBlocker dc;
    IQDcBlocker iq_dc;

    std::vector<uint8_t> buf(16384); // must be even length
    uint64_t audio_written = 0;
    const uint64_t target_audio = (uint64_t)fa * 10; // 10 seconds

    std::vector<float> audio;
    audio.reserve(target_audio);

    std::cout << "Beginning DSP pipeline" <<std::endl;

    // Temp write to 10 second .wav file
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
            iq_dc.process(x);
            if (!LPF.Filter(x,x1)) continue;        // First stage LPF - 240kS/s output

            float fm = demod.push(x1);              // demodulate
            fm = dc.push(fm);

            float a1;
            if (!audio_LPF.Filter(fm, a1)) continue;    // Second stage LPF - 48kS/s output

            float y = deemph.push(a1);                  // IIR filter

            // gain + clip to int16
            float gain = 10.0f;
            audio.push_back(y * gain);
            if (audio.size() >= target_audio) break;
        }
    }


    //std::cout << "Streaming IQ @ " << fs << " sps, tuned to " << fc << " Hz...\n";
    //std::cout << "Press Ctrl+C to stop.\n";

    // This blocks until canceled or error
    //r = rtlsdr_read_async(dev, rtlsdr_cb, nullptr, 0, 16384);
    //std::cerr << "read_async returned: " << r << "\n";

    rtlsdr_close(dev);

    std::cout << "Audio buffer filled. Processing..."<<std::endl;

    // Normalize and clip to [-1,1]
    float peak = 1e-9f;
    for (float s : audio) peak = std::max(peak, std::abs(s));
    float scale = 0.9f / peak;
    for (float &s : audio) {
        s = std::clamp(s * scale, -1.0f, 1.0f);
    }

    std::cout<<"Saving to out.wav file..."<<std::endl;

    // Initialize .wav file
    AudioFile<float> wav;
    wav.setNumChannels(1);
    wav.setNumSamplesPerChannel(target_audio);    // 10 seconds
    wav.setSampleRate(fa);

    for (int i = 0; i < audio.size(); i++) {
        wav.samples[0][i] = audio[i];
    }

    wav.save("out.wav");

    return 0;


}