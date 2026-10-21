#include "jvs/player.h"

#include <algorithm>

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#endif

namespace jvs {

Player::~Player() { stop(); }

void Player::push(const float* stereo, int frames) {
    std::lock_guard<std::mutex> lk(mu_);
    fifo_.insert(fifo_.end(), stereo, stereo + frames * 2);
    // Keep at most ~100 ms queued; the Mac's clock and the network clock
    // drift apart slowly, so trim instead of growing forever.
    size_t max = size_t(rate_ / 10) * 2;
    if (fifo_.size() > max) fifo_.erase(fifo_.begin(), fifo_.begin() + long(fifo_.size() - max / 2));
}

void Player::pull(float* out, int frames) {
    std::lock_guard<std::mutex> lk(mu_);
    size_t want = size_t(frames) * 2;
    size_t have = std::min(want, fifo_.size());
    std::copy(fifo_.begin(), fifo_.begin() + long(have), out);
    fifo_.erase(fifo_.begin(), fifo_.begin() + long(have));
    if (have < want) {
        std::fill(out + have, out + want, 0.0f);
        underruns_++;
    }
}

#if defined(__APPLE__)

static void aq_callback(void* user, AudioQueueRef q, AudioQueueBufferRef buf) {
    auto* p = static_cast<Player*>(user);
    int frames = int(buf->mAudioDataBytesCapacity / (2 * sizeof(float)));
    p->pull(static_cast<float*>(buf->mAudioData), frames);
    buf->mAudioDataByteSize = UInt32(frames * 2 * sizeof(float));
    AudioQueueEnqueueBuffer(q, buf, 0, nullptr);
}

bool Player::start(uint32_t rate) {
    stop();
    rate_ = rate;
    AudioStreamBasicDescription f{};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 32;
    f.mBytesPerFrame = 8;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = 8;
    AudioQueueRef q = nullptr;
    if (AudioQueueNewOutput(&f, aq_callback, this, nullptr, nullptr, 0, &q) != noErr) return false;
    for (int i = 0; i < 3; ++i) {
        AudioQueueBufferRef b;
        AudioQueueAllocateBuffer(q, UInt32((rate / 100) * 8), &b);  // 10 ms
        aq_callback(this, q, b);
    }
    if (AudioQueueStart(q, nullptr) != noErr) {
        AudioQueueDispose(q, true);
        return false;
    }
    queue_ = q;
    return true;
}

void Player::stop() {
    if (!queue_) return;
    auto q = static_cast<AudioQueueRef>(queue_);
    AudioQueueStop(q, true);
    AudioQueueDispose(q, true);
    queue_ = nullptr;
}

#else

bool Player::start(uint32_t) { return false; }
void Player::stop() {}

#endif

}  // namespace jvs
