#include "fill_simulator.h"

#include <algorithm>

#include "common/thread_utils.h"

namespace Backtest {
  using Exchange::ClientResponseType;
  using Exchange::MEClientRequest;
  using Exchange::MEClientResponse;
  using Exchange::ClientRequestType;
  using Exchange::MarketUpdateType;
  using Exchange::MEMarketUpdate;
  using Exchange::MDPMarketUpdate;
  using Trading::BBO;

  FillSimulator::FillSimulator(Common::ClientId client_id,
                               Exchange::ClientRequestLFQueue *incoming_requests,
                               Exchange::ClientResponseLFQueue *outgoing_responses,
                               const BBO *bbo,
                               FillMode mode,
                               const Trading::MarketOrderBook *book,
                               Exchange::MDPMarketUpdateLFQueue *sim_md_events)
      : client_id_(client_id),
        incoming_requests_(incoming_requests),
        outgoing_responses_(outgoing_responses),
        bbo_(bbo),
        mode_(mode),
        book_(book),
        sim_md_events_(sim_md_events),
        logger_("backtest_fill_simulator.log") {
    if (mode_ == FillMode::QUEUE_AWARE) {
      ASSERT(book_ != nullptr, "QUEUE_AWARE mode requires a MarketOrderBook pointer.");
      ASSERT(sim_md_events_ != nullptr, "QUEUE_AWARE mode requires a sim-event queue.");
    }
  }

  FillSimulator::~FillSimulator() {
    stop();
    if (thread_) {
      if (thread_->joinable()) thread_->join();
      delete thread_;
      thread_ = nullptr;
    }
  }

  auto FillSimulator::start() -> void {
    run_ = true;
    thread_ = new std::thread([this]() { run(); });
    ASSERT(thread_ != nullptr, "Failed to start FillSimulator thread.");
  }

  auto FillSimulator::stop() -> void {
    run_ = false;
  }

  auto FillSimulator::emitResponse(const MEClientRequest &req,
                                   ClientResponseType type,
                                   Common::Qty exec_qty,
                                   Common::Qty leaves_qty,
                                   Common::Price fill_price,
                                   Common::OrderId market_order_id) noexcept -> void {
    auto *slot = outgoing_responses_->getNextToWriteTo();
    MEClientResponse resp{};
    resp.type_ = type;
    resp.client_id_ = req.client_id_;
    resp.ticker_id_ = req.ticker_id_;
    resp.client_order_id_ = req.order_id_;
    resp.market_order_id_ = market_order_id;
    resp.side_ = req.side_;
    resp.price_ = fill_price;
    resp.exec_qty_ = exec_qty;
    resp.leaves_qty_ = leaves_qty;
    *slot = resp;
    outgoing_responses_->updateWriteIndex();
  }

  auto FillSimulator::tryAggressiveFill(const MEClientRequest &req,
                                        const BBO &cur,
                                        Common::Qty leaves_qty) noexcept -> bool {
    // Aggressive-only fills. `min(...)` truncates fill qty to available opposite-side qty.
    if (req.side_ == Common::Side::BUY
        && cur.ask_price_ != Common::Price_INVALID
        && req.price_ >= cur.ask_price_) {
      const auto avail = (cur.ask_qty_ == Common::Qty_INVALID) ? 0U : cur.ask_qty_;
      const Common::Qty fill_qty = std::min(leaves_qty, avail);
      if (fill_qty == 0) return false;
      emitResponse(req, ClientResponseType::FILLED, fill_qty, 0, cur.ask_price_, next_market_order_id_++);
      ++fills_emitted_;
      return true;
    }
    if (req.side_ == Common::Side::SELL
        && cur.bid_price_ != Common::Price_INVALID
        && req.price_ <= cur.bid_price_) {
      const auto avail = (cur.bid_qty_ == Common::Qty_INVALID) ? 0U : cur.bid_qty_;
      const Common::Qty fill_qty = std::min(leaves_qty, avail);
      if (fill_qty == 0) return false;
      emitResponse(req, ClientResponseType::FILLED, fill_qty, 0, cur.bid_price_, next_market_order_id_++);
      ++fills_emitted_;
      return true;
    }
    return false;
  }

  auto FillSimulator::snapshotQueueAhead(Common::Price price, Common::Side side) const noexcept -> Common::Qty {
    if (!book_) return 0;
    // MarketOrderBook keeps its FIFO lists private. We reuse the BBO's view for now: if
    // our price matches the best on our side, queue_ahead = the side's total resting qty;
    // otherwise we can't see behind the top, so queue_ahead defaults to 0 (we assume head
    // of queue at a non-top price — conservative the other way, but the strategy we're
    // testing places orders near the top).
    //
    // NOTE: walking the full FIFO would require a friend accessor on MarketOrderBook. The
    // BBO already sums qty across orders at the best level (see updateBBO in
    // market_order_book.h), which is what we need for top-of-book snapshotting.
    const BBO cur = *bbo_;
    if (side == Common::Side::BUY && price == cur.bid_price_ && cur.bid_qty_ != Common::Qty_INVALID) {
      return cur.bid_qty_;
    }
    if (side == Common::Side::SELL && price == cur.ask_price_ && cur.ask_qty_ != Common::Qty_INVALID) {
      return cur.ask_qty_;
    }
    return 0;
  }

  auto FillSimulator::onSimMarketEvent(const MEMarketUpdate &ev) noexcept -> void {
    // Track the running priority watermark — used by any NEW that hasn't yet been
    // snapshotted (lazy init on next iteration after this event).
    if (ev.priority_ != Common::Priority_INVALID && ev.priority_ > global_priority_watermark_) {
      global_priority_watermark_ = ev.priority_;
    }

    // Fan out to every matching pending order on the same price+side. Walk by iterator so
    // we can erase filled orders safely.
    for (auto it = pending_.begin(); it != pending_.end(); ) {
      auto &po = it->second;
      if (!po.queue_aware || po.req.price_ != ev.price_ || po.req.side_ != ev.side_) {
        ++it;
        continue;
      }

      if (ev.type_ == MarketUpdateType::TRADE) {
        // The resting order being hit is identified by ev.order_id_ (LOBSTER) / slot. Its
        // priority tells us whether it was ahead of us at submit. Priority_INVALID (e.g.
        // Binance-synthetic TRADEs that carry priority=0) falls through via the <= check.
        const bool ahead = (ev.priority_ <= po.priority_watermark);
        Common::Qty consumed_ahead = 0;
        if (ahead) {
          consumed_ahead = std::min(ev.qty_, po.queue_ahead_qty);
          po.queue_ahead_qty -= consumed_ahead;
        }
        // Any trade qty left over after clearing the queue ahead hits us.
        const Common::Qty trade_left = ev.qty_ - consumed_ahead;
        if (po.queue_ahead_qty == 0 && trade_left > 0 && po.leaves_qty > 0) {
          const Common::Qty fill_qty = std::min(po.leaves_qty, trade_left);
          po.leaves_qty -= fill_qty;
          emitResponse(po.req, ClientResponseType::FILLED, fill_qty, po.leaves_qty,
                       po.req.price_, next_market_order_id_++);
          ++fills_emitted_;
          ++passive_fills_emitted_;
          if (po.leaves_qty == 0) {
            it = pending_.erase(it);
            continue;
          }
        }
      } else if (ev.type_ == MarketUpdateType::CANCEL) {
        // An order ahead of us left the queue. Decrement by its qty (bounded at 0).
        if (ev.priority_ <= po.priority_watermark) {
          const Common::Qty dec = std::min(ev.qty_, po.queue_ahead_qty);
          po.queue_ahead_qty -= dec;
        }
      }
      // ADD/MODIFY: ignored. ADD with priority > watermark goes behind us. MODIFY (LOBSTER
      // type 2, partial cancel) is rare — deferred for MVP, conservative.
      ++it;
    }
  }

  namespace {
    inline auto bboChanged(const BBO &a, const BBO &b) noexcept -> bool {
      return a.bid_price_ != b.bid_price_ || a.ask_price_ != b.ask_price_
          || a.bid_qty_ != b.bid_qty_   || a.ask_qty_ != b.ask_qty_;
    }
  }

  auto FillSimulator::run() noexcept -> void {
    logger_.log("%:% %() % FillSimulator started client_id=% mode=%\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), client_id_,
                mode_ == FillMode::QUEUE_AWARE ? "QUEUE_AWARE" : "AGGRESSIVE_ONLY");

    while (run_) {
      // 1. Drain the incoming strategy request queue (NEW / CANCEL).
      for (auto *req = incoming_requests_->getNextToRead(); req;
           req = incoming_requests_->getNextToRead()) {
        if (req->type_ == ClientRequestType::NEW) {
          // Always accept first — matches real exchange ACK-then-match semantics.
          emitResponse(*req, ClientResponseType::ACCEPTED, 0, req->qty_,
                       req->price_, next_market_order_id_++);
          PendingOrder po{*req, *bbo_};
          po.leaves_qty = req->qty_;
          pending_.emplace(req->order_id_, po);
        } else if (req->type_ == ClientRequestType::CANCEL) {
          auto it = pending_.find(req->order_id_);
          if (it != pending_.end()) {
            emitResponse(*req, ClientResponseType::CANCELED, 0, 0, req->price_, next_market_order_id_++);
            pending_.erase(it);
          } else {
            emitResponse(*req, ClientResponseType::CANCEL_REJECTED, 0, 0, req->price_, next_market_order_id_++);
          }
        }
        incoming_requests_->updateReadIndex();
      }

      // 2. Drain the sim market-event queue (only in QUEUE_AWARE mode).
      if (mode_ == FillMode::QUEUE_AWARE && sim_md_events_) {
        for (auto *mdp = sim_md_events_->getNextToRead(); mdp;
             mdp = sim_md_events_->getNextToRead()) {
          onSimMarketEvent(mdp->me_market_update_);
          sim_md_events_->updateReadIndex();
        }
      }

      // 3. Attempt aggressive fills + lazy queue snapshot on the current BBO. Only orders
      //    whose submit-BBO differs from the current BBO are eligible for the aggressive
      //    path — enforces the "next tick" rule.
      const BBO cur = *bbo_;  // snapshot; torn reads acceptable for backtest accuracy
      for (auto it = pending_.begin(); it != pending_.end(); ) {
        auto &po = it->second;
        if (!bboChanged(cur, po.bbo_at_submit)) {
          ++it;
          continue;
        }
        if (tryAggressiveFill(po.req, cur, po.leaves_qty)) {
          it = pending_.erase(it);
          continue;
        }
        // Passive — hand off to queue tracking if we're in queue-aware mode and haven't
        // snapshotted yet. First aggressive miss is our cue that the order will rest.
        if (mode_ == FillMode::QUEUE_AWARE && !po.queue_aware) {
          po.queue_ahead_qty = snapshotQueueAhead(po.req.price_, po.req.side_);
          po.priority_watermark = global_priority_watermark_;
          po.queue_aware = true;
        }
        po.bbo_at_submit = cur;  // update so we don't repeatedly retry against same BBO
        ++it;
      }
    }

    logger_.log("%:% %() % FillSimulator stopping. fills=% passive=% pending=%\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), fills_emitted_, passive_fills_emitted_,
                pending_.size());
  }
}
