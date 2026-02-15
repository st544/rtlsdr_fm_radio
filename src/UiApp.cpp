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


static void BuildFreqAxis(std::vector<float>& x_freq, int N, int fs_hz, double center_freq_hz) {
    x_freq.resize(N);
    float center_mhz = (float)center_freq_hz / 1e6;
    float bandwidth_mhz = (float)fs_hz / 1e6;
    float start_mhz = center_mhz - (bandwidth_mhz/2.0f);
    float step_mhz = bandwidth_mhz / (float)N;
    for (int i = 0; i < N; i++) {
        x_freq[i] = start_mhz + (step_mhz * i);
    }
}

static int InvisibleYAxisFormatter(double value, char* buff, int size, void* data) {
    // Print 5-6 spaces. This is roughly the width of "-100.0"
    return snprintf(buff, size, "      "); 
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
    BuildFreqAxis(x_axis, cfg.fft_size, cfg.rf_sample_rate, cfg.center_freq_hz);

    static double link_x_min = cfg.center_freq_hz / 1.0e6 - 0.9; 
    static double link_x_max = cfg.center_freq_hz / 1.0e6 + 0.9; 

    std::vector<float> wf_linear;
    float wf_db_min = -70.0f;
    float wf_db_max = -30.0f;

    float spec_db_min = -85.0f;
    float spec_db_max = -20.0;

    static std::vector<float> spec_smooth;
    spec_smooth.resize(cfg.fft_size);

    float smooth_alpha = 0.75f; // 0=no smoothing, 0.95=lots of smoothing
    bool enable_smoothing = true;

    bool paused = false;
    bool quit = false;

    static double last_freq = cfg.center_freq_hz;


    while (!quit) {
        // Events
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) quit = true;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE) quit = true;
        }

        // Detect of center frequency changed
        if (cfg.center_freq_hz != last_freq) {
            
            double shift_mhz = (cfg.center_freq_hz - last_freq) / 1e6;

            BuildFreqAxis(x_axis, cfg.fft_size, cfg.rf_sample_rate, cfg.center_freq_hz);
            link_x_min += shift_mhz;
            link_x_max += shift_mhz;

            last_freq = cfg.center_freq_hz;
        }

        // New frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // ---- Play/Volume ----
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(250, 60), ImGuiCond_FirstUseEver);
        ImGui::Begin("##Transport", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);

        // Play button
        bool is_playing = cfg.stream_active->load();

        // Style: White Button, Black Text
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1.0f, 1.0f, 1.0f, 1.0f)); // White
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.8f, 0.8f, 1.0f)); // Light Grey
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.6f, 0.6f, 0.6f, 1.0f)); // Grey
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.0f, 0.0f, 0.0f, 1.0f)); // Black Text

        if (is_playing) {
            if (ImGui::Button("STOP", ImVec2(80, 40))) {
                cfg.stream_active->store(false);
            }
        } else {
            if (ImGui::Button("PLAY", ImVec2(80, 40))) {
                cfg.stream_active->store(true);
            }
        }
        
        ImGui::PopStyleColor(4);

        // Volume Slider
        ImGui::SameLine(); // Put next element on the same line
        
        // Read current volume atomic
        float current_vol = cfg.volume_level->load();
        
        // Setup vertical slider or knob style
        ImGui::PushItemWidth(120); // Width of slider
        
        // Vertical centering trick
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10); 
        if (ImGui::SliderFloat("Vol", &current_vol, 0.0f, 2.0f, "")) {
             cfg.volume_level->store(current_vol);
        }
        ImGui::PopItemWidth();

        ImGui::End();


        // ---- Controls ----
        ImGui::SetNextWindowPos(ImVec2(0, 60), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(250, 400), ImGuiCond_FirstUseEver);

        ImGui::Begin("Controls");

        // Tuning feature
        ImGui::Separator();
        //ImGui::Text("Tuner");
        float current_mhz = cfg.center_freq_hz / 1e6f;
        float step = 0.1f; // 100 kHz step

        ImGui::PushButtonRepeat(true);
        // --- STEP DOWN BUTTON (<) ---
        if (ImGui::ArrowButton("##Left", ImGuiDir_Left)) {
            if (cfg.retune_callback) {
                float new_freq = std::max(88.0f, current_mhz - step); // Subtract 0.1 MHz and clamp to 88.0
                cfg.retune_callback(new_freq);
            }
        }
        ImGui::SameLine();
        ImGui::Text("  Center freq: %.1f MHz  ", current_mhz);
        ImGui::SameLine();

        // --- STEP UP BUTTON (>) ---
        if (ImGui::ArrowButton("##Right", ImGuiDir_Right)) {
            if (cfg.retune_callback) {
                float new_freq = std::min(108.0f, current_mhz + step); // Add 0.1 MHz and clamp to 108.0
                cfg.retune_callback(new_freq);
            }
        }

        // Disable Repeat 
        ImGui::PopButtonRepeat();
        
        // Tooltip for help
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Click to step 100kHz, Hold to scan");
        }

        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Sample rate: %d Hz", cfg.rf_sample_rate);
        ImGui::Text("FFT: %d", cfg.fft_size);

        // Gain control
        int current_gain = cfg.rf_gain->load();
        float gain_db = current_gain / 10.0f; // Convert 300 -> 30.0

        // Slider from 0.0dB to 50.0dB
        if (ImGui::SliderFloat("RF Gain", &gain_db, 0.0f, 50.0f, "%.1f dB")) {
            int new_gain = (int)(gain_db * 10.0f);
            
            // Call the lambda to update hardware
            if (cfg.set_gain_callback) {
                cfg.set_gain_callback(new_gain);
            } else {
                cfg.rf_gain->store(new_gain);
            }
        }

        ImGui::SliderFloat("RF dB min", &spec_db_min, -100.0f, 0.0f);
        ImGui::SliderFloat("RF dB max", &spec_db_max, -100.0f, 0.0f);
        ImGui::SliderFloat("WF dB min", &wf_db_min, -180.0f, 0.0f);
        ImGui::SliderFloat("WF dB max", &wf_db_max, -180.0f, 0.0f);
        ImGui::SliderFloat("Smooth", &smooth_alpha, 0.0f, 0.98f);

        ImGui::Spacing();

        if (ImGui::Button("Reset Defaults")) {
            spec_db_min = -60.0f;
            spec_db_max = -20.0f;
            smooth_alpha = 0.75f;
            wf_db_min = -60.0f;
            wf_db_max = -20.0f;
        }


        ImGui::End();


        // Snapshot the latest data (no heavy copies)
        const SpectrumFrame& spec = rf_spec.latest();

        if (!paused) {
           rf_wf.linearize(wf_linear); // contiguous rows*cols
        }


        const float* spec_plot = spec.db.data();

        // Apply Exponential Moving Average to smooth RF Spectrum plot (single-pole IIR low pass)
        if (enable_smoothing && (int)spec.db.size() == cfg.fft_size) {
            static bool init = false;
            if (is_playing) {
                if (!init) {
                    std::copy(spec.db.begin(), spec.db.end(), spec_smooth.begin());
                    init = true;
                } else {
                    const float a = smooth_alpha;
                    const float b = 1.0f - a;
                    for (int i = 0; i < cfg.fft_size; ++i) {
                        spec_smooth[i] = a * spec_smooth[i] + b * spec.db[i];   // EMA = (val * a) + (prev_val * (1-a))
                    }
                }
            }
            spec_plot = spec_smooth.data();

        }


        // ---- Spectrum Plot ----
        ImGui::SetNextWindowPos(ImVec2(250, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(1030, 720), ImGuiCond_FirstUseEver);

        ImGui::Begin("RF View");

        if (ImPlot::BeginPlot("Spectrum", ImVec2(-1, 260))) {
            // x axis in MHz
            ImPlot::SetupAxisFormat(ImAxis_X1, "%.1f");
            ImPlot::SetupAxes("Freq (MHz)", "dB"); 

            // Manual y-limits so it doesn't jump around
            ImPlot::SetupAxisLimits(ImAxis_Y1, spec_db_min, spec_db_max, ImGuiCond_Always);

            // Link X-Axis
            ImPlot::SetupAxisLinks(ImAxis_X1, &link_x_min, &link_x_max);
 
             // 100kHz spacing only if bandwidth is < 2MHz
            double current_width = link_x_max - link_x_min;
            if (current_width < 2.0) {
                double start_tick = ceil(link_x_min * 10.0) / 10.0;
                double end_tick = floor(link_x_max * 10.0) / 10.0;
                int tick_count = (int)(round((end_tick - start_tick) / 0.1)) + 1;

                if (tick_count > 1) {
                    ImPlot::SetupAxisTicks(ImAxis_X1, start_tick, end_tick, tick_count);
                }
            }

            if ((int)spec.db.size() == cfg.fft_size && (int)x_axis.size() == cfg.fft_size) {
                ImPlot::PlotLine("RF", x_axis.data(), spec_plot, cfg.fft_size);
            }
            ImPlot::EndPlot();
        }

        ImGui::Spacing();
        
        // ---- Waterfall Heatmap ----
        // X-Axis: Frequency in MHz 
        double x_min = x_axis.front();
        double x_max = x_axis.back();
        
        // Y-Axis: Time/History (0 to Height)
        double y_min = 0;
        double y_max = rf_wf.max_rows();

        if (ImPlot::BeginPlot("##Waterfall", ImVec2(-1, -1))) { // -1,-1 fills remaining space

            // Setup Axes
            ImPlot::SetupAxes(nullptr, "dB");
            ImPlot::SetupAxis(ImAxis_Y1, nullptr);
            ImPlot::SetupAxis(ImAxis_Y1, nullptr);
            ImPlot::SetupAxisFormat(ImAxis_Y1, InvisibleYAxisFormatter);
            ImPlot::SetupAxisLinks(ImAxis_X1, &link_x_min, &link_x_max);    // Link X-Axis (Connects to the same variables as Spectrum)

            // 100kHz increments
            double current_width = link_x_max - link_x_min;
            if (current_width < 2.0) {
                double start_tick = ceil(link_x_min * 10.0) / 10.0;
                double end_tick = floor(link_x_max * 10.0) / 10.0;
                int tick_count = (int)(round((end_tick - start_tick) / 0.1)) + 1;

                if (tick_count > 1) {
                    ImPlot::SetupAxisTicks(ImAxis_X1, start_tick, end_tick, tick_count);
                }
            }
            ImPlot::SetupAxisLimits(ImAxis_Y1, y_min, y_max, ImGuiCond_Always);

            // Color Map (Jet is standard for waterfalls)
            ImPlot::PushColormap(ImPlotColormap_Jet);

            // Draw Heatmap
            // rows = current filled height, cols = fft bins
            int rows = wf_linear.size() / cfg.fft_size;
            int cols = cfg.fft_size;
            
            if (rows > 0) {
                double bottom_y = y_max - rows;
                // Point 1 (Bottom-Left): x_min, top_y
                // Point 2 (Top-Right):   x_max, bottom_y
                ImPlot::PlotHeatmap("##WF", wf_linear.data(), rows, cols, 
                                    wf_db_min, wf_db_max, 
                                    nullptr, 
                                    {x_min, y_max}, {x_max, bottom_y});
            }

            ImPlot::PopColormap();
            ImPlot::EndPlot();
        }
        

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