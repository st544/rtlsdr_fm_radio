#pragma once

#include <vector>
#include <atomic>

struct SpectrumFrame {
    std::vector<float> db;   // dB values, size = bins
    double timestamp = 0.0;
};

class SpectrumBuffer {
public:
    explicit SpectrumBuffer(size_t bins) {
        frames_[0].db.resize(bins);             // Resize buffers to FFT size
        frames_[1].db.resize(bins);
    }

    float* write_ptr() {
        int w = 1 - idx_.load(std::memory_order_acquire);   // UI reads from idx_, DSP writes to 1 - idx_
        return frames_[w].db.data();                        // Return pointer to producer buffer
    }

    void publish(double ts) {
        int w = 1 - idx_.load(std::memory_order_acquire);   // Get new index for UI
        frames_[w].timestamp = ts;                          // Update timestamp in new buffer
        idx_.store(w, std::memory_order_release);           // Index Atomic swap
    }

    const SpectrumFrame& latest() const {
        return frames_[idx_.load(std::memory_order_acquire)];   // Latest index called by UI thread
    }

private:
    SpectrumFrame frames_[2];       // Double buffer for producer/consumer visualization
    std::atomic<int> idx_{0};       // Index for read buffer
};