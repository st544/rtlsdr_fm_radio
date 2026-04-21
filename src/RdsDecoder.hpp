#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

struct RdsSnapshot {
    bool synced = false;
    std::string pi;
    std::string program_service;
    std::string radio_text;
    uint64_t groups = 0;
    uint64_t blocks = 0;
};

class RdsDecoder {
public:
    explicit RdsDecoder(float sampleRate)
        : chip_samples_(sampleRate / 2375.0f),
          rds_bandpass_(sampleRate, 57000.0f, 4.0f) {
        program_service_.fill(' ');
        radio_text_.fill(' ');

        constexpr int phase_count = 16;
        paths_.reserve(phase_count * 2 * 2);

        for (int phase = 0; phase < phase_count; ++phase) {
            ChipClock clock;
            clock.countdown = (chip_samples_ * static_cast<float>(phase + 1)) / static_cast<float>(phase_count);
            clocks_.push_back(clock);

            for (int chip_offset = 0; chip_offset < 2; ++chip_offset) {
                for (int inverted = 0; inverted < 2; ++inverted) {
                    RdsPath path;
                    path.clock_index = phase;
                    path.chip_offset = chip_offset;
                    path.inverted = inverted != 0;
                    paths_.push_back(path);
                }
            }
        }
    }

    void process(float mpx, float pilotPhase) {
        constexpr float gain = 8.0f;
        mpx = rds_bandpass_.push(mpx);
        std::complex<float> rds_lo(std::cos(-3.0f * pilotPhase), std::sin(-3.0f * pilotPhase));
        std::complex<float> mixed = rds_lo * (mpx * gain);

        for (size_t i = 0; i < clocks_.size(); ++i) {
            ChipClock& clock = clocks_[i];
            clock.accum += mixed;
            clock.samples++;
            clock.countdown -= 1.0f;

            if (clock.countdown > 0.0f) {
                continue;
            }

            std::complex<float> chip{};
            if (clock.samples > 0) {
                chip = clock.accum / static_cast<float>(clock.samples);
            }

            clock.accum = {};
            clock.samples = 0;
            clock.countdown += chip_samples_;

            for (RdsPath& path : paths_) {
                if (path.clock_index == static_cast<int>(i)) {
                    processChip(path, chip);
                }
            }
        }
    }

    RdsSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mtx_);

        RdsSnapshot snap;
        snap.synced = synced_.load(std::memory_order_relaxed);
        snap.pi = pi_;
        snap.program_service = trimCopy(program_service_);
        snap.radio_text = trimCopy(radio_text_);
        snap.groups = groups_;
        snap.blocks = blocks_;
        return snap;
    }

private:
    enum class Offset {
        A,
        B,
        C,
        Cp,
        D,
        Unknown
    };

    struct ChipClock {
        float countdown = 0.0f;
        int samples = 0;
        std::complex<float> accum{};
    };

    struct BiquadBandpass {
        float b0 = 0.0f;
        float b1 = 0.0f;
        float b2 = 0.0f;
        float a1 = 0.0f;
        float a2 = 0.0f;
        float x1 = 0.0f;
        float x2 = 0.0f;
        float y1 = 0.0f;
        float y2 = 0.0f;

        BiquadBandpass(float sampleRate, float centerHz, float q) {
            const float pi = 3.14159265358979323846f;
            const float w0 = 2.0f * pi * centerHz / sampleRate;
            const float alpha = std::sin(w0) / (2.0f * q);
            const float c = std::cos(w0);
            const float a0 = 1.0f + alpha;

            b0 = alpha / a0;
            b1 = 0.0f;
            b2 = -alpha / a0;
            a1 = -2.0f * c / a0;
            a2 = (1.0f - alpha) / a0;
        }

        float push(float x) {
            const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            return y;
        }
    };

    struct BlockParser {
        uint32_t shift = 0;
        int bit_count = 0;
        int expected = 0;
        std::array<uint16_t, 4> data{};
        bool third_is_cp = false;
        uint64_t local_blocks = 0;
        uint64_t corrected_blocks = 0;
    };

    struct RdsPath {
        int clock_index = 0;
        int chip_offset = 0;
        bool inverted = false;
        int chip_index = 0;
        std::complex<float> chip_pair[2]{};
        bool have_prev_symbol = false;
        std::complex<float> prev_symbol{};
        BlockParser parser;
        uint16_t pi = 0;
        int pi_confidence = 0;
    };

    static constexpr uint16_t kOffsetA = 0x0fc;
    static constexpr uint16_t kOffsetB = 0x198;
    static constexpr uint16_t kOffsetC = 0x168;
    static constexpr uint16_t kOffsetCp = 0x350;
    static constexpr uint16_t kOffsetD = 0x1b4;
    static constexpr uint16_t kPoly = 0x5b9;

    struct DecodedBlock {
        Offset offset = Offset::Unknown;
        uint32_t corrected = 0;
        bool corrected_one_bit = false;
    };

    void processChip(RdsPath& path, std::complex<float> chip) {
        const int chip_number = path.chip_index++;
        if ((chip_number & 1) == path.chip_offset) {
            path.chip_pair[0] = chip;
            return;
        }

        path.chip_pair[1] = chip;
        std::complex<float> symbol = path.chip_pair[1] - path.chip_pair[0];

        if (path.have_prev_symbol) {
            float diff = std::real(symbol * std::conj(path.prev_symbol));
            bool bit = diff < 0.0f;
            if (path.inverted) {
                bit = !bit;
            }
            processBit(path, bit);
        }

        path.prev_symbol = symbol;
        path.have_prev_symbol = true;
    }

    void processBit(RdsPath& path, bool bit) {
        BlockParser& parser = path.parser;
        parser.shift = ((parser.shift << 1) | (bit ? 1u : 0u)) & 0x03ffffffu;
        parser.bit_count++;

        if (parser.bit_count < 26) {
            return;
        }

        DecodedBlock decoded = decodeBlock(parser.shift, path.pi_confidence >= 6);
        if (decoded.offset == Offset::Unknown) {
            return;
        }

        uint16_t data = static_cast<uint16_t>((decoded.corrected >> 10) & 0xffffu);
        if (decoded.corrected_one_bit) {
            parser.corrected_blocks++;
        }

        if (decoded.offset == Offset::A) {
            parser.expected = 1;
            parser.data[0] = data;
            parser.third_is_cp = false;
            parser.local_blocks++;
            updateBlockCounter(parser.local_blocks);
            return;
        }

        if (parser.expected == 1 && decoded.offset == Offset::B) {
            parser.expected = 2;
            parser.data[1] = data;
            parser.local_blocks++;
            updateBlockCounter(parser.local_blocks);
            return;
        }

        if (parser.expected == 2 && (decoded.offset == Offset::C || decoded.offset == Offset::Cp)) {
            parser.expected = 3;
            parser.data[2] = data;
            parser.third_is_cp = decoded.offset == Offset::Cp;
            parser.local_blocks++;
            updateBlockCounter(parser.local_blocks);
            return;
        }

        if (parser.expected == 3 && decoded.offset == Offset::D) {
            parser.expected = 0;
            parser.data[3] = data;
            parser.local_blocks++;
            updateBlockCounter(parser.local_blocks);
            parseGroup(path, parser.data, parser.third_is_cp);
            return;
        }

        parser.expected = 0;
    }

    static uint16_t syndrome(uint32_t block) {
        uint32_t reg = block;
        for (int bit = 25; bit >= 10; --bit) {
            if (reg & (1u << bit)) {
                reg ^= static_cast<uint32_t>(kPoly) << (bit - 10);
            }
        }
        return static_cast<uint16_t>(reg & 0x03ffu);
    }

    static Offset syndromeToOffset(uint16_t s) {
        if (s == kOffsetA) return Offset::A;
        if (s == kOffsetB) return Offset::B;
        if (s == kOffsetC) return Offset::C;
        if (s == kOffsetCp) return Offset::Cp;
        if (s == kOffsetD) return Offset::D;
        return Offset::Unknown;
    }

    static DecodedBlock decodeBlock(uint32_t block, bool allowCorrection) {
        const std::array<uint16_t, 5> offsets{kOffsetA, kOffsetB, kOffsetC, kOffsetCp, kOffsetD};
        const std::array<Offset, 5> offset_names{Offset::A, Offset::B, Offset::C, Offset::Cp, Offset::D};
        const uint16_t s = syndrome(block);

        for (size_t i = 0; i < offsets.size(); ++i) {
            if (s == offsets[i]) {
                return {offset_names[i], block, false};
            }
        }

        if (allowCorrection) {
            for (size_t i = 0; i < offsets.size(); ++i) {
                const uint16_t error_syndrome = static_cast<uint16_t>(s ^ offsets[i]);
                for (int bit = 0; bit < 26; ++bit) {
                    if (syndrome(1u << bit) == error_syndrome) {
                        return {offset_names[i], block ^ (1u << bit), true};
                    }
                }
            }
        }

        return {};
    }

    void parseGroup(RdsPath& path, const std::array<uint16_t, 4>& block, bool thirdIsCp) {
        const uint16_t pi = block[0];
        const uint16_t b = block[1];
        const int group_type = (b >> 12) & 0x0f;
        const bool version_b = ((b >> 11) & 0x01) != 0;

        if (path.pi == pi) {
            path.pi_confidence = std::min(path.pi_confidence + 1, 20);
        } else {
            path.pi = pi;
            path.pi_confidence = 1;
        }

        if (path.pi_confidence < 3) {
            return;
        }

        updateProgramIdentification(pi);
        synced_.store(true, std::memory_order_relaxed);

        if (group_type == 0) {
            const int segment = b & 0x03;
            updateProgramService(segment, block[3]);
            return;
        }

        if (group_type == 2 && !thirdIsCp) {
            const bool text_ab = ((b >> 4) & 0x01) != 0;
            const int segment = b & 0x0f;

            if (!version_b) {
                updateRadioText(text_ab, segment * 4, block[2]);
                updateRadioText(text_ab, segment * 4 + 2, block[3]);
            } else {
                updateRadioText(text_ab, segment * 2, block[3]);
            }
        }
    }

    void updateProgramIdentification(uint16_t pi) {
        std::lock_guard<std::mutex> lock(mtx_);

        char pi_buf[8]{};
        std::snprintf(pi_buf, sizeof(pi_buf), "%04X", pi);

        if (pi_ != pi_buf) {
            pi_ = pi_buf;
            std::fill(program_service_.begin(), program_service_.end(), ' ');
            std::fill(radio_text_.begin(), radio_text_.end(), ' ');
            ps_candidate_count_.fill(0);
            rt_candidate_count_.fill(0);
            have_text_ab_ = false;
        }

        groups_++;
    }

    void updateBlockCounter(uint64_t pathBlocks) {
        uint64_t old = blocks_.load(std::memory_order_relaxed);
        while (pathBlocks > old && !blocks_.compare_exchange_weak(old, pathBlocks, std::memory_order_relaxed)) {
        }
    }

    void updateProgramService(int segment, uint16_t chars) {
        if (segment < 0 || segment > 3) {
            return;
        }

        const char raw_hi = static_cast<char>((chars >> 8) & 0xff);
        const char raw_lo = static_cast<char>(chars & 0xff);
        const char hi = sanitizeChar(raw_hi);
        const char lo = sanitizeChar(raw_lo);

        std::lock_guard<std::mutex> lock(mtx_);
        if (ps_candidate_[segment][0] == hi && ps_candidate_[segment][1] == lo) {
            ps_candidate_count_[segment] = std::min<uint8_t>(3, ps_candidate_count_[segment] + 1);
        } else {
            ps_candidate_[segment][0] = hi;
            ps_candidate_[segment][1] = lo;
            ps_candidate_count_[segment] = 1;
        }

        if (ps_candidate_count_[segment] >= 2) {
            program_service_[segment * 2] = hi;
            program_service_[segment * 2 + 1] = lo;
        }
    }

    void updateRadioText(bool textAb, int offset, uint16_t chars) {
        if (offset < 0 || offset + 1 >= static_cast<int>(radio_text_.size())) {
            return;
        }

        const char raw_hi = static_cast<char>((chars >> 8) & 0xff);
        const char raw_lo = static_cast<char>(chars & 0xff);
        const char hi = sanitizeChar(raw_hi);
        const char lo = sanitizeChar(raw_lo);

        std::lock_guard<std::mutex> lock(mtx_);
        if (have_text_ab_ && textAb != text_ab_) {
            std::fill(radio_text_.begin(), radio_text_.end(), ' ');
            rt_candidate_count_.fill(0);
        }

        have_text_ab_ = true;
        text_ab_ = textAb;

        const int pair_index = offset / 2;
        if (rt_candidate_[pair_index][0] == hi && rt_candidate_[pair_index][1] == lo) {
            rt_candidate_count_[pair_index] = std::min<uint8_t>(3, rt_candidate_count_[pair_index] + 1);
        } else {
            rt_candidate_[pair_index][0] = hi;
            rt_candidate_[pair_index][1] = lo;
            rt_candidate_count_[pair_index] = 1;
        }

        if (rt_candidate_count_[pair_index] >= 1) {
            radio_text_[offset] = hi;
            radio_text_[offset + 1] = lo;

            if (raw_hi == '\r' || raw_lo == '\r') {
                const int end = raw_hi == '\r' ? offset : offset + 1;
                for (int i = end; i < static_cast<int>(radio_text_.size()); ++i) {
                    radio_text_[i] = ' ';
                }
            }
        }
    }

    static char sanitizeChar(char c) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc >= 32 && uc <= 126) {
            return static_cast<char>(uc);
        }
        return ' ';
    }

    static std::string trimCopy(const std::array<char, 8>& value) {
        return trimCopy(std::string(value.begin(), value.end()));
    }

    static std::string trimCopy(const std::array<char, 64>& value) {
        return trimCopy(std::string(value.begin(), value.end()));
    }

    static std::string trimCopy(std::string value) {
        while (!value.empty() && value.back() == ' ') {
            value.pop_back();
        }
        return value;
    }

    float chip_samples_;
    BiquadBandpass rds_bandpass_;
    std::vector<ChipClock> clocks_;
    std::vector<RdsPath> paths_;

    mutable std::mutex mtx_;
    std::atomic<bool> synced_{false};
    std::atomic<uint64_t> blocks_{0};
    uint64_t groups_ = 0;
    std::string pi_;
    std::array<char, 8> program_service_{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    std::array<char, 64> radio_text_{};
    std::array<std::array<char, 2>, 4> ps_candidate_{};
    std::array<uint8_t, 4> ps_candidate_count_{};
    std::array<std::array<char, 2>, 32> rt_candidate_{};
    std::array<uint8_t, 32> rt_candidate_count_{};
    bool have_text_ab_ = false;
    bool text_ab_ = false;
};
