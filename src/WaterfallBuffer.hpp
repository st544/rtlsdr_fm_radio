#include <vector>
#include <cstring>
#include <algorithm>

class WaterfallBuffer {
public:
    WaterfallBuffer(int height, int bins, float fill_db = -200.0f)
        : H(height), B(bins), data(height * bins, fill_db) {}

    void push_row(const float* row_db) {
        float* dst = &data[write_row * B];
        std::memcpy(dst, row_db, sizeof(float) * B);
        write_row = (write_row + 1) % H;
        filled = std::min(filled + 1, H);
    }

    // Oldest->newest contiguous for plotting
    void linearize(std::vector<float>& out) const {
        out.resize(filled * B);
        int start = (filled == H) ? write_row : 0;
        for (int r = 0; r < filled; ++r) {
            int src = (start + r) % H;
            std::memcpy(&out[r * B], &data[src * B], sizeof(float) * B);
        }
    }

    int rows() const { return filled; }
    int bins() const { return B; }

private:
    int H, B;
    std::vector<float> data;
    int write_row = 0;
    int filled = 0;
};