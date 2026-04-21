#include "WebServer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {
std::filesystem::path WebRoot() {
#ifdef RTLSDR_WEB_ROOT
    return std::filesystem::path(RTLSDR_WEB_ROOT);
#else
    return std::filesystem::path("web");
#endif
}

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "[HTTP] could not open " << path.string() << "\n";
        return {};
    }

    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

template <typename T>
void AppendBytes(std::string& out, const T& value) {
    const char* bytes = reinterpret_cast<const char*>(&value);
    out.append(bytes, sizeof(T));
}
}

WebSocketStreamer::WebSocketStreamer(int port)
    : port_(port) {}

WebSocketStreamer::~WebSocketStreamer() {
    stop();
}

void WebSocketStreamer::start() {
    if (running_.exchange(true)) {
        return;
    }

    thread_ = std::thread([this] {
        uWS::App app;
        app_ = &app;
        loop_ = uWS::Loop::get();

        uWS::App::WebSocketBehavior<PerSocketData> audio_behavior;
        audio_behavior.compression = uWS::DISABLED;
        audio_behavior.maxPayloadLength = 64 * 1024;
        audio_behavior.maxBackpressure = 256 * 1024;
        audio_behavior.closeOnBackpressureLimit = false;

        audio_behavior.open = [](auto* ws) {
            ws->subscribe("audio");
            std::cout << "[WS] audio client connected\n";
        };

        audio_behavior.close = [](auto*, int, std::string_view) {
            std::cout << "[WS] audio client disconnected\n";
        };

        uWS::App::WebSocketBehavior<PerSocketData> spectrum_behavior;
        spectrum_behavior.compression = uWS::DISABLED;
        spectrum_behavior.maxPayloadLength = 256 * 1024;
        spectrum_behavior.maxBackpressure = 512 * 1024;
        spectrum_behavior.closeOnBackpressureLimit = false;

        spectrum_behavior.open = [](auto* ws) {
            ws->subscribe("spectrum");
            std::cout << "[WS] spectrum client connected\n";
        };

        spectrum_behavior.close = [](auto*, int, std::string_view) {
            std::cout << "[WS] spectrum client disconnected\n";
        };

        uWS::App::WebSocketBehavior<PerSocketData> rds_behavior;
        rds_behavior.compression = uWS::DISABLED;
        rds_behavior.maxPayloadLength = 16 * 1024;
        rds_behavior.maxBackpressure = 64 * 1024;
        rds_behavior.closeOnBackpressureLimit = false;

        rds_behavior.open = [](auto* ws) {
            ws->subscribe("rds");
            std::cout << "[WS] RDS client connected\n";
        };

        rds_behavior.close = [](auto*, int, std::string_view) {
            std::cout << "[WS] RDS client disconnected\n";
        };

        app.get("/", [](auto* res, auto*) {
                std::string html = ReadTextFile(WebRoot() / "index.html");
                if (html.empty()) {
                    res->writeStatus("500 Internal Server Error")
                        ->end("Could not load web/index.html. Check the server log for the resolved path.");
                    return;
                }

                res->writeHeader("Content-Type", "text/html; charset=utf-8")
                    ->end(html);
            })
            .get("/audio-worklet.js", [](auto* res, auto*) {
                std::string js = ReadTextFile(WebRoot() / "audio-worklet.js");
                if (js.empty()) {
                    res->writeStatus("500 Internal Server Error")
                        ->end("Could not load web/audio-worklet.js. Check the server log for the resolved path.");
                    return;
                }

                res->writeHeader("Content-Type", "application/javascript; charset=utf-8")
                    ->end(js);
            })
            .ws<PerSocketData>("/audio", std::move(audio_behavior))
            .ws<PerSocketData>("/spectrum", std::move(spectrum_behavior))
            .ws<PerSocketData>("/rds", std::move(rds_behavior))
            .listen(port_, [this](auto* token) {
                if (token) {
                    std::cout << "[WS] listening on http://localhost:" << port_ << "/\n";
                } else {
                    std::cerr << "[WS] failed to listen on port " << port_ << "\n";
                    running_.store(false);
                }
            })
            .run();

        app_ = nullptr;
        loop_ = nullptr;
        running_.store(false);
    });
}

void WebSocketStreamer::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (loop_ && app_) {
        loop_->defer([this] {
            if (app_) {
                app_->close();
            }
        });
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

void WebSocketStreamer::publishAudioPcm16(const float* interleavedStereo, size_t sampleCount) {
    if (!running_.load(std::memory_order_relaxed) || !loop_) {
        return;
    }

    std::string frame;
    frame.resize(sampleCount * sizeof(int16_t));

    auto* pcm = reinterpret_cast<int16_t*>(frame.data());

    for (size_t i = 0; i < sampleCount; ++i) {
        float x = std::clamp(interleavedStereo[i], -1.0f, 1.0f);
        pcm[i] = static_cast<int16_t>(std::lrintf(x * 32767.0f));
    }

    loop_->defer([this, frame = std::move(frame)] {
        if (app_) {
            app_->publish("audio", frame, uWS::OpCode::BINARY, false);
        }
    });
}

void WebSocketStreamer::publishRds(std::string json) {
    if (!running_.load(std::memory_order_relaxed) || !loop_) {
        return;
    }

    loop_->defer([this, json = std::move(json)] {
        if (app_) {
            app_->publish("rds", json, uWS::OpCode::TEXT, false);
        }
    });
}

void WebSocketStreamer::publishSpectrum(const float* db, size_t binCount, double centerFreqHz, int sampleRateHz) {
    if (!running_.load(std::memory_order_relaxed) || !loop_ || binCount == 0) {
        return;
    }

    constexpr uint32_t magic = 0x31534652; // "RFS1" in little-endian byte order
    const uint32_t bins = static_cast<uint32_t>(binCount);
    const uint32_t sampleRate = static_cast<uint32_t>(sampleRateHz);
    const uint32_t reserved = 0;

    std::string frame;
    frame.reserve(24 + binCount * sizeof(float));
    AppendBytes(frame, magic);
    AppendBytes(frame, bins);
    AppendBytes(frame, centerFreqHz);
    AppendBytes(frame, sampleRate);
    AppendBytes(frame, reserved);
    frame.append(reinterpret_cast<const char*>(db), binCount * sizeof(float));

    loop_->defer([this, frame = std::move(frame)] {
        if (app_) {
            app_->publish("spectrum", frame, uWS::OpCode::BINARY, false);
        }
    });
}
