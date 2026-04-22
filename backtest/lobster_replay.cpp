#include "lobster_replay.h"

#include <charconv>
#include <fstream>
#include <string_view>

#include "common/thread_utils.h"

namespace Backtest {
  using Exchange::MarketUpdateType;
  using Exchange::MEMarketUpdate;
  using Exchange::MDPMarketUpdate;

  LobsterReplay::LobsterReplay(const std::string &csv_path,
                               Exchange::MDPMarketUpdateLFQueue *output_queue,
                               Common::TickerId ticker_id,
                               Exchange::MDPMarketUpdateLFQueue *sim_output_queue)
      : csv_path_(csv_path), output_queue_(output_queue), ticker_id_(ticker_id),
        sim_output_queue_(sim_output_queue),
        logger_("backtest_lobster_replay.log") {
  }

  LobsterReplay::~LobsterReplay() {
    stop();
    if (thread_) {
      if (thread_->joinable()) thread_->join();
      delete thread_;
      thread_ = nullptr;
    }
  }

  auto LobsterReplay::start() -> void {
    run_ = true;
    finished_.store(false, std::memory_order_release);
    thread_ = new std::thread([this]() { run(); });
    ASSERT(thread_ != nullptr, "Failed to start LobsterReplay thread.");
  }

  auto LobsterReplay::stop() -> void {
    run_ = false;
  }

  namespace {
    /// Parse next comma-delimited field from `s` starting at `pos`, advance `pos` past the comma.
    /// Returns the field as string_view (no allocation). Last field has no trailing comma.
    inline auto nextField(std::string_view s, size_t &pos) noexcept -> std::string_view {
      const auto start = pos;
      while (pos < s.size() && s[pos] != ',') ++pos;
      auto field = s.substr(start, pos - start);
      if (pos < s.size()) ++pos;  // skip the comma
      return field;
    }

    template<typename T>
    inline auto parseInt(std::string_view sv) noexcept -> T {
      T value = 0;
      std::from_chars(sv.data(), sv.data() + sv.size(), value);
      return value;
    }

    /// Parse a signed integer (LOBSTER direction is -1/+1).
    inline auto parseSigned(std::string_view sv) noexcept -> int {
      int value = 0;
      std::from_chars(sv.data(), sv.data() + sv.size(), value);
      return value;
    }

    /// Map LOBSTER message type -> MarketUpdateType (INVALID means "skip this row").
    inline auto mapType(int lobster_type) noexcept -> MarketUpdateType {
      switch (lobster_type) {
        case 1: return MarketUpdateType::ADD;
        case 2: return MarketUpdateType::MODIFY;
        case 3: return MarketUpdateType::CANCEL;
        case 4: return MarketUpdateType::TRADE;  // execution of visible order
        case 5: return MarketUpdateType::TRADE;  // execution of hidden order
        default: return MarketUpdateType::INVALID;  // type 7 (halt) or unknown -> skip
      }
    }
  }

  auto LobsterReplay::run() noexcept -> void {
    std::ifstream in(csv_path_);
    if (!in.is_open()) {
      logger_.log("%:% %() % ERROR could not open %\n", __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_), csv_path_);
      finished_.store(true, std::memory_order_release);
      return;
    }

    logger_.log("%:% %() % Replaying %\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), csv_path_);

    size_t seq_num = 1;  // MDC expects incremental seq starting at 1
    std::string line;
    line.reserve(256);

    while (run_ && std::getline(in, line)) {
      if (line.empty()) continue;

      // LOBSTER row: time, type, order_id, size, price, direction
      size_t pos = 0;
      std::string_view sv(line);

      nextField(sv, pos);  // time — skip (informational only for MVP)
      const auto type_field = nextField(sv, pos);
      const auto oid_field = nextField(sv, pos);
      const auto size_field = nextField(sv, pos);
      const auto price_field = nextField(sv, pos);
      const auto dir_field = nextField(sv, pos);

      const auto lobster_type = parseInt<int>(type_field);
      const auto mu_type = mapType(lobster_type);
      if (mu_type == MarketUpdateType::INVALID) {
        ++rows_skipped_;
        continue;
      }

      const auto lobster_oid = parseInt<uint64_t>(oid_field);
      Common::OrderId slot;
      auto it = lobster_id_to_slot_.find(lobster_oid);
      if (it != lobster_id_to_slot_.end()) {
        slot = it->second;
      } else {
        // First time we've seen this order ID. LOBSTER's message file only records events
        // inside the capture window (09:30-16:00 for the AAPL sample), so MODIFY/CANCEL
        // that reference an order whose ADD happened earlier in the day — or hidden-order
        // executions (type 5) that never had an ADD — will land here. We can only
        // meaningfully emit events for orders whose lifecycle we have observed from birth,
        // so skip anything that isn't a fresh ADD (type 1). TRADE events (mapped from
        // type 4/5) are harmless to drop since MarketOrderBook treats TRADE as a
        // stateless notification.
        if (mu_type != MarketUpdateType::ADD) {
          ++rows_skipped_;
          continue;
        }
        if (UNLIKELY(next_slot_ >= Common::ME_MAX_ORDER_IDS)) {
          logger_.log("%:% %() % ERROR ran out of order-id slots after % unique IDs\n",
                      __FILE__, __LINE__, __FUNCTION__,
                      Common::getCurrentTimeStr(&time_str_), next_slot_);
          break;
        }
        slot = next_slot_++;
        lobster_id_to_slot_.emplace(lobster_oid, slot);
      }

      // After a CANCEL, the order is gone from MarketOrderBook's oid_to_order_ (set to
      // nullptr). If LOBSTER emits another event for the same order id (we've observed
      // this in the AAPL sample — presumably due to data artifacts or duplicate cancel
      // records), we must *not* re-emit it, since MODIFY/CANCEL would then dereference
      // the nullptr slot. Forget the mapping so any subsequent event is treated as an
      // unknown pre-existing order and skipped.
      const bool emit_then_forget = (mu_type == MarketUpdateType::CANCEL);

      MEMarketUpdate me{};
      me.type_ = mu_type;
      me.order_id_ = slot;
      me.ticker_id_ = ticker_id_;
      const auto direction = parseSigned(dir_field);
      me.side_ = (direction == 1) ? Common::Side::BUY : Common::Side::SELL;
      me.price_ = parseInt<Common::Price>(price_field);
      me.qty_ = parseInt<Common::Qty>(size_field);
      me.priority_ = slot;  // dense, monotonically-assigned priorities preserve FIFO order

      // Backpressure: the SPSC LFQueue has no overflow check in getNextToWriteTo(), so a
      // fast producer can wrap and overwrite unread slots. Spin-wait until the slowest
      // consumer has drained enough room. We target <75% full to stay safely away from wrap.
      while (run_ && (output_queue_->size() >= Common::ME_MAX_MARKET_UPDATES * 3 / 4
                   || (sim_output_queue_ && sim_output_queue_->size() >= Common::ME_MAX_MARKET_UPDATES * 3 / 4))) {
        std::this_thread::yield();
      }

      // Busy-wait push — output_queue is drained by MDC replay thread, should not block long.
      MDPMarketUpdate mdp{};
      mdp.seq_num_ = seq_num++;
      mdp.me_market_update_ = me;
      {
        auto *out = output_queue_->getNextToWriteTo();
        *out = mdp;
        output_queue_->updateWriteIndex();
      }
      if (sim_output_queue_) {
        auto *sim_out = sim_output_queue_->getNextToWriteTo();
        *sim_out = mdp;
        sim_output_queue_->updateWriteIndex();
      }
      ++rows_emitted_;

      if (emit_then_forget) {
        lobster_id_to_slot_.erase(lobster_oid);
      }
    }

    in.close();
    logger_.log("%:% %() % Finished. emitted:% skipped:%\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), rows_emitted_, rows_skipped_);
    finished_.store(true, std::memory_order_release);
  }
}
