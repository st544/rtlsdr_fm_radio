#include <iostream>
#include <fstream>
#include <vector>
#include <cassert>
#include <cmath>
#include <complex>
#include <algorithm>

#include "../src/FIRFilter.hpp"
#include "../src/DSPBlocks.hpp"
#include "../src/RfFFTAnalyzer.hpp" 

// Mock constants matching main.cpp
const uint32_t fs = 2'400'000;
const uint32_t fq = fs / 5;   // 480k
const uint32_t fa = fq / 10;  // 48k

int main() {
    std::cout << "[TEST] Starting Integration Test...\n";

    // Load the Recorded Data
    std::ifstream file("raw_iq_samples.bin", std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[FAIL] Could not open raw_iq_samples.bin. Run main with --record first.\n";
        return 1;
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> raw_data(size);
    if (!file.read((char*)raw_data.data(), size)) {
        std::cerr << "[FAIL] Read error\n";
        return 1;
    }
    std::cout << "[INFO] Loaded " << size << " bytes of raw IQ data.\n";

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
    
    // For FFT Analysis
    const int Nfft = 2048;
    RfFFTAnalyzer fft(Nfft, fq);
    std::vector<float> iq_history;      // Buffer to hold one frame for FFT
    std::vector<float> mpx_history;      // To check for 19k pilot
    std::vector<float> audio_output;     // To check for silence/volume

    // LUT setup
    float lut[256];
    for (int i = 0; i < 256; i++) lut[i] = (i - 127.5f) / 128.0f;

    // Output tracking
    size_t processed_iq_samples = 0;
    size_t decimated_samples = 0;

    ////////////////////////////////////////////////////////
    // Run DSP Pipeline
    ////////////////////////////////////////////////////////
    for (size_t i = 0; i + 1 < raw_data.size(); i += 2) {
        // Convert byte -> float
        float I = lut[raw_data[i]];
        float Q = lut[raw_data[i+1]];
        std::complex<float> x(I, Q);
        std::complex<float> x_out;

        processed_iq_samples++;

        // Run Pre-DSP (DC Block + LPF)
        iq_dc.process(x);
        
        if (LPF.Filter(x, x_out)) {
            decimated_samples++;

            // Accumulate for FFT check
            if (iq_history.size() < Nfft * 2) {
                iq_history.push_back(x_out.real());
                iq_history.push_back(x_out.imag());
            }

            // Demodulate
            float fm = demod.push(x_out);
            fm = dc.push(fm);

            // Save multiplexed samples for 19kHz Pilot check
            if (mpx_history.size() < 4096) {
                mpx_history.push_back(fm);
            }

            // Stereo and audio
            auto [raw_mono, raw_diff] = stereo.process(fm);
            float mono_out;
            
            // Just test Mono path for audio validity
            if (LPF_mono.Filter(raw_mono, mono_out)) {
                float audio = deemph_L.push(mono_out);
                audio_output.push_back(audio);
            }

        }
    }

    ////////////////////////////////////////////////////////
    // Verify Sample Rate / Decimation Ratio
    ////////////////////////////////////////////////////////

    // Input is 2.4MS/s, Output of LPF is 480kS/s (Factor of 5)
    // Tolerance: Allow small startup jitter
    double ratio = (double)processed_iq_samples / (double)decimated_samples;
    std::cout << "[INFO] Decimation Ratio: " << ratio << " (Expected: 5.0)\n";
    
    if (std::abs(ratio - 5.0) > 0.1) {
        std::cerr << "[FAIL] Sample rate conversion incorrect!\n";
        return 1;
    }
    std::cout << "[PASS] Sample Rate Check.\n";

    ////////////////////////////////////////////////////////
    // Peak Frequency Detector (FFT Check)
    ////////////////////////////////////////////////////////

    if (iq_history.size() < Nfft * 2) {
        std::cerr << "[WARN] Not enough data for FFT test (Need " << Nfft << " samples)\n";
        return 1;
    }

    // Run FFT
    std::vector<float> spectrum(Nfft);
    fft.compute_db_shifted(iq_history.data(), spectrum.data());

    // Find Peak
    float max_db = -1000.0f;
    int max_bin = 0;
    for (int k = 0; k < Nfft; k++) {
        if (spectrum[k] > max_db) {
            max_db = spectrum[k];
            max_bin = k;
        }
    }

    // Calculate Frequency of Peak
    // 0Hz is at index Nfft/2 due to fftshift logic in compute_db_shifted
    int center_index = Nfft / 2;
    int bin_distance = max_bin - center_index;
    float hz_per_bin = (float)fq / (float)Nfft;
    float peak_freq = bin_distance * hz_per_bin;
    int drift_tolerance_bins = 40;

    std::cout << "[INFO] Peak Frequency: " << peak_freq << " Hz (dB: " << max_db << ")\n";

    // Assertion: The signal should be the FM carrier at 0Hz (DC)
    // We allow +/- 40 bins (~9.3kHz) of slop for DC offset or tuning drift
    if (std::abs(bin_distance) > drift_tolerance_bins) {
        std::cerr << "[FAIL] Peak frequency is too far from 0Hz! Tuning or LPF issue.\n";
        std::cerr << "       Expected ~0Hz (Bin " << center_index << "), Found " << peak_freq << "Hz (Bin " << max_bin << ")\n";
        return 1;
    }

    if (max_db < -60.0f) {
        std::cerr << "[FAIL] Signal too weak! Is the recording empty/noise?\n";
        return 1;
    }

    std::cout << "[PASS] Peak Frequency Detector (Carrier Found at DC).\n";

    ////////////////////////////////////////////////////////
    // Audio Test
    ////////////////////////////////////////////////////////

    if (audio_output.empty()) {
        std::cerr << "[FAIL] No audio samples produced! Pipeline blocked?\n";
        return 1;
    }

    double sum_sq = 0.0;
    for (float s : audio_output) {
        if (std::isnan(s) || std::isinf(s)) {
            std::cerr << "[FAIL] Audio contains NaN or Inf values!\n";
            return 1;
        }
        sum_sq += s * s;
    }
    double rms = std::sqrt(sum_sq / audio_output.size());
    
    std::cout << "[INFO] Final Audio RMS: " << rms << "\n";
    
    // RMS threshold: 0.01 is -40dB, a reasonable floor for "not dead silence"
    if (rms < 0.01) { 
        std::cerr << "[FAIL] Audio is too quiet! Demodulator might be broken.\n";
        return 1;
    }
    std::cout << "[PASS] Audio Pipeline produces valid sound.\n";

    ////////////////////////////////////////////////////////
    // 19kHz Pilot Tone Test
    ////////////////////////////////////////////////////////

    // We run an FFT on the MPX signal (demodulator output)
    const int N_MPX = 2048; // enough resolution
    if (mpx_history.size() < N_MPX) return 1;

    RfFFTAnalyzer fft2(N_MPX, fq); // fq = 480000 Hz
    std::vector<float> spectrum2(N_MPX);
    
    // Note: compute_db_shifted expects complex I/Q interleaved, but MPX is Real float.
    // Hack: Create "fake" interleaved buffer [Sample, 0.0, Sample, 0.0 ...]
    std::vector<float> mpx_complex(N_MPX * 2, 0.0f);
    for(int i=0; i<N_MPX; i++) mpx_complex[i*2] = mpx_history[i];

    fft2.compute_db_shifted(mpx_complex.data(), spectrum2.data());

    // Pilot frequency calculation
    // FFT is shifted, so 0Hz is at N/2 (1024).
    // Hz per bin = 480000 / 2048 = 234.375 Hz
    // 19000 / 234.375 = ~81 bins offset
    int center = N_MPX / 2;
    int pilot_bin = center + 81; 
    
    // Check signal strength at 19kHz vs 25kHz (noise)
    float pilot_db = spectrum2[pilot_bin];
    float noise_db = spectrum2[pilot_bin + 25]; // Look ~6kHz higher (empty space)

    std::cout << "[INFO] 19kHz Pilot: " << pilot_db << " dB | Noise Floor: " << noise_db << " dB\n";

    if (pilot_db < noise_db + 6.0f) { // Expect pilot to be at least 6dB above noise
        std::cerr << "[FAIL] 19kHz Pilot tone missing or too weak! Stereo decoding will fail.\n";
        return 1;
    }

    std::cout << "[PASS] 19kHz Stereo Pilot detected.\n";



    std::cout << "[SUCCESS] All Integration Tests Passed.\n";
    return 0;
}