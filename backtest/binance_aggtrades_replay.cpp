#include "binance_aggtrades_replay.h"

#include <charconv>
#include <fstream>
#include <string_view>
#include <thread>

#include "common/thread_utils.h"

namespace Backtest {
  using Exchange::MarketUpdateType;
  using Exchange::MEMarketUpdate;
  using Exchange::MDPMarketUpdate;

  BinanceAggTradesReplay::BinanceAggTradesReplay(const std::string &csv_path,
                                                 Exchange::MDPMarketUpdateLFQueue *output_queue,
                                                 Common::TickerId ticker_id)
      : csv_path_(csv_path), output_queue_(output_queue), ticker_id_(ticker_id),
        logger_("backtest_binance_aggtrades_replay.log") {
  }

  BinanceAggTradesReplay::~BinanceAggTradesReplay() {
    stop();
    if (thread_) {
      if (thread_->joinable()) thread_->join();
      delete thread_;
      thread_ = nullptr;
    }
  }

  auto BinanceAggTradesReplay::start() -> void {
    run_ = true;
    finished_.store(false, std::memory_order_release);
    thread_ = new std::thread([this]() { run(); });
    ASSERT(thread_ != nullptr, "Failed to start BinanceAggTradesReplay thread.");
  }

  auto BinanceAggTradesReplay::stop() -> void {
    run_ = false;
  }

  namespace {
    inline auto nextField(std::string_view s, size_t &pos) noexcept -> std::string_view {
      const auto start = pos;
      while (pos < s.size() && s[pos] != ',') ++pos;
      auto field = s.substr(start, pos - start);
      if (pos < s.size()) ++pos;
      return field;
    }

    inline auto parseDouble(std::string_view sv) noexcept -> double {
      double v = 0;
      std::from_chars(sv.data(), sv.data() + sv.size(), v);
      return v;
    }

    inline auto parseBoolField(std::string_view sv) noexcept -> bool {
      // Binance writes "True" / "False" (capitalized Python repr).
      return !sv.empty() && (sv[0] == 'T' || sv[0] == 't' || sv[0] == '1');
    }
  }

  auto BinanceAggTradesReplay::run() noexcept -> void {
    std::ifstream in(csv_path_);
    if (!in.is_open()) {
      logger_.log("%:% %() % ERROR could not open %\n", __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_), csv_path_);
      finished_.store(true, std::memory_order_release);
      return;
    }

    logger_.log("%:% %() % Replaying %\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), csv_path_);

    // Synthetic L1 book state (pre-scaled integer prices/qtys).
    Common::Price best_bid = Common::Price_INVALID, best_ask = Common::Price_INVALID;
    Common::OrderId bid_oid = 0, ask_oid = 0;

    // Use high-bit OIDs to avoid colliding with any other internal IDs; bounded by
    // ME_MAX_ORDER_IDS so we still fit in the pool.
    Common::OrderId next_oid = 1;
    const auto max_oid = Common::ME_MAX_ORDER_IDS - 1;

    size_t seq_num = 1;
    std::string line;
    line.reserve(256);

    auto push = [&](MEMarketUpdate me) {
      // Backpressure: the SPSC output queue has no overflow check.
      while (run_ && output_queue_->size() >= Common::ME_MAX_MARKET_UPDATES * 3 / 4) {
        std::this_thread::yield();
      }
      MDPMarketUpdate mdp{};
      mdp.seq_num_ = seq_num++;
      mdp.me_market_update_ = me;
      auto *out = output_queue_->getNextToWriteTo();
      *out = mdp;
      output_queue_->updateWriteIndex();
      ++rows_emitted_;
    };

    auto fresh_oid = [&]() {
      auto oid = next_oid++;
      if (UNLIKELY(next_oid > max_oid)) next_oid = 1;  // best-effort wrap; for multi-month
                                                        // runs over 1M bookside updates this
                                                        // can collide, but in practice order
                                                        // churn keeps the pool mostly empty.
      return oid;
    };

    while (run_ && std::getline(in, line)) {
      if (line.empty()) continue;
      // aggTrades CSV does not have a header row — real data starts at line 1.
      // But the first line of an older file could be a header; detect and skip.
      if (rows_emitted_ == 0 && trades_processed_ == 0 && !std::isdigit(line[0])) {
        ++rows_skipped_;
        continue;
      }

      size_t pos = 0;
      std::string_view sv(line);

      nextField(sv, pos);  // agg_trade_id (skip)
      const auto price_field = nextField(sv, pos);
      const auto qty_field = nextField(sv, pos);
      nextField(sv, pos);  // first_trade_id (skip)
      nextField(sv, pos);  // last_trade_id (skip)
      nextField(sv, pos);  // transact_time (skip)
      const auto is_maker_field = nextField(sv, pos);
      // is_best_match (optional 8th field) is ignored

      const double raw_price = parseDouble(price_field);
      const double raw_qty = parseDouble(qty_field);
      const bool is_buyer_maker = parseBoolField(is_maker_field);

      const auto scaled_price = static_cast<Common::Price>(raw_price * PRICE_SCALE);
      const auto scaled_qty = static_cast<Common::Qty>(raw_qty * QTY_SCALE);
      if (scaled_qty == 0) { ++rows_skipped_; continue; }

      ++trades_processed_;

      // Update the synthetic side whose best changes based on aggressor.
      if (is_buyer_maker) {
        // Aggressive seller hit the bid — trade price is approximately best_bid.
        if (scaled_price != best_bid) {
          if (best_bid != Common::Price_INVALID) {
            MEMarketUpdate cxl{};
            cxl.type_ = MarketUpdateType::CANCEL;
            cxl.order_id_ = bid_oid;
            cxl.ticker_id_ = ticker_id_;
            cxl.side_ = Common::Side::BUY;
            cxl.price_ = best_bid;
            cxl.qty_ = SYNTHETIC_SIDE_QTY;
            cxl.priority_ = bid_oid;
            push(cxl);
          }
          bid_oid = fresh_oid();
          best_bid = scaled_price;
          MEMarketUpdate add{};
          add.type_ = MarketUpdateType::ADD;
          add.order_id_ = bid_oid;
          add.ticker_id_ = ticker_id_;
          add.side_ = Common::Side::BUY;
          add.price_ = best_bid;
          add.qty_ = SYNTHETIC_SIDE_QTY;
          add.priority_ = bid_oid;
          push(add);
        }
      } else {
        // Aggressive buyer lifted the ask — trade price ≈ best_ask.
        if (scaled_price != best_ask) {
          if (best_ask != Common::Price_INVALID) {
            MEMarketUpdate cxl{};
            cxl.type_ = MarketUpdateType::CANCEL;
            cxl.order_id_ = ask_oid;
            cxl.ticker_id_ = ticker_id_;
            cxl.side_ = Common::Side::SELL;
            cxl.price_ = best_ask;
            cxl.qty_ = SYNTHETIC_SIDE_QTY;
            cxl.priority_ = ask_oid;
            push(cxl);
          }
          ask_oid = fresh_oid();
          best_ask = scaled_price;
          MEMarketUpdate add{};
          add.type_ = MarketUpdateType::ADD;
          add.order_id_ = ask_oid;
          add.ticker_id_ = ticker_id_;
          add.side_ = Common::Side::SELL;
          add.price_ = best_ask;
          add.qty_ = SYNTHETIC_SIDE_QTY;
          add.priority_ = ask_oid;
          push(add);
        }
      }

      // Emit the TRADE event itself — FeatureEngine uses this for agg_trade_qty_ratio.
      MEMarketUpdate trade{};
      trade.type_ = MarketUpdateType::TRADE;
      trade.order_id_ = 0;
      trade.ticker_id_ = ticker_id_;
      trade.side_ = is_buyer_maker ? Common::Side::SELL : Common::Side::BUY;  // aggressor side
      trade.price_ = scaled_price;
      trade.qty_ = scaled_qty;
      trade.priority_ = 0;
      push(trade);
    }

    in.close();
    logger_.log("%:% %() % Finished. trades:% emitted:% skipped:%\n",
                __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                trades_processed_, rows_emitted_, rows_skipped_);
    finished_.store(true, std::memory_order_release);
  }
}
