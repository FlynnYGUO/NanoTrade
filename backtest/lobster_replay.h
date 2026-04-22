#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>

#include "common/types.h"
#include "common/logging.h"
#include "exchange/market_data/market_update.h"

namespace Backtest {
  /// Replays LOBSTER message.csv events into an MDPMarketUpdate queue.
  ///
  /// LOBSTER schema (https://lobsterdata.com, academic NASDAQ sample):
  ///   time, type, order_id, size, price, direction
  ///   time:      seconds after midnight (decimal, sub-ms precision) — informational only
  ///   type:      1=new, 2=partial cancel, 3=full cancel, 4=exec (visible), 5=exec (hidden), 7=halt
  ///   price:     dollar price times 10000 (e.g., $585.33 -> 5853300)
  ///   direction: 1=BUY, -1=SELL (matches Common::Side::BUY/SELL integer values)
  ///
  /// MEMarketUpdate mapping:
  ///   type 1 -> ADD, type 2 -> MODIFY, type 3 -> CANCEL, type 4/5 -> TRADE, type 7 -> skip.
  ///
  /// Output: MDPMarketUpdates with monotonically-increasing seq_num, pushed to the queue
  /// passed in. All rows are mapped to a single ticker_id supplied at construction.
  class LobsterReplay {
  public:
    /// `sim_output_queue` is optional: if non-null, every MDPMarketUpdate emitted to
    /// `output_queue` is also written to `sim_output_queue` (same producer thread, so both
    /// SPSC writes are safe). Used by FillSimulator's QUEUE_AWARE mode to see the same
    /// market event stream MarketOrderBook consumes.
    LobsterReplay(const std::string &csv_path,
                  Exchange::MDPMarketUpdateLFQueue *output_queue,
                  Common::TickerId ticker_id,
                  Exchange::MDPMarketUpdateLFQueue *sim_output_queue = nullptr);

    ~LobsterReplay();

    auto start() -> void;
    auto stop() -> void;

    auto finished() const noexcept -> bool { return finished_.load(std::memory_order_acquire); }
    auto rowsEmitted() const noexcept -> size_t { return rows_emitted_; }
    auto rowsSkipped() const noexcept -> size_t { return rows_skipped_; }

    LobsterReplay() = delete;
    LobsterReplay(const LobsterReplay &) = delete;
    LobsterReplay &operator=(const LobsterReplay &) = delete;

  private:
    auto run() noexcept -> void;

    const std::string csv_path_;
    Exchange::MDPMarketUpdateLFQueue *output_queue_;
    const Common::TickerId ticker_id_;
    Exchange::MDPMarketUpdateLFQueue *sim_output_queue_ = nullptr;

    std::thread *thread_ = nullptr;
    volatile bool run_ = false;
    std::atomic<bool> finished_{false};

    size_t rows_emitted_ = 0;
    size_t rows_skipped_ = 0;

    /// LOBSTER order IDs are raw NASDAQ IDs (often 8-digit, exceeding our
    /// ME_MAX_ORDER_IDS array bound). We remap each unique LOBSTER ID to a dense slot
    /// in [0, ME_MAX_ORDER_IDS) on first sight and reuse the same slot on later events.
    std::unordered_map<uint64_t, Common::OrderId> lobster_id_to_slot_;
    Common::OrderId next_slot_ = 0;

    Common::Logger logger_;
    std::string time_str_;
  };
}
