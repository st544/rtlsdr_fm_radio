cmake_minimum_required(VERSION 3.21)
project(rtlsdr_fm_radio LANGUAGES CXX)

add_executable(FM_Radio src/main.cpp)
set_target_properties(FM_Radio PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

find_package(rtlsdr CONFIG REQUIRED)
find_package(portaudio CONFIG REQUIRED)
find_package(FFTW3f CONFIG REQUIRED)

target_link_libraries(FM_Radio PRIVATE rtlsdr::rtlsdr portaudio FFTW3::fftw3f)

add_executable(DSPPipelineTest test/DSPPipelineTest.cpp)
set_target_properties(DSPPipelineTest PROPERTIES CXX_STANDARD 20)

target_link_libraries(DSPPipelineTest PRIVATE FFTW3::fftw3f)

