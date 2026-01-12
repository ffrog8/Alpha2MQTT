# Agent Instructions — alpha2mqtt

This file defines repository-specific AI / Codex guidance.
If conflicts arise, **this file is the source of truth** for this repo.

## Scope
- Repository: alpha2mqtt
- Firmware toolchain: Arduino CLI–based (ESP-class targets)

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

## Build script parity
- Keep `Alpha2MQTT/build.sh` aligned with `.github/workflows/arduino-build.yml` for core versions and library lists.
- If one changes, update the other in the same change set to prevent drift.

## CI relationship
- Local runs provide provisional feedback only.
- GitHub CI is the final authority for correctness.
- Work must not be labeled “verified” unless CI passes.
