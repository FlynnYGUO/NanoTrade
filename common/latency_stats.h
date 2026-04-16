#pragma once

#include <array>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <cmath>

namespace Common {
  /// Latency percentile results in nanoseconds.
  struct LatencyPercentiles {
    double p50 = 0;
    double p90 = 0;
    double p99 = 0;
    double p999 = 0;
    double max = 0;
    double mean = 0;
    double min = 0;
    size_t count = 0;
  };

  /// Collects RDTSC cycle samples and computes percentile statistics.
  /// Fixed-capacity ring buffer — no heap allocation on the hot path.
  template<size_t MAX_SAMPLES = 1'000'000>
  class LatencyStats {
  public:
    LatencyStats() = default;

    /// Record a single RDTSC cycle delta. O(1), no branching on the hot path beyond wrap-around.
    auto record(uint64_t cycles) noexcept -> void {
      samples_[next_index_] = cycles;
      next_index_ = (next_index_ + 1) % MAX_SAMPLES;
      if (count_ < MAX_SAMPLES)
        ++count_;
    }

    /// Compute percentiles. Copies and sorts the samples — call off the hot path.
    auto computePercentiles(double nanos_per_cycle) const -> LatencyPercentiles {
      if (count_ == 0)
        return {};

      // Copy active samples into a working buffer.
      std::vector<double> buf(count_);
      for (size_t i = 0; i < count_; ++i)
        buf[i] = static_cast<double>(samples_[i]) * nanos_per_cycle;

      std::sort(buf.begin(), buf.end());

      LatencyPercentiles result;
      result.count = count_;
      result.min = buf[0];
      result.max = buf[count_ - 1];
      result.p50 = buf[static_cast<size_t>(count_ * 0.50)];
      result.p90 = buf[static_cast<size_t>(count_ * 0.90)];
      result.p99 = buf[static_cast<size_t>(count_ * 0.99)];
      result.p999 = buf[std::min(static_cast<size_t>(count_ * 0.999), count_ - 1)];

      double sum = 0;
      for (size_t i = 0; i < count_; ++i)
        sum += buf[i];
      result.mean = sum / count_;

      return result;
    }

    /// Dump raw cycle samples to CSV for notebook analysis.
    auto dumpToCSV(const std::string &path, double nanos_per_cycle) const -> void {
      std::ofstream ofs(path);
      ofs << "latency_ns\n";
      for (size_t i = 0; i < count_; ++i)
        ofs << static_cast<double>(samples_[i]) * nanos_per_cycle << "\n";
    }

    auto count() const noexcept { return count_; }

    auto reset() noexcept -> void {
      count_ = 0;
      next_index_ = 0;
    }

  private:
    std::array<uint64_t, MAX_SAMPLES> samples_{};
    size_t next_index_ = 0;
    size_t count_ = 0;
  };
}
