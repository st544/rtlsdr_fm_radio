cmake_minimum_required(VERSION 3.21)
project(rtlsdr_fm_radio LANGUAGES CXX)

add_executable(FM_Radio src/main.cpp)
set_target_properties(FM_Radio PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

find_package(rtlsdr CONFIG REQUIRED)
find_package(portaudio CONFIG REQUIRED)

target_link_libraries(FM_Radio PRIVATE rtlsdr::rtlsdr portaudio)