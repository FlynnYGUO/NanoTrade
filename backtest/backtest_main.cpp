#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "common/types.h"
#include "common/logging.h"

#include "exchange/market_data/market_update.h"
#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"

#include "trading/market_data/market_data_consumer.h"
#include "trading/strategy/trade_engine.h"

#include "backtest/lobster_replay.h"
#include "backtest/binance_aggtrades_replay.h"
#include "backtest/fill_simulator.h"
#include "backtest/equity_recorder.h"

/// Usage:
///   backtest_main --data-format {lobster|binance-aggtrades} --data <csv> [--ticker N]
///                 [--clip N] [--threshold F] [--max-pos N] [--equity-csv PATH]
///
/// Only the `lobster` data format is wired up in 1.2.A; `binance-aggtrades` is reserved for
/// 1.2.B. The strategy is hard-coded to LiquidityTaker (TAKER) for the MVP.
namespace {
  struct Args {
    std::string data_format = "lobster";
    std::string data_path;
    Common::TickerId ticker_id = 0;
    Common::Qty clip = 10;
    double threshold = 0.5;
    Common::Qty max_order_size = 100;
    Common::Qty max_position = 1000;
    double max_loss = -100000.0;
    std::string equity_csv = "benchmarks/backtest/lobster/equity.csv";
    /// Per-book price-array anchor. Must be set so all observed prices fall within
    /// [base_price, base_price + ME_MAX_PRICE_LEVELS). For AAPL LOBSTER 2012-06-21 the
    /// price range is ~5,800,000–5,830,000 — 5,000,000 gives 200K+ of padding above.
    /// For Binance BTCUSDT cents in Oct 2025, prices ~5,800,000–7,300,000 — 5,000,000
    /// fits in a 2M window.
    Common::Price base_price = 0;
  };

  auto parseArgs(int argc, char **argv) -> Args {
    Args a;
    for (int i = 1; i < argc; ++i) {
      auto arg = std::string(argv[i]);
      auto next = [&]() -> std::string {
        if (i + 1 >= argc) { std::cerr << "Missing value for " << arg << "\n"; std::exit(1); }
        return std::string(argv[++i]);
      };
      if (arg == "--data-format")   a.data_format = next();
      else if (arg == "--data")     a.data_path = next();
      else if (arg == "--ticker")   a.ticker_id = std::stoul(next());
      else if (arg == "--clip")     a.clip = std::stoul(next());
      else if (arg == "--threshold") a.threshold = std::stod(next());
      else if (arg == "--max-pos")  a.max_position = std::stoul(next());
      else if (arg == "--equity-csv") a.equity_csv = next();
      else if (arg == "--base-price") a.base_price = std::stoll(next());
      else {
        std::cerr << "Unknown arg: " << arg << "\n";
        std::exit(1);
      }
    }
    if (a.data_path.empty()) {
      std::cerr << "ERROR: --data <csv> is required\n";
      std::exit(1);
    }
    return a;
  }
}

int main(int argc, char **argv) {
  const Args args = parseArgs(argc, argv);

  std::cout << "=== NanoTrade Backtest ===\n"
            << "format:     " << args.data_format << "\n"
            << "data:       " << args.data_path << "\n"
            << "ticker:     " << args.ticker_id << "\n"
            << "clip:       " << args.clip << "\n"
            << "threshold:  " << args.threshold << "\n"
            << "max_pos:    " << args.max_position << "\n"
            << "base_price: " << args.base_price << "\n"
            << "equity_csv: " << args.equity_csv << "\n\n";

  // Queues that tie together the full pipeline.
  auto replay_md_queue = std::make_unique<Exchange::MDPMarketUpdateLFQueue>(Common::ME_MAX_MARKET_UPDATES);
  auto te_md_updates   = std::make_unique<Exchange::MEMarketUpdateLFQueue>(Common::ME_MAX_MARKET_UPDATES);
  auto ogw_requests    = std::make_unique<Exchange::ClientRequestLFQueue>(Common::ME_MAX_CLIENT_UPDATES);
  auto ogw_responses   = std::make_unique<Exchange::ClientResponseLFQueue>(Common::ME_MAX_CLIENT_UPDATES);

  // Trade engine configuration — MVP hardcodes only the target ticker's slot.
  Common::TradeEngineCfgHashMap ticker_cfg{};
  ticker_cfg.at(args.ticker_id) = {
      args.clip,
      args.threshold,
      {args.max_order_size, args.max_position, args.max_loss}
  };

  // base_price anchors the book's dense price array. One slot per ticker; all other
  // tickers use 0 since they never see traffic in this backtest.
  Common::TickerBasePriceHashMap ticker_base_prices{};
  ticker_base_prices.at(args.ticker_id) = args.base_price;

  const Common::ClientId client_id = 1;

  std::cout << "Starting TradeEngine (LiquidityTaker)...\n";
  auto trade_engine = std::make_unique<Trading::TradeEngine>(
      client_id, Common::AlgoType::TAKER, ticker_cfg,
      ogw_requests.get(), ogw_responses.get(), te_md_updates.get(),
      ticker_base_prices);
  trade_engine->start();

  // BBO pointer for the FillSimulator. MarketOrderBook is created inside TradeEngine's
  // constructor, so we can read it out now.
  const Trading::MarketOrderBook *book = trade_engine->getMarketOrderBook(args.ticker_id);
  ASSERT(book != nullptr, "TradeEngine did not create a MarketOrderBook for the target ticker");
  const Trading::BBO *bbo = book->getBBO();

  std::cout << "Starting FillSimulator...\n";
  auto fill_sim = std::make_unique<Backtest::FillSimulator>(
      client_id, ogw_requests.get(), ogw_responses.get(), bbo);
  fill_sim->start();

  std::cout << "Starting MarketDataConsumer (replay mode)...\n";
  auto mdc = std::make_unique<Trading::MarketDataConsumer>(
      client_id, te_md_updates.get(), replay_md_queue.get());
  mdc->start();

  std::cout << "Starting EquityRecorder...\n";
  auto recorder = std::make_unique<Backtest::EquityRecorder>(
      trade_engine->getPositionKeeper(), args.ticker_id, args.equity_csv, 100);
  recorder->start();

  // Launch the replay AFTER everything downstream is running so we don't drop events.
  std::unique_ptr<Backtest::LobsterReplay> lobster_replay;
  std::unique_ptr<Backtest::BinanceAggTradesReplay> binance_replay;
  auto replay_finished = [&]() -> bool {
    if (lobster_replay) return lobster_replay->finished();
    if (binance_replay) return binance_replay->finished();
    return true;
  };

  if (args.data_format == "lobster") {
    std::cout << "Starting LobsterReplay on " << args.data_path << "\n";
    lobster_replay = std::make_unique<Backtest::LobsterReplay>(
        args.data_path, replay_md_queue.get(), args.ticker_id);
    lobster_replay->start();
  } else if (args.data_format == "binance-aggtrades") {
    std::cout << "Starting BinanceAggTradesReplay on " << args.data_path << "\n";
    binance_replay = std::make_unique<Backtest::BinanceAggTradesReplay>(
        args.data_path, replay_md_queue.get(), args.ticker_id);
    binance_replay->start();
  } else {
    std::cerr << "Unknown --data-format " << args.data_format
              << " (expected 'lobster' or 'binance-aggtrades')\n";
    std::exit(1);
  }

  // Drive the main thread: wait for replay to signal finished, then drain queues.
  while (!replay_finished()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  if (lobster_replay) {
    std::cout << "Replay finished: emitted=" << lobster_replay->rowsEmitted()
              << " skipped=" << lobster_replay->rowsSkipped() << "\n";
  } else {
    std::cout << "Replay finished: trades=" << binance_replay->tradesProcessed()
              << " emitted=" << binance_replay->rowsEmitted()
              << " skipped=" << binance_replay->rowsSkipped() << "\n";
  }
  std::cout << "Draining queues...\n";

  // Wait until all queues are empty (TradeEngine has processed everything).
  for (int idle_iters = 0; idle_iters < 20; ) {
    const auto busy = replay_md_queue->size() + te_md_updates->size()
                    + ogw_requests->size() + ogw_responses->size();
    if (busy == 0) ++idle_iters;
    else            idle_iters = 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::cout << "Drain complete. Stopping threads...\n";
  if (lobster_replay) lobster_replay->stop();
  if (binance_replay) binance_replay->stop();
  recorder->stop();
  fill_sim->stop();
  mdc->stop();
  trade_engine->stop();

  // Give threads time to notice run_=false and exit.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  std::cout << "FillSimulator: fills=" << fill_sim->fillsEmitted()
            << " pending=" << fill_sim->pendingCount() << "\n";

  recorder->finalize();
  std::cout << "\nDone.\n";
  return 0;
}
