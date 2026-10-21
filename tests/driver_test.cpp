// End-to-end test of the installed Core Audio driver (macOS, run with sudo):
//
//  1. a fake Dante clock leader (PTP) on the given network port,
//  2. the driver's device is configured through its 'jcfg' property,
//  3. a second Dante device B runs in this process,
//  4. we play 1 kHz into the driver's output 1 and check B receives it,
//     without clicks, while B's test tones arrive on the driver's input.
//
// Usage: jvs_driver_test <network port> [ptp version 1|2]
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "jvs/clock.h"
#include "jvs/dante_device.h"
#include "test_master.h"

using namespace jvs;

namespace {

constexpr AudioObjectPropertySelector kPropConfig = 'jcfg';
constexpr AudioObjectPropertySelector kPropStatus = 'jsts';
constexpr AudioObjectPropertySelector kPropLog = 'jlog';
constexpr int kCh = 64;

void wait_ms(int ms) { sleep_until_mono(mono_ns() + int64_t(ms) * 1000000); }

AudioObjectPropertyAddress addr(AudioObjectPropertySelector s,
                                AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return {s, scope, kAudioObjectPropertyElementMain};
}

AudioObjectID find_device() {
    CFStringRef uid = CFSTR("JoseVirtualSoundcard_UID");
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 size = sizeof dev;
    AudioValueTranslation t{&uid, sizeof uid, &dev, sizeof dev};
    size = sizeof t;
    auto a = addr(kAudioHardwarePropertyDeviceForUID);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, &t) != 0) return kAudioObjectUnknown;
    return dev;
}

std::string get_string(AudioObjectID dev, AudioObjectPropertySelector sel) {
    CFStringRef s = nullptr;
    UInt32 size = sizeof s;
    auto a = addr(sel);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &s) != 0 || !s) return "(unreadable)";
    std::vector<char> buf(size_t(CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8)) + 1);
    CFStringGetCString(s, buf.data(), CFIndex(buf.size()), kCFStringEncodingUTF8);
    CFRelease(s);
    return buf.data();
}

bool set_config(AudioObjectID dev, const std::string& iface, int ptp) {
    CFMutableDictionaryRef d =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    auto put_str = [&](CFStringRef k, const char* v) {
        CFStringRef cv = CFStringCreateWithCString(nullptr, v, kCFStringEncodingUTF8);
        CFDictionarySetValue(d, k, cv);
        CFRelease(cv);
    };
    auto put_int = [&](CFStringRef k, int v) {
        CFNumberRef n = CFNumberCreate(nullptr, kCFNumberIntType, &v);
        CFDictionarySetValue(d, k, n);
        CFRelease(n);
    };
    CFDictionarySetValue(d, CFSTR("enabled"), kCFBooleanTrue);
    put_str(CFSTR("name"), "jvs-drv");
    put_str(CFSTR("iface"), iface.c_str());
    put_int(CFSTR("rate"), 48000);
    put_int(CFSTR("latency_us"), 10000);
    put_int(CFSTR("ptp"), ptp);
    put_int(CFSTR("rx"), 8);
    put_int(CFSTR("tx"), 8);
    CFDictionarySetValue(d, CFSTR("tones"), kCFBooleanFalse);
    // Driver input 1 and 2 receive device B's channels 1 and 2.
    CFStringRef subs[2] = {CFSTR("1=TX 1@jvs-b"), CFSTR("2=TX 2@jvs-b")};
    CFArrayRef arr = CFArrayCreate(nullptr, reinterpret_cast<const void**>(subs), 2, &kCFTypeArrayCallBacks);
    CFDictionarySetValue(d, CFSTR("subscribe"), arr);
    CFRelease(arr);
    CFPropertyListRef pl = d;
    auto a = addr(kPropConfig);
    OSStatus st = AudioObjectSetPropertyData(dev, &a, 0, nullptr, sizeof pl, &pl);
    CFRelease(d);
    if (st) std::printf("setting 'jcfg' failed: %d\n", int(st));
    return st == 0;
}

// What the IO callback saw.
struct IoState {
    std::atomic<uint64_t> out_frames{0};
    std::mutex mu;
    std::vector<float> in_ch1;  // driver input channel 1, last ~2 s
    double phase = 0;
};

OSStatus io_proc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* in, const AudioTimeStamp*,
                 AudioBufferList* out, const AudioTimeStamp*, void* user) {
    auto* st = static_cast<IoState*>(user);
    if (out && out->mNumberBuffers > 0) {
        auto& b = out->mBuffers[0];
        const UInt32 ch = b.mNumberChannels;
        const UInt32 frames = b.mDataByteSize / (4 * ch);
        auto* p = static_cast<float*>(b.mData);
        for (UInt32 f = 0; f < frames; ++f) {
            float v = float(0.25 * std::sin(st->phase));
            st->phase += 2 * M_PI * 1000.0 / 48000.0;
            if (st->phase > 2 * M_PI) st->phase -= 2 * M_PI;
            for (UInt32 c = 0; c < ch; ++c) p[f * ch + c] = c == 0 ? v : 0.0f;
        }
        st->out_frames += frames;
    }
    if (in && in->mNumberBuffers > 0) {
        auto& b = in->mBuffers[0];
        const UInt32 ch = b.mNumberChannels;
        const UInt32 frames = b.mDataByteSize / (4 * ch);
        auto* p = static_cast<const float*>(b.mData);
        std::lock_guard<std::mutex> lk(st->mu);
        for (UInt32 f = 0; f < frames; ++f) st->in_ch1.push_back(p[f * ch]);
        if (st->in_ch1.size() > 96000) st->in_ch1.erase(st->in_ch1.begin(), st->in_ch1.end() - 96000);
    }
    return 0;
}

// Amplitude of a tone at hz in x (48 kHz).
double tone_amp(const float* x, size_t n, double hz) {
    double re = 0, im = 0;
    for (size_t i = 0; i < n; ++i) {
        re += x[i] * std::cos(2 * M_PI * hz * double(i) / 48000);
        im += x[i] * std::sin(2 * M_PI * hz * double(i) / 48000);
    }
    return 2 * std::hypot(re, im) / double(n);
}

// Counts jumps in the waveform; prints where the first few are.
int clicks(const float* x, size_t n, uint64_t start = 0, uint64_t m0 = 0) {
    int c = 0;
    for (size_t i = 2; i < n; ++i)
        if (std::fabs(x[i] - 2 * x[i - 1] + x[i - 2]) > 0.02f) {
            if (c < 12 && start)
                std::printf("  click at media %llu (device sample %llu, mod 16384 = %llu): %.3f %.3f %.3f\n",
                            (unsigned long long)(start + i), (unsigned long long)(start + i - m0),
                            (unsigned long long)((start + i - m0) % 16384), x[i - 2], x[i - 1], x[i]);
            ++c;
        }
    return c;
}

uint64_t json_u64(const std::string& j, const char* key) {
    auto p = j.find(std::string("\"") + key + "\":");
    return p == std::string::npos ? 0 : std::strtoull(j.c_str() + p + std::strlen(key) + 3, nullptr, 10);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <network port> [ptp version]\n", argv[0]);
        return 2;
    }
    std::string ifname = argv[1];
    int ptp = argc > 2 ? std::atoi(argv[2]) : 1;
    NetInterface ni;
    if (!find_interface(ifname, ni)) return 2;

    AudioObjectID dev = kAudioObjectUnknown;
    for (int i = 0; i < 30 && dev == kAudioObjectUnknown; ++i) {
        dev = find_device();
        if (dev == kAudioObjectUnknown) wait_ms(1000);
    }
    if (dev == kAudioObjectUnknown) {
        std::printf("FAIL: the Jose Virtual Soundcard device is not in Core Audio\n");
        return 1;
    }
    std::printf("found device %u: %s\n", dev, get_string(dev, kAudioObjectPropertyName).c_str());
    {
        UInt32 n = 0, size = 0;
        auto a = addr(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput);
        AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size);
        std::vector<uint8_t> buf(size);
        if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, buf.data()) == 0) {
            auto* bl = reinterpret_cast<AudioBufferList*>(buf.data());
            for (UInt32 i = 0; i < bl->mNumberBuffers; ++i) n += bl->mBuffers[i].mNumberChannels;
        }
        std::printf("output channels: %u\n", n);
        if (n != kCh) {
            std::printf("FAIL: expected %d output channels\n", kCh);
            return 1;
        }
    }

    TestMaster master;
    if (!master.start(ni, 0, ptp)) {
        std::printf("FAIL: couldn't start the test clock leader\n");
        return 1;
    }
    if (!set_config(dev, ifname, ptp)) return 1;

    bool running = false;
    std::string status;
    for (int i = 0; i < 60 && !running; ++i) {
        wait_ms(1000);
        status = get_string(dev, kPropStatus);
        running = status.find("\"state\":\"running\"") != std::string::npos;
    }
    std::printf("driver status: %s\n", status.c_str());
    if (!running) {
        std::printf("FAIL: the driver's Dante device didn't start\n%s\n", get_string(dev, kPropLog).c_str());
        return 1;
    }

    DanteConfig cb;
    cb.name = "jvs-b";
    cb.iface = ifname;
    cb.rx_channels = 2;
    cb.tx_channels = 2;
    cb.process_id = 2;
    cb.alt_port = 41100;
    cb.ptp_mode = ptp == 1 ? PtpMode::V1 : PtpMode::V2;
    DanteDevice b;
    std::string err;
    if (!b.start(cb, &err)) {
        std::printf("FAIL: device B: %s\n", err.c_str());
        return 1;
    }
    b.set_test_tones(true);
    for (int i = 0; i < 300 && b.state() != DanteState::Running; ++i) wait_ms(100);
    wait_ms(3000);  // let discovery settle
    b.subscribe(0, "TX 1", "jvs-drv");

    // Count Core Audio overloads (the IO thread missed its deadline).
    static std::atomic<int> overloads{0};
    {
        auto a = addr(kAudioDeviceProcessorOverload);
        AudioObjectAddPropertyListener(
            dev, &a,
            [](AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void*) -> OSStatus {
                ++overloads;
                return 0;
            },
            nullptr);
    }
    IoState io;
    AudioDeviceIOProcID proc = nullptr;
    if (AudioDeviceCreateIOProcID(dev, io_proc, &io, &proc) != 0 || AudioDeviceStart(dev, proc) != 0) {
        std::printf("FAIL: couldn't start audio on the device\n");
        return 1;
    }

    bool out_ok = false, in_ok = false;
    for (int attempt = 0; attempt < 30 && !(out_ok && in_ok); ++attempt) {
        wait_ms(1000);
        uint64_t t;
        if (b.now(t)) {
            const int N = 4800;
            std::vector<float> buf(size_t(N) * 2), ch1(N);
            b.read_rx(t - uint64_t(N) - 480, N, buf.data());
            for (int i = 0; i < N; ++i) ch1[size_t(i)] = buf[size_t(i) * 2];
            double amp = tone_amp(ch1.data(), N, 1000);
            std::printf("t+%ds: Mac output 1 -> Dante: 1 kHz at %.3f (want 0.25); played %llu frames", attempt + 1, amp,
                        (unsigned long long)io.out_frames.load());
            out_ok = amp > 0.2 && amp < 0.3;
        }
        {
            std::lock_guard<std::mutex> lk(io.mu);
            if (io.in_ch1.size() >= 4800) {
                double amp = tone_amp(io.in_ch1.data() + io.in_ch1.size() - 4800, 4800, 300);
                std::printf("; Dante -> Mac input 1: 300 Hz at %.3f (want 0.125)", amp);
                in_ok = amp > 0.1 && amp < 0.15;
            }
        }
        std::printf("\n");
    }

    // Clicks: record what B receives on channel 1 for 3 s.
    auto record_b = [&](std::vector<float>& rec) -> uint64_t {
        const int B = 2400;
        std::vector<float> blk(size_t(B) * 2);
        uint64_t t0 = 0;
        b.now(t0);
        uint64_t pos = t0 - 4800;
        while (pos < t0 + 3 * 48000) {
            uint64_t now = 0;
            b.now(now);
            if (pos + B + 960 > now) {
                wait_ms(5);
                continue;
            }
            b.read_rx(pos, B, blk.data());
            for (int i = 0; i < B; ++i) rec.push_back(blk[size_t(i) * 2]);
            pos += B;
        }
        return t0 - 4800;
    };
    int out_clicks = -1;
    if (out_ok) {
        std::vector<float> rec;
        uint64_t start = record_b(rec);
        uint64_t m0 = json_u64(get_string(dev, kPropStatus), "m0");
        out_clicks = clicks(rec.data(), rec.size(), start, m0);
        std::printf("clicks in 3 s from the Mac to Dante: %d\n", out_clicks);
    }
    int in_clicks = -1;
    if (in_ok) {
        wait_ms(2000);
        std::lock_guard<std::mutex> lk(io.mu);
        in_clicks = clicks(io.in_ch1.data(), io.in_ch1.size(), 1, 0);
        std::printf("clicks in %.1f s from Dante to the Mac: %d\n", double(io.in_ch1.size()) / 48000, in_clicks);
    }

    AudioDeviceStop(dev, proc);
    AudioDeviceDestroyIOProcID(dev, proc);

    // For comparison: the driver's own test tones (no Core Audio in the path).
    {
        auto a = addr(kPropConfig);
        CFMutableDictionaryRef d =
            CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(d, CFSTR("tones"), kCFBooleanTrue);
        CFPropertyListRef pl = d;
        AudioObjectSetPropertyData(dev, &a, 0, nullptr, sizeof pl, &pl);
        CFRelease(d);
        wait_ms(1500);
        std::vector<float> rec;
        uint64_t start = record_b(rec);
        int c = clicks(rec.data(), rec.size(), start, 0);
        std::printf("for comparison, clicks in the driver's own test tone (Core Audio not involved): %d, level %.3f\n", c,
                    tone_amp(rec.data(), 4800, 300));
    }
    std::printf("driver status: %s\n", get_string(dev, kPropStatus).c_str());
    std::printf("Core Audio overloads during the test: %d\n", overloads.load());
    bool ok = out_ok && in_ok && out_clicks == 0 && in_clicks == 0;
    if (!ok) std::printf("driver log:\n%s\n", get_string(dev, kPropLog).c_str());
    b.stop();
    master.stop();
    std::printf(ok ? "driver test passed\n" : "driver test FAILED\n");
    return ok ? 0 : 1;
}
