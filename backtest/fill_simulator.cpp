#include "fill_simulator.h"

#include <algorithm>

#include "common/thread_utils.h"

namespace Backtest {
  using Exchange::ClientResponseType;
  using Exchange::MEClientRequest;
  using Exchange::MEClientResponse;
  using Exchange::ClientRequestType;
  using Trading::BBO;

  FillSimulator::FillSimulator(Common::ClientId client_id,
                               Exchange::ClientRequestLFQueue *incoming_requests,
                               Exchange::ClientResponseLFQueue *outgoing_responses,
                               const BBO *bbo)
      : client_id_(client_id),
        incoming_requests_(incoming_requests),
        outgoing_responses_(outgoing_responses),
        bbo_(bbo),
        logger_("backtest_fill_simulator.log") {
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

  auto FillSimulator::tryFill(const MEClientRequest &req, const BBO &cur) noexcept -> bool {
    // Aggressive-only fills. `min(...)` truncates fill qty to available opposite-side qty.
    if (req.side_ == Common::Side::BUY
        && cur.ask_price_ != Common::Price_INVALID
        && req.price_ >= cur.ask_price_) {
      const auto avail = (cur.ask_qty_ == Common::Qty_INVALID) ? 0U : cur.ask_qty_;
      const Common::Qty fill_qty = std::min(req.qty_, avail);
      if (fill_qty == 0) return false;
      emitResponse(req, ClientResponseType::FILLED, fill_qty, 0, cur.ask_price_, next_market_order_id_++);
      ++fills_emitted_;
      return true;
    }
    if (req.side_ == Common::Side::SELL
        && cur.bid_price_ != Common::Price_INVALID
        && req.price_ <= cur.bid_price_) {
      const auto avail = (cur.bid_qty_ == Common::Qty_INVALID) ? 0U : cur.bid_qty_;
      const Common::Qty fill_qty = std::min(req.qty_, avail);
      if (fill_qty == 0) return false;
      emitResponse(req, ClientResponseType::FILLED, fill_qty, 0, cur.bid_price_, next_market_order_id_++);
      ++fills_emitted_;
      return true;
    }
    return false;
  }

  namespace {
    inline auto bboChanged(const BBO &a, const BBO &b) noexcept -> bool {
      return a.bid_price_ != b.bid_price_ || a.ask_price_ != b.ask_price_
          || a.bid_qty_ != b.bid_qty_   || a.ask_qty_ != b.ask_qty_;
    }
  }

  auto FillSimulator::run() noexcept -> void {
    logger_.log("%:% %() % FillSimulator started client_id=%\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), client_id_);

    while (run_) {
      // 1. Drain the incoming request queue.
      for (auto *req = incoming_requests_->getNextToRead(); req;
           req = incoming_requests_->getNextToRead()) {
        if (req->type_ == ClientRequestType::NEW) {
          // Always accept first — matches real exchange ACK-then-match semantics.
          emitResponse(*req, ClientResponseType::ACCEPTED, 0, req->qty_,
                       req->price_, next_market_order_id_++);
          pending_.emplace(req->order_id_, PendingOrder{*req, *bbo_});
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

      // 2. Attempt fills against the current BBO. Only orders whose submit-BBO differs from
      //    the current BBO are eligible — enforces the "next tick" rule.
      const BBO cur = *bbo_;  // snapshot
      for (auto it = pending_.begin(); it != pending_.end(); ) {
        if (!bboChanged(cur, it->second.bbo_at_submit)) {
          ++it;  // still same tick as submit
          continue;
        }
        if (tryFill(it->second.req, cur)) {
          it = pending_.erase(it);
        } else {
          // Passive — MVP leaves resting forever. Update the snapshot so it doesn't retry
          // repeatedly against the same BBO (still waits for the next change).
          it->second.bbo_at_submit = cur;
          ++it;
        }
      }
    }

    logger_.log("%:% %() % FillSimulator stopping. fills=% pending=%\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), fills_emitted_, pending_.size());
  }
}
