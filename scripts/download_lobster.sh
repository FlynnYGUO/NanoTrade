#!/usr/bin/env bash
# Download LOBSTER free sample data (NASDAQ 2012-06-21, 10 levels of depth).
#
# Sample files contain two CSVs per stock:
#   1. <STOCK>_<DATE>_<START>-<END>_message_10.csv
#      — event stream: time, type, order_id, size, price, direction
#   2. <STOCK>_<DATE>_<START>-<END>_orderbook_10.csv
#      — book snapshot after each event (40 columns = 10 levels × {ask_p, ask_q, bid_p, bid_q})
#
# See https://lobsterdata.com/info/DataSamples.php (redirects to data.lobsterdata.com).
#
# Usage: ./scripts/download_lobster.sh [STOCK]
#   STOCK — one of AAPL | AMZN | GOOG | INTC | MSFT (default: AAPL)

set -euo pipefail

STOCK="${1:-AAPL}"
LEVELS=10
DATE="2012-06-21"
FILE="LOBSTER_SampleFile_${STOCK}_${DATE}_${LEVELS}.zip"
URL="https://data.lobsterdata.com/info/sample/${FILE}"

OUT_DIR="data/lobster/${STOCK}"
mkdir -p "${OUT_DIR}"

if ls "${OUT_DIR}"/*_message_${LEVELS}.csv >/dev/null 2>&1; then
  echo "Already have ${OUT_DIR}/*_message_${LEVELS}.csv"
  exit 0
fi

echo "Downloading ${URL}"
curl --fail --location --show-error --progress-bar \
     --output "${OUT_DIR}/${FILE}" \
     "${URL}"

python3 -c "
import zipfile, sys
with zipfile.ZipFile('${OUT_DIR}/${FILE}') as z:
    z.extractall('${OUT_DIR}')
    print('Extracted:', z.namelist())
"
rm "${OUT_DIR}/${FILE}"

echo ""
echo "Files in ${OUT_DIR}:"
ls -la "${OUT_DIR}"

MSG=$(ls "${OUT_DIR}"/*_message_${LEVELS}.csv)
echo ""
echo "First 3 rows of ${MSG}:"
head -n 3 "${MSG}"
