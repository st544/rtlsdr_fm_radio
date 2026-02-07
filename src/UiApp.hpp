#include "SpectrumBuffer.hpp"
#include "WaterfallBuffer.hpp"

struct UiAppConfig {
    int fft_size;
    int rf_sample_rate;
    double center_freq_hz;
};

class UiApp {
public:
    // Blocks until window closes. Returns when user quits.
    static void Run(const UiAppConfig& cfg,
                    const SpectrumBuffer& rf_spec,
                    const WaterfallBuffer& rf_wf);
};