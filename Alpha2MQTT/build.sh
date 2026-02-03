#!/bin/bash
#  export GIT_BASE=/volume1/docker_cached/apps/vscode/git/Alpha2MQTT
#  docker run -d --name arduino-cli-build -v  ${GIT_BASE}:/project --entrypoint tail arduinoci/ci-arduino-cli:v1.3.1 -f /dev/null
#  chmod -R a+rwX $GIT_BASE/Alpha2MQTT/build/
#  docker exec -it arduino-cli-build /project/Alpha2MQTT/build.sh

set -euo pipefail

if [[ "${BUILD_TRACE:-0}" == "1" ]]; then
	set -x
fi

echo "[build] Updating Arduino core + libs..."
arduino-cli core update-index --additional-urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli core install esp8266:esp8266@3.1.2  --additional-urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli lib install "Adafruit BusIO"
arduino-cli lib install "Adafruit SSD1306"
arduino-cli lib install "Adafruit GFX Library"
arduino-cli lib install "WiFiManager"
arduino-cli lib install "Preferences"
arduino-cli lib install PubSubClient

cd /project

BUILD_TS_MS="$(date +%s%3N)"
BASE_BUILD_FLAGS="-DMP_ESP8266 -UMP_ESP32 -UMP_XIAO_ESP32C6 -DBUILD_TS_MS=${BUILD_TS_MS}ULL"
OUTDIR="Alpha2MQTT/build/firmware"
OUTFILE_REAL="${OUTDIR}/Alpha2MQTT_${BUILD_TS_MS}_real.bin"
OUTFILE_STUB="${OUTDIR}/Alpha2MQTT_${BUILD_TS_MS}_stub.bin"

mkdir -p "${OUTDIR}"

compile_one() {
	local label="$1"
	local extra_flags="$2"
	local outfile="$3"

	echo "[build] Building ${label}..."
	echo "[build] build.extra_flags=${extra_flags}"

	if [[ "${BUILD_VERBOSE:-0}" == "1" ]]; then
		arduino-cli compile -v -e --build-property build.extra_flags="${extra_flags}" --fqbn esp8266:esp8266:d1_mini Alpha2MQTT
	else
		arduino-cli compile -e --build-property build.extra_flags="${extra_flags}" --fqbn esp8266:esp8266:d1_mini Alpha2MQTT
	fi

	mv Alpha2MQTT/build/esp8266.esp8266.d1_mini/Alpha2MQTT.ino.bin "${outfile}"
	echo "[build] Wrote ${outfile} ($(wc -c < "${outfile}") bytes)"
}

# Build the real RS485/Modbus backend firmware.
compile_one "real" "${BASE_BUILD_FLAGS}" "${OUTFILE_REAL}"

# Build the RS485/Modbus stub backend firmware for E2E testing without inverter hardware.
STUB_FLAGS="${BASE_BUILD_FLAGS} -DRS485_STUB=1"
if [[ -n "${RS485_STUB_MODE:-}" ]]; then
	STUB_FLAGS="${STUB_FLAGS} -DRS485_STUB_MODE=${RS485_STUB_MODE}"
fi
if [[ -n "${RS485_STUB_FAIL_FIRST_N:-}" ]]; then
	STUB_FLAGS="${STUB_FLAGS} -DRS485_STUB_FAIL_FIRST_N=${RS485_STUB_FAIL_FIRST_N}"
fi
if [[ -n "${RS485_STUB_FAIL_REGISTER:-}" ]]; then
	STUB_FLAGS="${STUB_FLAGS} -DRS485_STUB_FAIL_REGISTER=${RS485_STUB_FAIL_REGISTER}"
fi

compile_one "stub" "${STUB_FLAGS}" "${OUTFILE_STUB}"

rm -f ./Alpha2MQTT.bin

printf '%s\n' "$(basename "${OUTFILE_REAL}")" > "${OUTDIR}/Alpha2MQTT_latest_real.txt"
printf '%s\n' "$(basename "${OUTFILE_STUB}")" > "${OUTDIR}/Alpha2MQTT_latest_stub.txt"
# Backwards-compatible pointer: keep Alpha2MQTT_latest.txt as the real backend firmware.
printf '%s\n' "$(basename "${OUTFILE_REAL}")" > "${OUTDIR}/Alpha2MQTT_latest.txt"

# Keep only the latest 3 artifacts per variant.
ls -1t "${OUTDIR}"/Alpha2MQTT_*_real.bin 2>/dev/null | tail -n +4 | xargs -r rm -f
ls -1t "${OUTDIR}"/Alpha2MQTT_*_stub.bin 2>/dev/null | tail -n +4 | xargs -r rm -f

echo "Firmware (real): ${OUTFILE_REAL}"
echo "Firmware (stub): ${OUTFILE_STUB}"
