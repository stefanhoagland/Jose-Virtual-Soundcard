#include "jvs/wav.h"

#include <cstring>

namespace jvs {

namespace {
void put32(FILE* f, uint32_t v) {
    uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
    std::fwrite(b, 1, 4, f);
}
void put16(FILE* f, uint16_t v) {
    uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
    std::fwrite(b, 1, 2, f);
}
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
}  // namespace

WavWriter::~WavWriter() { close(); }

bool WavWriter::open(const std::string& path, int channels, uint32_t rate) {
    close();
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return false;
    channels_ = channels;
    frames_ = 0;
    std::fwrite("RIFF", 1, 4, f_);
    put32(f_, 0);
    std::fwrite("WAVE", 1, 4, f_);
    std::fwrite("fmt ", 1, 4, f_);
    put32(f_, 16);
    put16(f_, 3);  // IEEE float
    put16(f_, uint16_t(channels));
    put32(f_, rate);
    put32(f_, rate * uint32_t(channels) * 4);
    put16(f_, uint16_t(channels * 4));
    put16(f_, 32);
    std::fwrite("data", 1, 4, f_);
    put32(f_, 0);
    return true;
}

void WavWriter::write(const float* in, int frames) {
    if (!f_) return;
    std::fwrite(in, sizeof(float), size_t(frames) * size_t(channels_), f_);  // little-endian hosts only
    frames_ += uint64_t(frames);
}

void WavWriter::close() {
    if (!f_) return;
    uint64_t data = frames_ * uint64_t(channels_) * 4;
    if (data > 0xFFFFFFFFull - 36) data = 0xFFFFFFFFull - 36;
    std::fseek(f_, 4, SEEK_SET);
    put32(f_, uint32_t(36 + data));
    std::fseek(f_, 40, SEEK_SET);
    put32(f_, uint32_t(data));
    std::fclose(f_);
    f_ = nullptr;
}

bool read_wav(const std::string& path, WavData& out, std::string* why) {
    auto fail = [&](const char* m) {
        if (why) *why = m;
        return false;
    };
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return fail("cannot open file");
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4))
        return fail("not a WAV file");
    int fmt = 0, bits = 0;
    size_t pos = 12;
    const uint8_t* data = nullptr;
    size_t data_len = 0;
    while (pos + 8 <= d.size()) {
        uint32_t len = get32(&d[pos + 4]);
        const uint8_t* body = &d[pos + 8];
        size_t avail = d.size() - pos - 8;
        if (!std::memcmp(&d[pos], "fmt ", 4) && len >= 16 && avail >= 16) {
            fmt = get16(body);
            out.channels = get16(body + 2);
            out.rate = get32(body + 4);
            bits = get16(body + 14);
            if (fmt == 0xFFFE && len >= 26) fmt = get16(body + 24);  // WAVE_FORMAT_EXTENSIBLE
        } else if (!std::memcmp(&d[pos], "data", 4)) {
            data = body;
            data_len = std::min<size_t>(len, avail);
        }
        pos += 8 + len + (len & 1);
    }
    if (!data || out.channels <= 0) return fail("missing fmt or data chunk");
    size_t bytes = size_t(bits / 8);
    if (bytes == 0) return fail("bad bit depth");
    size_t count = data_len / bytes;
    out.samples.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* p = data + i * bytes;
        float v;
        if (fmt == 3 && bits == 32) {
            std::memcpy(&v, p, 4);
        } else if (fmt == 1 && bits == 16) {
            v = float(int16_t(get16(p))) / 32768.0f;
        } else if (fmt == 1 && bits == 24) {
            int32_t s = int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 8;
            v = float(s) / 8388608.0f;
        } else if (fmt == 1 && bits == 32) {
            v = float(int32_t(get32(p))) / 2147483648.0f;
        } else {
            return fail("unsupported WAV format (use 16/24/32-bit PCM or 32-bit float)");
        }
        out.samples[i] = v;
    }
    return true;
}

}  // namespace jvs
