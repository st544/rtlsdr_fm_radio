#include <vector>
#include <atomic>
#include <cstring>
#include <stdexcept>

class CircularBuffer {
public:
    explicit CircularBuffer(size_t size) : buffer(size), mask(size - 1) {
        if ((size&mask) != 0) {
            throw std::invalid_argument("Buffer size must be a power of 2");
        }
    }

    // Producer function
    size_t push(const float* data, size_t count) {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_acquire);

        size_t available = buffer.size() - (h - t);
        if (available == 0) return 0;   // Full

        size_t to_write = std::min(count, available);

        size_t idx = h & mask;
        size_t first = std::min(to_write, buffer.size() - idx);

        std::memcpy(&buffer[idx], data, first * sizeof(float));
        std::memcpy(&buffer[0], data + first, (to_write - first) * sizeof(float));

        head_.store(h + to_write, std::memory_order_release);
        return to_write;
    }

    // Consumer function
    size_t pop(float* out, size_t count) {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_relaxed);

        size_t available = h - t;
        if (available == 0) return 0;   // Empty

        size_t to_read = std::min(count, available);

        size_t idx = t & mask;
        size_t first = std::min(to_read, buffer.size() - idx);

        std::memcpy(out, &buffer[idx], first * sizeof(float));
        std::memcpy(out + first, &buffer[0], (to_read - first) * sizeof(float));

        tail_.store(t + to_read, std::memory_order_release);
        return to_read;
    }

    size_t read_available() const {
        size_t t = tail_.load(std::memory_order_relaxed);
        size_t h = head_.load(std::memory_order_acquire);
        return h - t;
    }

    size_t write_available() const {
        return buffer.size() - read_available();
    }

private:
    std::vector<float> buffer;
    size_t mask;
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};