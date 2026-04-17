#pragma once

#include <cstdint>
#include <thread>
#include <chrono>

namespace Common {
  /// Read from the TSC register and return a uint64_t value to represent elapsed CPU clock cycles.
  inline auto rdtsc() noexcept {
    unsigned int lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t) hi << 32) | lo;
  }

  /// Calibrate RDTSC: returns nanoseconds per cycle.
  /// Call once at startup; busy-waits for ~10ms to measure.
  inline auto calibrateRdtsc() noexcept -> double {
    const auto t0 = rdtsc();
    const auto n0 = std::chrono::steady_clock::now();

    // Busy-wait ~10ms
    while (true) {
      const auto n1 = std::chrono::steady_clock::now();
      const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(n1 - n0).count();
      if (elapsed_ns >= 10'000'000) {
        const auto t1 = rdtsc();
        return static_cast<double>(elapsed_ns) / static_cast<double>(t1 - t0);
      }
    }
  }
}

/// Start latency measurement using rdtsc(). Creates a variable called TAG in the local scope.
#define START_MEASURE(TAG) const auto TAG = Common::rdtsc()

/// End latency measurement using rdtsc(). Expects a variable called TAG to already exist in the local scope.
/// Under NT_BENCHMARK_NO_LOG, compiles to a no-op that still references TAG so the
/// START_MEASURE-declared variable doesn't trigger -Wunused-variable under -Werror.
#ifdef NT_BENCHMARK_NO_LOG
  #define END_MEASURE(TAG, LOGGER) ((void)(TAG))
  #define TTT_MEASURE(TAG, LOGGER) ((void)0)
#else
  #define END_MEASURE(TAG, LOGGER)                                                              \
        do {                                                                                    \
          const auto end = Common::rdtsc();                                                     \
          LOGGER.log("% RDTSC "#TAG" %\n", Common::getCurrentTimeStr(&time_str_), (end - TAG)); \
        } while(false)

  #define TTT_MEASURE(TAG, LOGGER)                                                              \
        do {                                                                                    \
          const auto TAG = Common::getCurrentNanos();                                           \
          LOGGER.log("% TTT "#TAG" %\n", Common::getCurrentTimeStr(&time_str_), TAG);           \
        } while(false)
#endif

/// Record RDTSC delta into a LatencyStats instance (silent, no logging).
#define MEASURE_AND_RECORD(TAG, STATS)                                                        \
      do {                                                                                    \
        const auto end = Common::rdtsc();                                                     \
        (STATS).record(end - TAG);                                                            \
      } while(false)
