#include <fftw3.h>
#include <vector>
#include <cmath>
#include <algorithm>

class RfFFTAnalyzer {
public:
    RfFFTAnalyzer(int fft_size, int sample_rate)
        : N(fft_size), fs(sample_rate),
          window(N),
          in((fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * N)),
          out((fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * N))
    {
        for (int n = 0; n < N; ++n)
            window[n] = 0.5f - 0.5f * std::cos(2.0f * 3.141592654f * n / (N - 1));

        plan = fftwf_plan_dft_1d(N, in, out, FFTW_FORWARD, FFTW_MEASURE);
    }

    ~RfFFTAnalyzer() {
        fftwf_destroy_plan(plan);
        fftwf_free(in);
        fftwf_free(out);
    }

    int fft_size() const { return N; }
    int sample_rate() const { return fs; }

    // iq interleaved floats: [I0,Q0,I1,Q1,...] length=2N
    // dst_db size=N (shifted: DC in middle)
    void compute_db_shifted(const float* iq_interleaved, float* dst_db,
                            float db_floor = -140.0f)
    {
        // window + copy
        for (int i = 0; i < N; ++i) {
            float I = iq_interleaved[2*i];
            float Q = iq_interleaved[2*i + 1];
            float w = window[i];
            in[i][0] = I * w;
            in[i][1] = Q * w;
        }

        fftwf_execute(plan);

        // power -> dB, then fftshift into dst
        const float eps = 1e-20f;
        const float norm = 1.0f / (float)(N * N);

        int half = N / 2;
        for (int k = 0; k < N; ++k) {
            float re = out[k][0];
            float im = out[k][1];
            float p = (re*re + im*im) * norm;
            float db = 10.0f * std::log10(p + eps);
            if (db < db_floor) db = db_floor;

            int ks = (k + half) % N; // shift
            dst_db[ks] = db;
        }
    }

private:
    int N, fs;
    std::vector<float> window;
    fftwf_complex* in;
    fftwf_complex* out;
    fftwf_plan plan;
};