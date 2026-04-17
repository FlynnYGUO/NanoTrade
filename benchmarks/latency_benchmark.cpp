#include <iostream>
#include <iomanip>
#include <memory>
#include <random>
#include <filesystem>

#include "common/types.h"
#include "common/lf_queue.h"
#include "common/perf_utils.h"
#include "common/latency_stats.h"
#include "common/thread_utils.h"

#include "exchange/matcher/matching_engine.h"
#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "exchange/market_data/market_update.h"

using namespace Common;
using namespace Exchange;

#ifdef NT_BENCHMARK_NO_LOG
  static constexpr const char *RESULTS_DIR = "benchmarks/results/release";
#else
  static constexpr const char *RESULTS_DIR = "benchmarks/results";
#endif

/// Print a LatencyPercentiles struct in a formatted table row.
static auto printPercentiles(const char *label, const LatencyPercentiles &p) -> void {
  std::cout << std::left << std::setw(35) << label
            << std::right
            << std::setw(8) << std::fixed << std::setprecision(0) << p.min
            << std::setw(8) << p.p50
            << std::setw(8) << p.p90
            << std::setw(8) << p.p99
            << std::setw(10) << p.p999
            << std::setw(10) << p.max
            << std::setw(10) << p.mean
            << std::setw(10) << p.count
            << std::endl;
}

static auto printHeader() -> void {
  std::cout << std::left << std::setw(35) << "Measurement"
            << std::right
            << std::setw(8) << "min"
            << std::setw(8) << "p50"
            << std::setw(8) << "p90"
            << std::setw(8) << "p99"
            << std::setw(10) << "p99.9"
            << std::setw(10) << "max"
            << std::setw(10) << "mean"
            << std::setw(10) << "count"
            << std::endl;
  std::cout << std::string(99, '-') << std::endl;
}

int main(int, char **) {
  std::cout << "=== NanoTrade Latency Benchmark ===" << std::endl;

  // Pin the benchmark thread to a P-core (Alder Lake: CPU 0-15 are P-cores).
  // Reduces tail latency caused by cross-core scheduling and cache migration.
  // Logger background threads spawned by MatchingEngine use default affinity,
  // so they remain free to run on other cores away from us.
  constexpr int BENCHMARK_CORE = 4;
  if (!Common::setThreadCore(BENCHMARK_CORE)) {
    std::cerr << "Warning: failed to pin thread to core " << BENCHMARK_CORE
              << " (continuing unpinned)" << std::endl;
  } else {
    std::cout << "Pinned benchmark thread to core " << BENCHMARK_CORE << std::endl;
  }

  std::cout << "Calibrating RDTSC..." << std::endl;

  const double nanos_per_cycle = calibrateRdtsc();
  std::cout << "RDTSC calibration: " << std::fixed << std::setprecision(4) << nanos_per_cycle << " ns/cycle" << std::endl;
  std::cout << std::endl;

  // --- Benchmark 1: Exchange Matching Engine (order add + match) ---
  // This measures the full processClientRequest path including order book operations
  // and response/market-update generation via the MatchingEngine.

  constexpr size_t NUM_ORDERS = 500'000;

  // Heap-allocate stats objects — they contain 1M uint64_t samples each (8MB).
  auto stats_me_total = std::make_unique<LatencyStats<>>();
  auto stats_ob_add = std::make_unique<LatencyStats<>>();

  // Create LF queues to wire up the matching engine.
  ClientRequestLFQueue client_requests(ME_MAX_CLIENT_UPDATES);
  ClientResponseLFQueue client_responses(ME_MAX_CLIENT_UPDATES);
  MEMarketUpdateLFQueue market_updates(ME_MAX_MARKET_UPDATES);

  MatchingEngine matching_engine(&client_requests, &client_responses, &market_updates);

  // We won't start the matching engine's thread — we'll call processClientRequest directly.
  // This isolates the matching logic from thread scheduling jitter.

  std::mt19937 rng(42);
  std::uniform_int_distribution<Price> price_dist(90, 110);
  std::uniform_int_distribution<Qty> qty_dist(1, 100);

  // Warmup: populate the book with some resting orders at various price levels.
  constexpr size_t WARMUP = 1000;
  for (size_t i = 0; i < WARMUP; ++i) {
    auto *req = client_requests.getNextToWriteTo();
    req->type_ = ClientRequestType::NEW;
    req->client_id_ = 0;
    req->order_id_ = i;
    req->ticker_id_ = 0;
    req->side_ = (i % 2 == 0) ? Side::BUY : Side::SELL;
    req->price_ = price_dist(rng);
    req->qty_ = qty_dist(rng);
    matching_engine.processClientRequest(req);
  }

  // Drain queues from warmup.
  while (client_responses.size()) client_responses.updateReadIndex();
  while (market_updates.size()) market_updates.updateReadIndex();

  std::cout << "Warmup complete (" << WARMUP << " orders). Book populated." << std::endl;
  std::cout << "Running " << NUM_ORDERS << " order insertions..." << std::endl << std::endl;

  // Benchmark: measure each order add.
  for (size_t i = 0; i < NUM_ORDERS; ++i) {
    auto *req = client_requests.getNextToWriteTo();
    req->type_ = ClientRequestType::NEW;
    req->client_id_ = 1;  // Use client 1 for benchmark orders.
    req->order_id_ = i;
    req->ticker_id_ = 0;
    req->side_ = (i % 2 == 0) ? Side::BUY : Side::SELL;
    req->price_ = price_dist(rng);
    req->qty_ = qty_dist(rng);

    START_MEASURE(me_total);
    matching_engine.processClientRequest(req);
    MEASURE_AND_RECORD(me_total, *stats_me_total);

    // Drain output queues to prevent them filling up.
    while (client_responses.size()) client_responses.updateReadIndex();
    while (market_updates.size()) market_updates.updateReadIndex();
  }

  // --- Benchmark 2: Trading-side order book update ---
  // Measure MarketOrderBook::onMarketUpdate for ADD operations.
  // We can't easily isolate this without a TradeEngine (callback coupling),
  // so we measure the exchange-side add() in isolation as well.

  // Create a fresh matching engine for isolated add measurement.
  ClientRequestLFQueue req2(ME_MAX_CLIENT_UPDATES);
  ClientResponseLFQueue resp2(ME_MAX_CLIENT_UPDATES);
  MEMarketUpdateLFQueue mdu2(ME_MAX_MARKET_UPDATES);
  MatchingEngine me2(&req2, &resp2, &mdu2);

  // Use non-crossing prices so orders just rest (no matching overhead).
  for (size_t i = 0; i < NUM_ORDERS; ++i) {
    auto *req = req2.getNextToWriteTo();
    req->type_ = ClientRequestType::NEW;
    req->client_id_ = 0;
    req->order_id_ = i % ME_MAX_ORDER_IDS;
    req->ticker_id_ = 0;
    // Buys at 90-99, sells at 101-110 — guaranteed no crossing.
    if (i % 2 == 0) {
      req->side_ = Side::BUY;
      req->price_ = 90 + static_cast<Price>(rng() % 10);
    } else {
      req->side_ = Side::SELL;
      req->price_ = 101 + static_cast<Price>(rng() % 10);
    }
    req->qty_ = qty_dist(rng);

    START_MEASURE(ob_add);
    me2.processClientRequest(req);
    MEASURE_AND_RECORD(ob_add, *stats_ob_add);

    while (resp2.size()) resp2.updateReadIndex();
    while (mdu2.size()) mdu2.updateReadIndex();
  }

  // --- Results ---
  std::cout << "All values in nanoseconds." << std::endl << std::endl;
  printHeader();
  printPercentiles("ME processClientRequest (mixed)", stats_me_total->computePercentiles(nanos_per_cycle));
  printPercentiles("ME order add (no-cross, resting)", stats_ob_add->computePercentiles(nanos_per_cycle));
  std::cout << std::endl;

  // --- Dump raw data for notebook ---
  std::filesystem::create_directories(RESULTS_DIR);
  const std::string me_csv = std::string(RESULTS_DIR) + "/me_total_latency.csv";
  const std::string ob_csv = std::string(RESULTS_DIR) + "/ob_add_latency.csv";
  stats_me_total->dumpToCSV(me_csv, nanos_per_cycle);
  stats_ob_add->dumpToCSV(ob_csv, nanos_per_cycle);
  std::cout << "Raw data written to " << RESULTS_DIR << "/*.csv" << std::endl;

  return 0;
}
