#include <vector>
#include <atomic>

struct SpectrumFrame {
    std::vector<float> db;   // dB values, size = bins
    double timestamp = 0.0;
};

class SpectrumBuffer {
public:
    explicit SpectrumBuffer(size_t bins) {
        frames_[0].db.resize(bins);
        frames_[1].db.resize(bins);
    }

    float* write_ptr() {
        int w = 1 - idx_.load(std::memory_order_acquire);
        return frames_[w].db.data();
    }

    void publish(double ts) {
        int w = 1 - idx_.load(std::memory_order_acquire);
        frames_[w].timestamp = ts;
        idx_.store(w, std::memory_order_release);
    }

    const SpectrumFrame& latest() const {
        return frames_[idx_.load(std::memory_order_acquire)];
    }

private:
    SpectrumFrame frames_[2];
    std::atomic<int> idx_{0};
};