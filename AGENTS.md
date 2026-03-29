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
- `README.md` — behavior and setup guidance

## General principles
- Do not claim builds, tests, or verification unless they actually ran.
- Explicitly label outcomes as:
  - Executed here
  - Deferred to CI
  - Not executable due to environment constraints
- If a blocking constraint is encountered, stop and report it clearly.

## Boot-loop / unreachable-device policy (OTA safety)
If the device is in a boot loop or is not stably reachable, do not keep iterating on OTA/E2E as if it can self-recover. First attempt serial-based diagnosis and any agent-executable rectification that follows from that analysis. Only stop after the serial-analysis path is unavailable or has been attempted and the device is still not stably reachable.

Definition (any of the following):
- Repeated resets observed on serial during NORMAL boot (e.g., soft WDT resets / exceptions).
- HTTP endpoints required for OTA (`/reboot/*`, `/u`, portal `/update`) consistently time out.
- MQTT connectivity is intermittent or absent (no stable `<device>/boot` / status publishes).

Required behavior for the agent:
- If serial access or serial logs are available, inspect them first:
  - capture/reset logs where possible
  - decode ESP8266 exception stacks using the panic-stack playbook below when applicable
  - use that analysis to attempt the narrowest safe fix or recovery action the agent can execute
- If serial access is not available, say so explicitly before stopping.
- Clearly state: “Device is not stably reachable; OTA/E2E cannot proceed.” only after serial analysis is unavailable or has been attempted without restoring stability.
- Ask the user to manually reflash or otherwise recover the device to a stable baseline (outside the agent) only after that serial-analysis attempt path is exhausted.
- Only resume OTA/E2E once the device remains up long enough to serve HTTP (for OTA) and/or publish MQTT status consistently.

Notes (why this policy exists):
- OTA flashing requires a stable runtime window to accept HTTP requests; a boot loop removes that window.
- Serial evidence is the highest-signal path for deciding whether a software fix is possible without blind reflashing.
- In this state, further “try another OTA” actions are counterproductive unless serial analysis has produced a concrete reason to do so; otherwise the correct next step is manual recovery (USB serial flash / physical intervention).


## OOM prevention policy (all firmware changes)

ESP8266 heap is tight; any change can trigger OOM even if it “looks small”.
Therefore, **any firmware change must avoid increasing steady‑state RAM usage**
or transient peak allocations without explicit justification.

Rules:
- Do not introduce new large static/global buffers or persistent objects.
- Avoid heap allocations in hot paths and config/persistence paths; prefer bounded stack or pre‑allocated scratch.
- If you must allocate dynamically, reserve once and reuse; do not grow Strings repeatedly.
- Any added buffers/arrays must have a hard upper bound and be sized conservatively.
- Without impacting performance ensure debug level of free and fragmentation are displayed perioidcally over serial

Verification (required after any firmware change):
- Confirm free heap does not regress at key points (boot, after Wi‑Fi, after MQTT).
- If free heap regresses or fragmentation increases, treat it as a regression to fix
  before proceeding—do not “accept” it as normal.

If you can’t verify heap impact, explicitly state “Not verified” and avoid claiming stability.

## Memory health reporting (required for core changes)
For any change that touches networking, webserver, MQTT, RS485, scheduling, or entity metadata:
- Include the toolchain “RAM used … / 80192” and “IRAM used … / 65536” lines in your summary.
- Capture `/status` memory health fields (heap free/max block/frag and boot/runtime levels) after boot.
- Do not introduce new large globals or static buffers without justification; prefer flash/PROGMEM.
These checks are required to prevent regressions like recent boot-time OOM failures.


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

## E2E run-control policy
- Use `tools/e2e/e2e.local.json` as the default and preferred control surface for E2E run behavior (case selection, verbosity, tracing, flash policy).
- Keep the E2E command stable (`scripts/e2e.sh`) and do not pass ad-hoc CLI arguments unless a task explicitly requires them.
- For focused diagnosis, change `e2e.local.json` `run` settings (`from_case`, `cases`, `trace_http`, `trace_mqtt`, etc.) instead of changing command-line flags.
- Persist E2E logs under `tools/e2e/logs/` so failures can be reviewed later without rerunning.

## ESP8266 panic stack decode playbook (required)
When a user provides an ESP8266 panic/exception stack, decode it before proposing fixes.
Do not rely on guesses from panic type alone.

Required procedure:
1. Extract firmware build timestamp from serial log:
   - `Firmware build ts: <ts>`
2. Select the matching ELF:
   - `/home/coder/git/Alpha2MQTT/Alpha2MQTT/build/firmware/Alpha2MQTT_<ts>_real.elf`
3. Decode inside the build container (`arduino-cli-build`) using the ESP8266 toolchain path:
   - `tail -f /dev/null | docker exec -i arduino-cli-build bash -lc "/root/.arduino15/packages/esp8266/tools/xtensa-lx106-elf-gcc/3.1.0-gcc10.3-e5f9fec/bin/xtensa-lx106-elf-addr2line -e /project/Alpha2MQTT/build/firmware/Alpha2MQTT_<ts>_real.elf -f -C <pc1> <pc2> ..."`
4. If that binary path changes, discover it in-container, then rerun decode:
   - `tail -f /dev/null | docker exec -i arduino-cli-build bash -lc "find /root/.arduino15 -maxdepth 8 -type f | grep 'xtensa-lx106-elf-addr2line'"`
5. Map decoded frames to exact source lines and report the concrete call chain.

Reporting requirements for panic analysis:
- Include the build timestamp used for decoding.
- Include the ELF path used.
- Include decoded function + file:line for each relevant PC.
- Explicitly label any unresolved frame as unresolved (do not invent a location).

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

## Preferences (NVS) non-volatile storage

- **Preferences / NVS policy**
  - `getString()` **must use buffer overloads only**; String-returning Preferences APIs are forbidden.
  - All `put*()` / `remove()` calls **must occur only** inside:
    - `persist_user_*()` (explicit user “Save / Apply”), or
    - `persist_defaults_if_missing()` (one-time default seeding).
  - **No NVS writes** from reconnect loops, polling loops, telemetry paths, background tasks, or periodic timers.
  - `Preferences::begin()` / `end()` **must be paired in the same scope**; reads must use RO mode where supported.
  - New string keys **must define explicit max lengths** and use bounded buffers; truncation is acceptable, overflow is not.
  - Preferences are a **configuration store**, not a runtime state database.


## Local Developer Operations (not committed)
This repository is public. Do not add environment-specific or secret-bearing content to tracked files:
- No IP addresses, hostnames, ports, or Wi-Fi SSIDs.
- No MQTT credentials, tokens, passfiles, or `.env` contents.
- No device-specific OTA URLs or curl commands tied to a private network.

Instead, maintain a local-only notes file and keep it out of git:
- Suggested filename: `LOCAL_DEV_NOTES.md` (ignored by `.gitignore`)
- Contents may include: device IP(s), broker address, local helper commands/scripts, and deployment steps.
