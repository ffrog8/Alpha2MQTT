# Testing / Build (Alpha2MQTT)

This repo uses two distinct build targets:

- **Host unit tests (doctest)**: validates pure C++ logic on a desktop toolchain.
- **Firmware build (ESP8266)**: produces `.bin` files for OTA/flash.

## Host unit tests (recommended during refactors)

Run via the existing docker wrapper:

`./scripts/test_host_docker.sh`

Notes:
- `./scripts/test_host.sh` runs the same tests directly on the host, but requires `cmake` locally.

## Firmware build (ESP8266)

The firmware is built inside the `arduino-cli-build` container:

`docker exec -i arduino-cli-build /project/Alpha2MQTT/build.sh`

Outputs:
- `Alpha2MQTT/build/firmware/Alpha2MQTT_latest_real.txt` → latest real backend artifact
- `Alpha2MQTT/build/firmware/Alpha2MQTT_latest_stub.txt` → latest stub backend artifact

## E2E (RS485 stub)

The E2E runner is pure Python3 and reads its defaults from:
- `tools/e2e/e2e.local.env` (gitignored)

Template:
- `tools/e2e/e2e.example.env`

Run:

`python3 tools/e2e/test_rs485_stub.py --ensure-stub`

