// Plays two channels to the Mac's default output, so you can listen to a
// received stream without a DAW. Not available on other systems.
#pragma once
#include <cstdint>
#include <deque>
#include <mutex>

namespace jvs {

class Player {
public:
    ~Player();
    bool start(uint32_t rate);
    // Queue stereo interleaved frames.
    void push(const float* stereo, int frames);
    void stop();
    uint64_t underruns() const { return underruns_; }

    // Called by the audio callback.
    void pull(float* stereo, int frames);

private:
    void* queue_ = nullptr;
    std::mutex mu_;
    std::deque<float> fifo_;
    uint64_t underruns_ = 0;
    uint32_t rate_ = 48000;
};

}  // namespace jvs
