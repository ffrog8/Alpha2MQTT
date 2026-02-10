#!/bin/bash
#  export GIT_BASE=/volume1/docker_cached/apps/vscode/git/Alpha2MQTT
#  docker run -d --device=/dev/ttyUSB0 --name arduino-cli-build -v  ${GIT_BASE}:/project  --log-driver json-file --log-opt max-size=20m --log-opt max-file=5   alpha2mqtt/arduino-cli:1.3.1-serial 
#  (only if needed) chmod -R a+rwX $GIT_BASE/Alpha2MQTT/build/
#  docker exec -it arduino-cli-build /project/Alpha2MQTT/build.sh

set -euo pipefail

if [[ "${BUILD_TRACE:-0}" == "1" ]]; then
	set -x
fi

cd /project

BUILD_TS_MS="$(date +%s%3N)"
BASE_BUILD_FLAGS="-DMP_ESP8266 -UMP_ESP32 -UMP_XIAO_ESP32C6 -DBUILD_TS_MS=${BUILD_TS_MS}ULL"
OUTDIR="Alpha2MQTT/build/firmware"
OUTFILE_REAL="${OUTDIR}/Alpha2MQTT_${BUILD_TS_MS}_real.bin"
OUTFILE_STUB="${OUTDIR}/Alpha2MQTT_${BUILD_TS_MS}_stub.bin"
TOOLCHAIN_NM="$(find /root/.arduino15 -name xtensa-lx106-elf-nm -print -quit || true)"
TOOLCHAIN_BIN=""
if [[ -n "${TOOLCHAIN_NM}" ]]; then
	TOOLCHAIN_BIN="$(dirname "${TOOLCHAIN_NM}")"
fi

mkdir -p "${OUTDIR}"

compile_one() {
	local label="$1"
	local extra_flags="$2"
	local outfile="$3"
	local build_dir="Alpha2MQTT/build/esp8266.esp8266.d1_mini"
	local elf_path="${build_dir}/Alpha2MQTT.ino.elf"
	local map_path="${build_dir}/Alpha2MQTT.ino.map"
	local elf_flags="-Wl,-Map=${map_path}"
	local out_elf="${OUTDIR}/Alpha2MQTT_${BUILD_TS_MS}_${label}.elf"
	local out_map="${OUTDIR}/Alpha2MQTT_${BUILD_TS_MS}_${label}.map"
	local out_report="${OUTDIR}/Alpha2MQTT_${BUILD_TS_MS}_${label}_ram_symbols.txt"

	echo "[build] Building ${label}..."
	echo "[build] build.extra_flags=${extra_flags}"

	if [[ "${BUILD_VERBOSE:-0}" == "1" ]]; then
		arduino-cli compile -v -e --build-property build.extra_flags="${extra_flags}" --build-property compiler.c.elf.extra_flags="${elf_flags}" --fqbn esp8266:esp8266:d1_mini Alpha2MQTT
	else
		arduino-cli compile -e --build-property build.extra_flags="${extra_flags}" --build-property compiler.c.elf.extra_flags="${elf_flags}" --fqbn esp8266:esp8266:d1_mini Alpha2MQTT
	fi

	mv Alpha2MQTT/build/esp8266.esp8266.d1_mini/Alpha2MQTT.ino.bin "${outfile}"
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

	if [[ -f "${elf_path}" && -n "${TOOLCHAIN_BIN}" ]]; then
		{
			echo "[ram] totals (bytes)"
			"${TOOLCHAIN_BIN}/xtensa-lx106-elf-size" -A -d "${elf_path}" | awk '$1 == ".data" || $1 == ".bss" { print }'
			echo
			echo "[ram] top 30 .bss symbols"
			"${TOOLCHAIN_BIN}/xtensa-lx106-elf-nm" -S --size-sort -t d "${elf_path}" | awk '$3 ~ /[bB]/ { print }' | tail -n 30
			echo
			echo "[ram] top 30 .data symbols"
			"${TOOLCHAIN_BIN}/xtensa-lx106-elf-nm" -S --size-sort -t d "${elf_path}" | awk '$3 ~ /[dD]/ { print }' | tail -n 30
		} > "${out_report}"
		echo "[build] Wrote ${out_report}"
		cat "${out_report}"
	elif [[ -z "${TOOLCHAIN_BIN}" ]]; then
		echo "[build] WARNING: xtensa toolchain not found for RAM symbol report."
	fi
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
