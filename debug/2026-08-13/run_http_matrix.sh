#!/usr/bin/env bash
# Run the 8-worker HTTP concurrency matrix used by PERFORMANCE.md.
#
# Examples:
#   USE_SUDO=1 OVERWRITE=1 MODE=short \
#     ./debug/2026-08-13/run_http_matrix.sh
#   USE_SUDO=1 OVERWRITE=1 MODE=keepalive \
#     ./debug/2026-08-13/run_http_matrix.sh
#
# The script runs one mode per invocation. It creates temporary scenarios and
# writes the stats CSVs directly to temp/.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
TG_DIR="${ROOT_DIR}/traffic-gen"

TRAFFIC_GEN_BIN="${TRAFFIC_GEN_BIN:-${TG_DIR}/build/traffic-gen}"
MODE="${MODE:-keepalive}"
WORKERS="${WORKERS:-8}"
TARGET_CPS="${TARGET_CPS:-100000}"
DURATION="${DURATION:-120}"
REPORT_INTERVAL="${REPORT_INTERVAL:-60}"
CONCURRENCIES="${CONCURRENCIES:-500 1000 5000}"
OUTPUT_DIR="${OUTPUT_DIR:-${ROOT_DIR}/temp}"
PEER_IP="${PEER_IP:-192.168.21.106}"
PEER_PORT="${PEER_PORT:-8888}"
HTTP_PATH="${HTTP_PATH:-/}"
USE_SUDO="${USE_SUDO:-1}"
OVERWRITE="${OVERWRITE:-0}"

if [[ "${TRAFFIC_GEN_BIN}" != /* ]]; then
  TRAFFIC_GEN_BIN="${ROOT_DIR}/${TRAFFIC_GEN_BIN}"
fi
if [[ "${OUTPUT_DIR}" != /* ]]; then
  OUTPUT_DIR="${ROOT_DIR}/${OUTPUT_DIR}"
fi

case "${MODE}" in
short)
  KEEPALIVE_JSON=false
  FILE_SUFFIX=
  ;;
keepalive)
  KEEPALIVE_JSON=true
  FILE_SUFFIX=-keepalive
  ;;
*)
  echo "error: MODE must be short or keepalive (got ${MODE})" >&2
  exit 2
  ;;
esac

if [[ ! -x "${TRAFFIC_GEN_BIN}" ]]; then
  echo "error: traffic-gen binary not found or not executable:" >&2
  echo "       ${TRAFFIC_GEN_BIN}" >&2
  echo "hint: build traffic-gen first, or set TRAFFIC_GEN_BIN=/path/to/traffic-gen" >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "error: python3 not found" >&2
  exit 1
fi

if [[ "${USE_SUDO}" == 1 && "${EUID}" -ne 0 ]] &&
   ! command -v sudo >/dev/null 2>&1; then
  echo "error: USE_SUDO=1 but sudo is not available" >&2
  exit 1
fi

if [[ "${OVERWRITE}" != 0 && "${OVERWRITE}" != 1 ]]; then
  echo "error: OVERWRITE must be 0 or 1" >&2
  exit 2
fi

mkdir -p "${OUTPUT_DIR}"
SCENARIO_DIR="$(mktemp -d "${TMPDIR:-/tmp}/traffic-gen-2026-08-13.XXXXXX")"
trap 'rm -rf -- "${SCENARIO_DIR}"' EXIT

if [[ "${USE_SUDO}" == 1 && "${EUID}" -ne 0 ]]; then
  RUNNER=(sudo)
else
  RUNNER=()
fi

for concurrency in ${CONCURRENCIES}; do
  scenario="${SCENARIO_DIR}/http-${MODE}-${concurrency}con.json"
  output="${OUTPUT_DIR}/8-13-8w-${TARGET_CPS}cps-${concurrency}-con${FILE_SUFFIX}.csv"

  if [[ -e "${output}" && "${OVERWRITE}" != 1 ]]; then
    echo "error: output exists; refusing to overwrite:" >&2
    echo "       ${output}" >&2
    echo "hint: choose another OUTPUT_DIR or set OVERWRITE=1" >&2
    exit 1
  fi

  KEEPALIVE_JSON="${KEEPALIVE_JSON}" \
  TARGET_CPS="${TARGET_CPS}" \
  DURATION="${DURATION}" \
  REPORT_INTERVAL="${REPORT_INTERVAL}" \
  CONCURRENCY="${concurrency}" \
  PEER_IP="${PEER_IP}" \
  PEER_PORT="${PEER_PORT}" \
  HTTP_PATH="${HTTP_PATH}" \
  python3 - "${scenario}" <<'PY'
import json
import os
import sys

scenario_path = sys.argv[1]
doc = {
    "name": f"http-{os.environ['CONCURRENCY']}con",
    "duration_sec": int(os.environ["DURATION"]),
    "max_concurrency": int(os.environ["CONCURRENCY"]),
    "target_cps": int(os.environ["TARGET_CPS"]),
    "report_interval_sec": int(os.environ["REPORT_INTERVAL"]),
    "classes": [
        {
            "name": "http_get",
            "weight": 1,
            "transport": "tcp",
            "peer": {
                "ip": os.environ["PEER_IP"],
                "port": int(os.environ["PEER_PORT"]),
            },
            "http": {
                "method": "GET",
                "path": os.environ["HTTP_PATH"],
                "keepalive": os.environ["KEEPALIVE_JSON"] == "true",
            },
        }
    ],
}

with open(scenario_path, "w", encoding="utf-8") as stream:
    json.dump(doc, stream, indent=4)
    stream.write("\n")
PY

  echo "running mode=${MODE} concurrency=${concurrency} output=${output}"
  (
    cd "${TG_DIR}"
    "${RUNNER[@]}" "${TRAFFIC_GEN_BIN}" -- \
      --workers "${WORKERS}" \
      --stats-csv "${output}" \
      "${scenario}"
  )
done

echo "completed: mode=${MODE}; outputs=${OUTPUT_DIR}"
