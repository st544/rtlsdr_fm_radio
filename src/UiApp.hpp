#include <atomic>
#include <functional>
#include "SpectrumBuffer.hpp"
#include "WaterfallBuffer.hpp"

struct UiAppConfig {
    std::atomic<bool>* stream_active;
    std::atomic<float>* volume_level;
    int fft_size;
    int rf_sample_rate;
    double center_freq_hz;

    std::function<void(float)> retune_callback;
};

class UiApp {
public:
    // Blocks until window closes. Returns when user quits.
    static void Run(const UiAppConfig& cfg,
                    const SpectrumBuffer& rf_spec,
                    const WaterfallBuffer& rf_wf);
};