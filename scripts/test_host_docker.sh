#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_NAME="alpha2mqtt-host-tests"

DOCKERFILE_PATH="${ROOT_DIR}/scripts/Dockerfile.test"

echo "[test] Building host-test image: ${IMAGE_NAME}"
docker build -f "${DOCKERFILE_PATH}" -t "${IMAGE_NAME}" "${ROOT_DIR}"
echo "[test] Running host unit tests"
set +e
set +o pipefail
tail -f /dev/null | docker run --rm -i --log-driver json-file --log-opt max-size=20m --log-opt max-file=5 "${IMAGE_NAME}" bash -lc "set -x; ./scripts/test_host.sh"
docker_status=${PIPESTATUS[1]}
set -e
set -o pipefail
exit "${docker_status}"
