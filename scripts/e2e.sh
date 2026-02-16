#!/usr/bin/env bash
set -euo pipefail

# Stable E2E entrypoint. Environment/device settings are loaded from:
#   tools/e2e/e2e.local.json (preferred, gitignored)
#   tools/e2e/e2e.local.env (gitignored)
# Template:
#   tools/e2e/e2e.local.example.json
#   tools/e2e/e2e.example.env

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PYTHONUNBUFFERED=1
LOG_DIR="${ROOT_DIR}/tools/e2e/logs"
mkdir -p "${LOG_DIR}"
TS="$(date +%Y%m%d_%H%M%S)"
LOG_FILE="${LOG_DIR}/e2e_${TS}.log"
LATEST_LOG="${LOG_DIR}/e2e_latest.log"

echo "[e2e.sh] writing log: ${LOG_FILE}"
python3 -u "${ROOT_DIR}/tools/e2e/test_rs485_stub.py" 2>&1 | tee "${LOG_FILE}"
cp -f "${LOG_FILE}" "${LATEST_LOG}"
