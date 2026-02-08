#pragma once

#include <vector>
#include <cstring>
#include <algorithm>
#include <mutex>

class WaterfallBuffer {
public:
    WaterfallBuffer(int height, int bins, float fill_db = -100.0f)
        : H(height), B(bins), empty_val(fill_db), data(height * bins, fill_db) {}

    // Main thread pushes FFT samples
    void push_row(const float* row_db) {
        std::lock_guard<std::mutex> lock(mtx);  // Protect write

        float* dst = &data[write_row * B];
        std::memcpy(dst, row_db, sizeof(float) * B);
        write_row = (write_row + 1) % H;
        filled = std::min(filled + 1, H);
    }

    // UI thread pulls buffer
    int linearize(std::vector<float>& out) const {
        std::lock_guard<std::mutex> lock(mtx);      // Protect read

        // Resize
        if (out.size() != (size_t)(filled * B)) {
            out.resize(filled * B);
        }
        
        // Copy rows in reverse order (newest -> oldest)
        for (int i = 0; i < filled; i++) {

            // Calculate source index in circular buffer
            int src_row_idx = (write_row - 1 - i);
            if (src_row_idx < 0) src_row_idx += H;

            // Copy row
            const float* src = &data[src_row_idx * B];
            float* dst = &out[i * B];
            std::memcpy(dst, src, B * sizeof(float));
        }

        return filled;
    }

    int max_rows() const { return H; }
    int bins() const { return B; }

private:
    int H, B;
    float empty_val;
    std::vector<float> data;
    int write_row = 0;
    int filled = 0;
    mutable std::mutex mtx;
};