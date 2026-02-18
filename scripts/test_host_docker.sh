#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_NAME="alpha2mqtt-host-tests"
LOG_DIR="${ROOT_DIR}/build/test_logs"
mkdir -p "${LOG_DIR}"
LOG_FILE="${LOG_DIR}/host_tests_$(date +%Y%m%d_%H%M%S).log"

DOCKERFILE_PATH="${ROOT_DIR}/scripts/Dockerfile.test"

echo "[test] Building host-test image: ${IMAGE_NAME}"
docker build -f "${DOCKERFILE_PATH}" -t "${IMAGE_NAME}" "${ROOT_DIR}"
echo "[test] Running host unit tests (summary)"
set +e
set +o pipefail
tail -f /dev/null | docker run --rm -i --log-driver json-file --log-opt max-size=20m --log-opt max-file=5 "${IMAGE_NAME}" bash -lc "set -x; ./scripts/test_host.sh" 2>&1 | tee "${LOG_FILE}"
docker_status=${PIPESTATUS[1]}
set -e
set -o pipefail

if [[ "${docker_status}" -ne 0 ]]; then
	echo "[test] Host tests failed; capturing detailed failure output."
	set +e
	tail -f /dev/null | docker run --rm -i --log-driver json-file --log-opt max-size=20m --log-opt max-file=5 "${IMAGE_NAME}" bash -lc "set -x; ./scripts/test_host.sh --console" >> "${LOG_FILE}" 2>&1
	set -e
	echo "[test] Detailed host test log: ${LOG_FILE}"
fi

if [[ "${docker_status}" -eq 0 ]]; then
	echo "[test] Host test log: ${LOG_FILE}"
fi

exit "${docker_status}"
