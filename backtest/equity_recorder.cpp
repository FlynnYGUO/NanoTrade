#include "equity_recorder.h"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>

#include "common/thread_utils.h"
#include "common/time_utils.h"

namespace Backtest {
  EquityRecorder::EquityRecorder(const Trading::PositionKeeper *position_keeper,
                                 Common::TickerId ticker_id,
                                 const std::string &output_csv,
                                 uint32_t sample_interval_ms)
      : position_keeper_(position_keeper),
        ticker_id_(ticker_id),
        output_csv_(output_csv),
        sample_interval_ms_(sample_interval_ms),
        logger_("backtest_equity_recorder.log") {
    // Reserve a day's worth of samples at 100ms cadence (864k) to avoid vector reallocations
    // during the hot loop.
    samples_.reserve(1'000'000);
  }

  EquityRecorder::~EquityRecorder() {
    stop();
    if (thread_) {
      if (thread_->joinable()) thread_->join();
      delete thread_;
      thread_ = nullptr;
    }
  }

  auto EquityRecorder::start() -> void {
    run_ = true;
    thread_ = new std::thread([this]() { run(); });
    ASSERT(thread_ != nullptr, "Failed to start EquityRecorder thread.");
  }

  auto EquityRecorder::stop() -> void {
    run_ = false;
  }

  auto EquityRecorder::run() noexcept -> void {
    logger_.log("%:% %() % Sampling ticker=% interval=%ms\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), ticker_id_, sample_interval_ms_);

    while (run_) {
      const auto *pi = position_keeper_->getPositionInfo(ticker_id_);
      samples_.push_back(Sample{
          Common::getCurrentNanos(),
          pi->total_pnl_,
          pi->real_pnl_,
          pi->unreal_pnl_,
          pi->position_,
          pi->volume_
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(sample_interval_ms_));
    }
  }

  auto EquityRecorder::finalize() -> void {
    // One last sample at shutdown — captures any final PnL move.
    if (position_keeper_) {
      const auto *pi = position_keeper_->getPositionInfo(ticker_id_);
      samples_.push_back(Sample{
          Common::getCurrentNanos(),
          pi->total_pnl_,
          pi->real_pnl_,
          pi->unreal_pnl_,
          pi->position_,
          pi->volume_
      });
    }

    // Write CSV.
    std::filesystem::create_directories(std::filesystem::path(output_csv_).parent_path());
    std::ofstream ofs(output_csv_);
    ofs << "wall_ns,total_pnl,real_pnl,unreal_pnl,position,volume\n";
    for (const auto &s : samples_) {
      ofs << s.wall_ns << ',' << s.total_pnl << ',' << s.realized_pnl << ','
          << s.unrealized_pnl << ',' << s.position << ',' << s.volume << '\n';
    }
    ofs.close();

    // Stats summary.
    double final_pnl = samples_.empty() ? 0.0 : samples_.back().total_pnl;
    double peak_pnl = 0.0;
    double max_drawdown = 0.0;
    uint32_t final_volume = samples_.empty() ? 0U : samples_.back().volume;
    for (const auto &s : samples_) {
      peak_pnl = std::max(peak_pnl, s.total_pnl);
      max_drawdown = std::max(max_drawdown, peak_pnl - s.total_pnl);
    }

    std::cout << "\n=== EquityRecorder summary (ticker " << ticker_id_ << ") ===\n"
              << "Samples:       " << samples_.size() << "\n"
              << "Final PnL:     " << std::fixed << std::setprecision(2) << final_pnl << "\n"
              << "Peak PnL:      " << peak_pnl << "\n"
              << "Max drawdown:  " << max_drawdown << "\n"
              << "Final volume:  " << final_volume << "\n"
              << "Equity CSV:    " << output_csv_ << "\n";
  }
}
