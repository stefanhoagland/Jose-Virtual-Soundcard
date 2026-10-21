// Jose Virtual Soundcard - Core Audio driver (AudioServerPlugIn).
// Copyright (C) 2026 Jose Virtual Soundcard contributors. GPL-3.0-or-later.
//
// One audio device, "Jose Virtual Soundcard", with 64 inputs and 64 outputs.
// The Dante engine runs inside the driver, so the device works whether or not
// the app is open. The app only changes settings and shows status, through
// three custom properties on the device:
//
//   'jcfg'  settings dictionary (read/write), saved by Core Audio
//   'jsts'  status as JSON text (read only)
//   'jlog'  recent log text (read only)
//
// Timing: while the Dante clock is locked, the device's sample clock is the
// Dante media clock (PTP time in samples), so audio leaves and arrives exactly
// in step with the network. Device sample S is media time m0 + S. Without a
// Dante clock the device runs on the Mac's own clock and passes silence.

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <initializer_list>
#include <iterator>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "jvs/clock.h"
#include "jvs/dante_device.h"
#include "../dante/jvs_dante.h"

using namespace jvs;

namespace {

// ---------------------------------------------------------------- constants

constexpr AudioObjectID kPlugIn = kAudioObjectPlugInObject;  // 1
constexpr AudioObjectID kDevice = 2;
constexpr AudioObjectID kInStream = 3;
constexpr AudioObjectID kOutStream = 4;

constexpr UInt32 kChannels = 64;
constexpr UInt32 kZeroPeriod = 16384;  // frames between zero timestamps
constexpr Float64 kRates[] = {44100.0, 48000.0, 88200.0, 96000.0};

constexpr AudioObjectPropertySelector kPropConfig = 'jcfg';
constexpr AudioObjectPropertySelector kPropStatus = 'jsts';
constexpr AudioObjectPropertySelector kPropLog = 'jlog';

#define JVS_DEVICE_UID "JoseVirtualSoundcard_UID"
#define JVS_MODEL_UID "JoseVirtualSoundcard_Model"
#define JVS_BUNDLE_ID "com.josevirtualsoundcard.driver"
#ifndef JVS_VERSION
#define JVS_VERSION "dev"
#endif

// ---------------------------------------------------------------- settings

struct Settings {
    bool enabled = true;
    std::string name = "Jose-Virtual-Soundcard";
    std::string iface;  // empty: not set up yet
    int rate = 48000;
    int latency_us = 10000;
    int ptp_mode = 0;  // 0 auto, 1 PTPv1, 2 PTPv2
    int rx = 64;
    int tx = 64;
    bool tones = false;
    // Routing to set up once the device runs, "local=TX channel@device"
    // (local channel from 1). Used by tests; Dante Controller does the rest.
    std::vector<std::string> subscribe;

    bool same_engine(const Settings& o) const {
        return enabled == o.enabled && name == o.name && iface == o.iface && rate == o.rate &&
               latency_us == o.latency_us && ptp_mode == o.ptp_mode && rx == o.rx && tx == o.tx;
    }
};

std::string cf_string(CFTypeRef v) {
    if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return {};
    auto s = static_cast<CFStringRef>(v);
    if (const char* p = CFStringGetCStringPtr(s, kCFStringEncodingUTF8)) return p;
    char buf[1024];
    if (CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return buf;
    return {};
}

bool cf_int(CFTypeRef v, int& out) {
    if (!v || CFGetTypeID(v) != CFNumberGetTypeID()) return false;
    return CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberIntType, &out);
}

bool cf_bool(CFTypeRef v, bool& out) {
    if (!v) return false;
    if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
        out = CFBooleanGetValue(static_cast<CFBooleanRef>(v));
        return true;
    }
    int i;
    if (cf_int(v, i)) {
        out = i != 0;
        return true;
    }
    return false;
}

// Reads the keys present in d over s (missing keys keep their value).
void settings_from_dict(CFDictionaryRef d, Settings& s) {
    if (!d || CFGetTypeID(d) != CFDictionaryGetTypeID()) return;
    auto get = [&](const char* k) -> CFTypeRef {
        CFStringRef key = CFStringCreateWithCString(nullptr, k, kCFStringEncodingUTF8);
        CFTypeRef v = CFDictionaryGetValue(d, key);
        CFRelease(key);
        return v;
    };
    bool b;
    int i;
    std::string str;
    if (cf_bool(get("enabled"), b)) s.enabled = b;
    if (!(str = cf_string(get("name"))).empty()) s.name = str;
    if (CFTypeRef v = get("iface")) s.iface = cf_string(v);
    if (cf_int(get("rate"), i) && std::find(std::begin(kRates), std::end(kRates), Float64(i)) != std::end(kRates))
        s.rate = i;
    if (cf_int(get("latency_us"), i)) s.latency_us = std::clamp(i, 1000, 40000);
    if (cf_int(get("ptp"), i)) s.ptp_mode = std::clamp(i, 0, 2);
    if (cf_int(get("rx"), i)) s.rx = std::clamp(i, 0, int(kChannels));
    if (cf_int(get("tx"), i)) s.tx = std::clamp(i, 0, int(kChannels));
    if (cf_bool(get("tones"), b)) s.tones = b;
    if (CFTypeRef v = get("subscribe"); v && CFGetTypeID(v) == CFArrayGetTypeID()) {
        s.subscribe.clear();
        auto arr = static_cast<CFArrayRef>(v);
        for (CFIndex k = 0; k < CFArrayGetCount(arr); ++k) s.subscribe.push_back(cf_string(CFArrayGetValueAtIndex(arr, k)));
    }
}

CFDictionaryRef settings_to_dict(const Settings& s) {
    CFMutableDictionaryRef d =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    auto put_str = [&](CFStringRef k, const std::string& v) {
        CFStringRef cv = CFStringCreateWithCString(nullptr, v.c_str(), kCFStringEncodingUTF8);
        CFDictionarySetValue(d, k, cv);
        CFRelease(cv);
    };
    auto put_int = [&](CFStringRef k, int v) {
        CFNumberRef n = CFNumberCreate(nullptr, kCFNumberIntType, &v);
        CFDictionarySetValue(d, k, n);
        CFRelease(n);
    };
    CFDictionarySetValue(d, CFSTR("enabled"), s.enabled ? kCFBooleanTrue : kCFBooleanFalse);
    put_str(CFSTR("name"), s.name);
    put_str(CFSTR("iface"), s.iface);
    put_int(CFSTR("rate"), s.rate);
    put_int(CFSTR("latency_us"), s.latency_us);
    put_int(CFSTR("ptp"), s.ptp_mode);
    put_int(CFSTR("rx"), s.rx);
    put_int(CFSTR("tx"), s.tx);
    CFDictionarySetValue(d, CFSTR("tones"), s.tones ? kCFBooleanTrue : kCFBooleanFalse);
    return d;
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += c;
        } else if (uint8_t(c) < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else {
            o += c;
        }
    }
    return o;
}

// ---------------------------------------------------------------- host time

struct HostClock {
    double ticks_per_ns = 1.0;
    HostClock() {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        ticks_per_ns = double(tb.denom) / double(tb.numer);
    }
    // Mach host time at monotonic time mono (both clocks run together).
    UInt64 host_from_mono(int64_t mono) const {
        int64_t now_mono = mono_ns();
        UInt64 now_host = mach_absolute_time();
        return UInt64(int64_t(now_host) + int64_t(std::llround(double(mono - now_mono) * ticks_per_ns)));
    }
};

// ---------------------------------------------------------------- driver state

struct Driver {
    AudioServerPlugInHostRef host = nullptr;
    HostClock clock;

    std::mutex mu;          // settings, status strings
    Settings settings;      // what the user asked for
    std::string last_error;
    std::atomic<Float64> rate{48000.0};  // the device's nominal rate

    // Engine. The IO thread uses it only while io_users > 0 and engine_ok.
    std::unique_ptr<DanteDevice> engine;
    std::atomic<bool> engine_ok{false};
    std::atomic<int> io_users{0};

    // Applies settings on its own thread (starting Dante takes a moment).
    std::thread worker;
    std::condition_variable cv;
    bool apply_pending = false;
    bool quit = false;

    // IO state (IO thread only, after StartIO).
    std::atomic<UInt32> io_clients{0};
    bool dante_timing = false;  // zero timestamps follow the Dante clock
    uint64_t m0 = 0;            // media time of device sample 0
    UInt64 host_anchor = 0;     // free-running: host time of sample 0
    UInt64 seed = 1;
    Float64 last_sample = 0;    // last zero timestamp's sample time
    std::atomic<bool> tones{false};
    // IO health, for the status: output cycles written too late to be sent,
    // jumps in the sample time, and the smallest head start (in frames).
    std::atomic<uint64_t> late_writes{0}, out_gaps{0}, out_cycles{0};
    std::atomic<int64_t> min_margin{1 << 30};
    uint64_t next_out = 0;
    std::atomic<uint64_t> m0_shared{0};

    std::atomic<UInt32> ref_count{1};
} g;

// Locks the engine for the IO thread. Never blocks.
struct EngineUse {
    DanteDevice* e = nullptr;
    EngineUse() {
        g.io_users.fetch_add(1, std::memory_order_acquire);
        if (g.engine_ok.load(std::memory_order_acquire)) e = g.engine.get();
    }
    ~EngineUse() { g.io_users.fetch_sub(1, std::memory_order_release); }
};

void notify(AudioObjectID obj, std::initializer_list<AudioObjectPropertySelector> sels) {
    if (!g.host) return;
    std::vector<AudioObjectPropertyAddress> a;
    for (auto s : sels) a.push_back({s, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain});
    g.host->PropertiesChanged(g.host, obj, UInt32(a.size()), a.data());
}

void save_settings() {
    if (!g.host) return;
    Settings s;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        s = g.settings;
    }
    CFDictionaryRef d = settings_to_dict(s);
    g.host->WriteToStorage(g.host, CFSTR("settings"), d);
    CFRelease(d);
}

std::string temp_dir() {
    char buf[1024];
    size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof buf);
    if (n > 0 && n <= sizeof buf) return buf;
    return "/tmp/";
}

// Stops the engine, waiting until the IO thread lets go of it.
void stop_engine() {
    g.engine_ok.store(false, std::memory_order_release);
    while (g.io_users.load(std::memory_order_acquire) > 0) std::this_thread::yield();
    if (g.engine) g.engine->stop();
    g.engine.reset();
}

void apply_settings() {
    Settings s;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        s = g.settings;
    }
    stop_engine();
    std::string err;
    if (s.enabled && !s.iface.empty()) {
        DanteConfig c;
        c.name = s.name;
        c.iface = s.iface;
        c.rx_channels = s.rx;
        c.tx_channels = s.tx;
        c.sample_rate = int(g.rate.load());
        c.rx_latency_us = s.latency_us;
        c.tx_latency_us = s.latency_us;
        c.ptp_mode = s.ptp_mode == 1 ? PtpMode::V1 : s.ptp_mode == 2 ? PtpMode::V2 : PtpMode::Auto;
        c.external_tx = true;
        auto e = std::make_unique<DanteDevice>();
        if (e->start(c, &err)) {
            e->set_test_tones(s.tones);
            g.engine = std::move(e);
            g.engine_ok.store(true, std::memory_order_release);
            err.clear();
        }
    }
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.last_error = err;
    }
    notify(kDevice, {kPropStatus});
    if (!g.engine_ok.load() || s.subscribe.empty()) return;
    // Wait (up to 60 s) for the device to run, then set up the routing.
    for (int i = 0; i < 600; ++i) {
        {
            std::lock_guard<std::mutex> lk(g.mu);
            if (g.quit || g.apply_pending) return;
        }
        if (g.engine->state() == DanteState::Running) break;
        sleep_until_mono(mono_ns() + 100000000);
    }
    for (const auto& sub : s.subscribe) {
        auto eq = sub.find('='), at = sub.rfind('@');
        if (eq == std::string::npos || at == std::string::npos || at < eq) continue;
        int local = std::atoi(sub.substr(0, eq).c_str());
        if (local >= 1) g.engine->subscribe(local - 1, sub.substr(eq + 1, at - eq - 1), sub.substr(at + 1));
    }
}

void worker_loop() {
    std::unique_lock<std::mutex> lk(g.mu);
    for (;;) {
        g.cv.wait(lk, [] { return g.apply_pending || g.quit; });
        if (g.quit) break;
        g.apply_pending = false;
        lk.unlock();
        apply_settings();
        lk.lock();
    }
    lk.unlock();
    stop_engine();
}

void request_apply() {
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.apply_pending = true;
    }
    g.cv.notify_one();
}

const char* state_name(DanteState s) {
    switch (s) {
    case DanteState::WaitingForClock: return "waiting_for_clock";
    case DanteState::Starting: return "starting";
    case DanteState::Running: return "running";
    case DanteState::Failed: return "failed";
    default: return "off";
    }
}

std::string status_json() {
    Settings s;
    std::string err;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        s = g.settings;
        err = g.last_error;
    }
    std::string j = "{";
    char b[512];
    std::snprintf(b, sizeof b, "\"version\":\"%s\",\"rate\":%d,\"io_running\":%s,\"tones\":%s,", JVS_VERSION,
                  int(g.rate.load()), g.io_clients.load() > 0 ? "true" : "false", s.tones ? "true" : "false");
    j += b;
    j += "\"error\":\"" + json_escape(err) + "\",";
    std::snprintf(b, sizeof b, "\"out_cycles\":%llu,\"late_writes\":%llu,\"out_gaps\":%llu,\"min_margin\":%lld,\"m0\":%llu,",
                  (unsigned long long)g.out_cycles.load(), (unsigned long long)g.late_writes.load(),
                  (unsigned long long)g.out_gaps.load(), (long long)g.min_margin.load(),
                  (unsigned long long)g.m0_shared.load());
    j += b;
    // Holds the engine like the IO thread does, so it can't be stopped meanwhile.
    EngineUse use;
    DanteDevice* e = use.e;
    if (!e) {
        j += std::string("\"state\":\"") + (s.enabled && !s.iface.empty() && !err.empty() ? "failed" : "off") + "\"}";
        return j;
    }
    PtpStatus c = e->clock_status();
    std::snprintf(b, sizeof b,
                  "\"state\":\"%s\",\"ptp_version\":%d,\"have_master\":%s,\"locked\":%s,\"master_ip\":\"%u.%u.%u.%u\","
                  "\"delay_us\":%.1f,\"jitter_us\":%.1f,\"rate_ppm\":%.3f,\"syncs\":%llu,\"arc_port\":%d,\"rx\":%d,\"tx\":%d,",
                  state_name(e->state()), c.version, c.have_master ? "true" : "false", c.locked ? "true" : "false",
                  (c.master_ip >> 24) & 255, (c.master_ip >> 16) & 255, (c.master_ip >> 8) & 255, c.master_ip & 255,
                  double(c.path_delay_ns) / 1000.0, c.offset_jitter_ns / 1000.0, c.rate_ppm,
                  (unsigned long long)c.syncs, e->arc_port(), e->config().rx_channels, e->config().tx_channels);
    j += b;
    j += "\"grandmaster\":\"" + json_escape(c.grandmaster) + "\",";
    j += "\"name\":\"" + json_escape(e->config().name) + "\",";
    // Peak level of each received channel over the last 100 ms.
    j += "\"peak_db\":[";
    uint64_t t;
    const int ch = e->config().rx_channels;
    if (ch > 0 && e->state() == DanteState::Running && e->now(t)) {
        const int n = e->config().sample_rate / 10;
        std::vector<float> buf(size_t(n) * size_t(ch));
        e->read_rx(t - uint64_t(n), n, buf.data());
        for (int c2 = 0; c2 < ch; ++c2) {
            float pk = 0;
            for (int i = 0; i < n; ++i) pk = std::max(pk, std::fabs(buf[size_t(i) * size_t(ch) + size_t(c2)]));
            std::snprintf(b, sizeof b, "%s%.1f", c2 ? "," : "", pk > 0 ? 20.0 * std::log10(pk) : -120.0);
            j += b;
        }
    }
    j += "]}";
    return j;
}

std::string log_text() {
    std::string out;
    std::vector<char> buf(48 * 1024);
    jvsd_net_summary(buf.data(), int(buf.size()));
    out += "Control traffic by port:\n";
    out += *buf.data() ? buf.data() : "  nothing received yet\n";
    jvsd_log_tail(buf.data(), int(buf.size()));
    out += "Log:\n";
    out += buf.data();
    return out;
}

// ---------------------------------------------------------------- formats

AudioStreamBasicDescription format_for(Float64 rate) {
    AudioStreamBasicDescription f{};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4 * kChannels;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4 * kChannels;
    f.mChannelsPerFrame = kChannels;
    f.mBitsPerChannel = 32;
    return f;
}

UInt32 frames_for_us(int us) { return UInt32(g.rate.load() * us / 1e6); }

// ---------------------------------------------------------------- plug-in interface

HRESULT QueryInterface(void* drv, REFIID iid, LPVOID* out);
ULONG AddRef(void*);
ULONG Release(void*);
OSStatus Initialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef host);
OSStatus CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*) {
    return kAudioHardwareUnsupportedOperationError;
}
OSStatus DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID) { return kAudioHardwareUnsupportedOperationError; }
OSStatus AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*) { return 0; }
OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*) { return 0; }
OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64 action, void*);
OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*) { return 0; }
Boolean HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*);
OSStatus IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, Boolean*);
OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32,
                             const void*, UInt32*);
OSStatus GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32,
                         const void*, UInt32, UInt32*, void*);
OSStatus SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32,
                         const void*, UInt32, const void*);
OSStatus StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
OSStatus StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64*, UInt64*, UInt64*);
OSStatus WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32 op, Boolean* will, Boolean* inPlace);
OSStatus BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) {
    return 0;
}
OSStatus DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID stream, UInt32, UInt32 op, UInt32 frames,
                       const AudioServerPlugInIOCycleInfo* cycle, void* main, void*);
OSStatus EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) {
    return 0;
}

AudioServerPlugInDriverInterface gInterface = {
    nullptr,
    QueryInterface,
    AddRef,
    Release,
    Initialize,
    CreateDevice,
    DestroyDevice,
    AddDeviceClient,
    RemoveDeviceClient,
    PerformDeviceConfigurationChange,
    AbortDeviceConfigurationChange,
    HasProperty,
    IsPropertySettable,
    GetPropertyDataSize,
    GetPropertyData,
    SetPropertyData,
    StartIO,
    StopIO,
    GetZeroTimeStamp,
    WillDoIOOperation,
    BeginIOOperation,
    DoIOOperation,
    EndIOOperation,
};
AudioServerPlugInDriverInterface* gInterfacePtr = &gInterface;
AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

HRESULT QueryInterface(void* drv, REFIID iid, LPVOID* out) {
    if (drv != gDriverRef || !out) return kAudioHardwareIllegalOperationError;
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(nullptr, iid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
        g.ref_count.fetch_add(1);
        *out = gDriverRef;
        r = S_OK;
    }
    CFRelease(req);
    return r;
}

ULONG AddRef(void* drv) {
    if (drv != gDriverRef) return 0;
    return g.ref_count.fetch_add(1) + 1;
}

ULONG Release(void* drv) {
    if (drv != gDriverRef) return 0;
    UInt32 n = g.ref_count.load();
    if (n > 0) n = g.ref_count.fetch_sub(1) - 1;
    return n;
}

OSStatus Initialize(AudioServerPlugInDriverRef drv, AudioServerPlugInHostRef host) {
    if (drv != gDriverRef) return kAudioHardwareBadObjectError;
    g.host = host;
    // Saved settings.
    CFPropertyListRef saved = nullptr;
    host->CopyFromStorage(host, CFSTR("settings"), &saved);
    {
        std::lock_guard<std::mutex> lk(g.mu);
        if (saved) settings_from_dict(static_cast<CFDictionaryRef>(saved), g.settings);
        g.settings.tones = false;  // test tones never come back on by themselves
        g.rate.store(Float64(g.settings.rate));
        g.tones.store(g.settings.tones);
    }
    if (saved) CFRelease(saved);
    // Inferno keeps routing (subscriptions) in files; use a folder we may write.
    std::string dir = temp_dir() + "JoseVirtualSoundcard";
    jvsd_set_state_dir(dir.c_str());
    jvsd_set_log_file((temp_dir() + "JoseVirtualSoundcard-driver.log").c_str());
    g.worker = std::thread(worker_loop);
    request_apply();
    return 0;
}

// Sample-rate changes go through Core Audio's configuration-change request.
OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt64 action, void*) {
    if (drv != gDriverRef || dev != kDevice) return kAudioHardwareBadObjectError;
    Float64 r = Float64(action);
    if (std::find(std::begin(kRates), std::end(kRates), r) == std::end(kRates)) return kAudioHardwareIllegalOperationError;
    g.rate.store(r);
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.settings.rate = int(r);
    }
    save_settings();
    request_apply();
    return 0;
}

// ---------------------------------------------------------------- properties

// Small helper: copy a POD value out, checking the space.
template <typename T>
OSStatus put(UInt32 size, UInt32* out_size, void* out, const T& v) {
    if (size < sizeof(T)) return kAudioHardwareBadPropertySizeError;
    std::memcpy(out, &v, sizeof(T));
    *out_size = sizeof(T);
    return 0;
}

bool is_stream(AudioObjectID o) { return o == kInStream || o == kOutStream; }

Boolean HasProperty(AudioServerPlugInDriverRef drv, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress* a) {
    Boolean settable;
    UInt32 size;
    if (drv != gDriverRef || !a) return false;
    (void)settable;
    (void)pid;
    return GetPropertyDataSize(drv, obj, pid, a, 0, nullptr, &size) == 0;
}

OSStatus IsPropertySettable(AudioServerPlugInDriverRef drv, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress* a,
                            Boolean* out) {
    if (drv != gDriverRef || !a || !out) return kAudioHardwareIllegalOperationError;
    UInt32 size;
    OSStatus st = GetPropertyDataSize(drv, obj, pid, a, 0, nullptr, &size);
    if (st) return st;
    *out = false;
    if (obj == kDevice && (a->mSelector == kAudioDevicePropertyNominalSampleRate || a->mSelector == kPropConfig)) *out = true;
    if (is_stream(obj) && (a->mSelector == kAudioStreamPropertyVirtualFormat ||
                           a->mSelector == kAudioStreamPropertyPhysicalFormat || a->mSelector == kAudioStreamPropertyIsActive))
        *out = true;
    return 0;
}

OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef drv, AudioObjectID obj, pid_t, const AudioObjectPropertyAddress* a,
                             UInt32, const void*, UInt32* out) {
    if (drv != gDriverRef || !a || !out) return kAudioHardwareIllegalOperationError;
    const auto sel = a->mSelector;
    switch (obj) {
    case kPlugIn:
        switch (sel) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass: *out = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner: *out = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyManufacturer:
        case kAudioPlugInPropertyResourceBundle: *out = sizeof(CFStringRef); return 0;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: *out = sizeof(AudioObjectID); return 0;
        case kAudioPlugInPropertyTranslateUIDToDevice: *out = sizeof(AudioObjectID); return 0;
        }
        break;
    case kDevice:
        switch (sel) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass: *out = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner: *out = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID: *out = sizeof(CFStringRef); return 0;
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyClockIsStable: *out = sizeof(UInt32); return 0;
        case kAudioDevicePropertyRelatedDevices: *out = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
            if (a->mScope == kAudioObjectPropertyScopeGlobal) *out = 2 * sizeof(AudioObjectID);
            else *out = sizeof(AudioObjectID);
            return 0;
        case kAudioObjectPropertyControlList: *out = 0; return 0;
        case kAudioDevicePropertyNominalSampleRate: *out = sizeof(Float64); return 0;
        case kAudioDevicePropertyAvailableNominalSampleRates:
            *out = UInt32(std::size(kRates) * sizeof(AudioValueRange));
            return 0;
        case kAudioDevicePropertyPreferredChannelsForStereo: *out = 2 * sizeof(UInt32); return 0;
        case kAudioDevicePropertyPreferredChannelLayout:
            *out = UInt32(offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription));
            return 0;
        case kAudioObjectPropertyCustomPropertyInfoList: *out = 3 * sizeof(AudioServerPlugInCustomPropertyInfo); return 0;
        case kPropConfig: *out = sizeof(CFPropertyListRef); return 0;
        case kPropStatus:
        case kPropLog: *out = sizeof(CFStringRef); return 0;
        }
        break;
    case kInStream:
    case kOutStream:
        switch (sel) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass: *out = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner: *out = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyOwnedObjects: *out = 0; return 0;
        case kAudioObjectPropertyName: *out = sizeof(CFStringRef); return 0;
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency: *out = sizeof(UInt32); return 0;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: *out = sizeof(AudioStreamBasicDescription); return 0;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            *out = UInt32(std::size(kRates) * sizeof(AudioStreamRangedDescription));
            return 0;
        }
        break;
    }
    return kAudioHardwareUnknownPropertyError;
}

OSStatus GetPropertyData(AudioServerPlugInDriverRef drv, AudioObjectID obj, pid_t, const AudioObjectPropertyAddress* a,
                         UInt32 qsize, const void* qdata, UInt32 size, UInt32* out_size, void* out) {
    if (drv != gDriverRef || !a || !out_size || !out) return kAudioHardwareIllegalOperationError;
    const auto sel = a->mSelector;
    const bool input = a->mScope == kAudioObjectPropertyScopeInput;
    const bool output = a->mScope == kAudioObjectPropertyScopeOutput;
    switch (obj) {
    case kPlugIn:
        switch (sel) {
        case kAudioObjectPropertyBaseClass: return put<AudioClassID>(size, out_size, out, kAudioObjectClassID);
        case kAudioObjectPropertyClass: return put<AudioClassID>(size, out_size, out, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: return put<AudioObjectID>(size, out_size, out, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer:
            return put<CFStringRef>(size, out_size, out, CFSTR("Jose Virtual Soundcard"));
        case kAudioPlugInPropertyResourceBundle: return put<CFStringRef>(size, out_size, out, CFSTR(""));
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
            if (size < sizeof(AudioObjectID)) {
                *out_size = 0;
                return 0;
            }
            return put<AudioObjectID>(size, out_size, out, kDevice);
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            if (qsize != sizeof(CFStringRef) || !qdata) return kAudioHardwareBadPropertySizeError;
            CFStringRef uid = *static_cast<const CFStringRef*>(qdata);
            AudioObjectID id = (uid && CFStringCompare(uid, CFSTR(JVS_DEVICE_UID), 0) == kCFCompareEqualTo) ? kDevice
                                                                                                                : kAudioObjectUnknown;
            return put<AudioObjectID>(size, out_size, out, id);
        }
        }
        break;

    case kDevice:
        switch (sel) {
        case kAudioObjectPropertyBaseClass: return put<AudioClassID>(size, out_size, out, kAudioObjectClassID);
        case kAudioObjectPropertyClass: return put<AudioClassID>(size, out_size, out, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: return put<AudioObjectID>(size, out_size, out, kPlugIn);
        case kAudioObjectPropertyName: return put<CFStringRef>(size, out_size, out, CFSTR("Jose Virtual Soundcard"));
        case kAudioObjectPropertyManufacturer:
            return put<CFStringRef>(size, out_size, out, CFSTR("Jose Virtual Soundcard"));
        case kAudioDevicePropertyDeviceUID: return put<CFStringRef>(size, out_size, out, CFSTR(JVS_DEVICE_UID));
        case kAudioDevicePropertyModelUID: return put<CFStringRef>(size, out_size, out, CFSTR(JVS_MODEL_UID));
        case kAudioDevicePropertyTransportType:
            return put<UInt32>(size, out_size, out, kAudioDeviceTransportTypeVirtual);
        case kAudioDevicePropertyRelatedDevices: return put<AudioObjectID>(size, out_size, out, kDevice);
        case kAudioDevicePropertyClockDomain: return put<UInt32>(size, out_size, out, 0);
        case kAudioDevicePropertyDeviceIsAlive: return put<UInt32>(size, out_size, out, 1);
        case kAudioDevicePropertyDeviceIsRunning: return put<UInt32>(size, out_size, out, g.io_clients.load() > 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: return put<UInt32>(size, out_size, out, 1);
        case kAudioDevicePropertyLatency: {
            int us;
            {
                std::lock_guard<std::mutex> lk(g.mu);
                us = g.settings.latency_us;
            }
            return put<UInt32>(size, out_size, out, frames_for_us(us));
        }
        case kAudioDevicePropertySafetyOffset: {
            // Output is sent the moment its time comes, so it must be written
            // ahead: half the latency setting (at least 3 ms) leaves room for
            // a late wake-up. Input is ready right away.
            int us;
            {
                std::lock_guard<std::mutex> lk(g.mu);
                us = g.settings.latency_us;
            }
            return put<UInt32>(size, out_size, out, input ? frames_for_us(1000) : frames_for_us(std::max(3000, us / 2)));
        }
        case kAudioDevicePropertyIsHidden: return put<UInt32>(size, out_size, out, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: return put<UInt32>(size, out_size, out, kZeroPeriod);
        case kAudioDevicePropertyClockIsStable: return put<UInt32>(size, out_size, out, 1);
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams: {
            AudioObjectID ids[2];
            UInt32 n = 0;
            if (!output) ids[n++] = kInStream;
            if (!input) ids[n++] = kOutStream;
            n = std::min<UInt32>(n, size / sizeof(AudioObjectID));
            std::memcpy(out, ids, n * sizeof(AudioObjectID));
            *out_size = n * sizeof(AudioObjectID);
            return 0;
        }
        case kAudioObjectPropertyControlList: *out_size = 0; return 0;
        case kAudioDevicePropertyNominalSampleRate: return put<Float64>(size, out_size, out, g.rate.load());
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            UInt32 n = std::min<UInt32>(UInt32(std::size(kRates)), size / sizeof(AudioValueRange));
            auto* r = static_cast<AudioValueRange*>(out);
            for (UInt32 i = 0; i < n; ++i) r[i] = {kRates[i], kRates[i]};
            *out_size = n * sizeof(AudioValueRange);
            return 0;
        }
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (size < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            auto* c = static_cast<UInt32*>(out);
            c[0] = 1;
            c[1] = 2;
            *out_size = 2 * sizeof(UInt32);
            return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            UInt32 need = UInt32(offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription));
            if (size < need) return kAudioHardwareBadPropertySizeError;
            auto* l = static_cast<AudioChannelLayout*>(out);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mChannelBitmap = 0;
            l->mNumberChannelDescriptions = kChannels;
            for (UInt32 i = 0; i < kChannels; ++i) {
                l->mChannelDescriptions[i].mChannelLabel = i == 0 ? kAudioChannelLabel_Left
                                                           : i == 1 ? kAudioChannelLabel_Right
                                                                    : kAudioChannelLabel_Discrete_0 + i;
                l->mChannelDescriptions[i].mChannelFlags = 0;
                l->mChannelDescriptions[i].mCoordinates[0] = 0;
                l->mChannelDescriptions[i].mCoordinates[1] = 0;
                l->mChannelDescriptions[i].mCoordinates[2] = 0;
            }
            *out_size = need;
            return 0;
        }
        case kAudioObjectPropertyCustomPropertyInfoList: {
            AudioServerPlugInCustomPropertyInfo info[3] = {
                {kPropConfig, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList,
                 kAudioServerPlugInCustomPropertyDataTypeNone},
                {kPropStatus, kAudioServerPlugInCustomPropertyDataTypeCFString, kAudioServerPlugInCustomPropertyDataTypeNone},
                {kPropLog, kAudioServerPlugInCustomPropertyDataTypeCFString, kAudioServerPlugInCustomPropertyDataTypeNone},
            };
            UInt32 n = std::min<UInt32>(3, size / sizeof(info[0]));
            std::memcpy(out, info, n * sizeof(info[0]));
            *out_size = n * sizeof(info[0]);
            return 0;
        }
        case kPropConfig: {
            Settings s;
            {
                std::lock_guard<std::mutex> lk(g.mu);
                s = g.settings;
            }
            return put<CFPropertyListRef>(size, out_size, out, settings_to_dict(s));
        }
        case kPropStatus: {
            std::string j = status_json();
            return put<CFStringRef>(size, out_size, out, CFStringCreateWithCString(nullptr, j.c_str(), kCFStringEncodingUTF8));
        }
        case kPropLog: {
            std::string t = log_text();
            CFStringRef s = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(t.data()), CFIndex(t.size()),
                                                    kCFStringEncodingUTF8, false);
            if (!s) s = CFStringCreateWithCString(nullptr, "(log not readable)", kCFStringEncodingUTF8);
            return put<CFStringRef>(size, out_size, out, s);
        }
        }
        break;

    case kInStream:
    case kOutStream: {
        const bool is_in = obj == kInStream;
        switch (sel) {
        case kAudioObjectPropertyBaseClass: return put<AudioClassID>(size, out_size, out, kAudioObjectClassID);
        case kAudioObjectPropertyClass: return put<AudioClassID>(size, out_size, out, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: return put<AudioObjectID>(size, out_size, out, kDevice);
        case kAudioObjectPropertyOwnedObjects: *out_size = 0; return 0;
        case kAudioObjectPropertyName:
            return put<CFStringRef>(size, out_size, out, is_in ? CFSTR("From Dante") : CFSTR("To Dante"));
        case kAudioStreamPropertyIsActive: return put<UInt32>(size, out_size, out, 1);
        case kAudioStreamPropertyDirection: return put<UInt32>(size, out_size, out, is_in ? 1 : 0);
        case kAudioStreamPropertyTerminalType:
            return put<UInt32>(size, out_size, out, is_in ? kAudioStreamTerminalTypeLine : kAudioStreamTerminalTypeLine);
        case kAudioStreamPropertyStartingChannel: return put<UInt32>(size, out_size, out, 1);
        case kAudioStreamPropertyLatency: return put<UInt32>(size, out_size, out, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            return put<AudioStreamBasicDescription>(size, out_size, out, format_for(g.rate.load()));
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            UInt32 n = std::min<UInt32>(UInt32(std::size(kRates)), size / sizeof(AudioStreamRangedDescription));
            auto* d = static_cast<AudioStreamRangedDescription*>(out);
            for (UInt32 i = 0; i < n; ++i) {
                d[i].mFormat = format_for(kRates[i]);
                d[i].mSampleRateRange = {kRates[i], kRates[i]};
            }
            *out_size = n * sizeof(AudioStreamRangedDescription);
            return 0;
        }
        }
        break;
    }
    }
    return kAudioHardwareUnknownPropertyError;
}

void request_rate(Float64 r) {
    if (r != g.rate.load() && g.host)
        g.host->RequestDeviceConfigurationChange(g.host, kDevice, UInt64(r), nullptr);
}

OSStatus SetPropertyData(AudioServerPlugInDriverRef drv, AudioObjectID obj, pid_t, const AudioObjectPropertyAddress* a,
                         UInt32, const void*, UInt32 size, const void* data) {
    if (drv != gDriverRef || !a || !data) return kAudioHardwareIllegalOperationError;
    const auto sel = a->mSelector;
    if (obj == kDevice && sel == kAudioDevicePropertyNominalSampleRate) {
        if (size != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        Float64 r = *static_cast<const Float64*>(data);
        if (std::find(std::begin(kRates), std::end(kRates), r) == std::end(kRates)) return kAudioHardwareIllegalOperationError;
        request_rate(r);
        return 0;
    }
    if (obj == kDevice && sel == kPropConfig) {
        if (size != sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
        auto d = *static_cast<const CFDictionaryRef*>(data);
        Settings before, after;
        {
            std::lock_guard<std::mutex> lk(g.mu);
            before = g.settings;
            settings_from_dict(d, g.settings);
            after = g.settings;
        }
        g.tones.store(after.tones);
        if (!after.same_engine(before)) {
            if (after.rate != int(g.rate.load())) request_rate(Float64(after.rate));  // restarts the engine too
            else request_apply();
        } else {
            EngineUse use;
            if (use.e) use.e->set_test_tones(after.tones);
        }
        save_settings();
        notify(kDevice, {kPropConfig, kPropStatus});
        return 0;
    }
    if (is_stream(obj) && (sel == kAudioStreamPropertyVirtualFormat || sel == kAudioStreamPropertyPhysicalFormat)) {
        if (size != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
        auto f = *static_cast<const AudioStreamBasicDescription*>(data);
        if (f.mFormatID != kAudioFormatLinearPCM || f.mChannelsPerFrame != kChannels)
            return kAudioDeviceUnsupportedFormatError;
        if (std::find(std::begin(kRates), std::end(kRates), f.mSampleRate) == std::end(kRates))
            return kAudioDeviceUnsupportedFormatError;
        request_rate(f.mSampleRate);
        return 0;
    }
    if (is_stream(obj) && sel == kAudioStreamPropertyIsActive) return 0;
    return kAudioHardwareUnknownPropertyError;
}

// ---------------------------------------------------------------- IO

OSStatus StartIO(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt32) {
    if (drv != gDriverRef || dev != kDevice) return kAudioHardwareBadObjectError;
    if (g.io_clients.fetch_add(1) == 0) {
        g.dante_timing = false;
        g.host_anchor = mach_absolute_time();
        g.last_sample = 0;
        g.next_out = 0;
        ++g.seed;
        notify(kDevice, {kAudioDevicePropertyDeviceIsRunning});
    }
    return 0;
}

OSStatus StopIO(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt32) {
    if (drv != gDriverRef || dev != kDevice) return kAudioHardwareBadObjectError;
    UInt32 n = g.io_clients.load();
    if (n > 0 && g.io_clients.fetch_sub(1) == 1) notify(kDevice, {kAudioDevicePropertyDeviceIsRunning});
    return 0;
}

// Media time now, if the Dante device is running and locked.
bool dante_now(DanteDevice* e, uint64_t& t) {
    return e && e->state() == DanteState::Running && e->now(t);
}

OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt32, Float64* sample, UInt64* host,
                          UInt64* seed) {
    if (drv != gDriverRef || dev != kDevice) return kAudioHardwareBadObjectError;
    const Float64 rate = g.rate.load();
    const double ticks_per_frame = g.clock.ticks_per_ns * 1e9 / rate;
    EngineUse use;
    uint64_t t;
    if (dante_now(use.e, t) && use.e->config().sample_rate == int(rate)) {
        if (!g.dante_timing) {
            // Switch to the Dante clock, continuing the current sample time.
            uint64_t now_host = mach_absolute_time();
            uint64_t s_now = uint64_t(double(now_host - g.host_anchor) / ticks_per_frame);
            g.m0 = t - s_now;
            g.dante_timing = true;
            ++g.seed;
        }
        uint64_t s = t - g.m0;
        uint64_t cycle = s / kZeroPeriod * kZeroPeriod;
        int64_t mono;
        use.e->mono_from_media(g.m0 + cycle, mono);
        *sample = Float64(cycle);
        *host = g.clock.host_from_mono(mono);
    } else {
        UInt64 now_host = mach_absolute_time();
        if (g.dante_timing) {
            // Lost the Dante clock: run on the Mac's clock from here on.
            g.host_anchor = now_host - UInt64(g.last_sample * ticks_per_frame);
            g.dante_timing = false;
            ++g.seed;
        }
        uint64_t s = uint64_t(double(now_host - g.host_anchor) / ticks_per_frame);
        uint64_t cycle = s / kZeroPeriod * kZeroPeriod;
        *sample = Float64(cycle);
        *host = g.host_anchor + UInt64(double(cycle) * ticks_per_frame);
    }
    g.last_sample = *sample;
    *seed = g.seed;
    return 0;
}

OSStatus WillDoIOOperation(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt32, UInt32 op, Boolean* will,
                           Boolean* in_place) {
    if (drv != gDriverRef || dev != kDevice) return kAudioHardwareBadObjectError;
    bool w = op == kAudioServerPlugInIOOperationReadInput || op == kAudioServerPlugInIOOperationWriteMix;
    if (will) *will = w;
    if (in_place) *in_place = true;
    return 0;
}

OSStatus DoIOOperation(AudioServerPlugInDriverRef drv, AudioObjectID dev, AudioObjectID, UInt32, UInt32 op, UInt32 frames,
                       const AudioServerPlugInIOCycleInfo* cycle, void* main, void*) {
    if (drv != gDriverRef || dev != kDevice) return kAudioHardwareBadObjectError;
    auto* buf = static_cast<float*>(main);
    EngineUse use;
    const bool dante = g.dante_timing && use.e && use.e->state() == DanteState::Running;
    if (op == kAudioServerPlugInIOOperationReadInput) {
        uint64_t s = uint64_t(cycle->mInputTime.mSampleTime);
        if (dante) use.e->read_rx(g.m0 + s, int(frames), buf, int(kChannels));
        else std::memset(buf, 0, size_t(frames) * kChannels * sizeof(float));
    } else if (op == kAudioServerPlugInIOOperationWriteMix) {
        uint64_t s = uint64_t(cycle->mOutputTime.mSampleTime);
        if (dante && !g.tones.load(std::memory_order_relaxed)) {
            use.e->write_tx(g.m0 + s, int(frames), buf, int(kChannels));
            uint64_t t;
            if (use.e->now(t)) {
                int64_t margin = int64_t(g.m0 + s) - int64_t(t);
                if (margin < 0) g.late_writes.fetch_add(1, std::memory_order_relaxed);
                if (margin < g.min_margin.load(std::memory_order_relaxed)) g.min_margin.store(margin, std::memory_order_relaxed);
            }
            if (g.next_out != 0 && s != g.next_out) g.out_gaps.fetch_add(1, std::memory_order_relaxed);
            g.next_out = s + frames;
            g.out_cycles.fetch_add(1, std::memory_order_relaxed);
            g.m0_shared.store(g.m0, std::memory_order_relaxed);
        }
    }
    return 0;
}

}  // namespace

// Factory named in Info.plist (CFPlugInFactories).
extern "C" __attribute__((visibility("default"))) void* JoseVirtualSoundcardDriverFactory(CFAllocatorRef, CFUUIDRef type) {
    if (!CFEqual(type, kAudioServerPlugInTypeUUID)) return nullptr;
    return gDriverRef;
}
