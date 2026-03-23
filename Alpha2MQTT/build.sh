#!/bin/bash
#  export GIT_BASE=/volume1/docker_cached/apps/vscode/git/Alpha2MQTT
#  docker run -d --device=/dev/ttyUSB0 --name arduino-cli-build -v  ${GIT_BASE}:/project  --log-driver json-file --log-opt max-size=20m --log-opt max-file=5   alpha2mqtt/arduino-cli:1.3.1-serial 
#  (only if needed) chmod -R a+rwX $GIT_BASE/Alpha2MQTT/build/
#  docker exec -it arduino-cli-build /project/Alpha2MQTT/build.sh

set -euo pipefail

if [[ "${BUILD_TRACE:-0}" == "1" ]]; then
	set -x
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "${SCRIPT_DIR}/platformio.ini" && -f "${SCRIPT_DIR}/Alpha2MQTT.ino" ]]; then
	REPO_ROOT="${SCRIPT_DIR}"
elif [[ -f /project/Alpha2MQTT/Alpha2MQTT.ino ]]; then
	REPO_ROOT="/project/Alpha2MQTT"
elif [[ -f /project/Alpha2MQTT/Alpha2MQTT/Alpha2MQTT.ino ]]; then
	REPO_ROOT="/project/Alpha2MQTT/Alpha2MQTT"
elif [[ -f "${SCRIPT_DIR}/platformio.ini" ]]; then
	REPO_ROOT="${SCRIPT_DIR}"
else
	REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
fi

cd "${REPO_ROOT}"
if [[ -f "${REPO_ROOT}/Alpha2MQTT.ino" ]]; then
	SKETCH_DIR="${REPO_ROOT}"
	OUTDIR="${REPO_ROOT}/build/firmware"
elif [[ -f "${REPO_ROOT}/Alpha2MQTT/Alpha2MQTT.ino" ]]; then
	SKETCH_DIR="${REPO_ROOT}/Alpha2MQTT"
	OUTDIR="${REPO_ROOT}/Alpha2MQTT/build/firmware"
else
	echo "[build] ERROR: unable to locate Alpha2MQTT.ino" >&2
	exit 1
fi

BUILD_TS_MS="$(date +%s%3N)"
BASE_BUILD_FLAGS="-DMP_ESP8266 -UMP_ESP32 -UMP_XIAO_ESP32C6 -DBUILD_TS_MS=${BUILD_TS_MS}ULL"
OUTFILE_REAL="${OUTDIR}/Alpha2MQTT_${BUILD_TS_MS}_real.bin"
OUTFILE_STUB="${OUTDIR}/Alpha2MQTT_${BUILD_TS_MS}_stub.bin"
ESP8266_INDEX_URL="https://arduino.esp8266.com/stable/package_esp8266com_index.json"
ESP8266_FQBN="esp8266:esp8266:d1_mini"
BUILD_SUBDIR="esp8266.esp8266.d1_mini"

mkdir -p "${OUTDIR}"

ensure_arduino_bootstrap() {
	arduino-cli core update-index --additional-urls "${ESP8266_INDEX_URL}"
	arduino-cli core install esp8266:esp8266@3.1.2 --additional-urls "${ESP8266_INDEX_URL}"
	arduino-cli lib install "Adafruit BusIO"
	arduino-cli lib install "Adafruit SSD1306"
	arduino-cli lib install "Adafruit GFX Library"
	arduino-cli lib install "WiFiManager"
	arduino-cli lib install "Preferences"
	arduino-cli lib install PubSubClient
}

find_toolchain_bin() {
	local toolchain_nm
	toolchain_nm="$(find /root/.arduino15 -name xtensa-lx106-elf-nm -print -quit || true)"
	if [[ -n "${toolchain_nm}" ]]; then
		dirname "${toolchain_nm}"
	fi
}

compile_one() {
	local label="$1"
	local extra_flags="$2"
	local outfile="$3"
	local build_dir="${SKETCH_DIR}/build/${BUILD_SUBDIR}"
	local elf_path="${build_dir}/Alpha2MQTT.ino.elf"
	local map_path="${build_dir}/Alpha2MQTT.ino.map"
	local elf_flags="-Wl,-Map=${map_path}"
	local out_elf="${OUTDIR}/Alpha2MQTT_${BUILD_TS_MS}_${label}.elf"
	local out_map="${OUTDIR}/Alpha2MQTT_${BUILD_TS_MS}_${label}.map"
	local out_report="${OUTDIR}/Alpha2MQTT_${BUILD_TS_MS}_${label}_ram_symbols.txt"

	echo "[build] Building ${label}..."
	echo "[build] build.extra_flags=${extra_flags}"

	if [[ "${BUILD_VERBOSE:-0}" == "1" ]]; then
		arduino-cli compile -v -e --build-property build.extra_flags="${extra_flags}" --build-property compiler.c.elf.extra_flags="${elf_flags}" --fqbn "${ESP8266_FQBN}" "${SKETCH_DIR}"
	else
		arduino-cli compile -e --build-property build.extra_flags="${extra_flags}" --build-property compiler.c.elf.extra_flags="${elf_flags}" --fqbn "${ESP8266_FQBN}" "${SKETCH_DIR}"
	fi

	local toolchain_bin=""
	toolchain_bin="$(find_toolchain_bin)"

	mv "${build_dir}/Alpha2MQTT.ino.bin" "${outfile}"
	echo "[build] Wrote ${outfile} ($(wc -c < "${outfile}") bytes)"

	if [[ -f "${elf_path}" ]]; then
		cp "${elf_path}" "${out_elf}"
		echo "[build] Wrote ${out_elf}"
	else
		echo "[build] WARNING: ELF not found at ${elf_path}"
	fi

	if [[ -f "${map_path}" ]]; then
		cp "${map_path}" "${out_map}"
		echo "[build] Wrote ${out_map}"
	else
		echo "[build] WARNING: map file not found at ${map_path}"
	fi

	if [[ -f "${elf_path}" && -n "${toolchain_bin}" ]]; then
		{
			echo "[ram] totals (bytes)"
			"${toolchain_bin}/xtensa-lx106-elf-size" -A -d "${elf_path}" | awk '$1 == ".data" || $1 == ".bss" { print }'
			echo
			echo "[ram] top 30 .bss symbols"
			"${toolchain_bin}/xtensa-lx106-elf-nm" -S --size-sort -t d "${elf_path}" | awk '$3 ~ /[bB]/ { print }' | tail -n 30
			echo
			echo "[ram] top 30 .data symbols"
			"${toolchain_bin}/xtensa-lx106-elf-nm" -S --size-sort -t d "${elf_path}" | awk '$3 ~ /[dD]/ { print }' | tail -n 30
		} > "${out_report}"
		echo "[build] Wrote ${out_report}"
		cat "${out_report}"
	elif [[ -z "${toolchain_bin}" ]]; then
		echo "[build] WARNING: xtensa toolchain not found for RAM symbol report."
	fi
}

ensure_arduino_bootstrap

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

prune_variant_artifacts() {
	local variant="$1"
	mapfile -t keep_ts < <(
		find "${OUTDIR}" -maxdepth 1 -type f -name "Alpha2MQTT_*_${variant}.bin" -printf '%f\n' \
			| sed -E "s/^Alpha2MQTT_([0-9]+)_${variant}\\.bin$/\\1/" \
			| sort -r \
			| head -n 5
	)

	find "${OUTDIR}" -maxdepth 1 -type f \
		\( -name "Alpha2MQTT_*_${variant}.bin" -o \
		   -name "Alpha2MQTT_*_${variant}.elf" -o \
		   -name "Alpha2MQTT_*_${variant}.map" -o \
		   -name "Alpha2MQTT_*_${variant}_ram_symbols.txt" \) \
		-print0 | while IFS= read -r -d '' path; do
			local file base ts keep=0
			file="$(basename "${path}")"
			base="${file%.*}"
			ts="${base#Alpha2MQTT_}"
			ts="${ts%%_*}"
			for kept in "${keep_ts[@]}"; do
				if [[ "${ts}" == "${kept}" ]]; then
					keep=1
					break
				fi
			done
			if [[ "${keep}" -eq 0 ]]; then
				rm -f "${path}"
			fi
		done
}

# Keep only the latest 5 firmware versions per variant, including companion
# ELF/map/RAM-report artifacts.
prune_variant_artifacts "real"
prune_variant_artifacts "stub"

echo "Firmware (real): ${OUTFILE_REAL}"
echo "Firmware (stub): ${OUTFILE_STUB}"
