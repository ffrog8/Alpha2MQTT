# Agent Instructions — alpha2mqtt

This file defines repository-specific AI / Codex guidance.
If conflicts arise, **this file is the source of truth** for this repo.

## Scope
- Repository: alpha2mqtt
- Firmware toolchain: Arduino CLI–based (ESP-class targets)

## Project map
- `Alpha2MQTT/` — main firmware sketch, headers, and PlatformIO project
- `Alpha2MQTT/build.sh` — Arduino CLI build script (ESP8266)
- `Alpha2MQTT/legacy/` — legacy Arduino sketch
- `AlphaSniffer/` — RS485 sniffer sketch
- `Home Assistant/` — HA examples and dashboards
- `Node Red Flows/` — Node-RED examples
- `README.md` / `README-orig.md` — behavior and setup guidance

## General principles
- Do not claim builds, tests, or verification unless they actually ran.
- Explicitly label outcomes as:
  - Executed here
  - Deferred to CI
  - Not executable due to environment constraints
- If a blocking constraint is encountered, stop and report it clearly.

## Arduino / arduino-cli usage policy

### Installation timing
- Check for `arduino-cli` availability up front.
- If it is missing, stop and report an environment error; do not attempt installation here.
- Treat `arduino-cli` as a prerequisite provided by the environment or setup scripts.

### Configuration file (`arduino-cli.yaml`)
- The file does not need to exist until `arduino-cli` is actually used.
- It may be created or overwritten at test/build time.

### Arduino config location rule (non-negotiable)
If `ARDUINO_DATA_DIR` is set, all `arduino-cli config ...` commands MUST run with the same
`ARDUINO_DATA_DIR`/`ARDUINO_SKETCHBOOK_DIR` environment, so the proxy is written to the
active config file (e.g. `/opt/arduino/arduino15/arduino-cli.yaml`), not `~/.arduino15`.

### Proxy verification
After setting the proxy, the agent MUST prove it is active by running:
`arduino-cli config dump` with `ARDUINO_DATA_DIR` set, and showing `network.proxy`.

### Cache locations
When running Arduino-related commands, prefer explicit cache locations:
- `ARDUINO_DATA_DIR=/opt/arduino/arduino15`
- `ARDUINO_SKETCHBOOK_DIR=/opt/arduino/sketchbook`

These are conventions, not guarantees; adjust only if the environment requires it.

### Indexes, cores, and tools
- Do not assume indexes, cores, or toolchains are preinstalled.
- Allow `arduino-cli` to download what it needs at build time.
- Index seeding via `curl` is acceptable as an optimisation, not a requirement.
- If downloads fail due to network constraints, report and defer.

## Expected Arduino build behavior
When an Arduino build or test is requested, or when the agent changes source code, the agent should attempt, in order:
1. Ensure `arduino-cli` is available (if missing, follow Installation timing policy).
2. Initialize or update configuration as needed (including proxy).
3. Install required cores and libraries.
4. Compile and report results using the same targets and options as `.github/workflows/arduino-build.yml`.

If any step cannot run due to environment constraints, stop and explain why.

## Canonical build/test command
- Default build command: `Alpha2MQTT/build.sh` (Arduino CLI ESP8266 builds).
- If no local build ran, state “Not executed” and why, then reference the command above.

## Containerized execution (environment-specific)
If your environment runs builds inside a long-lived container, run the canonical build command inside that container to avoid repeated setup costs.

Guidance:
- Use the repo’s canonical command (`Alpha2MQTT/build.sh`); only the execution substrate changes.
- Prefer reusing an existing long-lived container over creating new ephemeral containers.
- Document the exact container name and exec command outside the repo (in your environment notes), to keep repo guidance tool-agnostic.

## Build script parity
- Keep `Alpha2MQTT/build.sh` aligned with `.github/workflows/arduino-build.yml` for core versions and library lists.
- If one changes, update the other in the same change set to prevent drift.

## CI relationship
- Local runs provide provisional feedback only.
- GitHub CI is the final authority for correctness.
- Work must not be labeled “verified” unless CI passes.

## Canonical Verification Checklist (non-sensitive)
When a change affects firmware behavior, prefer this minimal verification set:
- Host unit tests: `./scripts/test_host.sh`
- Firmware build: `Alpha2MQTT/build.sh`
- Normal-mode HTTP smoke: `GET /` should return 200; config-only pages like `/update` should be unavailable outside portals.
- MQTT smoke (if MQTT is enabled in MODE_NORMAL):
  - Confirm a retained boot/presence message is published under the device root topic (e.g., `<device>/boot`, `<device>/status`).
  - Confirm periodic status messages publish at the expected cadence (topics may include `<device>/status/*`).

If any item is not run, explicitly state “Not executed” and why.

### RS485 probing liveness (headless, no inverter required)
If RS485/inverter is offline, the firmware should continue background probing without blocking NORMAL-mode services.
Verify probing is still active via MQTT using `status/poll` uptime fields:
- `rs485_probe_last_attempt_ms` (uptime millis at last probe attempt)
- `rs485_probe_backoff_ms` (current backoff delay; capped in firmware, 0 when connected)

How to check:
- Read `uptime_s` from `<device>/status/net` and compute `uptime_ms = uptime_s * 1000`.
- Compute `age_ms = uptime_ms - rs485_probe_last_attempt_ms`.
- Probing is considered active when `age_ms <= rs485_probe_backoff_ms + slack_ms` (use a generous slack, e.g. 20000ms, for publish cadence and jitter).

Notes:
- With no inverter connected, `poll_ok_count` may remain 0; treat `rs485_probe_*` fields as the liveness signal.

## Local Developer Operations (not committed)
This repository is public. Do not add environment-specific or secret-bearing content to tracked files:
- No IP addresses, hostnames, ports, or Wi-Fi SSIDs.
- No MQTT credentials, tokens, passfiles, or `.env` contents.
- No device-specific OTA URLs or curl commands tied to a private network.

Instead, maintain a local-only notes file and keep it out of git:
- Suggested filename: `LOCAL_DEV_NOTES.md` (ignored by `.gitignore`)
- Contents may include: device IP(s), broker address, local helper commands/scripts, and deployment steps.
