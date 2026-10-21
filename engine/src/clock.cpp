#include "jvs/clock.h"

#include <ctime>
#include <cerrno>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif

namespace jvs {

int64_t mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

int64_t realtime_ns() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

int64_t realtime_to_mono(int64_t realtime) {
    // Sample both clocks back to back and take the tightest pair.
    int64_t best_gap = INT64_MAX, best_offset = 0;
    for (int i = 0; i < 3; ++i) {
        int64_t m0 = mono_ns();
        int64_t r = realtime_ns();
        int64_t m1 = mono_ns();
        if (m1 - m0 < best_gap) {
            best_gap = m1 - m0;
            best_offset = r - (m0 + (m1 - m0) / 2);
        }
    }
    return realtime - best_offset;
}

void sleep_until_mono(int64_t t_ns) {
    for (;;) {
        int64_t now = mono_ns();
        int64_t d = t_ns - now;
        if (d <= 0) return;
        timespec ts{time_t(d / 1000000000LL), long(d % 1000000000LL)};
        if (nanosleep(&ts, nullptr) == 0) return;
        if (errno != EINTR) return;
    }
}

void set_thread_realtime(int period_us, int computation_us) {
#if defined(__APPLE__)
    // Mach time-constraint policy, as Core Audio uses for its own threads.
    // Keeps the thread on time even when macOS throttles the process (for
    // example the audio driver's process while no app plays audio).
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    auto ticks = [&](int us) { return uint32_t(double(us) * 1000.0 * tb.denom / tb.numer); };
    thread_time_constraint_policy_data_t p;
    p.period = ticks(period_us);
    p.computation = ticks(computation_us);
    p.constraint = ticks(period_us);
    p.preemptible = 1;
    thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&p),
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#else
    (void)period_us;
    (void)computation_us;
#endif
}

}  // namespace jvs
