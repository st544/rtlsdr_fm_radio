#include "UiApp.hpp"

#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <SDL.h>
#include <glad/glad.h>
#include <SDL_opengl.h>
#include "imgui.h"
#include "implot.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"


/*
creates a window + GL context

initializes ImGui + ImPlot

every frame:

reads rf_spec.latest() (no copies besides what ImPlot needs)

linearizes the waterfall into a contiguous vector (for PlotHeatmap)
*/

static void BuildFreqAxis(std::vector<float>& x_khz, int N, int fs_hz) {
    x_khz.resize(N);
    const float df = (float)fs_hz / (float)N / 1000.0f;
    const float f0 = -(float)fs_hz / 2.0 / 1000.0f;
    for (int i = 0; i < N; ++i) x_khz[i] = f0 + df * i;
}


void UiApp::Run(const UiAppConfig& cfg, const SpectrumBuffer& rf_spec, const WaterfallBuffer& rf_wf) {

    // SDL + GL Initialize
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        return;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window* window = SDL_CreateWindow(
        "RTL-SDR RF Spectrum",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);

    if (!window) {
        SDL_Quit();
        return;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1); // vsync

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return;
    }

    // ImGui + ImPlot init
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 330");

    // UI
    std::vector<float> x_axis;
    BuildFreqAxis(x_axis, cfg.fft_size, cfg.rf_sample_rate);

    //std::vector<float> wf_linear;
    //float wf_db_min = -120.0f;
    //float wf_db_max = -30.0f;

    float spec_db_min = -60.0f;
    float spec_db_max = -20.0;

    static std::vector<float> spec_smooth;
    spec_smooth.resize(cfg.fft_size);

    float smooth_alpha = 0.75f; // 0=no smoothing, 0.95=lots of smoothing
    bool enable_smoothing = true;

    bool paused = false;
    bool quit = false;


    while (!quit) {
        // Events
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) quit = true;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE) quit = true;
        }

        // New frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // ---- Controls ----
        ImGui::Begin("Controls");
        ImGui::Text("Center: %.3f MHz", cfg.center_freq_hz / 1e6);
        ImGui::Text("RF rate: %d Hz", cfg.rf_sample_rate);
        ImGui::Text("FFT: %d", cfg.fft_size);

        ImGui::Checkbox("Pause", &paused);
        ImGui::SliderFloat("Spec dB min", &spec_db_min, -100.0f, 0.0f);
        ImGui::SliderFloat("Spec dB max", &spec_db_max, -100.0f, 0.0f);
        ImGui::SliderFloat("Smooth alpha", &smooth_alpha, 0.0f, 0.98f);
        //ImGui::SliderFloat("WF dB min", &wf_db_min, -180.0f, 0.0f);
        //ImGui::SliderFloat("WF dB max", &wf_db_max, -180.0f, 0.0f);
        ImGui::End();

        // Snapshot the latest data (no heavy copies)
        const SpectrumFrame& spec = rf_spec.latest();

        //if (!paused) {
        //   rf_wf.linearize(wf_linear); // contiguous rows*cols
        //}


        const float* spec_plot = spec.db.data();

        if (enable_smoothing && (int)spec.db.size() == cfg.fft_size) {
            // initialize once (optional)
            static bool init = false;
            if (!init) {
                std::copy(spec.db.begin(), spec.db.end(), spec_smooth.begin());
                init = true;
            } else {
                const float a = smooth_alpha;
                const float b = 1.0f - a;
                for (int i = 0; i < cfg.fft_size; ++i) {
                    spec_smooth[i] = a * spec_smooth[i] + b * spec.db[i];
                }
            }
            spec_plot = spec_smooth.data();
        }


        // ---- Spectrum Plot ----
        ImGui::Begin("RF View");

        if (ImPlot::BeginPlot("Spectrum", ImVec2(-1, 260))) {
            // x axis in kHz is often nicer:
            // (Option: build a kHz axis once; but keeping Hz is fine)
            ImPlot::SetupAxes("Freq Offset (kHz)", "dB", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);

            // Manual y-limits so it doesn't jump around
            ImPlot::SetupAxisLimits(ImAxis_Y1, spec_db_min, spec_db_max, ImGuiCond_Always);

            if ((int)spec.db.size() == cfg.fft_size && (int)x_axis.size() == cfg.fft_size) {
                ImPlot::PlotLine("RF", x_axis.data(), spec_plot, cfg.fft_size);
            }
            ImPlot::EndPlot();
        }

        /*
        // ---- Waterfall Heatmap ----
        const int rows = rf_wf.rows();
        const int cols = rf_wf.bins();

        if (ImPlot::BeginPlot("Waterfall", ImVec2(-1, 360))) {
            ImPlot::SetupAxes(nullptr, nullptr,
                              ImPlotAxisFlags_NoTickLabels,
                              ImPlotAxisFlags_NoTickLabels);

            if (rows > 0 && cols == cfg.fft_size && (int)wf_linear.size() == rows * cols) {
                ImPlot::PlotHeatmap("wf", wf_linear.data(), rows, cols, wf_db_min, wf_db_max);
            }
            ImPlot::EndPlot();
        }
        */

        ImGui::End(); // RF View

        // Render
        ImGui::Render();
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

}