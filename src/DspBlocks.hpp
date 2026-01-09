#include <complex>
#include <cmath>
#include <algorithm>

// Fast FM Demodulator
struct FmDemodFast {
    std::complex<float> prev{1.0f, 0.0f};

    float push(std::complex<float> x) {
        float I  = x.real(),  Q  = x.imag();
        float Ip = prev.real(), Qp = prev.imag();
        prev = x;

        // numerator ~ sin(Δφ), denominator ~ cos(Δφ)
        float num = I*Qp - Q*Ip;
        float den = I*Ip + Q*Qp;

        // For small angles, num/den ≈ Δφ. Add epsilon to avoid blowups.
        return num / (den + 1e-12f);
    }
};

// Audio DC Blocker (High Pass)
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

// IQ DC Blocker (Notch at 0Hz)
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

// Standard De-emphasis filter (IIR)
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

// Automatic Gain Control
struct SimpleAgc {
    float env = 1e-3f;
    float target = 0.2f;     // desired average amplitude
    float attack = 0.01f;    // faster up
    float release = 0.001f;  // slower down

    float apply(float x) {
        float ax = std::abs(x);
        float k = (ax > env) ? attack : release;
        env += k * (ax - env);
        float g = target / (env + 1e-6f);
        return x * g;
    }
};

// Cubic Soft Cliper
inline float softclip(float x) {
    // linear for |x|<1, saturates smoothly beyond.
    float ax = std::abs(x);
    if (ax <= 1.0f) {
        return x - (x*x*x)/3.0f;
    } else {
        return (x > 0.0f) ? (2.0f/3.0f) : (-2.0f/3.0f);
    }
}