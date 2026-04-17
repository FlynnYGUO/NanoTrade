#!/usr/bin/env bash
# Download Binance Spot aggTrades daily archive from the public data vision archive.
#
# Binance does NOT publish an L2 depth archive for spot (bookTicker and depth endpoints
# both return 404 as of 2026). aggTrades is the finest granularity available:
#   agg_trade_id, price, qty, first_trade_id, last_trade_id, transact_time, is_buyer_maker
# We reconstruct a synthetic L1 book inside BinanceAggTradesReplay:
#   is_buyer_maker=true  -> an aggressive seller hit the bid, so trade_price ~= best_bid
#   is_buyer_maker=false -> an aggressive buyer lifted the ask, so trade_price ~= best_ask
#
# Usage:
#   ./scripts/download_binance.sh                      # BTCUSDT, yesterday UTC
#   ./scripts/download_binance.sh ETHUSDT 2025-10-15
#   ./scripts/download_binance.sh BTCUSDT 2025-10       # whole month — see MONTHLY mode

set -euo pipefail

SYMBOL="${1:-BTCUSDT}"
DATE="${2:-$(date -u -d 'yesterday' +%Y-%m-%d)}"
MODE="${3:-daily}"  # daily | monthly

if [[ "${MODE}" == "monthly" || "${DATE}" =~ ^[0-9]{4}-[0-9]{2}$ ]]; then
  # Monthly archive — single zip containing ~30 days concatenated.
  MODE="monthly"
  FILE="${SYMBOL}-aggTrades-${DATE}.zip"
  URL="https://data.binance.vision/data/spot/monthly/aggTrades/${SYMBOL}/${FILE}"
else
  FILE="${SYMBOL}-aggTrades-${DATE}.zip"
  URL="https://data.binance.vision/data/spot/daily/aggTrades/${SYMBOL}/${FILE}"
fi

OUT_DIR="data/binance/spot/aggTrades/${SYMBOL}"
mkdir -p "${OUT_DIR}"

CSV_NAME="${FILE%.zip}.csv"
if [[ -f "${OUT_DIR}/${CSV_NAME}" ]]; then
  echo "Already have ${OUT_DIR}/${CSV_NAME}"
  exit 0
fi

echo "Downloading ${URL}"
curl --fail --location --show-error --progress-bar \
     --output "${OUT_DIR}/${FILE}" \
     "${URL}"

python3 -c "
import zipfile
with zipfile.ZipFile('${OUT_DIR}/${FILE}') as z:
    z.extractall('${OUT_DIR}')
    print('Extracted:', z.namelist())
"
rm "${OUT_DIR}/${FILE}"

ROWS=$(wc -l < "${OUT_DIR}/${CSV_NAME}")
SIZE=$(du -h "${OUT_DIR}/${CSV_NAME}" | cut -f1)
echo "Done: ${OUT_DIR}/${CSV_NAME}  (${ROWS} rows, ${SIZE})"
echo "First 2 rows:"
head -n 2 "${OUT_DIR}/${CSV_NAME}"
