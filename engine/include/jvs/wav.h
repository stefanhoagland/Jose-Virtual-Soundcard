// Minimal WAV reading/writing (32-bit float out; 16/24/32-bit PCM or float in).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace jvs {

class WavWriter {
public:
    ~WavWriter();
    bool open(const std::string& path, int channels, uint32_t rate);
    void write(const float* interleaved, int frames);
    void close();

private:
    FILE* f_ = nullptr;
    int channels_ = 0;
    uint64_t frames_ = 0;
};

struct WavData {
    int channels = 0;
    uint32_t rate = 0;
    std::vector<float> samples;  // interleaved
    size_t frames() const { return channels ? samples.size() / size_t(channels) : 0; }
};

bool read_wav(const std::string& path, WavData& out, std::string* why = nullptr);

}  // namespace jvs
