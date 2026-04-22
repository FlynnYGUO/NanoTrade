#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "common/types.h"
#include "common/logging.h"
#include "exchange/market_data/market_update.h"

namespace Backtest {
  /// Replays Binance Spot aggTrades into an MDPMarketUpdate queue, reconstructing a
  /// synthetic top-of-book as we go.
  ///
  /// Binance aggTrades schema (data.binance.vision):
  ///   agg_trade_id, price, qty, first_trade_id, last_trade_id, transact_time, is_buyer_maker [, is_best_match]
  ///
  ///   price / qty:   floating-point strings, scaled internally (see PRICE_SCALE / QTY_SCALE)
  ///   transact_time: epoch microseconds (newer files) or milliseconds (older files) — we don't use it
  ///   is_buyer_maker: True/False string. True means the aggressive side was a seller (the
  ///                   buyer was passively resting on the bid and got hit), so the trade
  ///                   price equals the best_bid; False means an aggressive buyer lifted
  ///                   the ask, so trade_price = best_ask.
  ///
  /// We maintain a synthetic L1 book: one order per side. When a trade reveals a new
  /// best_bid or best_ask, we:
  ///   1. emit CANCEL for the previous synthetic order on that side (if any)
  ///   2. emit ADD for a fresh synthetic order at the new best price
  ///   3. emit TRADE with the actual trade price / qty
  ///
  /// The synthetic qty on each side is a constant (SYNTHETIC_SIDE_QTY); we never know the
  /// real resting qty, so we assume "plenty" — FillSimulator's qty-truncation will still
  /// bite because trade qtys are typically tiny compared to this default.
  class BinanceAggTradesReplay {
  public:
    /// Price scale: Binance raw price * PRICE_SCALE -> internal Price (integer). With
    /// PRICE_SCALE=100 ($0.01 resolution), BTCUSDT at $114048.94 becomes 11,404,894.
    static constexpr double PRICE_SCALE = 100.0;

    /// Qty scale: Binance raw qty * QTY_SCALE -> internal Qty (uint32). With QTY_SCALE=10^6
    /// (micro-BTC), a 0.00143 BTC trade becomes 1430. Max representable ~4.3K BTC per
    /// single trade, which is well above any realistic BTCUSDT trade.
    static constexpr double QTY_SCALE = 1'000'000.0;

    /// Default resting qty for the synthetic book (1 BTC in micro-BTC units). Large enough
    /// that FillSimulator never hits the opposite-side qty cap on realistic strategy clips.
    static constexpr Common::Qty SYNTHETIC_SIDE_QTY = 1'000'000;

    /// `sim_output_queue` is optional — see LobsterReplay. Symmetric interface; the Binance
    /// path runs FillSimulator in AGGRESSIVE_ONLY mode so this queue is typically unused.
    BinanceAggTradesReplay(const std::string &csv_path,
                           Exchange::MDPMarketUpdateLFQueue *output_queue,
                           Common::TickerId ticker_id,
                           Exchange::MDPMarketUpdateLFQueue *sim_output_queue = nullptr);

    ~BinanceAggTradesReplay();

    auto start() -> void;
    auto stop() -> void;

    auto finished() const noexcept -> bool { return finished_.load(std::memory_order_acquire); }
    auto rowsEmitted() const noexcept -> size_t { return rows_emitted_; }
    auto rowsSkipped() const noexcept -> size_t { return rows_skipped_; }
    auto tradesProcessed() const noexcept -> size_t { return trades_processed_; }

    BinanceAggTradesReplay() = delete;
    BinanceAggTradesReplay(const BinanceAggTradesReplay &) = delete;
    BinanceAggTradesReplay &operator=(const BinanceAggTradesReplay &) = delete;

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
    size_t trades_processed_ = 0;

    Common::Logger logger_;
    std::string time_str_;
  };
}
