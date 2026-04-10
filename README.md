# Alpha2MQTT (A2M)
## A Home Assistant controller for AlphaESS solar and battery inverters.

_**Credit!** This project is entirely derived from <https://github.com/dxoverdy/Alpha2MQTT>, which is a really great bit of work. My hardware includes some tweaks and modernizations on that project's hardware. My software has changed a lot from that project, but at its core, this project is derived from that project and would not exist without that project. So huge credit and thanks._

**What is this?** This project is a controller (a small hardware device) that connects Home Assistant to an AlphaESS system via RS485 and uses MQTT discovery to set up a ready to go integration.  This is local control with NO cloud dependancy.  Once you plug-in and turn on this A2M hardware, your ESS will appear as an inverter-focused HA/MQTT device with HA entities ready to monitor and [control](#controlling-your-ess) your AlphaESS system, and the controller may also expose a small companion diagnostic device for WiFi/MQTT/RS485 health.  No HA configuration is needed.  The HA device and HA entities are ready to be used with all units, classes, icons, unique IDs, and more and provides up-to-date availability status of each entity.   Entities are ready to be used by the Energy dashboard and can be included in other [dashboards](#dashboard-example) and [automations](#automation-examples).  The firmware also provides a direct MQTT dispatch request interface so that ESS control can be done with one compact payload rather than a series of loosely-related control writes.

**What's Different?** The big change is that rather than being a general purpose interface between MQTT and the raw AlphaESS system data, this project is a controller specifically designed to work as a Home Assistant integration for your AlphaESS system.  It uses MQTT discovery to be plug-and-play so that no HA configuration is needed.  And it adds some "smart", real-time capabilities in the controller so that HA automation control is simplified.  This also incorporates several newer versions of the [AlphaESS specs](#alphaess-specs), and has enhancements/tweaks/fixes to WiFi and RS485 functionality.

The configuration (WiFi and MQTT settings) of this hardware itself is now done through built-in web portals.  It no longer requires hard coding these values in `Definitions.h`.  On first boot or recovery the device can create its own captive AP portal; once it is on your LAN it can also serve the same configuration pages through a WiFi portal.  The configuration only needs to be done once, and then the values are saved in flash.  See the [configuration](#configuring-wifi-and-mqtt) section for details.

![Alpha2MQTT](Pics/Normal.jpg)

## Features
- Home Assistant MQTT discovery with inverter-focused entities ready to use as soon as the controller joins WiFi and MQTT.
- Two MQTT/HA devices with distinct roles: the main inverter-focused device carries ESS telemetry and control, while a small controller-side diagnostic device exposes WiFi, MQTT, RS485, and polling health so controller problems are visible without cluttering the inverter entity surface.
- A direct atomic dispatch request API (`alpha2mqtt_inv_<serial>/dispatch/set`) for ESS control, plus `Dispatch *` readback entities that show what the inverter is actually doing.
- Three operating modes with distinct purposes:
  - `normal` runtime for MQTT, RS485 polling, ESS control, and the lightweight HTTP status/control page.
  - `wifi_config` for configuration and OTA updates over your normal LAN when saved WiFi credentials already exist.
  - `ap_config` for first-time setup and recovery through a captive AP portal hosted by the controller itself.
- OTA firmware updates from either configuration portal, with automatic reboot back into normal runtime after a successful update.
- Persistent polling buckets so you can choose which entities are active, how often they are refreshed, and whether they should appear in Home Assistant at all.
- Runtime diagnostics over MQTT and on the display so WiFi, MQTT, polling pressure, and dispatch status are visible instead of silent.

### Supported AlphaESS devices are:
- I have only tested this on a SMILE5 system.

- The original upstream project reported testing on these systems, which _should_ work, but I really can't say until someone tests them.
  - SMILE-SPB,
  - SMILE-B3,
  - SMILE-T10,
  - Storion T30,
  - SMILE-B3-PLUS
  - *Others are likely to work too.

## Steps to get running.
- Build the hardware.  (See [Hardware](#hardware) below.)
- Build and flash the firmware.  Follow the current repository build instructions and the comments at the top of `Definitions.h`.  Few, if any, changes are now needed in `Definitions.h`; usually only the small hardware-specific section at the top needs attention.
- Enable MQTT discovery in Home Assistant if it is not already enabled.
- Plug in RS485 and power (USB), then [configure WiFi and MQTT](#configuring-wifi-and-mqtt).
- Once the controller has joined your WiFi and connected to MQTT, Home Assistant will discover the Alpha2MQTT device and its entities automatically.  You can then monitor your ESS from the MQTT integration/device page.
- (optional) Configure the HA Energy dashboard to use the new grid, solar, and battery entities.
- (optional) Create an ESS dashboard.  (See [Dashboard Example](#dashboard-example) below.)
- (optional) Create ESS automations to control the ESS.  (See [Automation Examples](#automation-examples) below.)

## More Details
### CI build check
GitHub Actions runs an Arduino CLI build for the ESP8266 targets on pull requests to validate compilation.
The Arduino sketch includes wrapper headers in the `Alpha2MQTT/` folder so Arduino CLI builds can find the headers moved under `Alpha2MQTT/include`.
Source files under `Alpha2MQTT/src` include the wrapper headers via relative `../` paths to keep Arduino CLI compilation aligned with PlatformIO.
Arduino CLI builds also map `ESP8266`/`ESP32` defines to the `MP_ESP*` flags expected by the shared headers.
The CI workflow installs the WiFiManager and Preferences libraries required by `Alpha2MQTT/src/main.cpp` before compiling.
CI uses the ESP8266 core version aligned with local Arduino CLI builds to reduce version drift.
When a compile fails, the workflow uploads the full logs as the `arduino-compile-logs` artifact and posts a PR comment with the first section of the failure output.

### Host unit tests
Host-based unit tests validate scheduling, Modbus CRC/frame generation, config serialization, and boot-mode gating without requiring Arduino hardware. Run them locally with:

- `./scripts/test_host.sh` (Linux host build)
- `./scripts/test_host_docker.sh` (Docker wrapper for a consistent Linux toolchain)

GitHub Actions also runs these tests via the `Host Tests` workflow alongside an ESP8266 smoke compile.

### Configuring WiFi and MQTT
Before this device can do anything useful it needs WiFi and MQTT settings.  It needs to know your WiFi SSID and password, and your MQTT server details (IP, port, username, and password).  These values are now configured through the built-in web UI and saved in flash.

The easiest way to understand the firmware is to treat the web UI as three distinct experiences:

- `normal`
  - This is the real runtime mode.
  - MQTT discovery/state publishing, ESS polling, dispatch control, and the lightweight HTTP control page all run here.
  - The normal HTTP page is the one that shows `Alpha2MQTT Control` and gives you the reboot actions for moving into the config portals.

- `wifi_config`
  - This is the configuration portal served over your normal LAN using the saved WiFi credentials.
  - Use this when the device is already reachable on your network and you want to change WiFi/MQTT settings, polling settings, or upload new firmware without joining the device AP.
  - You normally get here by using `Reboot WiFi Config` from the runtime HTTP page.

- `ap_config`
  - This is the captive portal AP mode.
  - The controller creates its own WiFi network (`Alpha2MQTT`, no password by default) and serves the same configuration pages directly from that network.
  - Use this for first-time setup, recovery from broken WiFi credentials, or when you want a local direct setup path without relying on your normal LAN.

Typical flow:
- First boot with no saved config usually lands in `ap_config`.
- After you save WiFi/MQTT settings, the device reboots and comes up in `normal`.
- A configured device can be moved into `wifi_config` from the normal runtime page whenever you want to change settings or perform an OTA update.
- If saved WiFi later becomes invalid because the SSID disappears or the password is wrong, the controller treats that as a recovery condition.  It retries in normal mode for a bounded window, then falls back to `ap_config`.  If the AP portal is left idle for 5 minutes it reboots back to normal and tries again.  That gives you a repeatable recovery window without leaving the device stranded in setup mode after a transient outage.
- If you explicitly reboot into `wifi_config`, the firmware now keeps retrying the saved STA connection on ordinary timeouts instead of dropping straight into the AP portal.  It only falls back to `ap_config` there when the saved WiFi looks genuinely invalid.

There are two main ways to enter the configuration portal:
- Button press.  This is the preferred path when a hardware button is defined.  (At the moment that mainly means the XIAO ESP32C6 `BOOT` button.)
- "As needed".  When no button is defined, the portal starts automatically when configuration is incomplete or recovery is required.

If you define WiFi/MQTT values via `Secrets.h` (or a `secrets.txt` workflow that generates it), those values are used as defaults for the portal fields and as the initial settings when no stored configuration exists.

### OTA Firmware Updates
Firmware updates are done through the configuration portals, not through the normal runtime page.

- In either `wifi_config` or `ap_config`, open the portal menu and go to `Update`.
- Upload the firmware binary there.
- After a successful update, the firmware now reboots automatically back into `normal` runtime.

That means the usual update flow is:
- reboot into `wifi_config` if the device is already on your LAN and reachable
- open the portal `Update` page
- upload the new firmware
- wait for the device to reboot back into normal runtime

If you are doing first-time setup or recovery through the device AP, the same `Update` page is also available in `ap_config`.

### Inverter identity and discovery
The controller no longer reuses a cached inverter serial across reboot. Inverter identity is considered unknown until the firmware reads a live serial from RS485.

Practical effect:
- controller-side runtime, WiFi, MQTT, HTTP, and portal features can still come up normally while RS485 is unavailable
- inverter-scoped Home Assistant discovery and inverter telemetry do not publish until live serial is known
- if the device boots while the inverter is offline, it will not guess an inverter identity from old flash state

If a device upgrades while the inverter is unavailable, old retained inverter discovery topics may remain in Home Assistant until the controller later reads a live serial again or the retained topics are purged manually.

### Configuring polling buckets
Alpha2MQTT can store per-entity polling buckets that persist across restarts and show up in Home Assistant via MQTT discovery.  The authoritative config is a retained payload published to `DEVICE_NAME/config`, with delta updates sent to `DEVICE_NAME/config/set`.  The firmware now supports a broad catalog of optional telemetry, and many of those entities are disabled by default.  This lets you choose the small set of values you actually care about instead of polling everything all the time.

The supported bucket names are:
- `ten_sec`
- `one_min`
- `five_min`
- `one_hour`
- `one_day`
- `user`
- `disabled`

`user` uses the global `poll_interval_s` value.

How buckets behave:
- Every entity is assigned to one bucket.
- That bucket decides how often the entity is polled.
- If you set an entity to `disabled`, the device stops polling it and removes that entity's discovery configuration in HA until it is re-enabled.
- `freqNever` is reserved for legacy defaults and is not user-settable.

There are two ways to change the polling config:

- MQTT
  - Read the current retained config from `DEVICE_NAME/config`.
  - Send updates to `DEVICE_NAME/config/set`.
  - You can update `poll_interval_s`, individual `entity_intervals`, or a bucket-map payload.
  - These MQTT updates apply live in normal runtime and are persisted by the firmware.

- Portal
  - In `wifi_config` or `ap_config`, open `Polling`.
  - Change bucket assignments there and save them.
  - The portal menu and the polling page also provide `Reset Polling Defaults`, which clears bad or unwanted polling state without needing the normal runtime page first.
  - The portal also supports direct import through the hidden `bucket_map_full` field if you want to post a whole assignment map at once.
  - After saving in the portal, reboot back to normal mode and the new polling plan will be active.

In practice:
- If you want to automate or version-control bucket changes, use MQTT.
- If you want to browse and adjust them interactively, use the portal.
- Both paths update the same underlying stored polling plan.

The WiFi/config portal also supports direct polling-config import via the `bucket_map_full` form field on `POST /config/polling/save`. The value is a semicolon-delimited assignment list like `Grid_Power=ten_sec;Battery_Temp=one_min;`. This merges onto the current config and persists the result; omitted entities keep their current bucket. If you want to explicitly remove an entity from polling and HA discovery, assign it `disabled`, for example `Battery_Temp=disabled;`.

### Captive Portal Verification
The captive portal has a separate real-device verification script because it depends on lab infrastructure that the main RS485 stub E2E suite does not use. The script starts from a true virgin state by fully erasing flash over serial, flashing the latest real firmware, completing onboarding through the AP portal from a remote Pi, and then verifying that the normal-mode WiFi portal can save polling bucket changes.

Prerequisites:
- serial access to the target ESP8266 device
- the `arduino-cli-build` helper container running and able to access the serial device
- an ESP8266 serial monitor configured for `115200` if you want to watch the firmware heap telemetry directly during portal and E2E work
- a Pi reachable over SSH with wired LAN plus a working WiFi interface for joining the ESP AP
- local-only `.secrets` entries for `PI_HOSTNAME`, `PI_USER`, `PI_SSH_PWD`, `WIFI_SSID`, and `WIFI_PWD`
- MQTT settings available through `tools/e2e/e2e.local.json`, `tools/e2e/e2e.local.env`, or `.secrets`

Run it with:

```bash
timeout 3600 /home/coder/git/Alpha2MQTT/scripts/e2e_captive_portal.sh
```

This test should be run periodically and whenever WiFi, captive portal, boot-mode, or onboarding changes are made.

### RS485 Stub E2E Verification
The main RS485 stub suite writes a wall-clock main log plus a raw serial sidecar under `tools/e2e/logs/`. The ESP8266 serial path runs at `115200` for the firmware `Heap ...` telemetry during reboots and runtime transitions.

Run it with:

```bash
timeout 3600 /home/coder/git/Alpha2MQTT/scripts/e2e.sh
```

The script refreshes `e2e_latest.log` and `e2e_latest.serial.log` symlinks for the current run and prunes E2E log files older than 10 days.

If you enable more telemetry than the selected polling cadence can comfortably sustain, the firmware stays bounded rather than trying to catch up forever. Some values may go stale for a while, and the controller publishes diagnostics so that this is visible instead of silent.

When the controller restarts, reconnects to MQTT, or needs to resend state, it does not immediately flood every slow bucket.  Instead it forces the normal 10-second status pass and then refreshes a few additional enabled entity states per loop.  This keeps ESP8266 reconnect behavior stable while still letting slow identity and diagnostic entities catch up much sooner than their natural one-hour or one-day cadence.

- **Config topic (retained):** `DEVICE_NAME/config`
- **Config update topic (non-retained):** `DEVICE_NAME/config/set`

### HTTP control plane (MODE_NORMAL only)
When boot mode is `normal`, the firmware exposes a lightweight HTTP page for requesting reboots into specific boot modes. The server is not started when boot mode is `ap_config` or `wifi_config`.

- **GET /**: status page (boot mode, boot intent, reset reason)
- **POST /reboot/normal**: set boot_mode=normal, boot_intent=normal, reboot
- **GET /reboot/ap**: confirmation page before entering AP config
- **POST /reboot/ap**: set boot_mode=ap_config, boot_intent=ap_config, reboot
- **POST /reboot/wifi**: set boot_mode=wifi_config, boot_intent=wifi_config, reboot
- **HA discovery:** a diagnostic sensor named **MQTT Config** exposes `last_change` as its state and the full JSON as attributes.

### MQTT status, boot, and events
The device publishes lightweight retained/status topics for observability:

- `DEVICE_NAME/HA_UNIQUE_ID/boot` (retained): `{"boot_intent":"...","reset_reason":"...","ts_ms":...}`
- `DEVICE_NAME/HA_UNIQUE_ID/status` (retained, ~10s): core fields `presence`, `a2mStatus`, `rs485Status`, `gridStatus`, `boot_intent`.
- `DEVICE_NAME/HA_UNIQUE_ID/status/net` (retained, ~10s): uptime, heap, WiFi RSSI/SSID/IP, WiFi/MQTT state + reconnect counters.
- `DEVICE_NAME/HA_UNIQUE_ID/status/poll` (retained, ~10s): poll ok/err counts, last poll duration, last ok/err timestamps, last error code, and polling-pressure diagnostics such as backlog and budget exhaustion.
- `DEVICE_NAME/HA_UNIQUE_ID/event` (non-retained): rate-limited fault events like `RS485_TIMEOUT`, `MODBUS_FRAME`, or `POLL_OVERRUN`.

Before live inverter identity is known, the masked HA identity remains `A2M-UNKNOWN` and inverter-scoped discovery/state topics are suppressed.

### What you will see
- Once your Alpha2MQTT device is working, in Home Assistant go to Settings->Devices & Services->Integrations->MQTT->devices
- In this list you will see your inverter-focused Alpha2MQTT device, and you may also see a small controller-side diagnostic device. The inverter-facing entities are the main telemetry and status surface. The controller-side entities are there to help you understand connectivity and polling health.
- Many optional telemetry entities are disabled by default, so the exact list you see will reflect what you have chosen to enable.
- When you send dispatch requests, you will also see `Dispatch Request Status` and the `Dispatch Start/Mode/Power/SOC/Time` readback entities update to reflect what the inverter is actually doing.

### Controlling Your ESS
The firmware control path is now centered on a single MQTT dispatch request.  Rather than trying to coordinate several separate control entities and hoping they are all updated before dispatch starts, you publish one JSON payload to the inverter-specific dispatch topic:

- **Topic:** `alpha2mqtt_inv_<serial>/dispatch/set`

That `alpha2mqtt_inv_<serial>` part is the inverter device id.  Alpha2MQTT derives it from the live inverter serial and uses that same id for inverter-scoped MQTT discovery and state topics.  The dispatch topic deliberately does **not** include the controller device name so that the command path follows the inverter rather than the controller hardware.

Here is an example:

```json
{
  "mode": "state_of_charge_control",
  "power_w": 3000,
  "soc_percent": 20,
  "duration_s": 1800
}
```

The payload fields are:
- `mode`: which AlphaESS dispatch mode to request
- `power_w`: signed power in watts.  Negative means charge.  Positive means discharge.
- `soc_percent`: target SOC percentage for modes that use it
- `duration_s`: dispatch duration in seconds for modes that use it

The currently supported mode names are:
- `battery_only_charges_from_pv`
- `state_of_charge_control`
- `load_following`
- `maximise_output`
- `normal_mode`
- `optimise_consumption`
- `maximise_consumption`

Not every mode uses every field.  For example, `normal_mode` is a stop/release request and ignores the other fields.  The firmware validates the payload, writes the AlphaESS dispatch registers as one atomic block, reads back the resulting dispatch values, and then force-publishes the inverter's dispatch state to MQTT/HA.

The atomic dispatch API is intentionally modeled as a timed lease on inverter-native dispatch state.  Each request sets the full dispatch intent, including its expiry (`duration_s`), in one transaction, and success is confirmed by inverter readback.  This is safer and simpler than exposing separate writable HA entities such as `Op Mode`, `SOC Target`, `Charge Power`, `Discharge Power`, and `Push Power`: if Home Assistant, MQTT, or the controller stops refreshing the request, the inverter itself will exit dispatch and return to `normal_mode` when the lease expires.  Clients should therefore treat non-zero `duration_s` requests as renewable leases and republish before expiry if they want dispatch to continue.  For example, a client that wants dispatch to remain active continuously might request a 5 minute lease (`duration_s = 300`) and refresh it every 2.5 minutes.  (`duration_s = 0` is effectively indefinite and does not provide this lease-based safety property.)

To see whether the request worked, use:
- `Dispatch Request Status`: `ok` on success, otherwise a short human-readable error message such as `invalid mode`, `invalid power`, `modbus write failed`, or a readback mismatch description
- `Dispatch Start`
- `Dispatch Mode`
- `Dispatch Power`
- `Dispatch SOC`
- `Dispatch Time`

Those `Dispatch *` entities are the actual inverter readback values, so they are the best way to see what the inverter is currently doing.

### AlphaESS Specs
Alpha2MQTT honours 1.28 AlphaESS Modbus documentation.  The latest register list I found came from October 2024.

- [AlphaESS Modbus Documentation](Pics/AlphaESS_Register_parameter_list_Oct_2024.pdf)

- [AlphaESS Register List](Pics/AlphaESS_Modbus_Protocol_V1.28.pdf)

### Hardware
To build this hardware, use the current wiring notes below together with the upstream project page for historical background if needed. I originally started with an ESP8266, then switched to an ESP32 for better WiFi and have only tested with the ESP32 for a while. (However, the ESP8266 _should_ still work.) I also went to a larger display. This is not necessary, but it did help with debugging. I used a different MAX3485 part because I liked the form factor better. If you use this, be sure to connect the EN pin. Finally I switched from a "generic" ESP32 to the Seeed XIAO ESP32C6, which is very small, has much better WiFi (including WiFi 6), and is quite cheap. The XIAO ESP32C6 supports both internal and external antennas (selectable via software). I found the XIAO ESP32C6 internal antenna gave me a stronger signal than my older ESP32 boards with an external antenna. (Select which antenna you are using in `Definitions.h`)


Device wiring:
- Display GND -> XIAO ESP32C6 GND (Pin 13)
- Display VCC -> XIAO ESP32C6 3.3V (Pin 12)
- Display SCL -> XIAO ESP32C6 SCL (Pin 6)
- Display SDA -> XIAO ESP32C6 SDA (Pin 5)
- MAX3485 EN  -> XIAO ESP32C6 D8/GPIO19 (Pin 9)
- MAX3485 VCC -> XIAO ESP32C6 3.3V (Pin 12)
- MAX3485 RXD -> XIAO ESP32C6 TX/D6/GPIO16 (Pin 7)
- MAX3485 TXD -> XIAO ESP32C6 RX/D7/GPIO17 (Pin 8)
- MAX3485 GND -> XIAO ESP32C6 GND (Pin 13)
- MAX3485 A   -> RJ45 (Pin 5)
- MAX3485 B   -> RJ45 (Pin 4)

### Dashboard Example
- I tied into the Home Assistant builtin Energy dashboard by simply editing its configuration and adding grid to/from, solar, battery to/from, and battery SOC.  Voila!
- Here is my ESS dashboard.  This was simple to make using the HA builtin visual editor.  I used the builtin "energy-distribution" card and "power-flow-card-plus" which is available through HACS.  In the middle section there are several entities that appear and disappear depending on the current dispatch state.
- Here is the same dashboard when the ESS is in a different dispatch state.  Note: The middle column has more entities showing in that state.
- The "Electricity Tariff" and "NWS Alerts" are the two entities on this page that don't come from Alpha2MQTT.  I include them here because my automations use these to help control the ESS.
- You will also note that this dashboard also has views for an "Energy" (almost identical to HA's builtin Energy Dashboard) and "Power".  I won't bore you with pics, but the yaml has all the details.

### Other Changes and Enhancements
- Quite a few new registers have been added.  (See [Specs](#alphaess-specs) above.)
- The firmware now supports a much broader optional telemetry catalog while keeping most additional entities disabled by default until you opt in.
- Polling is now intended to be user-shaped rather than treated as one fixed built-in schedule. If you ask for too much at a given cadence, the controller stays responsive and surfaces the pressure through diagnostics rather than silently hiding it.
- This uses an MQTT Last Will and Testament (LWT) to set availability for all entities.  In addition some entities also use the RS485 status to set their availability.  And for others, even the grid status is used.
- This uses the MQTT retain flag along with other MQTT options to ensure as-graceful-as-possible handling of situations where this device, HA, or the MQTT broker might reboot or go offline.
- The WiFi code has been tweaked to better handle Multi-AP environments and low signal situations.
- The RS485 code has been tweaked to better handle more than two peers on the RS485 bus.  While this situation is rare, I had to deal with it during development because my vendor provided control via RS485 as well.  So until I trusted my device to take over, I had to make both controllers coexist.  RS485 fully supports more than two peers.  However, the MAX3485 doesn't provide enough control to make this perfect.  But I was able to make it robust enough.  Also, MODBUS says there can be only one "master" and Alpha2MQTT and my vendor's device were both masters.  But again, it still works well enough.  And now that I trust this controller, I have disconnected the vendor's device.
- The larger display support allows for easier debugging.  This really isn't needed for anything other than debugging.
- There's a lot of debugging information available via MQTT and the large display.  Enabling the different DEBUG options in `Definitions.h` will add more rotating debug values on the screen and add new Diagnostic entities under the MQTT device.
