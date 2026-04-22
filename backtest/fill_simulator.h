#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <thread>
#include <unordered_map>

#include "common/types.h"
#include "common/logging.h"
#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "exchange/market_data/market_update.h"
#include "trading/strategy/market_order.h"         // BBO, MarketOrder, MarketOrdersAtPrice
#include "trading/strategy/market_order_book.h"    // forward-usable here via include

namespace Backtest {
  /// Fill model the simulator runs in.
  ///  - AGGRESSIVE_ONLY: only fills orders whose price crosses the BBO when the BBO ticks
  ///    (the Phase 1.2 MVP behavior). Used for Binance synthetic L1 where we have no real
  ///    queue information.
  ///  - QUEUE_AWARE: aggressive fills PLUS passive fills with FIFO queue-position tracking.
  ///    Requires real L2 (LOBSTER). See design notes in the .cpp.
  enum class FillMode : uint8_t { AGGRESSIVE_ONLY, QUEUE_AWARE };

  /// Conservative in-process fill simulator — drop-in replacement for Trading::OrderGateway's
  /// TCP leg. Consumes MEClientRequests produced by OrderManager, emits MEClientResponses
  /// that the TradeEngine consumes.
  ///
  /// QUEUE_AWARE mode additionally consumes a copy of the market-event stream (teed off
  /// the replay source). For each resting order it tracks a virtual queue position:
  ///   queue_ahead_qty: total LOBSTER qty at our price/side ahead of us
  ///   priority_watermark: max LOBSTER priority_ observed at submit time — any
  ///     event whose priority_ <= watermark was already in the book when we arrived
  ///     and is therefore ahead of us.
  /// TRADE events on our price/side decrement queue_ahead_qty; once it reaches zero, the
  /// next TRADE fills us (partial fills supported).
  ///
  /// Concurrency: BBO pointer is read non-atomically (same as Phase 1.2 MVP; comment
  /// preserved below). The book's FIFO linked list is only walked lazily on the sim-event
  /// thread at the moment we process a NEW — that moment is serialized with the event
  /// stream produced by the single replay thread (and the book mutates on TradeEngine's
  /// thread from the same stream), so the book is quiescent for the event we're between.
  class FillSimulator {
  public:
    FillSimulator(Common::ClientId client_id,
                  Exchange::ClientRequestLFQueue *incoming_requests,
                  Exchange::ClientResponseLFQueue *outgoing_responses,
                  const Trading::BBO *bbo,
                  FillMode mode = FillMode::AGGRESSIVE_ONLY,
                  const Trading::MarketOrderBook *book = nullptr,
                  Exchange::MDPMarketUpdateLFQueue *sim_md_events = nullptr);

    ~FillSimulator();

    auto start() -> void;
    auto stop() -> void;

    auto pendingCount() const noexcept -> size_t { return pending_.size(); }
    auto fillsEmitted() const noexcept -> size_t { return fills_emitted_; }
    auto passiveFillsEmitted() const noexcept -> size_t { return passive_fills_emitted_; }

    FillSimulator() = delete;
    FillSimulator(const FillSimulator &) = delete;
    FillSimulator &operator=(const FillSimulator &) = delete;

  private:
    auto run() noexcept -> void;

    auto emitResponse(const Exchange::MEClientRequest &req,
                      Exchange::ClientResponseType type,
                      Common::Qty exec_qty,
                      Common::Qty leaves_qty,
                      Common::Price fill_price,
                      Common::OrderId market_order_id) noexcept -> void;

    /// Aggressive fill against the current BBO snapshot. Returns true if FILLED emitted
    /// (full fill for the aggressive path — partial execs vs BBO qty are collapsed to a
    /// single response per Phase 1.2 semantics).
    auto tryAggressiveFill(const Exchange::MEClientRequest &req,
                           const Trading::BBO &cur,
                           Common::Qty leaves_qty) noexcept -> bool;

    /// Walk the book's FIFO list at (price, side) and sum resting qty. Used to snapshot
    /// queue_ahead_qty at submit time. Returns 0 if the level is empty or the book is
    /// absent.
    auto snapshotQueueAhead(Common::Price price, Common::Side side) const noexcept -> Common::Qty;

    /// Apply a single market event to all pending orders (queue decrements + passive fills).
    auto onSimMarketEvent(const Exchange::MEMarketUpdate &ev) noexcept -> void;

    struct PendingOrder {
      Exchange::MEClientRequest req;
      Trading::BBO bbo_at_submit;
      Common::Qty leaves_qty = 0;              // remaining qty after any fills so far
      Common::Qty queue_ahead_qty = 0;         // LOBSTER qty ahead of us at our price/side
      Common::Priority priority_watermark = 0; // max LOBSTER priority_ known at submit
      bool queue_aware = false;                // queue snapshot taken & passive fills enabled
    };

    const Common::ClientId client_id_;
    Exchange::ClientRequestLFQueue *incoming_requests_;
    Exchange::ClientResponseLFQueue *outgoing_responses_;
    const Trading::BBO *bbo_;

    const FillMode mode_;
    const Trading::MarketOrderBook *book_ = nullptr;
    Exchange::MDPMarketUpdateLFQueue *sim_md_events_ = nullptr;

    /// Running max of priority_ seen on the sim-event stream. Used as the watermark for
    /// any pending order whose queue snapshot we compute lazily on first event after NEW.
    Common::Priority global_priority_watermark_ = 0;

    std::unordered_map<Common::OrderId, PendingOrder> pending_;
    Common::OrderId next_market_order_id_ = 1;
    size_t fills_emitted_ = 0;
    size_t passive_fills_emitted_ = 0;

    std::thread *thread_ = nullptr;
    volatile bool run_ = false;

    Common::Logger logger_;
    std::string time_str_;
  };
}
