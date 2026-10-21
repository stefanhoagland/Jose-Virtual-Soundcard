#include "jvs/media.h"

#include <cmath>

namespace jvs {

uint64_t media_time_from_ptp(int64_t ptp_ns, uint32_t rate, uint32_t offset) {
    if (ptp_ns < 0) ptp_ns = 0;
    uint64_t sec = uint64_t(ptp_ns / 1000000000LL);
    uint64_t rem = uint64_t(ptp_ns % 1000000000LL);
    return sec * rate + rem * rate / 1000000000ULL + offset;
}

int64_t ptp_from_media_time(uint64_t mt, uint32_t rate, uint32_t offset) {
    mt -= offset;
    uint64_t sec = mt / rate;
    uint64_t rem = mt % rate;
    return int64_t(sec * 1000000000ULL + (rem * 1000000000ULL + rate - 1) / rate);
}

AudioSource make_test_tone(int first_ch, int channels, uint32_t rate) {
    return [first_ch, channels, rate](uint64_t mt, float* out, int frames) {
        for (int i = 0; i < frames; ++i) {
            uint64_t t = (mt + uint64_t(i)) % rate;  // whole-Hz tones repeat every second
            for (int c = 0; c < channels; ++c) {
                uint64_t f_hz = uint64_t(200 + 100 * ((first_ch + c) % 64 + 1));
                double ph = 2.0 * M_PI * double(t * f_hz % rate) / double(rate);
                out[i * channels + c] = float(0.125 * std::sin(ph));
            }
        }
    };
}

}  // namespace jvs
