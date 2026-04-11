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
LOG_RETENTION_DAYS=10
LOG_RETENTION_MINUTES=$((LOG_RETENTION_DAYS * 24 * 60))
mapfile -t STALE_LOGS < <(find "${LOG_DIR}" -maxdepth 1 -type f \( -name 'e2e_*.log' -o -name 'e2e_*.serial.log' \) -mmin "+${LOG_RETENTION_MINUTES}" -print)
if ((${#STALE_LOGS[@]} > 0)); then
  printf '[e2e.sh] pruning %d log(s) older than %d days\n' "${#STALE_LOGS[@]}" "${LOG_RETENTION_DAYS}"
  rm -f -- "${STALE_LOGS[@]}"
fi
RUN_ID="${E2E_RUN_ID:-$(date +%Y%m%d_%H%M%S)}"
LOG_FILE="${LOG_DIR}/e2e_${RUN_ID}.log"
SERIAL_LOG="${LOG_DIR}/e2e_${RUN_ID}.serial.log"
LATEST_LOG="${LOG_DIR}/e2e_latest.log"
LATEST_SERIAL_LOG="${LOG_DIR}/e2e_latest.serial.log"

echo "[e2e.sh] writing log: ${LOG_FILE}"
echo "[e2e.sh] serial log: ${SERIAL_LOG}"
ln -sfn "${LOG_FILE}" "${LATEST_LOG}"
ln -sfn "${SERIAL_LOG}" "${LATEST_SERIAL_LOG}"
status=0
E2E_RUN_ID="${RUN_ID}" python3 -u "${ROOT_DIR}/tools/e2e/test_rs485_stub.py" 2>&1 | tee "${LOG_FILE}" || status=$?
exit "${status}"
