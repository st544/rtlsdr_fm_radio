cmake_minimum_required(VERSION 3.21)
project(rtlsdr_fm_radio LANGUAGES CXX)

add_executable(FM_Radio src/main.cpp src/UiApp.cpp)
set_target_properties(FM_Radio PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

find_package(rtlsdr CONFIG REQUIRED)
find_package(portaudio CONFIG REQUIRED)
find_package(FFTW3f CONFIG REQUIRED)
find_package(SDL2 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(OpenGL REQUIRED)

# ---- vendored imgui ----
add_library(imgui STATIC
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_sdl2.cpp
    external/imgui/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    external/imgui
    external/imgui/backends
)
target_link_libraries(imgui PUBLIC SDL2::SDL2 glad::glad OpenGL::GL)

# ---- vendored implot ----
add_library(implot STATIC
    external/implot/implot.cpp
    external/implot/implot_items.cpp
)
target_include_directories(implot PUBLIC external/implot)
target_link_libraries(implot PUBLIC imgui)

target_link_libraries(FM_Radio PRIVATE 
        rtlsdr::rtlsdr 
        portaudio 
        FFTW3::fftw3f
        SDL2::SDL2
        SDL2::SDL2main
        OpenGL::GL
        imgui 
        implot
)

add_executable(DSPPipelineTest test/DSPPipelineTest.cpp)
set_target_properties(DSPPipelineTest PROPERTIES CXX_STANDARD 20)

target_link_libraries(DSPPipelineTest PRIVATE FFTW3::fftw3f)

