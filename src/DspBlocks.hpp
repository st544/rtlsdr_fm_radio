#include <complex>
#include <cmath>
#include <algorithm>

// FM Quadrature Demodulator
struct FmDemod {
    std::complex<float> prev{1.0f, 0.0f};

    float push(std::complex<float> x) {
        // Calculate phase difference between current sample (x) and previous (prev)
        // result is x * conj(prev)
        float re = x.real() * prev.real() + x.imag() * prev.imag();
        float im = x.imag() * prev.real() - x.real() * prev.imag();
        
        prev = x; // Update state

        // Standard atan2 extracts the exact angle in radians (-pi to +pi)
        return std::atan2(im, re);
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

// Biquad De-emphasis filter (2nd order IIR)
struct DeemphasisBiquad {
    float b0, b1, a1;
    float x1 = 0.0f;
    float y1 = 0.0f;

    DeemphasisBiquad(float sampleRate, float tau = 75e-6f) {
        // Digital model of an analog RC Low Pass Filter
        // Transfer function H(s) = 1 / (1 + s*tau)
        // Bilinear transform substitution s -> (2/T) * (1-z^-1)/(1+z^-1)
        
        float T = 1.0f / sampleRate;
        float k = 2.0f * tau; 
        
        // Resulting coefficients
        float norm = T + k;
        b0 = T / norm;
        b1 = T / norm;
        a1 = (T - k) / norm;
    }

    float push(float x) {
        // Difference equation y[n] = b0*x[n] + b1*x[n-1] - a1*y[n-1]
        float y = b0 * x + b1 * x1 - a1 * y1;
        
        x1 = x;
        y1 = y;
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

// Notch Filter
struct NotchFilter19k {
    float b0, b1, b2, a1, a2;
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;

    NotchFilter19k(float sampleRate) {
        float fc = 19000.0f;
        float Q = 10.0f;

        float w0 = 2.0f * 3.141592654f * fc / sampleRate;
        float alpha = std::sin(w0) / (2.0f * Q);
        float cosw0 = std::cos(w0);

        float a0 = 1.0f + alpha;
        b0 = 1.0f / a0;
        b1 = -2.0f * cosw0 / a0;
        b2 = 1.0f / a0;
        a1 = -2.0f * cosw0 / a0;
        a2 = 1.0f - alpha / a0;

    }

    float push(float x) {
        // Direct Form I difference equation
        float y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2;

        // Shift state
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;

        return y;
    }
};