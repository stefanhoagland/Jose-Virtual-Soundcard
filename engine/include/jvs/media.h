// The media clock (PTP time counted in samples) and the built-in test signal.
#pragma once
#include <cstdint>
#include <functional>

namespace jvs {

// Samples since the PTP epoch, plus an offset.
uint64_t media_time_from_ptp(int64_t ptp_ns, uint32_t rate, uint32_t offset = 0);
int64_t ptp_from_media_time(uint64_t media_time, uint32_t rate, uint32_t offset = 0);

// Fills `frames` interleaved samples that start at media_time.
using AudioSource = std::function<void(uint64_t media_time, float* interleaved, int frames)>;

// Test tones: channel n (1-based, counting from first_channel) plays
// 200 + 100*n Hz at -18 dBFS, so every channel can be told apart.
AudioSource make_test_tone(int first_channel, int channels, uint32_t rate = 48000);

}  // namespace jvs
