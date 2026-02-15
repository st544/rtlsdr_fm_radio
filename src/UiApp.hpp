#include <atomic>
#include <functional>
#include "SpectrumBuffer.hpp"
#include "WaterfallBuffer.hpp"

struct UiAppConfig {
    std::atomic<bool>* stream_active;
    std::atomic<float>* volume_level;
    std::atomic<int>* rf_gain;
    int fft_size;
    int rf_sample_rate;
    double center_freq_hz;

    std::function<void(float)> retune_callback;
    std::function<void(int)> set_gain_callback;
};

class UiApp {
public:
    // Blocks until window closes. Returns when user quits.
    static void Run(const UiAppConfig& cfg,
                    const SpectrumBuffer& rf_spec,
                    const WaterfallBuffer& rf_wf);
};