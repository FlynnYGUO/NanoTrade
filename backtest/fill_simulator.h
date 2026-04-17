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
#include "trading/strategy/market_order.h"  // BBO

namespace Backtest {
  /// Conservative in-process fill simulator — drop-in replacement for Trading::OrderGateway's
  /// TCP leg. Consumes MEClientRequests produced by OrderManager, emits MEClientResponses
  /// that the TradeEngine consumes.
  ///
  /// Fill model (MVP, LiquidityTaker-only):
  ///   - NEW request:
  ///       1. Immediately emit ACCEPTED.
  ///       2. Snapshot the current BBO as "submit BBO" and park the order as pending.
  ///       3. On later iterations, once the BBO has moved (the "next tick"), check if the
  ///          order is aggressive against the current opposite side:
  ///            buy @ price >= best_ask  -> fill at best_ask, qty = min(req.qty, ask_qty)
  ///            sell @ price <= best_bid -> fill at best_bid, qty = min(req.qty, bid_qty)
  ///       4. Passive orders (price not crossing) remain pending — MVP never fills them.
  ///   - CANCEL request:
  ///       emit CANCELED if found in pending; CANCEL_REJECTED otherwise.
  ///
  /// Requirements:
  ///   - `bbo_` must be a pointer to a BBO that is updated externally (from MarketOrderBook)
  ///     as the replay progresses. We read it non-atomically; torn reads are acceptable for
  ///     backtest accuracy (rare, and the next iteration sees a clean value).
  class FillSimulator {
  public:
    FillSimulator(Common::ClientId client_id,
                  Exchange::ClientRequestLFQueue *incoming_requests,
                  Exchange::ClientResponseLFQueue *outgoing_responses,
                  const Trading::BBO *bbo);

    ~FillSimulator();

    auto start() -> void;
    auto stop() -> void;

    auto pendingCount() const noexcept -> size_t { return pending_.size(); }
    auto fillsEmitted() const noexcept -> size_t { return fills_emitted_; }

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

    /// Try to fill a pending aggressive order against the current BBO snapshot. Returns true
    /// if the order was filled (partial counts as filled for MVP purposes — see class docs).
    auto tryFill(const Exchange::MEClientRequest &req, const Trading::BBO &cur) noexcept -> bool;

    struct PendingOrder {
      Exchange::MEClientRequest req;
      Trading::BBO bbo_at_submit;
    };

    const Common::ClientId client_id_;
    Exchange::ClientRequestLFQueue *incoming_requests_;
    Exchange::ClientResponseLFQueue *outgoing_responses_;
    const Trading::BBO *bbo_;

    std::unordered_map<Common::OrderId, PendingOrder> pending_;
    Common::OrderId next_market_order_id_ = 1;
    size_t fills_emitted_ = 0;

    std::thread *thread_ = nullptr;
    volatile bool run_ = false;

    Common::Logger logger_;
    std::string time_str_;
  };
}
