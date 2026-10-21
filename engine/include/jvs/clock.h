// Local clocks used by the engine.
#pragma once
#include <cstdint>

namespace jvs {

// Monotonic nanoseconds. Never jumps; this is the base for everything.
int64_t mono_ns();

// Wall-clock nanoseconds since the Unix epoch.
int64_t realtime_ns();

// Convert a wall-clock timestamp taken by the kernel (e.g. SO_TIMESTAMP)
// into the monotonic timebase.
int64_t realtime_to_mono(int64_t realtime);

// Sleep until the given monotonic time.
void sleep_until_mono(int64_t t_ns);

// Gives the calling thread real-time scheduling on macOS (about
// computation_us of work every period_us). Does nothing elsewhere.
void set_thread_realtime(int period_us, int computation_us);

}  // namespace jvs
