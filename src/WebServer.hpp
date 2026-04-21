#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <uwebsockets/App.h>

class WebSocketStreamer {
public:
    explicit WebSocketStreamer(int port = 9001);
    ~WebSocketStreamer();

    void start();
    void stop();

    void publishAudioPcm16(const float* interleavedStereo, size_t sampleCount);
    void publishSpectrum(const float* db, size_t binCount, double centerFreqHz, int sampleRateHz);
    void publishRds(std::string json);

private:
    struct PerSocketData {};

    int port_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    uWS::Loop* loop_ = nullptr;
    uWS::App* app_ = nullptr;
};
