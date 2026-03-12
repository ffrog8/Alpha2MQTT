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

Use the wrapper script:

`./scripts/e2e.sh`

Preferred run control is:
- `tools/e2e/e2e.local.json` (gitignored)

Legacy env-file control is still supported:
- `tools/e2e/e2e.local.env` (gitignored)
- template: `tools/e2e/e2e.example.env`

The JSON control file is the preferred place to select cases, tracing, and flash behavior without changing the command itself.
