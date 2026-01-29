# Changes

## 2026-01-08
- Add host-based doctest unit tests with CMake and scripts (including a Docker wrapper).
- Add helper modules for scheduling, boot modes, config serialization, and Modbus framing/CRC.
- Add a GitHub Actions workflow for host tests plus an ESP8266 Arduino CLI smoke build.

## 2026-01-07
- Add persisted per-entity polling intervals with MQTT config topics and Home Assistant discovery visibility.
- Add freqDisabled for user-controlled discovery removal; reserve freqNever for legacy defaults.

## 2025-01-05
- Merge upstream BMorgan1296/Alpha2MQTT master into the fork (robustness updates).
- Preserve legacy Arduino sketch support and second-by-second status schedule.

## 2025-01-05
- Use Secrets.h defaults for captive-portal configuration when no stored settings exist.

## 2025-01-05
- Add GitHub Actions Arduino CLI build workflow for ESP8266 targets.

## 2025-01-05
- Remove binary assets added during upstream merge to keep change set text-only.

## 2025-01-05
- Add Arduino header shims so CI builds find the moved headers.

## 2025-01-05
- Surface Arduino CLI compile failures in PR comments and upload compile logs as artifacts.

## 2025-01-05
- Add gh bootstrap helper for CI log polling.

## 2026-01-06
- Align handler includes with Arduino CLI sketch-root wrapper headers and map ESP8266/ESP32 defines.

## 2026-01-06
- Move legacy Arduino sketch out of the build path so Arduino CLI targets the src implementation.

## 2026-01-06
- Install WiFiManager and Preferences in Arduino CI to fix compile failures.

## 2026-01-06
- Align ESP8266 disconnect usage and CI core version with local builds.
