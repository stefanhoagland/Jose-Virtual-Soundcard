#include "jvs/clock.h"

#include <ctime>
#include <cerrno>

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

}  // namespace jvs
