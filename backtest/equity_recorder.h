#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "common/types.h"
#include "common/time_utils.h"
#include "common/logging.h"
#include "trading/strategy/position_keeper.h"

namespace Backtest {
  /// Samples total PnL, position, and volume from a PositionKeeper on a fixed cadence and
  /// dumps the series to CSV. Time-series analysis (Sharpe, Sortino, drawdown curves) is
  /// better done in Python — the notebook reads the CSV and computes metrics. We print a
  /// few simple stats on finalize() as a sanity check.
  class EquityRecorder {
  public:
    EquityRecorder(const Trading::PositionKeeper *position_keeper,
                   Common::TickerId ticker_id,
                   const std::string &output_csv,
                   uint32_t sample_interval_ms = 100);

    ~EquityRecorder();

    auto start() -> void;
    auto stop() -> void;

    /// Write samples to CSV and print min stats to stdout.
    auto finalize() -> void;

    auto sampleCount() const noexcept -> size_t { return samples_.size(); }

    EquityRecorder() = delete;
    EquityRecorder(const EquityRecorder &) = delete;
    EquityRecorder &operator=(const EquityRecorder &) = delete;

  private:
    auto run() noexcept -> void;

    struct Sample {
      Common::Nanos wall_ns;
      double total_pnl;
      double realized_pnl;
      double unrealized_pnl;
      int32_t position;
      uint32_t volume;
    };

    const Trading::PositionKeeper *position_keeper_;
    const Common::TickerId ticker_id_;
    const std::string output_csv_;
    const uint32_t sample_interval_ms_;

    std::vector<Sample> samples_;
    std::thread *thread_ = nullptr;
    volatile bool run_ = false;

    Common::Logger logger_;
    std::string time_str_;
  };
}
