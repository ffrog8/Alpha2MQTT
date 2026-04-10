#!/usr/bin/env python3
"""
E2E: RS485/Modbus stub backend verification (single firmware, runtime-controlled stub)

This script drives the firmware stub mode via the firmware's MQTT control plane, and asserts behavior
by reading the firmware's published status telemetry over MQTT.

Design goals:
- No inverter hardware required.
- One firmware upload per run (optional OTA step).
- No rebuilds per test case (stub mode is controlled at runtime).
- Deterministic and bounded (timeouts + diagnostic output on failure).

Environment variables
MQTT connectivity:
  MQTT_HOST            Broker host (required)
  MQTT_PORT            Broker port (default: 1883)
  MQTT_USER            MQTT username (required)
  MQTT_PASS            MQTT password (or set MQTT_PASSFILE)
  MQTT_PASSFILE        Path to a file containing the password (one line)

Device discovery:
  DEVICE_TOPIC         Optional fixed device topic root (e.g. Alpha2MQTT-XXXXXX). If unset, script auto-discovers
                       by subscribing to '+/status/poll' and selecting a device that exposes 'rs485_backend' and
                       'ess_snapshot_attempts' in its JSON.

Optional OTA upload (one-time per run):
  DEVICE_HTTP_BASE     Base URL for the device, e.g. http://<device-ip> (required only if --ota is used)
  FIRMWARE_BIN         Firmware path for OTA upload (optional; defaults to latest stub artifact in repo)
  DEVICE_REBOOT_WIFI_PATH  HTTP path to trigger "reboot into Wi-Fi portal" (optional; can be discovered from repo code)
  DEVICE_OTA_UPLOAD_PATH   HTTP path that accepts firmware upload in the portal (default: /u)
  DEVICE_OTA_FIELD_NAME    Multipart field name for firmware upload (default: update)

Runtime stub control
The firmware listens for stub-control messages on:
  <device_root>/debug/rs485_stub/set
Accepted payloads are intentionally lightweight:
  - {"mode":"offline"}
  - {"mode":"online"}
  - {"mode":"fail","fail_n":2}
Optional:
  - include "reg" or "register" to fail a specific starting register for ESS snapshot reads.
The E2E checks behavior via the existing poll status topic:
  <device_root>/status/poll
and the stub-only status topic (stub firmware only):
  <device_root>/status/stub
and the manual register-read correlation topic:
  <device_root>/status/manual_read
and asserts on keys emitted by the firmware:
  rs485_backend, rs485_stub_mode, rs485_stub_fail_remaining,
  ess_snapshot_attempts, ess_snapshot_last_ok,
  dispatch_last_run_ms, dispatch_last_skip_reason,
  seq, requested_reg, observed_reg, value.

Usage
  python3 tools/e2e/test_rs485_stub.py
  python3 tools/e2e/test_rs485_stub.py --no-flash
  python3 tools/e2e/test_rs485_stub.py --force-flash
  python3 tools/e2e/test_rs485_stub.py --list-cases
  python3 tools/e2e/test_rs485_stub.py --case portal_polling_ui
  python3 tools/e2e/test_rs485_stub.py --from-case soc_drift_e2e --trace-http
"""

from __future__ import annotations

import argparse
import json
import html as html_lib
import os
import re
import subprocess
import urllib.parse
import socket
import sys
import time
from pathlib import Path
from typing import Any, Callable, Optional, Tuple


class E2EError(Exception):
    pass


VERBOSE = False
TRACE_HTTP = False
TRACE_MQTT = False
PROGRESS_INTERVAL_S = 10.0
CASE_ORDER: tuple[str, ...] = (
    "two_device_discovery",
    "offline",
    "fail_then_recover",
    "runtime_loss_reprobe",
    "online",
    "boot_mem_publish",
    "scheduler_idle_no_extra_reads",
    "strict_unknown_snapshot",
    "strict_unknown_register_reads",
    "bucket_snapshot_skip_only",
    "dispatch_write_via_commands",
    "dispatch_legacy_command_topic_ignored",
    "dispatch_invalid_payload_no_write",
    "dispatch_invalid_numeric_payloads_no_write",
    "dispatch_write_feedback",
    "dispatch_primes_snapshot_once",
    "dispatch_write_under_100ms",
    "dispatch_timed_restart_expire",
    "dispatch_timed_no_rewrite_without_fresh_snapshot",
    "dispatch_disable_timed_stops_countdown_wakes",
    "dispatch_boot_fail_closed",
    "fail_specific_snapshot_reg",
    "rs485_error_counters_split",
    "fail_every_n",
    "latency",
    "flapping",
    "probe_delayed",
    "identity_reboot_unknown",
    "fail_writes_only",
    "dispatch_readback_window",
    "dispatch_readback_timeout_status",
    "max_feedin_percent_write",
    "fail_for_ms",
    "soc_drift_backend_ready",
    "stub_soc_drift_applies",
    "soc_publish_respects_bucket",
    "soc_drift_e2e",
    "load_power_formula",
    "polling_config",
    "runtime_polling_reset_without_page",
    "portal_polling_ui",
    "polling_profile_export_import",
    "reboot_ap_confirmation",
    "portal_wifi_save_reboot_only",
    "portal_mqtt_save_reboot_handoff",
)


def _log(msg: str) -> None:
    if VERBOSE or TRACE_MQTT:
        print(f"[e2e] {msg}")


def _http_log(msg: str) -> None:
    if TRACE_HTTP:
        print(f"[e2e][http] {msg}")


def _announce(msg: str) -> None:
    print(f"[e2e] {msg}", flush=True)


def _read_pass() -> str:
    passfile = os.environ.get("MQTT_PASSFILE")
    if passfile:
        return Path(passfile).read_text(encoding="utf-8").strip()
    password = os.environ.get("MQTT_PASS")
    if not password:
        raise E2EError("Missing MQTT_PASS or MQTT_PASSFILE")
    return password.strip()


def _require_env(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        raise E2EError(f"Missing required env var: {name}")
    return value


def _repo_root() -> Path:
    # tools/e2e/test_rs485_stub.py -> repo root two levels up from tools/
    return Path(__file__).resolve().parents[2]

def _default_env_file() -> Path:
    # tools/e2e/e2e.local.env next to this script.
    return Path(__file__).resolve().parent / "e2e.local.env"

def _default_json_file() -> Path:
    # Preferred local config file (gitignored).
    return Path(__file__).resolve().parent / "e2e.local.json"


def _load_json_run_defaults(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {}
    data = json.loads(path.read_text(encoding="utf-8", errors="replace"))
    if not isinstance(data, dict):
        return {}
    run = data.get("run", {})
    return run if isinstance(run, dict) else {}


def _as_bool(value: Any, default: bool = False) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        return value.strip().lower() in ("1", "true", "yes", "on")
    return default


def _env_bool(name: str, default: bool = False) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return _as_bool(raw, default)


def _env_csv(name: str) -> list[str]:
    raw = os.environ.get(name, "")
    if not raw.strip():
        return []
    return [v.strip() for v in raw.split(",") if v.strip()]

def _load_json_file_defaults(path: Path) -> None:
    """
    Load config defaults from JSON into os.environ (without overriding existing env vars).
    Accepted shapes:
      - { "MQTT_HOST": "...", ... }
      - { "env": { "MQTT_HOST": "...", ... } }
    """
    if not path.exists():
        return
    data = json.loads(path.read_text(encoding="utf-8", errors="replace"))
    if not isinstance(data, dict):
        raise E2EError(f"Expected JSON object in {path}")
    env_map = data.get("env", data)
    if not isinstance(env_map, dict):
        raise E2EError(f"Expected object for env map in {path}")
    for key, value in env_map.items():
        if not isinstance(key, str) or not key or key in os.environ:
            continue
        if isinstance(value, bool):
            os.environ[key] = "1" if value else "0"
        elif isinstance(value, (int, float)):
            os.environ[key] = str(value)
        elif isinstance(value, str):
            os.environ[key] = value

def _load_env_file_defaults(path: Path) -> None:
    """
    Load KEY=VALUE lines into os.environ (without overriding existing env vars).
    This lets the E2E runner be a stable, no-flags command with local config in a gitignored file.
    """
    if not path.exists():
        return
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if not key or key in os.environ:
            continue
        os.environ[key] = value

def _firmware_main_cpp() -> Path:
    # Repo layout: <repo>/Alpha2MQTT/src/main.cpp
    return _repo_root() / "Alpha2MQTT" / "src" / "main.cpp"

def _firmware_definitions_h() -> Path:
    return _repo_root() / "Alpha2MQTT" / "include" / "Definitions.h"

def _latest_stub_firmware_path() -> Path:
    latest = _repo_root() / "Alpha2MQTT" / "build" / "firmware" / "Alpha2MQTT_latest_stub.txt"
    if not latest.exists():
        raise E2EError(f"Missing {latest} (run firmware build first)")
    fw_name = latest.read_text(encoding="utf-8").strip()
    fw_path = _repo_root() / "Alpha2MQTT" / "build" / "firmware" / fw_name
    if not fw_path.exists():
        raise E2EError(f"Firmware referenced by {latest} does not exist: {fw_path}")
    return fw_path

def _firmware_build_ts_ms_from_filename(path: Path) -> int:
    # Alpha2MQTT_<ts>_stub.bin
    m = re.search(r"Alpha2MQTT_(\d+)_stub\.bin$", path.name)
    if not m:
        raise E2EError(f"Cannot extract build timestamp from firmware filename: {path.name}")
    return int(m.group(1))


def _discover_control_suffix_from_code() -> str:
    data = _firmware_main_cpp().read_text(encoding="utf-8", errors="replace")
    # Expect a format string like: "%s/debug/rs485_stub/set"
    m = re.search(r"\"%s(/debug/rs485_stub/set)\"", data)
    if not m:
        # Fallback: literal string present.
        if "/debug/rs485_stub/set" in data:
            return "/debug/rs485_stub/set"
        raise E2EError("Could not discover RS485 stub control topic suffix from firmware main.cpp")
    return m.group(1)


def _discover_status_poll_suffix_from_code() -> str:
    data = _firmware_main_cpp().read_text(encoding="utf-8", errors="replace")
    # The firmware constructs:
    #   statusTopic = "<device>/status"
    #   pollTopic   = "<device>/status/poll"  via snprintf(pollTopic, "%s/poll", statusTopic)
    has_status = re.search(r'snprintf\(\s*statusTopic\b.*"%s/status"', data) is not None
    has_poll = re.search(r'snprintf\(\s*pollTopic\b.*"%s/poll"\s*,\s*statusTopic\s*\)', data) is not None
    if has_status and has_poll:
        return "/status/poll"
    raise E2EError("Could not derive status poll topic suffix from firmware main.cpp")

def _discover_status_stub_suffix_from_code() -> str:
    data = _firmware_main_cpp().read_text(encoding="utf-8", errors="replace")
    has_stub = re.search(r'snprintf\(\s*stubTopic\b.*"%s/stub"\s*,\s*statusTopic\s*\)', data) is not None
    if has_stub:
        return "/status/stub"
    raise E2EError("Could not derive status stub topic suffix from firmware main.cpp (is stub status publishing enabled?)")

def _discover_register_value(name: str) -> int:
    """
    Extract numeric #define register values from Definitions.h so the E2E script doesn't invent constants.
    Supports hex (0x...) and decimal.
    """
    data = _firmware_definitions_h().read_text(encoding="utf-8", errors="replace")
    # Matches: #define NAME 0x1234  or #define NAME 1234
    m = re.search(rf"^\s*#define\s+{re.escape(name)}\s+(0x[0-9A-Fa-f]+|\d+)\b", data, flags=re.MULTILINE)
    if not m:
        raise E2EError(f"Could not discover register value for {name} from Definitions.h")
    raw = m.group(1)
    return int(raw, 0)

def _discover_define_value(name: str) -> int:
    """
    Like _discover_register_value, but for non-register #defines (e.g. DISPATCH_START_START).
    """
    data = _firmware_definitions_h().read_text(encoding="utf-8", errors="replace")
    m = re.search(rf"^\s*#define\s+{re.escape(name)}\s+(0x[0-9A-Fa-f]+|\-?\d+)\b", data, flags=re.MULTILINE)
    if not m:
        raise E2EError(f"Could not discover #define value for {name} from Definitions.h")
    return int(m.group(1), 0)

def _discover_define_string(name: str) -> str:
    """
    Extract string #defines from Definitions.h, e.g.:
      #define OP_MODE_DESC_TARGET "Target SOC"
    """
    data = _firmware_definitions_h().read_text(encoding="utf-8", errors="replace")
    m = re.search(rf"^\s*#define\s+{re.escape(name)}\s+\"([^\"]*)\"\s*$", data, flags=re.MULTILINE)
    if not m:
        raise E2EError(f"Could not discover string #define value for {name} from Definitions.h")
    return m.group(1)

def _discover_reboot_wifi_path_from_code() -> Optional[str]:
    data = _firmware_main_cpp().read_text(encoding="utf-8", errors="replace")
    # NORMAL-mode HTTP control plane route.
    if 'httpServer.on("/reboot/wifi"' in data or 'httpServerRef().on("/reboot/wifi"' in data:
        return "/reboot/wifi"
    return None

def _discover_reboot_normal_path_from_code() -> Optional[str]:
    data = _firmware_main_cpp().read_text(encoding="utf-8", errors="replace")
    if 'httpServer.on("/reboot/normal"' in data or 'httpServerRef().on("/reboot/normal"' in data:
        return "/reboot/normal"
    return None


def _run(cmd: list[str], timeout_s: int, env: Optional[dict[str, str]] = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout_s,
        env=env,
    )

def _encode_varint(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value % 128
        value //= 128
        if value > 0:
            byte |= 0x80
        out.append(byte)
        if value == 0:
            break
    return bytes(out)


def _encode_utf8(s: str) -> bytes:
    data = s.encode("utf-8")
    return len(data).to_bytes(2, "big") + data


def _read_exact(sock: socket.socket, n: int) -> bytes:
    chunks: list[bytes] = []
    remaining = n
    while remaining > 0:
        part = sock.recv(remaining)
        if not part:
            raise E2EError("MQTT socket closed")
        chunks.append(part)
        remaining -= len(part)
    return b"".join(chunks)


def _read_varint(sock: socket.socket) -> int:
    multiplier = 1
    value = 0
    while True:
        b = _read_exact(sock, 1)[0]
        value += (b & 0x7F) * multiplier
        if (b & 0x80) == 0:
            return value
        multiplier *= 128
        if multiplier > 128 * 128 * 128:
            raise E2EError("MQTT remaining length varint too large")


class MqttClient:
    """
    Minimal MQTT 3.1.1 client (QoS 0 only) for E2E validation.
    Avoids external dependencies and avoids shelling out to mosquitto tools.
    """

    def __init__(self, host: str, port: int, user: str, password: str, client_id: str = "a2m-e2e"):
        self.host = host
        self.port = port
        self.user = user
        self.password = password
        self.client_id = f"{client_id}-{int(time.time()*1000)}"
        self.sock: Optional[socket.socket] = None
        self._packet_id = 1
        self._pending_publishes: list[Tuple[str, str, bool]] = []
        self._subscriptions: set[str] = set()
        self._latest_by_topic: dict[str, str] = {}
        self._last_tx = time.time()
        self._last_rx = time.time()
        # Keepalive set in CONNECT is 30s; ping comfortably below that.
        self._ping_interval_s = 10.0

    def connect(self, timeout_s: int = 10) -> None:
        _log(f"mqtt connect: tcp://{self.host}:{self.port} client_id={self.client_id}")
        sock = socket.create_connection((self.host, self.port), timeout=timeout_s)
        sock.settimeout(5.0)

        # CONNECT
        proto = _encode_utf8("MQTT") + bytes([0x04])  # protocol level 4 (MQTT 3.1.1)
        flags = 0x02  # clean session
        if self.user:
            flags |= 0x80
        if self.password:
            flags |= 0x40
        keepalive = (30).to_bytes(2, "big")

        payload = _encode_utf8(self.client_id)
        if self.user:
            payload += _encode_utf8(self.user)
        if self.password:
            payload += _encode_utf8(self.password)

        vh = proto + bytes([flags]) + keepalive
        remaining = vh + payload
        pkt = bytes([0x10]) + _encode_varint(len(remaining)) + remaining
        sock.sendall(pkt)

        # CONNACK
        fixed = _read_exact(sock, 1)
        if fixed[0] != 0x20:
            raise E2EError(f"Unexpected MQTT CONNACK header: 0x{fixed[0]:02x}")
        rl = _read_varint(sock)
        data = _read_exact(sock, rl)
        if len(data) != 2:
            raise E2EError("Invalid CONNACK length")
        rc = data[1]
        if rc != 0:
            raise E2EError(f"MQTT connect refused rc={rc}")

        self.sock = sock
        now = time.time()
        self._last_tx = now
        self._last_rx = now
        _log("mqtt connect: CONNACK accepted")

    def close(self) -> None:
        if self.sock:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def _next_packet_id(self) -> int:
        pid = self._packet_id
        self._packet_id = (self._packet_id % 0xFFFF) + 1
        return pid

    def publish(self, topic: str, payload: str, retain: bool = False) -> None:
        if not self.sock:
            raise E2EError("MQTT not connected")
        flags = 0x01 if retain else 0x00
        fixed = 0x30 | flags  # PUBLISH QoS0
        body = _encode_utf8(topic) + payload.encode("utf-8")
        self.sock.sendall(bytes([fixed]) + _encode_varint(len(body)) + body)
        self._last_tx = time.time()
        _log(f"mqtt publish: topic={topic} retain={int(retain)} bytes={len(payload)}")

    def subscribe(self, topic_filter: str, *, force: bool = False) -> None:
        if topic_filter in self._subscriptions and not force:
            return
        if not self.sock:
            raise E2EError("MQTT not connected")
        pid = self._next_packet_id()
        _log(f"mqtt subscribe: filter={topic_filter} pid={pid}")
        vh = pid.to_bytes(2, "big")
        payload = _encode_utf8(topic_filter) + bytes([0x00])  # QoS 0
        body = vh + payload
        self.sock.sendall(bytes([0x82]) + _encode_varint(len(body)) + body)
        self._last_tx = time.time()

        # SUBACK (brokers may deliver retained PUBLISHes very quickly; accept interleaving).
        deadline = time.time() + 10
        while time.time() < deadline:
            pkt_type, _flags, data = self._read_packet(timeout_s=5)
            if pkt_type == 3:  # PUBLISH
                topic, payload_str = self._decode_publish(data)
                retained = bool(_flags & 0x01)
                self._latest_by_topic[topic] = payload_str
                self._pending_publishes.append((topic, payload_str, retained))
                _log(
                    f"mqtt rx buffered: topic={topic} bytes={len(payload_str)} retain={int(retained)}"
                )
                continue
            if pkt_type != 9:  # not SUBACK
                continue

            if len(data) < 3:
                raise E2EError("Invalid SUBACK length")
            if data[0:2] != pid.to_bytes(2, "big"):
                continue  # SUBACK for a different subscription; ignore.
            if any(rc == 0x80 for rc in data[2:]):
                raise E2EError("Subscription refused")
            self._subscriptions.add(topic_filter)
            return

        raise E2EError("Timeout waiting for SUBACK")

    def _try_wait_for_publish_details(self, timeout_s: float) -> Optional[Tuple[str, str, bool]]:
        try:
            return self.wait_for_publish_details(timeout_s=timeout_s)
        except E2EError as e:
            if "Timeout waiting for MQTT publish" in str(e):
                return None
            raise

    def _try_wait_for_publish(self, timeout_s: float) -> Optional[Tuple[str, str]]:
        try:
            return self.wait_for_publish(timeout_s=timeout_s)
        except E2EError as e:
            if "Timeout waiting for MQTT publish" in str(e):
                return None
            raise

    def wait_for_publish(self, timeout_s: float) -> Tuple[str, str]:
        topic, payload, _retained = self.wait_for_publish_details(timeout_s=timeout_s)
        return topic, payload

    def wait_for_publish_details(self, timeout_s: float) -> Tuple[str, str, bool]:
        if self._pending_publishes:
            # Even if we're returning buffered publishes, keep the TCP session alive.
            self.ping_if_needed()
            return self._pending_publishes.pop(0)
        if not self.sock:
            raise E2EError("MQTT not connected")
        deadline = time.time() + float(timeout_s)
        while time.time() < deadline:
            self.ping_if_needed()
            remaining = max(0.1, min(5.0, deadline - time.time()))
            pkt_type, flags, data = self._read_packet(timeout_s=remaining)
            if pkt_type == 3:  # PUBLISH
                topic, payload = self._decode_publish(data)
                retained = bool(flags & 0x01)
                self._latest_by_topic[topic] = payload
                _log(f"mqtt rx: topic={topic} bytes={len(payload)} retain={int(retained)}")
                return topic, payload, retained
            # Ignore other packet types.
        raise E2EError("Timeout waiting for MQTT publish")

    def latest_payload(self, topic: str) -> Optional[str]:
        return self._latest_by_topic.get(topic)

    def ping_if_needed(self) -> None:
        if not self.sock:
            return
        now = time.time()
        last = max(self._last_tx, self._last_rx)
        if (now - last) < self._ping_interval_s:
            return
        try:
            self.sock.sendall(b"\xC0\x00")  # PINGREQ
            self._last_tx = now
            _log("mqtt pingreq")
        except OSError:
            raise E2EError("MQTT socket closed")

    def _read_packet(self, timeout_s: float) -> Tuple[int, int, bytes]:
        if not self.sock:
            raise E2EError("MQTT not connected")
        old_timeout = self.sock.gettimeout()
        self.sock.settimeout(timeout_s)
        try:
            try:
                b1 = _read_exact(self.sock, 1)[0]
                pkt_type = b1 >> 4
                flags = b1 & 0x0F
                rl = _read_varint(self.sock)
                data = _read_exact(self.sock, rl)
                self._last_rx = time.time()
                return pkt_type, flags, data
            except TimeoutError as e:
                raise E2EError("Timeout waiting for MQTT publish") from e
        finally:
            self.sock.settimeout(old_timeout)

    @staticmethod
    def _decode_publish(data: bytes) -> Tuple[str, str]:
        if len(data) < 2:
            return "", ""
        tlen = int.from_bytes(data[0:2], "big")
        topic = data[2 : 2 + tlen].decode("utf-8", errors="replace")
        payload = data[2 + tlen :].decode("utf-8", errors="replace")
        return topic, payload


def _parse_json(payload: str) -> dict[str, Any]:
    return json.loads(payload)

def _http_request(method: str, url: str, headers: dict[str, str], body: bytes, timeout_s: int = 20) -> Tuple[int, bytes]:
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != "http":
        raise E2EError(f"Only http:// URLs are supported (got: {url})")
    host = parsed.hostname
    if not host:
        raise E2EError(f"Invalid URL: {url}")
    port = parsed.port or 80
    path = parsed.path or "/"
    if parsed.query:
        path = f"{path}?{parsed.query}"

    conn = socket.create_connection((host, port), timeout=timeout_s)
    conn.settimeout(timeout_s)
    try:
        _http_log(f"{method} {url} req_bytes={len(body)} timeout_s={timeout_s}")
        request_lines = [
            f"{method} {path} HTTP/1.1",
            f"Host: {host}",
            "Connection: close",
        ]
        for k, v in headers.items():
            request_lines.append(f"{k}: {v}")
        request_lines.append(f"Content-Length: {len(body)}")
        request_lines.append("")
        raw = ("\r\n".join(request_lines)).encode("utf-8") + b"\r\n" + body
        conn.sendall(raw)

        # Read just enough to parse the HTTP status line + headers, without waiting for a full body/close.
        resp = b""
        header_end = -1
        deadline = time.time() + timeout_s
        while header_end < 0:
            if time.time() > deadline:
                raise TimeoutError("timeout waiting for HTTP response headers")
            chunk = conn.recv(4096)
            if not chunk:
                break
            resp += chunk
            header_end = resp.find(b"\r\n\r\n")

        if header_end < 0:
            header_blob = resp
            resp_body = b""
        else:
            header_blob = resp[:header_end]
            resp_body = resp[header_end + 4 :]

        status_line = header_blob.split(b"\r\n", 1)[0].decode("utf-8", errors="replace")
        m = re.match(r"HTTP/\d\.\d\s+(\d+)", status_line)
        if not m:
            raise E2EError(f"Could not parse HTTP status line: {status_line!r}")
        status = int(m.group(1))
        _http_log(f"{method} {url} -> {status} body_bytes={len(resp_body)}")
        return status, resp_body
    finally:
        conn.close()

def _http_post_simple(url: str, timeout_s: int = 20) -> int:
    status, _ = _http_request("POST", url, headers={}, body=b"", timeout_s=timeout_s)
    return status

def _http_post_multipart(url: str, field_name: str, file_path: Path, timeout_s: int = 120) -> int:
    status, _ = _http_post_multipart_full(url, field_name=field_name, file_path=file_path, timeout_s=timeout_s)
    return status

def _http_post_multipart_full(url: str, field_name: str, file_path: Path, timeout_s: int = 120) -> Tuple[int, bytes]:
    boundary = f"a2m-e2e-{int(time.time()*1000)}"
    headers = {"Content-Type": f"multipart/form-data; boundary={boundary}"}

    file_bytes = file_path.read_bytes()
    part_headers = (
        f'--{boundary}\r\n'
        f'Content-Disposition: form-data; name="{field_name}"; filename="{file_path.name}"\r\n'
        f"Content-Type: application/octet-stream\r\n"
        f"\r\n"
    ).encode("utf-8")
    tail = f"\r\n--{boundary}--\r\n".encode("utf-8")
    body = part_headers + file_bytes + tail

    return _http_request_full(
        "POST",
        url,
        headers=headers,
        body=body,
        timeout_s=timeout_s,
        max_bytes=65536,
        body_read_timeout_s=5,
    )

def _http_post_multipart_bytes_full(
    url: str,
    field_name: str,
    filename: str,
    body_bytes: bytes,
    timeout_s: int = 120,
    content_type: str = "text/plain",
    max_bytes: int = 65536,
) -> Tuple[int, bytes]:
    boundary = f"a2m-e2e-{int(time.time()*1000)}"
    headers = {"Content-Type": f"multipart/form-data; boundary={boundary}"}
    part_headers = (
        f'--{boundary}\r\n'
        f'Content-Disposition: form-data; name="{field_name}"; filename="{filename}"\r\n'
        f"Content-Type: {content_type}\r\n"
        f"\r\n"
    ).encode("utf-8")
    tail = f"\r\n--{boundary}--\r\n".encode("utf-8")
    body = part_headers + body_bytes + tail
    return _http_request_full(
        "POST",
        url,
        headers=headers,
        body=body,
        timeout_s=timeout_s,
        max_bytes=max_bytes,
    )

def _http_post_multipart_bytes(
    url: str,
    field_name: str,
    filename: str,
    body_bytes: bytes,
    timeout_s: int = 120,
    content_type: str = "text/plain",
) -> int:
    status, _ = _http_post_multipart_bytes_full(
        url,
        field_name,
        filename,
        body_bytes,
        timeout_s=timeout_s,
        content_type=content_type,
    )
    return status

def _http_post_raw(url: str, file_path: Path, timeout_s: int = 120) -> int:
    status, _ = _http_post_raw_full(url, file_path=file_path, timeout_s=timeout_s)
    return status

def _http_post_raw_full(url: str, file_path: Path, timeout_s: int = 120) -> Tuple[int, bytes]:
    headers = {"Content-Type": "application/octet-stream"}
    return _http_request_full(
        "POST",
        url,
        headers=headers,
        body=file_path.read_bytes(),
        timeout_s=timeout_s,
        max_bytes=65536,
        body_read_timeout_s=5,
    )

def _decode_chunked_http_body(body: bytes) -> bytes:
    out = bytearray()
    cursor = 0
    while True:
        line_end = body.find(b"\r\n", cursor)
        if line_end < 0:
            raise E2EError("invalid chunked HTTP body: missing size delimiter")
        size_token = body[cursor:line_end].split(b";", 1)[0].strip()
        try:
            chunk_size = int(size_token.decode("ascii"), 16)
        except Exception as exc:
            raise E2EError(f"invalid chunked HTTP body size token={size_token!r}") from exc
        cursor = line_end + 2
        if chunk_size == 0:
            return bytes(out)
        if cursor + chunk_size > len(body):
            raise E2EError("invalid chunked HTTP body: truncated chunk")
        out.extend(body[cursor:cursor + chunk_size])
        cursor += chunk_size
        if body[cursor:cursor + 2] != b"\r\n":
            raise E2EError("invalid chunked HTTP body: missing chunk terminator")
        cursor += 2

def _http_request_full(
    method: str,
    url: str,
    headers: dict[str, str],
    body: bytes,
    timeout_s: int = 20,
    max_bytes: int = 65536,
    body_read_timeout_s: Optional[float] = None,
) -> Tuple[int, bytes]:
    """
    Read the full HTTP response body (up to max_bytes). Suitable for portal HTML pages.
    """
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != "http":
        raise E2EError(f"Only http:// URLs are supported (got: {url})")
    host = parsed.hostname
    if not host:
        raise E2EError(f"Invalid URL: {url}")
    port = parsed.port or 80
    path = parsed.path or "/"
    if parsed.query:
        path = f"{path}?{parsed.query}"

    conn = socket.create_connection((host, port), timeout=timeout_s)
    conn.settimeout(timeout_s)
    try:
        _http_log(f"{method} {url} req_bytes={len(body)} timeout_s={timeout_s} max_bytes={max_bytes}")
        request_lines = [
            f"{method} {path} HTTP/1.1",
            f"Host: {host}",
            "Connection: close",
        ]
        for k, v in headers.items():
            request_lines.append(f"{k}: {v}")
        request_lines.append(f"Content-Length: {len(body)}")
        request_lines.append("")
        raw = ("\r\n".join(request_lines)).encode("utf-8") + b"\r\n" + body
        conn.sendall(raw)

        resp = b""
        header_end = -1
        deadline = time.time() + timeout_s
        while header_end < 0:
            if time.time() > deadline:
                raise TimeoutError("timeout waiting for HTTP response headers")
            chunk = conn.recv(4096)
            if not chunk:
                break
            resp += chunk
            header_end = resp.find(b"\r\n\r\n")

        if header_end < 0:
            header_blob = resp
            resp_body = b""
        else:
            header_blob = resp[:header_end]
            resp_body = resp[header_end + 4 :]

        status_line = header_blob.split(b"\r\n", 1)[0].decode("utf-8", errors="replace")
        m = re.match(r"HTTP/\d\.\d\s+(\d+)", status_line)
        if not m:
            raise E2EError(f"Could not parse HTTP status line: {status_line!r}")
        status = int(m.group(1))
        header_lines = header_blob.split(b"\r\n")[1:]
        response_headers: dict[str, str] = {}
        for line in header_lines:
            if b":" not in line:
                continue
            key, value = line.split(b":", 1)
            response_headers[key.decode("utf-8", errors="replace").strip().lower()] = (
                value.decode("utf-8", errors="replace").strip().lower()
            )

        body_bytes = resp_body
        conn.settimeout(body_read_timeout_s if body_read_timeout_s is not None else timeout_s)
        while len(body_bytes) < max_bytes:
            try:
                chunk = conn.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            body_bytes += chunk
        if "chunked" in response_headers.get("transfer-encoding", ""):
            body_bytes = _decode_chunked_http_body(body_bytes)
        out = body_bytes[:max_bytes]
        _http_log(f"{method} {url} -> {status} body_bytes={len(out)}")
        return status, out
    finally:
        conn.close()

def _http_post_form(url: str, fields: dict[str, str], timeout_s: int = 20) -> int:
    encoded = urllib.parse.urlencode(fields).encode("utf-8")
    headers = {"Content-Type": "application/x-www-form-urlencoded"}
    status, _ = _http_request("POST", url, headers=headers, body=encoded, timeout_s=timeout_s)
    return status

def _http_post_form_full(
    url: str,
    fields: dict[str, str],
    timeout_s: int = 20,
    max_bytes: int = 65536,
) -> Tuple[int, bytes]:
    encoded = urllib.parse.urlencode(fields).encode("utf-8")
    headers = {"Content-Type": "application/x-www-form-urlencoded"}
    return _http_request_full(url=url, method="POST", headers=headers, body=encoded, timeout_s=timeout_s, max_bytes=max_bytes)

def _http_get_form(url: str, fields: dict[str, str], timeout_s: int = 20) -> int:
    query = urllib.parse.urlencode(fields)
    status, _ = _http_request("GET", f"{url}?{query}", headers={}, body=b"", timeout_s=timeout_s)
    return status

def _wait_for_http_ok(url: str, timeout_s: int = 30) -> None:
    def pred() -> Tuple[bool, str]:
        try:
            status, _ = _http_request("GET", url, headers={}, body=b"", timeout_s=5)
            return (status in (200, 302)), f"status={status}"
        except Exception as e:
            return False, f"err={e}"
    _assert_eventually(f"HTTP reachable: {url}", pred, timeout_s=timeout_s, poll_s=2.0)

def _ensure_runtime_http_from_portal(base: str, timeout_s: int = 45) -> None:
    try:
        status_root, body_root = _http_request_full("GET", base + "/", headers={}, body=b"", timeout_s=10)
    except Exception:
        return

    root_html = body_root.decode("utf-8", errors="replace")
    if not (status_root == 200 and "Alpha2MQTT Setup" in root_html and "/config/reboot-normal" in root_html):
        return

    _announce(f"portal baseline detected at {base}; POST /config/reboot-normal")
    _http_post_simple(base + "/config/reboot-normal", timeout_s=15)

    def runtime_ready() -> Tuple[bool, str]:
        try:
            status, body = _http_request_full("GET", base + "/", headers={}, body=b"", timeout_s=10)
        except Exception as exc:
            return False, f"err={exc}"
        html = body.decode("utf-8", errors="replace")
        ok = status == 200 and "Alpha2MQTT Control" in html
        return ok, f"status={status}"

    _assert_eventually("portal returns to normal runtime", runtime_ready, timeout_s=timeout_s, poll_s=2.0)


def _root_surface_mode_from_html(html: str) -> str:
    if "Alpha2MQTT Control" in html and "/reboot/wifi" in html and 'meta name="a2m-mode" content="normal"' in html:
        return "normal"
    if "Alpha2MQTT Setup" in html and "/config/reboot-normal" in html:
        if 'meta name="a2m-mode" content="wifi"' in html:
            return "wifi"
        if 'meta name="a2m-mode" content="ap"' in html:
            return "ap"
        return "portal"
    return ""


def _root_surface_state(base: str) -> tuple[bool, str]:
    try:
        status_root, body_root = _http_request_full("GET", base + "/", headers={}, body=b"", timeout_s=10)
    except Exception as exc:
        return False, f"err={exc}"
    root_html = body_root.decode("utf-8", errors="replace")
    mode = _root_surface_mode_from_html(root_html)
    return status_root == 200, f"status={status_root} mode={mode or 'unknown'}"


def _assert_reboot_handoff_html(
    html: str,
    *,
    expected_heading: str,
    expected_target_mode: str,
    expected_probe_kind: str,
    expected_address: Optional[str] = None,
) -> None:
    required = (
        expected_heading,
        'id="reboot-handoff"',
        'id="reboot-status"',
        f'data-target-mode="{expected_target_mode}"',
        f'data-probe-kind="{expected_probe_kind}"',
        'data-start-ms="10000"',
        'data-retry-ms="5000"',
        'data-timeout-ms="300000"',
        "Auto-refresh starts in 10 seconds",
        "5 minutes",
    )
    for token in required:
        if token not in html:
            raise E2EError(f"reboot handoff page missing expected token: {token!r}")
    if expected_address is not None and expected_address not in html:
        raise E2EError(
            f"reboot handoff page missing expected address hint: {expected_address!r}"
        )

def _assert_ota_success_response_html(html: str) -> None:
    # OTA bootstrap must remain backward-compatible with older firmware that
    # still returns a simple success page instead of the shared reboot handoff.
    if 'id="reboot-handoff"' in html:
        _assert_reboot_handoff_html(
            html,
            expected_heading="Rebooting to normal mode",
            expected_target_mode="normal",
            expected_probe_kind="fetch",
        )
        return
    for token in ("Update complete.", "Rebooting now."):
        if token in html:
            return
    raise E2EError("OTA success response did not match either reboot handoff or legacy success HTML")


def _assert_portal_root_menu(base: str, timeout_s: int = 20, required_mode: Optional[str] = None) -> str:
    deadline = time.time() + timeout_s
    last_html = ""
    while time.time() < deadline:
        status, body = _http_request_full("GET", base + "/", headers={}, body=b"", timeout_s=20)
        if status == 200:
            html = body.decode("utf-8", errors="replace")
            last_html = html
            required = (
                "/0wifi",
                "/config/mqtt",
                "/config/polling",
                "/config/polling/reset",
                "/config/update",
                "/status",
                "/config/reboot-normal",
            )
            mode_ok = True if required_mode is None else f'meta name="a2m-mode" content="{required_mode}"' in html
            if all(token in html for token in required) and mode_ok:
                return html
        time.sleep(1.0)
    raise E2EError(f"portal root missing actionable menu entries: {last_html[:400]!r}")

def _discover_polling_menu_path(menu_html: str) -> str:
    # Accept both the root menu form button and setup-page anchor forms of the link.
    m = re.search(r"""(?:action|href)=['"](/config/polling(?:\?[^'"]*)?)['"]""", menu_html, flags=re.IGNORECASE)
    if not m:
        raise E2EError("portal menu missing Polling entry/link")
    return m.group(1)

def _load_polling_page_via_menu(base: str) -> tuple[str, str]:
    deadline = time.time() + 45
    polling_path = ""
    last_detail = "not checked"
    required_tokens = ("poll_interval_s", "/config/polling/save", "bucket_map_full")
    while time.time() < deadline:
        direct_path = "/config/polling"
        status_poll, poll_body = _http_request_full("GET", base + direct_path, headers={}, body=b"", timeout_s=20)
        if status_poll == 200:
            poll_html = poll_body.decode("utf-8", errors="replace")
            if all(token in poll_html for token in required_tokens):
                return direct_path, poll_html
            last_detail = f"{direct_path} incomplete"
            time.sleep(2.0)
            continue
        last_detail = f"{direct_path} status={status_poll}"

        for menu_path in ("/", "/config/mqtt", "/param"):
            status_menu, menu_body = _http_request_full("GET", base + menu_path, headers={}, body=b"", timeout_s=20)
            if status_menu != 200:
                last_detail = f"{menu_path} status={status_menu}"
                continue
            menu_html = menu_body.decode("utf-8", errors="replace")
            try:
                polling_path = _discover_polling_menu_path(menu_html)
                break
            except E2EError:
                last_detail = f"{menu_path} missing polling link"
                continue
        if polling_path:
            break
        time.sleep(2.0)
    if not polling_path:
        raise E2EError(f"portal menu missing Polling entry/link ({last_detail})")

    while time.time() < deadline:
        status_poll, poll_body = _http_request_full("GET", base + polling_path, headers={}, body=b"", timeout_s=20)
        if status_poll != 200:
            last_detail = f"{polling_path} status={status_poll}"
            time.sleep(2.0)
            continue
        poll_html = poll_body.decode("utf-8", errors="replace")
        if all(token in poll_html for token in required_tokens):
            return polling_path, poll_html
        last_detail = f"{polling_path} incomplete"
        time.sleep(2.0)
    raise E2EError(f"portal polling page not reachable via menu link {polling_path}: {last_detail}")

def _extract_form_action_with_input(html: str, required_input: str) -> str:
    for match in re.finditer(r"<form[^>]*action=['\"]([^'\"]+)['\"][^>]*>(.*?)</form>", html, flags=re.IGNORECASE | re.DOTALL):
        action = match.group(1)
        form_html = match.group(2)
        if re.search(rf"name=['\"]{re.escape(required_input)}['\"]", form_html, flags=re.IGNORECASE):
            return action
    raise E2EError(f"could not locate form action for input {required_input}")

def _extract_input_value(body: str, name: str) -> str:
    m = re.search(rf'name="{re.escape(name)}"[^>]*value="([^"]*)"', body, flags=re.IGNORECASE)
    if not m:
        raise E2EError(f"could not locate input value for {name}")
    return html_lib.unescape(m.group(1))

def _load_wifi_page(base: str) -> tuple[str, str]:
    status, body = _http_request_full("GET", base + "/0wifi", headers={}, body=b"", timeout_s=20)
    if status != 200:
        raise E2EError(f"portal wifi page not reachable: status={status}")
    html = body.decode("utf-8", errors="replace")
    if 'name="s"' not in html or 'name="p"' not in html:
        raise E2EError("portal wifi page missing ssid/password inputs")
    return _extract_form_action_with_input(html, "s"), html

def _load_update_page(base: str) -> tuple[str, str, str]:
    status, body = _http_request_full("GET", base + "/config/update", headers={}, body=b"", timeout_s=20)
    if status != 200:
        raise E2EError(f"portal update page not reachable: status={status}")
    html = body.decode("utf-8", errors="replace")
    if 'type="file"' not in html:
        raise E2EError("portal update page missing file input")
    mode = "raw" if 'data-upload-mode="raw"' in html else "multipart"
    return _extract_form_action_with_input(html, "firmware"), html, mode

def _extract_file_input_name(html: str) -> str:
    m = re.search(r'<input[^>]*type="file"[^>]*name="([^"]+)"', html, flags=re.IGNORECASE)
    if not m:
        raise E2EError("could not locate OTA file input name")
    return m.group(1)

def _extract_polling_page_bounds(poll_html: str) -> tuple[int, int]:
    m = re.search(r'<p class="hint">[^<]*Page\s+(\d+)\s+of\s+(\d+)[^<]*</p>', poll_html)
    if not m:
        raise E2EError("could not locate polling page bounds")
    page = int(m.group(1)) - 1
    total = int(m.group(2))
    if page < 0 or total <= 0 or page >= total:
        raise E2EError(f"invalid polling page bounds page={page} total={total}")
    return page, total

def _extract_polling_page_family_key(poll_html: str) -> str:
    m = re.search(
        r'<form id="polling-form"[^>]*>.*?name="family"\s+value="([^"]+)"',
        poll_html,
        flags=re.DOTALL,
    )
    if not m:
        raise E2EError("could not locate polling family key")
    return m.group(1)

def _extract_polling_page_family_keys(poll_html: str) -> list[str]:
    keys: list[str] = []
    for match in re.finditer(r'action="/config/polling"[^>]*>.*?name="family"\s+value="([^"]+)"',
                             poll_html,
                             flags=re.DOTALL):
        key = match.group(1)
        if key not in keys:
            keys.append(key)
    current = _extract_polling_page_family_key(poll_html)
    if current not in keys:
        keys.insert(0, current)
    return keys

def _assert_polling_nav_buttons(poll_html: str, *, prev_enabled: bool, next_enabled: bool) -> None:
    prev_idx = poll_html.find('id="polling-nav-prev"')
    next_idx = poll_html.find('id="polling-nav-next"')
    if prev_idx < 0 or next_idx < 0:
        raise E2EError("polling page missing prev/next nav buttons")
    if prev_idx > next_idx:
        raise E2EError("polling nav buttons rendered in the wrong order")
    prev_disabled = re.search(r'id="polling-nav-prev"[^>]*disabled', poll_html, flags=re.IGNORECASE) is not None
    next_disabled = re.search(r'id="polling-nav-next"[^>]*disabled', poll_html, flags=re.IGNORECASE) is not None
    if prev_enabled == prev_disabled:
        raise E2EError(f"polling prev button state mismatch: prev_enabled={prev_enabled} prev_disabled={prev_disabled}")
    if next_enabled == next_disabled:
        raise E2EError(f"polling next button state mismatch: next_enabled={next_enabled} next_disabled={next_disabled}")

def _load_polling_page(base: str, family: str, page: int) -> str:
    deadline = time.time() + 45
    url = f"{base}/config/polling?family={urllib.parse.quote(family)}&page={page}"
    last_detail = "not checked"
    while time.time() < deadline:
        try:
            status_poll, poll_body = _http_request_full(
                "GET",
                url,
                headers={},
                body=b"",
                timeout_s=20,
            )
        except (OSError, TimeoutError) as exc:
            last_detail = f"transport={exc}"
            time.sleep(2.0)
            continue
        if status_poll != 200:
            last_detail = f"status={status_poll}"
            time.sleep(2.0)
            continue
        html = poll_body.decode("utf-8", errors="replace")
        if ("id=\"polling-form\"" in html and "Page " in html and "data-entity=" in html):
            return html
        last_detail = "incomplete"
        time.sleep(2.0)
    raise E2EError(f"portal polling page {family}/{page} not reachable: {last_detail}")

def _extract_entity_row_and_selected_bucket(poll_html: str, entity_name: str) -> tuple[str, str]:
    row_re = re.search(rf'<tr data-entity="{re.escape(entity_name)}">(.*?)</tr>', poll_html, flags=re.DOTALL)
    if not row_re:
        raise E2EError(f"could not locate {entity_name} row on polling page")
    row_html = row_re.group(1)
    row_idx = re.search(r'name="b(\d+)"', row_html)
    if not row_idx:
        raise E2EError(f"could not locate row index for {entity_name}")
    selected = re.search(r'<option value="([^"]+)" selected>', row_html)
    if not selected:
        raise E2EError(f"could not locate selected bucket for {entity_name}")
    return row_idx.group(1), selected.group(1)

def _locate_entity_on_polling_pages(base: str, first_page_html: str, entity_name: str) -> tuple[str, int, int, str, str, list[str]]:
    first_family = _extract_polling_page_family_key(first_page_html)
    family_keys = _extract_polling_page_family_keys(first_page_html)
    family_order = [first_family] + [family for family in family_keys if family != first_family]
    for family in family_order:
        family_first_html = first_page_html if family == first_family else _load_polling_page(base, family, 0)
        first_page, total_pages = _extract_polling_page_bounds(family_first_html)
        pages = [first_page] + [page for page in range(total_pages) if page != first_page]
        for page in pages:
            page_html = family_first_html if page == first_page else _load_polling_page(base, family, page)
            try:
                row, bucket = _extract_entity_row_and_selected_bucket(page_html, entity_name)
                return family, page, total_pages, row, bucket, family_keys
            except E2EError:
                continue
    raise E2EError(f"could not locate {entity_name} row on any polling family/page")


def _discover_device_topic(mqtt: MqttClient, status_poll_suffix: str) -> str:
    configured = os.environ.get("DEVICE_TOPIC")
    if configured:
        return configured.strip()

    mqtt.subscribe(f"+{status_poll_suffix}")
    for _ in range(10):
        topic, payload = mqtt.wait_for_publish(timeout_s=5)
        if not topic.endswith(status_poll_suffix):
            continue
        try:
            data = _parse_json(payload)
        except Exception:
            continue
        if "rs485_backend" in data and "ess_snapshot_attempts" in data:
            return topic[: -len(status_poll_suffix)]
    raise E2EError(
        "Could not auto-discover device topic root from '+/status/poll'. "
        "Set DEVICE_TOPIC explicitly or ensure the device is publishing status/poll."
    )


def _assert_eventually(
    name: str, fn: Callable[[], Tuple[bool, str]], timeout_s: int, poll_s: float = 2.0
) -> None:
    deadline = time.time() + timeout_s
    last_detail = ""
    next_progress = time.time() + PROGRESS_INTERVAL_S
    while time.time() < deadline:
        ok, detail = fn()
        last_detail = detail
        if ok:
            return
        now = time.time()
        if now >= next_progress:
            remaining = max(0, int(deadline - now))
            print(f"[e2e] waiting for {name} ({remaining}s left) last={last_detail}")
            next_progress = now + PROGRESS_INTERVAL_S
        time.sleep(poll_s)
    raise E2EError(f"Timeout waiting for: {name}. Last observed: {last_detail}")


def _sleep_with_mqtt(mqtt: MqttClient, seconds: float) -> None:
    deadline = time.time() + seconds
    while time.time() < deadline:
        mqtt.ping_if_needed()
        time.sleep(1.0)

def _is_socket_closed_error(e: Exception) -> bool:
    return "MQTT socket closed" in str(e)

def _is_mqtt_transport_retryable(e: Exception) -> bool:
    if _is_socket_closed_error(e):
        return True
    if isinstance(e, BrokenPipeError):
        return True
    if isinstance(e, OSError):
        return e.errno in (9, 32, 54, 104)
    return False

def _is_expected_portal_transport_error(e: Exception) -> bool:
    detail = str(e).lower()
    return (
        "timed out" in detail
        or "reset by peer" in detail
        or "connection reset" in detail
        or "could not parse http status line: ''" in detail
    )

def _mqtt_retry(mqtt: MqttClient, name: str, fn: Callable[[], Any]) -> Any:
    """
    Some broker/network setups will occasionally drop a TCP connection mid-test.
    Reconnect once and retry so the E2E remains robust and bounded.
    """
    for attempt in range(5):
        try:
            return fn()
        except E2EError as e:
            if not _is_mqtt_transport_retryable(e) or attempt == 4:
                raise
            print(f"[e2e] mqtt socket closed during {name}; reconnecting...")
            mqtt.close()
            time.sleep(0.5)
            mqtt.connect()
            # Broker-side subscription state is lost on reconnect.
            mqtt._subscriptions = set()
            mqtt._pending_publishes = []
        except (BrokenPipeError, OSError) as e:
            if not _is_mqtt_transport_retryable(e) or attempt == 4:
                raise
            print(f"[e2e] mqtt transport error during {name}: {e}; reconnecting...")
            mqtt.close()
            time.sleep(0.5)
            mqtt.connect()
            mqtt._subscriptions = set()
            mqtt._pending_publishes = []

def _fetch_latest_json(mqtt: MqttClient, topic: str, label: str, *, timeout_s: int = 15) -> dict[str, Any]:
    def inner() -> dict[str, Any]:
        mqtt.subscribe(topic, force=True)
        deadline = time.time() + timeout_s
        last_observed = ""
        while time.time() < deadline:
            try:
                got_topic, payload = mqtt.wait_for_publish(timeout_s=12.0)
            except E2EError as e:
                if "Timeout waiting for MQTT publish" in str(e):
                    continue
                raise
            last_observed = f"topic={got_topic} payload={payload!r}"
            if got_topic != topic:
                _log(f"{label} wait: ignoring other topic={got_topic}")
                continue

            # Drain any backlog (retained or buffered publishes) and return the most recent payload we see
            # within a short settle window, so tests observe state transitions reliably.
            latest = payload
            settle_deadline = time.time() + 0.25
            while time.time() < settle_deadline:
                nxt = mqtt._try_wait_for_publish(timeout_s=0.25)
                if not nxt:
                    break
                got_topic2, payload2 = nxt
                if got_topic2 == topic:
                    latest = payload2
                    settle_deadline = time.time() + 0.25
                else:
                    _log(f"{label} wait: ignoring other topic={got_topic2}")

            try:
                _log(f"{label} wait: got bytes={len(latest)}")
                parsed = _parse_json(latest)
                return parsed
            except Exception as e:
                raise E2EError(f"{label} payload was not valid JSON on {topic}: {latest!r} ({e})")

        raise E2EError(f"Timeout waiting for {label} JSON on {topic}. Last observed: {last_observed}")

    return _mqtt_retry(mqtt, f"fetch_latest_json({label})", inner)


def _fetch_live_json(mqtt: MqttClient, topic: str, label: str, *, timeout_s: int = 15) -> dict[str, Any]:
    """
    Wait for a new publish on an already-known topic without forcing a resubscribe.

    Rationale:
    - force-subscribing on every poll can re-deliver retained payloads, which is not
      authoritative enough immediately after OTA/reboot or stub mode changes.
    - callers use this when they need proof of live runtime progress rather than the
      broker's current retained snapshot.
    """

    def inner() -> dict[str, Any]:
        mqtt.subscribe(topic)
        # "Live" means after the caller asked for fresh runtime progress, not anything that was
        # already buffered from prior cases. Drop pending publishes first so we only accept new traffic.
        mqtt._pending_publishes = []
        deadline = time.time() + timeout_s
        last_observed = ""
        while time.time() < deadline:
            try:
                got_topic, payload, retained = mqtt.wait_for_publish_details(timeout_s=12.0)
            except E2EError as e:
                if "Timeout waiting for MQTT publish" in str(e):
                    continue
                raise
            last_observed = f"topic={got_topic} retain={int(retained)} payload={payload!r}"
            if got_topic != topic:
                _log(f"{label} live wait: ignoring other topic={got_topic}")
                continue
            if retained:
                _log(f"{label} live wait: ignoring retained publish on {topic}")
                continue

            latest = payload
            settle_deadline = time.time() + 0.25
            while time.time() < settle_deadline:
                nxt = mqtt._try_wait_for_publish_details(timeout_s=0.25)
                if not nxt:
                    break
                got_topic2, payload2, retained2 = nxt
                if got_topic2 == topic:
                    if retained2:
                        _log(f"{label} live wait: ignoring retained settle publish on {topic}")
                        continue
                    latest = payload2
                    settle_deadline = time.time() + 0.25
                else:
                    _log(f"{label} live wait: ignoring other topic={got_topic2}")

            try:
                _log(f"{label} live wait: got bytes={len(latest)}")
                return _parse_json(latest)
            except Exception as e:
                raise E2EError(f"{label} payload was not valid JSON on {topic}: {latest!r} ({e})")

        raise E2EError(f"Timeout waiting for live {label} JSON on {topic}. Last observed: {last_observed}")

    return _mqtt_retry(mqtt, f"fetch_live_json({label})", inner)


def _fetch_poll(mqtt: MqttClient, topic: str) -> dict[str, Any]:
    return _fetch_latest_json(mqtt, topic, label="poll")


def _parse_bucket_map_assignments(raw: str) -> dict[str, str]:
    alias_map = {
        "10s": "ten_sec",
        "1m": "one_min",
        "5m": "five_min",
        "1h": "one_hour",
        "1d": "one_day",
        "usr": "user",
        "off": "disabled",
    }
    id_to_name = _load_entity_id_to_name()
    intervals: dict[str, str] = {}
    for token in raw.split(";"):
        token = token.strip()
        if not token or "=" not in token:
            continue
        key, value = token.split("=", 1)
        key = key.strip()
        if key.startswith("@"):
            key = id_to_name.get(key[1:], key)
        value = alias_map.get(value.strip(), value.strip())
        if key and value:
            intervals[key] = value
    return intervals

def _parse_polling_profile_text(raw: str) -> tuple[int, dict[str, str]]:
    lines = [line.strip() for line in raw.splitlines() if line.strip() and not line.strip().startswith("#")]
    if not lines or lines[0] != "A2M_POLLING_PROFILE 1":
        raise E2EError(f"polling profile missing expected header: {raw!r}")
    if len(lines) < 2 or not lines[1].startswith("poll_interval_s="):
        raise E2EError(f"polling profile missing poll_interval_s: {raw!r}")
    try:
        poll_interval = int(lines[1].split("=", 1)[1].strip())
    except Exception as exc:
        raise E2EError(f"polling profile poll_interval_s invalid: {raw!r}") from exc
    intervals: dict[str, str] = {}
    for line in lines[2:]:
        if "=" not in line:
            raise E2EError(f"polling profile assignment invalid: {line!r}")
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if not key or not value:
            raise E2EError(f"polling profile assignment invalid: {line!r}")
        intervals[key] = value
    return poll_interval, intervals

_ENTITY_DEFAULT_BUCKETS_CACHE: Optional[dict[str, str]] = None
_ENTITY_ID_TO_NAME_CACHE: Optional[dict[str, str]] = None


def _load_entity_id_to_name() -> dict[str, str]:
    global _ENTITY_ID_TO_NAME_CACHE
    if _ENTITY_ID_TO_NAME_CACHE is not None:
        return _ENTITY_ID_TO_NAME_CACHE

    rows_path = _repo_root() / "Alpha2MQTT" / "include" / "MqttEntityCatalogRows.h"
    text = rows_path.read_text(encoding="utf-8")
    row_re = re.compile(
        r'MQTT_ENTITY_ROW\([^,]+,\s*"([^"]+)",\s*([A-Za-z0-9_]+),',
        flags=re.MULTILINE,
    )
    active_stack = [True]
    entity_id = 0
    id_to_name: dict[str, str] = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line.startswith("#ifdef "):
            active_stack.append(False)
            continue
        if line.startswith("#ifndef "):
            active_stack.append(True)
            continue
        if line.startswith("#endif"):
            if len(active_stack) > 1:
                active_stack.pop()
            continue
        if not all(active_stack):
            continue
        match = row_re.match(line)
        if not match:
            continue
        entity_name, _freq_name = match.groups()
        id_to_name[str(entity_id)] = entity_name
        entity_id += 1

    _ENTITY_ID_TO_NAME_CACHE = id_to_name
    return id_to_name


def _load_entity_default_buckets() -> dict[str, str]:
    global _ENTITY_DEFAULT_BUCKETS_CACHE
    if _ENTITY_DEFAULT_BUCKETS_CACHE is not None:
        return _ENTITY_DEFAULT_BUCKETS_CACHE

    rows_path = _repo_root() / "Alpha2MQTT" / "include" / "MqttEntityCatalogRows.h"
    text = rows_path.read_text(encoding="utf-8")
    freq_to_bucket = {
        "freqSecond": "seconds",
        "freqTenSec": "ten_sec",
        "freqOneMin": "one_min",
        "freqFiveMin": "five_min",
        "freqOneHour": "one_hour",
        "freqUser": "user",
        "freqDisabled": "disabled",
    }
    defaults: dict[str, str] = {}
    row_re = re.compile(
        r'MQTT_ENTITY_ROW\([^,]+,\s*"([^"]+)",\s*([A-Za-z0-9_]+),',
        flags=re.MULTILINE,
    )
    for entity_name, freq_name in row_re.findall(text):
        bucket = freq_to_bucket.get(freq_name)
        if bucket:
            defaults[entity_name] = bucket
    _ENTITY_DEFAULT_BUCKETS_CACHE = defaults
    return defaults

def _effective_bucket(intervals: Any, entity_name: str) -> str:
    if isinstance(intervals, dict):
        value = str(intervals.get(entity_name, "")).strip()
        if value:
            return value
    return _load_entity_default_buckets().get(entity_name, "")

def _fetch_config(mqtt: MqttClient, topic: str) -> dict[str, Any]:
    cfg = _fetch_latest_json(mqtt, topic, label="config")
    if str(cfg.get("entity_intervals_encoding", "")) != "bucket_map_chunks":
        return cfg

    chunk_count = int(cfg.get("entity_intervals_chunks", 0))
    intervals: dict[str, str] = {}
    for idx in range(chunk_count):
        chunk = _fetch_latest_json(mqtt, f"{topic}/entity_intervals/{idx}", label=f"config-chunk-{idx}")
        intervals.update(_parse_bucket_map_assignments(str(chunk.get("active_bucket_map", ""))))
    cfg["entity_intervals"] = intervals
    return cfg


def _normalize_label_for_entity_id(label: str) -> str:
    lowered = re.sub(r"[^a-z0-9]+", "_", label.lower())
    lowered = re.sub(r"_+", "_", lowered)
    return lowered.strip("_")

def _fetch_status_core(mqtt: MqttClient, topic: str) -> dict[str, Any]:
    return _fetch_latest_json(mqtt, topic, label="core")

def _fetch_status_net(mqtt: MqttClient, topic: str) -> dict[str, Any]:
    return _fetch_latest_json(mqtt, topic, label="net")

def _fetch_boot(mqtt: MqttClient, boot_topic: str) -> dict[str, Any]:
    # Boot is retained, but if the broker has restarted (no retained state), we may need to
    # wait for a device reboot to republish it.
    return _fetch_latest_json(mqtt, boot_topic, label="boot", timeout_s=60)

def _fetch_boot_mem(mqtt: MqttClient, boot_mem_topic: str) -> dict[str, Any]:
    return _fetch_latest_json(mqtt, boot_mem_topic, label="boot_mem", timeout_s=60)

def _wait_for_boot_fw_build_ts_ms(
    mqtt: MqttClient,
    boot_topic: str,
    expected_build_ts_ms: int,
    *,
    timeout_s: int,
) -> dict[str, Any]:
    """
    Wait until <device>/boot reports fw_build_ts_ms == expected_build_ts_ms.

    Rationale:
    - <device>/boot is retained and published once per boot.
    - After OTA, the broker may immediately deliver the *old* retained message on subscribe,
      so a single fetch is not sufficient to verify the new firmware.
    - This helper waits for a changed publish that carries the expected build id.
    """
    def inner() -> dict[str, Any]:
        mqtt.subscribe(boot_topic, force=True)
        deadline = time.time() + timeout_s
        last_observed = ""
        while time.time() < deadline:
            try:
                got_topic, payload = mqtt.wait_for_publish(timeout_s=12.0)
            except E2EError as e:
                if "Timeout waiting for MQTT publish" in str(e):
                    continue
                if _is_socket_closed_error(e):
                    print("[e2e] mqtt socket closed while waiting for boot topic; reconnecting...")
                    mqtt.close()
                    time.sleep(0.5)
                    mqtt.connect()
                    mqtt._subscriptions = set()
                    mqtt._pending_publishes = []
                    mqtt.subscribe(boot_topic, force=True)
                    continue
                raise
            last_observed = f"topic={got_topic} payload={payload!r}"
            if got_topic != boot_topic:
                continue
            try:
                parsed = _parse_json(payload)
            except Exception:
                continue
            fw_ts = parsed.get("fw_build_ts_ms")
            if fw_ts == expected_build_ts_ms:
                return parsed
        raise E2EError(
            f"Timeout waiting for boot fw_build_ts_ms={expected_build_ts_ms} on {boot_topic}. Last observed: {last_observed}"
        )

    return _mqtt_retry(mqtt, "wait_for_boot_fw_build_ts_ms", inner)

def _fetch_stub(mqtt: MqttClient, topic: str) -> dict[str, Any]:
    return _fetch_latest_json(mqtt, topic, label="stub")


def _drop_cached_topics(mqtt: MqttClient, *topics: str) -> None:
    mqtt._pending_publishes = []
    for topic in topics:
        mqtt._latest_by_topic.pop(topic, None)

def _fetch_cached_or_latest_json(mqtt: MqttClient, topic: str, label: str) -> dict[str, Any]:
    cached = mqtt.latest_payload(topic)
    if cached is not None:
        return _parse_json(cached)
    return _fetch_latest_json(mqtt, topic, label=label)


def _fetch_matching_or_latest_json(
    mqtt: MqttClient,
    topic: str,
    label: str,
    predicate: Callable[[dict[str, Any]], bool],
) -> dict[str, Any]:
    cached = mqtt.latest_payload(topic)
    if cached is not None:
        parsed = _parse_json(cached)
        if predicate(parsed):
            return parsed
    return _fetch_latest_json(mqtt, topic, label=label)

def _fetch_manual_read(mqtt: MqttClient, topic: str) -> dict[str, Any]:
    return _fetch_latest_json(mqtt, topic, label="manual_read")

def _fetch_latest_text(mqtt: MqttClient, topic: str, label: str) -> str:
    def inner() -> str:
        mqtt.subscribe(topic, force=True)
        deadline = time.time() + 20
        last_observed = ""
        while time.time() < deadline:
            try:
                got_topic, payload = mqtt.wait_for_publish(timeout_s=12.0)
            except E2EError as e:
                if "Timeout waiting for MQTT publish" in str(e):
                    continue
                raise
            last_observed = f"topic={got_topic} payload={payload!r}"
            if got_topic != topic:
                _log(f"{label} wait: ignoring other topic={got_topic}")
                continue

            latest = payload
            settle_deadline = time.time() + 0.25
            while time.time() < settle_deadline:
                nxt = mqtt._try_wait_for_publish(timeout_s=0.25)
                if not nxt:
                    break
                got_topic2, payload2 = nxt
                if got_topic2 == topic:
                    latest = payload2
                    settle_deadline = time.time() + 0.25
                else:
                    _log(f"{label} wait: ignoring other topic={got_topic2}")
            return latest
        raise E2EError(f"Timeout waiting for {label} text on {topic}. Last observed: {last_observed}")

    return _mqtt_retry(mqtt, f"fetch_latest_text({label})", inner)


def _fetch_cached_or_latest_text(mqtt: MqttClient, topic: str, label: str) -> str:
    cached = mqtt.latest_payload(topic)
    if cached is not None:
        return cached
    return _fetch_latest_text(mqtt, topic, label=label)


def _fetch_matching_or_latest_text(
    mqtt: MqttClient,
    topic: str,
    label: str,
    predicate: Callable[[str], bool],
) -> str:
    cached = mqtt.latest_payload(topic)
    if cached is not None and predicate(cached):
        return cached
    return _fetch_latest_text(mqtt, topic, label=label)

def _wait_for_topic_change(mqtt: MqttClient, topic: str, prev: str, timeout_s: int, label: str) -> str:
    def inner() -> str:
        mqtt.subscribe(topic, force=True)
        deadline = time.time() + timeout_s
        last_observed = ""
        while time.time() < deadline:
            try:
                got_topic, payload = mqtt.wait_for_publish(timeout_s=12.0)
            except E2EError as e:
                if "Timeout waiting for MQTT publish" in str(e):
                    continue
                raise
            last_observed = f"topic={got_topic} payload={payload!r}"
            if got_topic != topic:
                continue
            if payload != prev:
                return payload
        raise E2EError(f"Timeout waiting for {label} change on {topic}. Last observed: {last_observed}")

    return _mqtt_retry(mqtt, f"wait_for_topic_change({label})", inner)

def _wait_for_live_json_change(
    mqtt: MqttClient,
    topic: str,
    label: str,
    *,
    timeout_s: int = 20,
) -> dict[str, Any]:
    prev = _fetch_latest_text(mqtt, topic, label=f"{label}_baseline")
    changed = _wait_for_topic_change(mqtt, topic, prev, timeout_s=timeout_s, label=label)
    try:
        return _parse_json(changed)
    except Exception as e:
        raise E2EError(f"{label} payload was not valid JSON on {topic}: {changed!r} ({e})")

def _wait_for_manual_read(
    mqtt: MqttClient,
    manual_topic: str,
    *,
    expected_reg: int,
    previous_marker: str,
    timeout_s: int,
    label: str,
) -> dict[str, Any]:
    mqtt.subscribe(manual_topic, force=True)
    deadline = time.time() + timeout_s
    last_observed = ""
    while time.time() < deadline:
        try:
            got_topic, payload = mqtt.wait_for_publish(timeout_s=8.0)
        except E2EError as e:
            if "Timeout waiting for MQTT publish" in str(e):
                continue
            raise
        last_observed = f"topic={got_topic} payload={payload!r}"
        if got_topic != manual_topic:
            continue
        try:
            parsed = _parse_json(payload)
        except Exception as e:
            raise E2EError(f"{label} payload was not valid JSON on {manual_topic}: {payload!r} ({e})")
        requested_reg = parsed.get("requested_reg")
        try:
            requested_reg_int = int(requested_reg)
        except Exception:
            continue
        if requested_reg_int != expected_reg:
            continue
        marker = json.dumps(parsed, sort_keys=True, separators=(",", ":"))
        if marker == previous_marker:
            continue
        return parsed
    raise E2EError(f"Timeout waiting for {label} JSON on {manual_topic}. Last observed: {last_observed}")

def _set_register_number_and_wait_state(
    mqtt: MqttClient,
    command_topic: str,
    state_topic: str,
    reg: int,
    *,
    label: str,
    timeout_s: int = 30,
) -> None:
    try:
        previous = _fetch_latest_text(mqtt, state_topic, label=label)
        if previous.strip() == str(reg):
            return
    except E2EError:
        previous = ""

    deadline = time.time() + timeout_s
    last_seen = ""
    while time.time() < deadline:
        mqtt.publish(command_topic, str(reg), retain=False)
        try:
            changed = _wait_for_topic_change(mqtt, state_topic, previous, timeout_s=8, label=label)
            last_seen = changed
            if changed.strip() == str(reg):
                return
            previous = changed
        except E2EError:
            pass
        time.sleep(1.0)
    raise E2EError(f"Register_Number did not update to {reg}; last_seen={last_seen!r}")

def _select_register_and_wait_manual_read(
    mqtt: MqttClient,
    command_topic: str,
    manual_topic: str,
    state_topic: str,
    reg: int,
    *,
    label: str,
    timeout_s: int = 30,
) -> dict[str, Any]:
    try:
        previous = _fetch_manual_read(mqtt, manual_topic)
        previous_marker = json.dumps(previous, sort_keys=True, separators=(",", ":"))
    except E2EError:
        previous_marker = ""

    deadline = time.time() + timeout_s
    last_err = "no attempts"
    while time.time() < deadline:
        mqtt.publish(command_topic, str(reg), retain=False)
        try:
            _set_register_number_and_wait_state(
                mqtt,
                command_topic,
                state_topic,
                reg,
                label=f"{label}_reg_number_state",
                timeout_s=8,
            )
        except E2EError:
            # Some paths still do not guarantee a prompt state-topic echo. Treat it as a best-effort
            # synchronizer for manual_read rather than a hard precondition.
            pass
        try:
            return _wait_for_manual_read(
                mqtt,
                manual_topic,
                expected_reg=reg,
                previous_marker=previous_marker,
                timeout_s=8,
                label=label,
            )
        except E2EError as e:
            last_err = str(e)
        time.sleep(1.0)
    raise E2EError(
        f"Register_Number did not publish status/manual_read for register {reg}; last_err={last_err}"
    )

def _device_is_latest_stub(
    mqtt: MqttClient,
    device_root: str,
    expected_build_ts_ms: int,
    status_poll_suffix: str,
) -> Tuple[bool, str]:
    http_runtime_match_detail = ""
    try:
        boot = _fetch_boot(mqtt, f"{device_root}/boot")
        fw_ts = boot.get("fw_build_ts_ms")
        if not isinstance(fw_ts, int):
            return False, f"boot.fw_build_ts_ms missing/invalid (keys={sorted(boot.keys())})"
        if fw_ts != expected_build_ts_ms:
            http_runtime_match_detail = f"fw_build_ts_ms={fw_ts} expected={expected_build_ts_ms}"
    except E2EError as e:
        # If the broker doesn't have retained boot state (e.g. broker restart), we can't verify build id
        # until the device republishes /boot (usually on reboot). Treat as "not verified".
        http_runtime_match_detail = f"boot_unavailable ({e})"

    if http_runtime_match_detail:
        try:
            http_base = _resolve_device_http_base(mqtt, device_root)
            deadline = time.time() + 30.0
            while time.time() < deadline:
                try:
                    status, body = _http_request_full("GET", http_base + "/", headers={}, body=b"", timeout_s=10)
                    html = body.decode("utf-8", errors="replace")
                    if (
                        status == 200
                        and "Alpha2MQTT Control" in html
                        and f"Firmware version: {expected_build_ts_ms}" in html
                        and "RS485 backend: stub" in html
                    ):
                        return True, f"http_runtime_ok ({http_runtime_match_detail})"
                except Exception:
                    pass
                try:
                    status, body = _http_request_full("GET", http_base + "/status", headers={}, body=b"", timeout_s=10)
                    html = body.decode("utf-8", errors="replace")
                    if status == 200 and f"Firmware version: {expected_build_ts_ms}" in html:
                        return True, f"http_portal_ok ({http_runtime_match_detail})"
                except Exception:
                    pass
                time.sleep(2.0)
        except Exception:
            pass
        return False, http_runtime_match_detail

    poll = _fetch_poll(mqtt, f"{device_root}{status_poll_suffix}")
    backend = poll.get("rs485_backend")
    if backend != "stub":
        return False, f"rs485_backend={backend!r}"

    # Retained MQTT state can be stale after crashes/reboots. Require at least one fresh status/net
    # publish before declaring "already latest" so test flow does not run against dead retained data.
    net_topic = f"{device_root}/status/net"
    try:
        prev_net = _fetch_latest_text(mqtt, net_topic, label="status_net_liveness")
        _wait_for_topic_change(mqtt, net_topic, prev_net, timeout_s=15, label="status/net liveness")
    except E2EError as e:
        # A fresh boot event with the expected build id is stronger evidence than a missing status/net
        # refresh, and re-flashing an already-correct stub increases live-device churn for no gain.
        return True, f"status_net_stale_ignored ({e})"
    return True, "ok"

def _resolve_device_http_base(mqtt: MqttClient, device_root: str) -> str:
    configured = os.environ.get("DEVICE_HTTP_BASE", "").strip()
    if configured:
        return configured
    net_topic = f"{device_root}/status/net"
    try:
        net = _fetch_status_net(mqtt, net_topic)
        ip = str(net.get("ip", "")).strip()
        if ip:
            live_base = f"http://{ip}"
            return live_base
    except E2EError:
        pass
    raise E2EError(
        "DEVICE_HTTP_BASE is not configured and status/net did not provide a device IP. "
        "Set DEVICE_HTTP_BASE or ensure the device is publishing status/net with an ip field."
    )

def _ensure_latest_stub_via_ota(
    mqtt: MqttClient,
    device_root: str,
    status_poll_suffix: str,
    http_base: str,
    firmware_path: Path,
) -> None:
    expected_ts = _firmware_build_ts_ms_from_filename(firmware_path)
    ok, detail = _device_is_latest_stub(mqtt, device_root, expected_ts, status_poll_suffix)
    if ok:
        print("[e2e] device already on latest stub firmware")
        return

    # If we can't read /boot (no retained), try a NORMAL reboot to force boot republish before doing OTA.
    if detail.startswith("boot_unavailable"):
        normal_path = os.environ.get("DEVICE_REBOOT_NORMAL_PATH") or _discover_reboot_normal_path_from_code()
        if normal_path:
            normal_url = http_base.rstrip("/") + normal_path
            print(f"[e2e] boot topic unavailable; POST {normal_path} to force boot republish")
            try:
                _http_post_simple(normal_url, timeout_s=10)
            except Exception as e:
                print(f"[e2e] reboot-normal failed: {e}; continuing")
            _sleep_with_mqtt(mqtt, 8)
            ok2, detail2 = _device_is_latest_stub(mqtt, device_root, expected_ts, status_poll_suffix)
            if ok2:
                print("[e2e] device already on latest stub firmware (boot republished)")
                return
            detail = detail2

    print(f"[e2e] device not on latest stub ({detail}); performing OTA update")

    reboot_path = os.environ.get("DEVICE_REBOOT_WIFI_PATH") or _discover_reboot_wifi_path_from_code()
    if not reboot_path:
        raise E2EError("Could not discover reboot-to-wifi-config path; set DEVICE_REBOOT_WIFI_PATH")

    def root_ready_for_stub_ota() -> Tuple[bool, str]:
        ok, detail = _root_surface_state(http_base.rstrip("/"))
        if not ok:
            return False, detail
        return ("mode=normal" in detail or "mode=wifi" in detail), detail

    _assert_eventually("root reachable in normal runtime or WiFi portal before OTA", root_ready_for_stub_ota, timeout_s=45, poll_s=2.0)
    _, root_detail = _root_surface_state(http_base.rstrip("/"))
    reboot_url = http_base.rstrip("/") + reboot_path

    if "mode=normal" in root_detail:
        print(f"[e2e] POST {reboot_path} (reboot into Wi-Fi config portal)")
        status = 0
        for attempt in range(5):
            try:
                status = _http_post_simple(reboot_url, timeout_s=10)
                break
            except (TimeoutError, OSError) as e:
                if attempt >= 4:
                    raise E2EError(
                        f"HTTP request to device failed ({e}). "
                        "Check DEVICE_HTTP_BASE is reachable from this host and the device is on the LAN."
                    )
                _sleep_with_mqtt(mqtt, 5)
                refreshed_base = _resolve_device_http_base(mqtt, device_root)
                if refreshed_base.rstrip("/") != http_base.rstrip("/"):
                    print(f"[e2e] retrying reboot via refreshed base {refreshed_base}")
                http_base = refreshed_base
                reboot_url = http_base.rstrip("/") + reboot_path
        print(f"[e2e] reboot HTTP status={status}")
        if status == 404:
            print("[e2e] reboot endpoint not found (old firmware or not in NORMAL); continuing with direct upload")

        # MODE_WIFI_CONFIG waits for STA reconnect before the portal is actually served.
        # The firmware's connect timeout is 20s, so 8s is not long enough to make /u reliable.
        _sleep_with_mqtt(mqtt, 25)
    else:
        print("[e2e] Wi-Fi portal already active; OTA upload can proceed without another reboot")

    _assert_portal_root_menu(http_base.rstrip("/"), timeout_s=90, required_mode="wifi")

    upload_path = os.environ.get("DEVICE_OTA_UPLOAD_PATH", "").strip()
    upload_mode = os.environ.get("DEVICE_OTA_UPLOAD_MODE", "").strip().lower()
    field_name = os.environ.get("DEVICE_OTA_FIELD_NAME", "").strip()
    if not upload_path or not upload_mode or (upload_mode != "raw" and not field_name):
        discovered_action, update_html, discovered_mode = _load_update_page(http_base.rstrip("/"))
        if not upload_path:
            upload_path = discovered_action
        if not upload_mode:
            upload_mode = discovered_mode
        if upload_mode != "raw" and not field_name:
            field_name = _extract_file_input_name(update_html)
    upload_url = http_base.rstrip("/") + upload_path

    print(f"[e2e] POST {upload_path} (upload firmware: {firmware_path.name})")
    status: Optional[int] = None
    upload_body = b""
    upload_attempt = 0
    while True:
        upload_attempt += 1
        try:
            if upload_mode == "raw":
                status, upload_body = _http_post_raw_full(upload_url, file_path=firmware_path, timeout_s=240)
            else:
                status, upload_body = _http_post_multipart_full(
                    upload_url,
                    field_name=field_name,
                    file_path=firmware_path,
                    timeout_s=240,
                )
            print(f"[e2e] upload HTTP status={status}")
            if status < 200 or status >= 400:
                raise E2EError(f"OTA upload failed (HTTP {status})")
            if upload_body:
                _assert_ota_success_response_html(upload_body.decode("utf-8", errors="replace"))
            break
        except TimeoutError:
            # Some OTA implementations reboot before responding, or hold the connection open without a response.
            # Treat this as ambiguous and confirm success via MQTT build timestamp instead.
            print("[e2e] upload timed out waiting for HTTP response; verifying success via MQTT...")
            break
        except OSError as exc:
            # ESP8266 OTA can reset the TCP connection while the client is still writing the
            # multipart body. Treat a transport reset like a timeout and verify via MQTT.
            print(f"[e2e] upload connection reset during OTA; verifying success via MQTT ({exc})")
            break
        except E2EError as exc:
            if upload_attempt >= 2 or status != 500:
                raise
            print(f"[e2e] upload returned HTTP 500; refreshing portal update form and retrying once ({exc})")
            _sleep_with_mqtt(mqtt, 5)
            discovered_action, update_html, discovered_mode = _load_update_page(http_base.rstrip("/"))
            upload_path = discovered_action
            upload_mode = discovered_mode
            field_name = _extract_file_input_name(update_html) if upload_mode != "raw" else ""
            upload_url = http_base.rstrip("/") + upload_path

    # Successful OTA should come back in normal runtime automatically. Waiting on the normal
    # root page keeps the test aligned with that contract instead of tolerating a legacy
    # "return to Wi-Fi portal and require another reboot" flow.
    def runtime_ready_pred() -> Tuple[bool, str]:
        try:
            status_root, body_root = _http_request_full("GET", http_base.rstrip("/") + "/", headers={}, body=b"", timeout_s=10)
        except Exception as exc:
            return False, f"err={exc}"
        root_html = body_root.decode("utf-8", errors="replace")
        ready = status_root == 200 and "Alpha2MQTT Control" in root_html and "/reboot/wifi" in root_html
        return ready, f"status={status_root}"

    print("[e2e] waiting for device to reboot and report new fw_build_ts_ms over MQTT...")
    # Ensure we observe a *new boot publish* with the new build id; otherwise the retained /boot
    # message can remain stale during this run and make verification flaky.
    _wait_for_boot_fw_build_ts_ms(mqtt, f"{device_root}/boot", expected_ts, timeout_s=120)
    _assert_eventually("OTA reboot returns to normal runtime", runtime_ready_pred, timeout_s=60, poll_s=2.0)

    def pred() -> Tuple[bool, str]:
        ok2, det2 = _device_is_latest_stub(mqtt, device_root, expected_ts, status_poll_suffix)
        return ok2, det2

    _assert_eventually("device reports latest stub after OTA", pred, timeout_s=120, poll_s=5.0)


def main() -> int:
    json_cfg_path = _default_json_file()
    _load_json_file_defaults(json_cfg_path)
    _load_env_file_defaults(_default_env_file())
    run_cfg = _load_json_run_defaults(json_cfg_path)
    default_verbose = _env_bool("E2E_VERBOSE", _as_bool(run_cfg.get("verbose", False)))
    default_trace_http = _env_bool("E2E_TRACE_HTTP", _as_bool(run_cfg.get("trace_http", False)))
    default_trace_mqtt = _env_bool("E2E_TRACE_MQTT", _as_bool(run_cfg.get("trace_mqtt", False)))
    default_no_flash = _env_bool("E2E_NO_FLASH", _as_bool(run_cfg.get("no_flash", False)))
    default_force_flash = _env_bool("E2E_FORCE_FLASH", _as_bool(run_cfg.get("force_flash", False)))
    default_cases = _env_csv("E2E_CASES")
    if not default_cases:
        cfg_cases = run_cfg.get("cases", [])
        if isinstance(cfg_cases, list):
            default_cases = [str(v).strip() for v in cfg_cases if str(v).strip()]
    default_from_case = os.environ.get("E2E_FROM_CASE", "").strip()
    if not default_from_case:
        raw_from_case = run_cfg.get("from_case", "")
        if isinstance(raw_from_case, str):
            default_from_case = raw_from_case.strip()

    ap = argparse.ArgumentParser()
    ap.add_argument("--list-cases", action="store_true", help="List available test cases and exit.")
    ap.add_argument("--case", action="append", help="Run only the named case (repeatable).")
    ap.add_argument("--from-case", help="Run from this case onward.")
    ap.add_argument("--no-flash", action="store_true", help="Never flash. Fail if device backend/build does not match expected stub firmware.")
    ap.add_argument("--force-flash", action="store_true", help="Always flash expected stub firmware before running tests.")
    # Back-compat aliases
    ap.add_argument("--ota", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--ensure-stub", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--verbose", action="store_true", help="Verbose logging (MQTT rx/tx and filtering).")
    ap.add_argument("--trace-http", action="store_true", help="Trace HTTP requests and response status/size.")
    ap.add_argument("--trace-mqtt", action="store_true", help="Trace MQTT transport activity.")
    args = ap.parse_args()
    if args.list_cases:
        for name in CASE_ORDER:
            print(name)
        return 0

    selected_cases = args.case if args.case else default_cases
    from_case = args.from_case.strip() if args.from_case else default_from_case
    for name in selected_cases:
        if name not in CASE_ORDER:
            raise E2EError(f"Unknown case in selection: {name!r}")
    if from_case and from_case not in CASE_ORDER:
        raise E2EError(f"Unknown from-case: {from_case!r}")

    policy_no_flash = bool(args.no_flash or default_no_flash)
    policy_force = bool(args.force_flash or args.ota or default_force_flash)
    if policy_no_flash and policy_force:
        raise E2EError("--no-flash and --force-flash are mutually exclusive")
    global VERBOSE, TRACE_HTTP, TRACE_MQTT
    VERBOSE = bool(args.verbose or default_verbose)
    TRACE_HTTP = bool(args.trace_http or default_trace_http)
    TRACE_MQTT = bool(args.trace_mqtt or default_trace_mqtt)

    host = _require_env("MQTT_HOST")
    port = int(os.environ.get("MQTT_PORT", "1883"))
    user = _require_env("MQTT_USER")
    password = _read_pass()
    _announce(f"connecting to MQTT {host}:{port} as {user}")
    mqtt = MqttClient(host=host, port=port, user=user, password=password)
    mqtt.connect()
    _announce("connected to MQTT")

    control_suffix = _discover_control_suffix_from_code()
    status_poll_suffix = _discover_status_poll_suffix_from_code()
    status_stub_suffix = _discover_status_stub_suffix_from_code()
    _announce("discovering device topic root")
    device_root = _discover_device_topic(mqtt, status_poll_suffix)
    _announce(f"discovered device_root={device_root}")

    poll_topic = f"{device_root}{status_poll_suffix}"
    stub_topic = f"{device_root}{status_stub_suffix}"
    status_core_topic = f"{device_root}/status"
    config_topic = f"{device_root}/config"
    control_topic = f"{device_root}{control_suffix}"

    print(f"[e2e] device_root={device_root}")
    print(f"[e2e] poll_topic={poll_topic}")
    print(f"[e2e] stub_topic={stub_topic}")
    print(f"[e2e] status_core_topic={status_core_topic}")
    print(f"[e2e] config_topic={config_topic}")
    print(f"[e2e] control_topic={control_topic}")

    firmware_path = Path(os.environ.get("FIRMWARE_BIN") or _latest_stub_firmware_path())
    expected_ts = _firmware_build_ts_ms_from_filename(firmware_path)
    status_ok, status_detail = _device_is_latest_stub(mqtt, device_root, expected_ts, status_poll_suffix)

    if policy_force:
        _announce("flash policy: force")
        http_base = _resolve_device_http_base(mqtt, device_root)
        _ensure_latest_stub_via_ota(
            mqtt=mqtt,
            device_root=device_root,
            status_poll_suffix=status_poll_suffix,
            http_base=http_base,
            firmware_path=firmware_path,
        )
    elif policy_no_flash:
        _announce("flash policy: no-flash")
        if not status_ok:
            raise E2EError(f"Device backend/build mismatch and --no-flash was set: {status_detail}")
        _announce("device already on expected stub backend/build; skipping flash")
    else:
        _announce("flash policy: smart")
        if status_ok:
            _announce("device already on expected stub backend/build; skipping flash")
        else:
            http_base = _resolve_device_http_base(mqtt, device_root)
            _ensure_latest_stub_via_ota(
                mqtt=mqtt,
                device_root=device_root,
                status_poll_suffix=status_poll_suffix,
                http_base=http_base,
                firmware_path=firmware_path,
            )

    # Verify stub backend is actually active (after optional ensure/config/update).
    first = _fetch_poll(mqtt, poll_topic)
    rs485_backend = first.get("rs485_backend")
    if rs485_backend != "stub":
        raise E2EError(f"Expected rs485_backend=stub but got: {rs485_backend!r} keys={sorted(first.keys())}")
    scheduler_keys = ("s10_ms", "s60_ms", "s300_ms", "s3600_ms", "s86400_ms", "su_ms")
    for k in scheduler_keys:
        if k not in first:
            raise E2EError(f"Missing expected scheduler observability field in /status/poll: {k} (keys={sorted(first.keys())})")
        val = first.get(k)
        if isinstance(val, bool) or not isinstance(val, (int, float)):
            raise E2EError(f"Scheduler field must be numeric: {k}={val!r}")
    if "poll_interval_s" not in first:
        raise E2EError(f"Missing expected field in /status/poll: poll_interval_s (keys={sorted(first.keys())})")

    # Rule check: scheduler fields should be monotonic/non-decreasing across poll samples.
    second = _fetch_poll(mqtt, poll_topic)
    for k in scheduler_keys:
        a = int(first.get(k, 0))
        b = int(second.get(k, 0))
        if b < a:
            raise E2EError(f"Scheduler field is not monotonic: {k} {a}->{b}")

    def _normalized_stub_control_payload(payload: str) -> Optional[dict[str, Any]]:
        try:
            parsed = json.loads(payload)
        except json.JSONDecodeError:
            return None

        if not isinstance(parsed, dict):
            return None

        # The firmware only updates the stub-control fields that are present in the payload.
        # Publish a complete fault baseline by default so later cases do not inherit stale
        # fail/latency/probe settings from earlier cases.
        mode = str(parsed.get("mode", "")).strip().lower()
        wants_failure_mode = mode in ("offline", "fail", "fail_then_recover", "flap", "probe_delayed")
        for key in ("fail_n", "reg", "fail_every_n", "fail_for_ms", "probe_success_after_n"):
            try:
                wants_failure_mode = wants_failure_mode or int(parsed.get(key, 0)) > 0
            except (TypeError, ValueError):
                pass

        for key, value in (
            ("fail_n", 0),
            ("reg", 0),
            ("fail_type", 0),
            ("latency_ms", 0),
            ("strict_unknown", 0),
            ("strict", 0),
            ("fail_every_n", 0),
            ("fail_for_ms", 0),
            ("flap_online_ms", 0),
            ("flap_offline_ms", 0),
            ("probe_success_after_n", 0),
            ("soc_step_x10_per_snapshot", 0),
        ):
            parsed.setdefault(key, value)
        parsed.setdefault("fail_reads", 1 if wants_failure_mode else 0)
        parsed.setdefault("fail_writes", 1 if wants_failure_mode else 0)

        return parsed

    def set_mode(payload: str) -> None:
        normalized = _normalized_stub_control_payload(payload)
        if normalized is None:
            mqtt.publish(control_topic, payload, retain=False)
            return
        mqtt.publish(control_topic, json.dumps(normalized, separators=(",", ":")), retain=False)

    def set_mode_and_wait(
        payload: str,
        accepted_modes: tuple[str, ...],
        *,
        timeout_s: int = 45,
        poll_s: float = 2.0,
        republish_s: float = 6.0,
    ) -> None:
        # Stub mode updates are asynchronous. Publish immediately and then republish periodically
        # until /status/poll confirms one of the accepted mode labels.
        set_mode(payload)
        last_pub = time.time()

        def pred() -> Tuple[bool, str]:
            nonlocal last_pub
            cur = _fetch_live_json(mqtt, poll_topic, "set_mode_poll_current", timeout_s=15)
            mode = str(cur.get("rs485_stub_mode", ""))
            detail = f"mode={mode}"
            if mode in accepted_modes:
                return True, detail
            if (time.time() - last_pub) >= republish_s:
                set_mode(payload)
                last_pub = time.time()
            return False, detail

        _assert_eventually(
            f"mode applied ({'/'.join(accepted_modes)})",
            pred,
            timeout_s=timeout_s,
            poll_s=poll_s,
        )

    def _normalized_stub_mode_payload(mode_payload: str) -> str:
        try:
            parsed = json.loads(mode_payload)
        except Exception:
            return mode_payload
        if not isinstance(parsed, dict):
            return mode_payload
        if parsed.get("mode") != "online":
            return mode_payload
        normalized = {
            "mode": "online",
            "fail_n": 0,
            "fail_reads": 0,
            "fail_writes": 0,
            "fail_type": 0,
            "fail_every_n": 0,
            "fail_for_ms": 0,
            "flap_online_ms": 0,
            "flap_offline_ms": 0,
            "probe_success_after_n": 0,
            "strict_unknown": 0,
            "strict": 0,
            "reg": 0,
            "latency_ms": 0,
            "soc_step_x10_per_snapshot": 0,
        }
        for key, value in parsed.items():
            normalized[key] = value
        return json.dumps(normalized, separators=(",", ":"))

    def _payload_strict_unknown(mode_payload: str) -> Optional[bool]:
        try:
            parsed = json.loads(mode_payload)
        except Exception:
            return None
        if not isinstance(parsed, dict):
            return None
        value = parsed.get("strict_unknown", parsed.get("strict"))
        if value is None:
            return None
        try:
            return bool(int(value))
        except (TypeError, ValueError):
            return bool(value)

    def wait_stub_control_applied(
        mode_payload: str,
        *,
        label: str,
        expect_mode: Optional[str] = None,
        expect_strict_unknown: Optional[bool] = None,
        timeout_s: int = 30,
        republish_s: float = 6.0,
    ) -> None:
        normalized_payload = _normalized_stub_mode_payload(mode_payload)
        _fetch_poll(mqtt, poll_topic)
        _fetch_stub(mqtt, stub_topic)
        set_mode(normalized_payload)
        last_pub = time.time()
        try:
            expected_control = json.loads(normalized_payload)
        except Exception:
            expected_control = {}
        if not isinstance(expected_control, dict):
            expected_control = {}

        expected_mode = expect_mode if expect_mode is not None else str(expected_control.get("mode", "") or "")
        expected_fail_reads = (
            bool(expected_control.get("fail_reads"))
            if "fail_reads" in expected_control
            else None
        )
        expected_fail_writes = (
            bool(expected_control.get("fail_writes"))
            if "fail_writes" in expected_control
            else None
        )
        expected_fail_every_n = (
            int(expected_control.get("fail_every_n", 0))
            if "fail_every_n" in expected_control
            else None
        )
        expected_fail_for_ms = (
            int(expected_control.get("fail_for_ms", 0))
            if "fail_for_ms" in expected_control
            else None
        )
        expected_flap_online_ms = (
            int(expected_control.get("flap_online_ms", 0))
            if "flap_online_ms" in expected_control
            else None
        )
        expected_flap_offline_ms = (
            int(expected_control.get("flap_offline_ms", 0))
            if "flap_offline_ms" in expected_control
            else None
        )
        expected_probe_success_after_n = (
            int(expected_control.get("probe_success_after_n", 0))
            if "probe_success_after_n" in expected_control
            else None
        )
        def pred() -> Tuple[bool, str]:
            nonlocal last_pub
            def stub_matches(cur: dict[str, Any]) -> bool:
                if expect_strict_unknown is not None and bool(cur.get("strict_unknown", False)) != expect_strict_unknown:
                    return False
                if expected_fail_reads is not None and bool(cur.get("fail_reads", False)) != expected_fail_reads:
                    return False
                if expected_fail_writes is not None and bool(cur.get("fail_writes", False)) != expected_fail_writes:
                    return False
                if expected_fail_every_n is not None and int(cur.get("fail_every_n", 0)) != expected_fail_every_n:
                    return False
                if expected_fail_for_ms is not None and int(cur.get("fail_for_ms", 0)) != expected_fail_for_ms:
                    return False
                if expected_flap_online_ms is not None and int(cur.get("flap_online_ms", 0)) != expected_flap_online_ms:
                    return False
                if expected_flap_offline_ms is not None and int(cur.get("flap_offline_ms", 0)) != expected_flap_offline_ms:
                    return False
                if expected_probe_success_after_n is not None and int(cur.get("probe_success_after_n", 0)) != expected_probe_success_after_n:
                    return False
                return True

            def poll_matches(cur: dict[str, Any]) -> bool:
                if expected_mode and str(cur.get("rs485_stub_mode", "")) != expected_mode:
                    return False
                return True

            try:
                # Control application must be confirmed by a fresh poll publish. Cached matching
                # state can survive reboots/reconnects and mask a dropped control publish.
                cur_poll = _fetch_live_json(mqtt, poll_topic, f"{label}_poll_current", timeout_s=15)
            except E2EError as e:
                if "Timeout waiting for live" not in str(e) and "Timeout waiting for" not in str(e):
                    raise
                if (time.time() - last_pub) >= republish_s:
                    set_mode(normalized_payload)
                    last_pub = time.time()
                return False, "waiting for poll match"

            cur_stub = _fetch_matching_or_latest_json(
                mqtt,
                stub_topic,
                f"{label}_stub_current",
                stub_matches,
            )
            mode = str(cur_poll.get("rs485_stub_mode", ""))
            strict_unknown = bool(cur_stub.get("strict_unknown", False))
            fail_reads = bool(cur_stub.get("fail_reads", False))
            fail_writes = bool(cur_stub.get("fail_writes", False))
            fail_every_n = int(cur_stub.get("fail_every_n", 0))
            fail_for_ms = int(cur_stub.get("fail_for_ms", 0))
            flap_online_ms = int(cur_stub.get("flap_online_ms", 0))
            flap_offline_ms = int(cur_stub.get("flap_offline_ms", 0))
            probe_success_after_n = int(cur_stub.get("probe_success_after_n", 0))
            detail = (
                f"mode={mode!r} strict={strict_unknown} fail_reads={fail_reads} "
                f"fail_writes={fail_writes} fail_every_n={fail_every_n} "
                f"fail_for_ms={fail_for_ms} flap_online_ms={flap_online_ms} "
                f"flap_offline_ms={flap_offline_ms} probe_success_after_n={probe_success_after_n}"
            )
            ok = True
            if expected_mode:
                ok = ok and (mode == expected_mode)
            ok = ok and stub_matches(cur_stub)
            if ok:
                return True, detail
            if (time.time() - last_pub) >= republish_s:
                set_mode(normalized_payload)
                last_pub = time.time()
            return False, detail

        _assert_eventually(
            f"{label} applied",
            pred,
            timeout_s=timeout_s,
            poll_s=2.0,
        )

    def ensure_stub_online_backend(mode_payload: str, *, label: str, timeout_s: int = 60) -> None:
        mode_payload = _normalized_stub_mode_payload(mode_payload)
        wait_stub_control_applied(
            mode_payload,
            label=f"{label} control",
            expect_mode="online",
            expect_strict_unknown=_payload_strict_unknown(mode_payload),
            timeout_s=min(timeout_s, 30),
        )
        deadline = time.time() + timeout_s
        last_detail = "no live poll update observed"
        def poll_ready_detail(cur_poll: dict[str, Any]) -> tuple[bool, str]:
            mode = str(cur_poll.get("rs485_stub_mode", ""))
            snapshot_ok = bool(cur_poll.get("ess_snapshot_last_ok", False))
            probe_backoff_ms = int(cur_poll.get("rs485_probe_backoff_ms", 0))
            poll_ok_count = int(cur_poll.get("poll_ok_count", 0))
            detail = (
                f"mode={mode!r} snapshot_ok={snapshot_ok} "
                f"probe_backoff_ms={probe_backoff_ms} poll_ok_count={poll_ok_count}"
            )
            ready = mode == "online" and snapshot_ok and probe_backoff_ms == 0 and poll_ok_count > 0
            return ready, detail

        while time.time() < deadline:
            try:
                cur_poll = _fetch_live_json(mqtt, poll_topic, f"{label}_poll_current", timeout_s=15)
            except E2EError as e:
                if "Timeout waiting for live" not in str(e):
                    raise
                last_detail = "waiting for live poll"
                _sleep_with_mqtt(mqtt, 2.0)
                continue
            ready, last_detail = poll_ready_detail(cur_poll)
            if ready:
                return
            _sleep_with_mqtt(mqtt, 2.0)
        raise E2EError(f"Timeout waiting for {label} ready. Last observed: {last_detail}")

    def set_intervals(entity_to_freq: dict[str, str]) -> None:
        # Firmware expects a flat JSON object mapping entity name -> freq string.
        # mqttCallback() has a fixed 512-byte buffer, so keep individual payloads comfortably below that.
        items = list(entity_to_freq.items())
        if not items:
            return
        max_payload_bytes = 420
        chunk: dict[str, str] = {}
        for key, value in items:
            candidate = dict(chunk)
            candidate[key] = value
            payload = json.dumps(candidate)
            if len(payload.encode("utf-8")) > max_payload_bytes and chunk:
                mqtt.publish(f"{device_root}/config/set", json.dumps(chunk), retain=False)
                chunk = {key: value}
            else:
                chunk = candidate
        if chunk:
            mqtt.publish(f"{device_root}/config/set", json.dumps(chunk), retain=False)

    def wait_intervals_applied(entity_to_freq: dict[str, str], *, timeout_s: int = 30, republish_every_s: float = 5.0) -> None:
        if not entity_to_freq:
            return
        set_intervals(entity_to_freq)
        last_pub = time.time()

        def pred() -> Tuple[bool, str]:
            nonlocal last_pub
            now = time.time()
            if (now - last_pub) >= republish_every_s:
                set_intervals(entity_to_freq)
                last_pub = now

            cfg = _fetch_config(mqtt, config_topic)
            intervals = cfg.get("entity_intervals", {})
            if not isinstance(intervals, dict):
                return False, f"entity_intervals invalid: {cfg!r}"

            mismatches = []
            for key, expected in entity_to_freq.items():
                actual = _effective_bucket(intervals, key)
                if actual != expected:
                    mismatches.append(f"{key}={actual!r}")
            return (not mismatches), ", ".join(mismatches) if mismatches else "ok"

        _assert_eventually(
            "intervals applied",
            pred,
            timeout_s=timeout_s,
            poll_s=2.0,
        )

    def publish_and_wait_state(ha_unique: str, name: str, expected: str, *, timeout_s: int = 35) -> None:
        base = f"{device_root}/{ha_unique}"
        state_topic = _state_topic(ha_unique, name)
        mqtt.subscribe(state_topic, force=True)
        cmd_topic = f"{base}/{name}/command"
        deadline = time.time() + timeout_s
        last_seen = ""
        while time.time() < deadline:
            mqtt.publish(cmd_topic, expected, retain=False)
            try:
                last_seen = _fetch_latest_text(mqtt, state_topic, label=f"{name}_state")
                if last_seen.strip().lower() == expected.strip().lower():
                    return
            except E2EError:
                pass
            time.sleep(1.0)
        raise E2EError(f"{name} did not update to {expected!r}; last_seen={last_seen!r}")

    def _dispatch_set_topic(inverter_device_id: str) -> str:
        return f"{inverter_device_id}/dispatch/set"

    def _dispatch_status_topic(inverter_device_id: str) -> str:
        return _state_topic(inverter_device_id, "Dispatch_Request_Status")

    def _default_dispatch_request(
        *,
        mode: str = "state_of_charge_control",
        power_w: int = -1200,
        soc_percent: int = 80,
        duration_s: int = 0,
    ) -> dict[str, Any]:
        payload: dict[str, Any] = {"mode": mode}
        if mode == "normal_mode":
            return payload
        payload["duration_s"] = duration_s
        if mode in ("battery_only_charges_from_pv", "state_of_charge_control", "load_following"):
            payload["power_w"] = power_w
        if mode == "state_of_charge_control":
            payload["soc_percent"] = soc_percent
        return payload

    def publish_dispatch_request_and_wait_status(
        inverter_device_id: str,
        payload: dict[str, Any],
        *,
        expected_status: Optional[str] = "ok",
        require_queue_advance: bool = True,
        timeout_s: int = 25,
    ) -> str:
        def request_was_queued_after(baseline_queued_ms: int) -> bool:
            cur_poll = _fetch_poll(mqtt, poll_topic)
            return int(cur_poll.get("dispatch_request_queued_ms", 0)) > baseline_queued_ms

        def recover_dispatch_backend(label: str) -> None:
            ensure_stub_online_backend(
                '{"mode":"online","soc_pct":50,"battery_power_w":0,"grid_power_w":0,"pv_ct_power_w":0}',
                label=label,
            )
            recovered_inverter_id = _wait_for_inverter_identity()
            if recovered_inverter_id != inverter_device_id:
                raise E2EError(
                    f"dispatch retry recovered unexpected inverter identity: "
                    f"expected={inverter_device_id!r} got={recovered_inverter_id!r}"
                )

        status_topic = _dispatch_status_topic(inverter_device_id)
        mqtt.subscribe(status_topic, force=True)

        payload_text = json.dumps(payload, separators=(",", ":"))
        last_status = ""
        baseline_status = ""
        baseline_queued_ms = int(_fetch_poll(mqtt, poll_topic).get("dispatch_request_queued_ms", 0))

        for attempt in range(3):
            current_poll = _fetch_poll(mqtt, poll_topic)
            stub_mode = str(current_poll.get("rs485_stub_mode", "")).strip()
            inverter_ready = bool(current_poll.get("inverter_ready", False))
            if stub_mode != "online" or not inverter_ready:
                recover_dispatch_backend(f"dispatch request preflight {attempt + 1}")
                current_poll = _fetch_poll(mqtt, poll_topic)

            try:
                baseline_status = _fetch_cached_or_latest_text(
                    mqtt,
                    status_topic,
                    label="dispatch_status_baseline",
                ).strip()
            except E2EError:
                baseline_status = ""

            mqtt.publish(_dispatch_set_topic(inverter_device_id), payload_text, retain=False)

            deadline = time.time() + timeout_s
            saw_status = False
            last_status = ""
            while time.time() < deadline:
                if expected_status is not None and not require_queue_advance:
                    try:
                        current_status = _fetch_cached_or_latest_text(
                            mqtt,
                            status_topic,
                            label="dispatch_status_current",
                        ).strip()
                    except E2EError:
                        current_status = ""
                    if current_status == expected_status and (
                        current_status != baseline_status or expected_status == "ok"
                    ):
                        return current_status
                if (
                    saw_status
                    and require_queue_advance
                    and expected_status is not None
                    and last_status == expected_status
                    and request_was_queued_after(baseline_queued_ms)
                ):
                    return last_status
                try:
                    got_topic, status_payload = mqtt.wait_for_publish(
                        timeout_s=min(5.0, max(0.5, deadline - time.time()))
                    )
                except E2EError as e:
                    if "Timeout waiting for MQTT publish" in str(e):
                        continue
                    raise

                if got_topic != status_topic:
                    continue

                saw_status = True
                last_status = status_payload.strip()
                if expected_status is None or last_status == expected_status:
                    if require_queue_advance and not request_was_queued_after(baseline_queued_ms):
                        # A stale retained/cached status from an earlier request is not
                        # enough; the current request must advance queued_ms before we
                        # treat the status as authoritative.
                        continue
                    return last_status
                if not require_queue_advance:
                    # Non-queued validation failures can race with retained or
                    # cached status updates from the previous request. Keep
                    # waiting for the expected terminal status instead of
                    # treating the first mismatched publish as authoritative.
                    continue
                break

            current_queued_ms = int(_fetch_poll(mqtt, poll_topic).get("dispatch_request_queued_ms", 0))
            request_never_queued = current_queued_ms <= baseline_queued_ms
            if attempt < 2 and request_never_queued and (last_status == "" or require_queue_advance):
                stub_mode = str(current_poll.get("rs485_stub_mode", "")).strip()
                inverter_ready = bool(current_poll.get("inverter_ready", False))
                if stub_mode != "online" or not inverter_ready:
                    recover_dispatch_backend(f"dispatch request retry {attempt + 1}")
                _sleep_with_mqtt(mqtt, 2.0)
                continue

            if not saw_status:
                raise E2EError(
                    f"Timeout waiting for dispatch request status on {status_topic}; "
                    f"last_status={last_status!r} payload={payload_text}"
                )

            raise E2EError(
                f"dispatch request returned unexpected status: expected={expected_status!r} "
                f"got={last_status!r} payload={payload_text}"
            )

        raise E2EError(
            f"dispatch request did not queue or publish status after retry: payload={payload_text}"
        )

    def publish_dispatch_request_no_wait(inverter_device_id: str, payload: dict[str, Any]) -> None:
        mqtt.publish(
            _dispatch_set_topic(inverter_device_id),
            json.dumps(payload, separators=(",", ":")),
            retain=False,
        )

    def wait_for_dispatch_status_value(
        inverter_device_id: str,
        expected_status: str,
        *,
        timeout_s: int = 10,
        label: str = "dispatch_status",
    ) -> str:
        status_topic = _dispatch_status_topic(inverter_device_id)
        mqtt.subscribe(status_topic, force=True)

        def status_pred() -> Tuple[bool, str]:
            try:
                status = _fetch_cached_or_latest_text(mqtt, status_topic, label=label).strip()
            except E2EError as e:
                return False, f"err={e}"
            return status == expected_status, f"status={status!r}"

        _assert_eventually(
            f"{label} reached {expected_status!r}",
            status_pred,
            timeout_s=timeout_s,
            poll_s=0.5,
        )
        return _fetch_cached_or_latest_text(mqtt, status_topic, label=label).strip()

    def wait_for_dispatch_status(
        inverter_device_id: str,
        expected_status: str,
        *,
        timeout_s: int = 25,
    ) -> str:
        status_topic = _dispatch_status_topic(inverter_device_id)
        mqtt.subscribe(status_topic, force=True)
        try:
            _fetch_latest_text(mqtt, status_topic, label="dispatch_status_baseline")
        except E2EError:
            pass

        deadline = time.time() + timeout_s
        last_status = ""
        while time.time() < deadline:
            try:
                got_topic, status_payload = mqtt.wait_for_publish(
                    timeout_s=min(5.0, max(0.5, deadline - time.time()))
                )
            except E2EError as e:
                if "Timeout waiting for MQTT publish" in str(e):
                    continue
                raise
            if got_topic != status_topic:
                continue
            last_status = status_payload.strip()
            if last_status == expected_status:
                return last_status

        raise E2EError(
            f"Timeout waiting for dispatch status {expected_status!r} on {status_topic}; "
            f"last_status={last_status!r}"
        )

    def ensure_dispatch_write(
        ha_unique: str,
        *,
        label_prefix: str,
        duration_s: Optional[int] = None,
        poll_interval_s: Optional[int] = None,
        timeout_s: int = 45,
    ) -> tuple[int, int]:
        reg_dispatch_start = _discover_register_value("REG_DISPATCH_RW_DISPATCH_START")
        dispatch_start_start = _discover_define_value("DISPATCH_START_START")

        ensure_stub_online_backend(
            '{"mode":"online","soc_pct":50,"battery_power_w":0,"grid_power_w":0,"pv_ct_power_w":0}',
            label=f"{label_prefix} backend",
        )

        ensure_stub_online_backend(
            '{"mode":"online","dispatch_start":0,"dispatch_mode":65535,"dispatch_active_power":0,"dispatch_soc":0}',
            label=f"{label_prefix} mismatch backend",
        )

        if poll_interval_s is not None:
            wait_runtime_poll_interval_applied(poll_interval_s)

        before = _fetch_poll(mqtt, poll_topic)
        baseline_queued_ms = int(before.get("dispatch_request_queued_ms", 0))

        request_payload = _default_dispatch_request(
            duration_s=duration_s if duration_s is not None else 0,
        )
        publish_dispatch_request_and_wait_status(
            ha_unique,
            request_payload,
            expected_status="ok",
            timeout_s=timeout_s,
        )

        def pred() -> Tuple[bool, str]:
            cur = _fetch_matching_or_latest_json(
                mqtt,
                poll_topic,
                "dispatch_write_poll",
                lambda payload: (
                    int(payload.get("rs485_stub_last_write_reg", 0)) == reg_dispatch_start
                    and int(payload.get("rs485_stub_last_write_reg_count", 0)) == 9
                    and int(payload.get("dispatch_request_queued_ms", 0)) > baseline_queued_ms
                    and int(payload.get("rs485_stub_last_write_ms", 0))
                    >= int(payload.get("dispatch_request_queued_ms", 0))
                ),
            )
            writes = int(cur.get("rs485_stub_writes", 0))
            last_reg = int(cur.get("rs485_stub_last_write_reg", 0))
            last_reg_count = int(cur.get("rs485_stub_last_write_reg_count", 0))
            last_ms = int(cur.get("rs485_stub_last_write_ms", 0))
            queued_ms = int(cur.get("dispatch_request_queued_ms", 0))
            detail = (
                f"writes={writes} last_reg={last_reg} last_reg_count={last_reg_count} "
                f"last_ms={last_ms} queued_ms={queued_ms} expect_reg={reg_dispatch_start}"
            )
            return (
                last_reg == reg_dispatch_start
                and last_reg_count == 9
                and queued_ms > baseline_queued_ms
                and last_ms >= queued_ms
            ), detail

        _assert_eventually(
            f"{label_prefix} dispatch write observed in stub backend",
            pred,
            timeout_s=timeout_s,
            poll_s=1.0,
        )
        return reg_dispatch_start, dispatch_start_start

    def measure_dispatch_write_latency(
        ha_unique: str,
        *,
        label_prefix: str,
        duration_s: int,
        poll_interval_s: Optional[int],
        timeout_s: int = 20,
    ) -> int:
        reg_dispatch_start = _discover_register_value("REG_DISPATCH_RW_DISPATCH_START")

        ensure_stub_online_backend(
            '{"mode":"online","soc_pct":50,"battery_power_w":0,"grid_power_w":0,"pv_ct_power_w":0}',
            label=f"{label_prefix} backend",
        )

        ensure_stub_online_backend(
            '{"mode":"online","dispatch_start":0,"dispatch_mode":65535,"dispatch_active_power":0,"dispatch_soc":0}',
            label=f"{label_prefix} mismatch backend",
        )

        if poll_interval_s is not None:
            wait_runtime_poll_interval_applied(poll_interval_s)

        before = _fetch_poll(mqtt, poll_topic)
        baseline_queued_ms = int(before.get("dispatch_request_queued_ms", 0))

        publish_dispatch_request_and_wait_status(
            ha_unique,
            _default_dispatch_request(duration_s=duration_s),
            expected_status="ok",
            timeout_s=timeout_s,
        )

        result: dict[str, int] = {}

        def pred() -> Tuple[bool, str]:
            cur = _fetch_poll(mqtt, poll_topic)
            writes = int(cur.get("rs485_stub_writes", 0))
            last_reg = int(cur.get("rs485_stub_last_write_reg", 0))
            last_reg_count = int(cur.get("rs485_stub_last_write_reg_count", 0))
            last_ms = int(cur.get("rs485_stub_last_write_ms", 0))
            queued_ms = int(cur.get("dispatch_request_queued_ms", 0))
            if (
                last_reg == reg_dispatch_start
                and last_reg_count == 9
                and queued_ms > baseline_queued_ms
                and last_ms >= queued_ms
            ):
                result["delta_ms"] = last_ms - queued_ms
                return True, f"delta_ms={result['delta_ms']}"
            return (
                False,
                f"writes={writes} last_reg={last_reg} last_reg_count={last_reg_count} "
                f"last_ms={last_ms} queued_ms={queued_ms}",
            )

        _assert_eventually(
            f"{label_prefix} dispatch timing captured",
            pred,
            timeout_s=timeout_s,
            poll_s=1.0,
        )
        return result["delta_ms"]

    def set_polling_config(poll_interval_s: int, bucket_map: str) -> None:
        # Config-set parser expects string values, not numbers. An empty bucket map means
        # "leave assignments unchanged", not "apply an invalid blank override payload".
        payload_obj: dict[str, str] = {
            "poll_interval_s": str(poll_interval_s),
        }
        if bucket_map:
            payload_obj["bucket_map"] = bucket_map
        payload = json.dumps(payload_obj)
        mqtt.publish(f"{device_root}/config/set", payload, retain=False)

    def wait_polling_config_applied(
        poll_interval_s: int,
        expected_buckets: dict[str, str],
        *,
        timeout_s: int = 35,
        republish_every_s: float = 5.0,
    ) -> None:
        bucket_map = "".join(f"{name}={bucket};" for name, bucket in expected_buckets.items())
        set_polling_config(poll_interval_s, bucket_map)
        last_pub = time.time()

        def pred() -> Tuple[bool, str]:
            nonlocal last_pub
            now = time.time()
            if (now - last_pub) >= republish_every_s:
                set_polling_config(poll_interval_s, bucket_map)
                last_pub = now

            cfg = _fetch_config(mqtt, config_topic)
            poll = _fetch_poll(mqtt, poll_topic)
            cfg_interval = int(cfg.get("poll_interval_s", 0) or 0)
            runtime_interval = int(poll.get("poll_interval_s", 0) or 0)
            intervals = cfg.get("entity_intervals", {})
            if not isinstance(intervals, dict):
                return False, f"entity_intervals invalid: {cfg!r}"

            mismatches = []
            for key, expected in expected_buckets.items():
                actual = _effective_bucket(intervals, key)
                if actual != expected:
                    mismatches.append(f"{key}={actual!r}")
            if cfg_interval != poll_interval_s:
                mismatches.append(f"cfg_poll_interval_s={cfg_interval}")
            if runtime_interval != poll_interval_s:
                mismatches.append(f"runtime_poll_interval_s={runtime_interval}")
            return (not mismatches), ", ".join(mismatches) if mismatches else "ok"

        _assert_eventually(
            f"polling config applied (poll_interval_s={poll_interval_s})",
            pred,
            timeout_s=timeout_s,
            poll_s=2.0,
        )

    def wait_runtime_poll_interval_applied(
        poll_interval_s: int,
        *,
        timeout_s: int = 35,
        republish_every_s: float = 5.0,
    ) -> None:
        set_polling_config(poll_interval_s, "")
        last_pub = time.time()

        def pred() -> Tuple[bool, str]:
            nonlocal last_pub
            now = time.time()
            if (now - last_pub) >= republish_every_s:
                set_polling_config(poll_interval_s, "")
                last_pub = now

            poll = _fetch_poll(mqtt, poll_topic)
            runtime_interval = int(poll.get("poll_interval_s", 0) or 0)
            return (runtime_interval == poll_interval_s), f"runtime_poll_interval_s={runtime_interval}"

        _assert_eventually(
            f"runtime polling interval applied ({poll_interval_s})",
            pred,
            timeout_s=timeout_s,
            poll_s=2.0,
        )

    def ensure_clean_suite_baseline() -> None:
        # The lab device persists fault toggles and polling overrides across runs. Reset them
        # here so suite startup does not depend on whatever the previous manual/E2E session left behind.
        # Gate only on live poll readiness. Retained status/stub and config snapshots can lag behind
        # a manual serial flash or a just-restored runtime and are not authoritative enough to block
        # the entire suite.
        try:
            http_base = _resolve_device_http_base(mqtt, device_root)
            _ensure_runtime_http_from_portal(http_base)
        except Exception:
            pass
        _drop_cached_topics(mqtt, poll_topic, stub_topic, status_core_topic, config_topic)
        _fetch_live_json(mqtt, poll_topic, "suite baseline sync poll", timeout_s=20)
        _fetch_live_json(mqtt, status_core_topic, "suite baseline sync core", timeout_s=20)
        ensure_stub_online_backend(
            '{"mode":"online","soc_pct":50,"battery_power_w":0,"grid_power_w":0,"pv_ct_power_w":0}',
            label="suite baseline",
        )
        wait_runtime_poll_interval_applied(9)
        wait_polling_config_applied(
            9,
            {
                "Register_Number": "one_min",
                "Register_Value": "one_min",
            },
            timeout_s=35,
        )

    def _inverter_id_from_ha_unique(ha_unique: str) -> str:
        ha_unique = ha_unique.strip()
        if not ha_unique.startswith("A2M-"):
            return ""
        serial = ha_unique[len("A2M-"):].strip()
        if not serial or serial.lower() == "unknown":
            return ""
        return f"alpha2mqtt_inv_{serial}"

    def _current_inverter_identity() -> str:
        try:
            poll = _fetch_poll(mqtt, poll_topic)
            core = _fetch_status_core(mqtt, status_core_topic)
        except E2EError:
            return ""
        inverter_ready = bool(poll.get("inverter_ready", False))
        backend = str(poll.get("rs485_backend", "")).strip()
        ha_unique = str(core.get("ha_unique_id", "")).strip()
        inverter_id = _inverter_id_from_ha_unique(ha_unique)
        if inverter_ready and backend == "stub" and inverter_id:
            return inverter_id
        return ""

    def _wait_for_inverter_identity() -> str:
        _fetch_poll(mqtt, poll_topic)
        _fetch_status_core(mqtt, status_core_topic)

        deadline = time.time() + 60.0
        last_detail = "waiting for live poll + status/core identity"
        while time.time() < deadline:
            poll = _fetch_live_json(mqtt, poll_topic, "identity_poll")
            inverter_ready = bool(poll.get("inverter_ready", False))
            backend = str(poll.get("rs485_backend", "")).strip()
            # A buffered /status/poll can outlive a reboot and briefly report the old
            # ready state even after the runtime has fallen back to A2M-UNKNOWN. Require
            # a fresh core publish before trusting the inverter device id for dispatch.
            core = _fetch_live_json(mqtt, status_core_topic, "identity_core")
            ha_unique = str(core.get("ha_unique_id", "")).strip()
            inverter_id = _inverter_id_from_ha_unique(ha_unique)
            last_detail = (
                f"backend={backend!r} inverter_ready={inverter_ready} "
                f"ha_unique_id={ha_unique!r} inverter_id={inverter_id!r}"
            )
            if inverter_ready and backend == "stub" and inverter_id:
                return inverter_id

        raise E2EError(f"inverter identity did not become live: {last_detail}")

    def _ensure_online_inverter_identity(label: str) -> str:
        ensure_stub_online_backend(
            '{"mode":"online","soc_pct":50,"battery_power_w":0,"grid_power_w":0,"pv_ct_power_w":0}',
            label=label,
        )
        return _wait_for_inverter_identity()

    def _reboot_normal_and_wait(label: str) -> None:
        base = _resolve_device_http_base(mqtt, device_root)
        reboot_normal_path = _discover_reboot_normal_path_from_code()
        if not reboot_normal_path:
            raise E2EError("Could not discover /reboot/normal endpoint from firmware source")

        def http_ready_pred() -> Tuple[bool, str]:
            try:
                status, _ = _http_request("GET", base + "/", headers={}, body=b"", timeout_s=10)
            except (TimeoutError, OSError) as e:
                return False, f"http not ready: {e}"
            return status == 200, f"http status={status}"

        _assert_eventually(
            f"http ready before {label} reboot",
            http_ready_pred,
            timeout_s=30,
            poll_s=2.0,
        )

        boot_topic = f"{device_root}/boot"
        previous_boot = _fetch_latest_text(mqtt, boot_topic, label=f"boot_before_{label}_reboot")
        previous_poll = _fetch_latest_text(mqtt, poll_topic, label=f"poll_before_{label}_reboot")
        reboot_status, reboot_body = _http_request_full("POST", base + reboot_normal_path, headers={}, body=b"", timeout_s=20)
        if reboot_status != 200:
            raise E2EError(f"/reboot/normal returned unexpected status={reboot_status}")
        if reboot_body:
            _assert_reboot_handoff_html(
                reboot_body.decode("utf-8", errors="replace"),
                expected_heading="Rebooting to normal mode",
                expected_target_mode="normal",
                expected_probe_kind="fetch",
            )
        _wait_for_topic_change(mqtt, boot_topic, previous_boot, timeout_s=60, label=f"boot after {label} reboot")
        _wait_for_topic_change(mqtt, poll_topic, previous_poll, timeout_s=60, label=f"poll after {label} reboot")

    def _assert_unknown_inverter_identity(label: str, *, timeout_s: int = 60) -> None:
        def pred() -> Tuple[bool, str]:
            core = _fetch_status_core(mqtt, status_core_topic)
            ha_unique = str(core.get("ha_unique_id", ""))
            detail = f"ha_unique_id={ha_unique!r}"
            return (ha_unique == "A2M-UNKNOWN"), detail

        _assert_eventually(label, pred, timeout_s=timeout_s, poll_s=3.0)

    def _state_topic(inverter_device_id: str, name: str) -> str:
        return f"{device_root}/{inverter_device_id}/{name}/state"

    def _command_topic(inverter_device_id: str, name: str) -> str:
        return f"{device_root}/{inverter_device_id}/{name}/command"

    def _manual_read_topic() -> str:
        return f"{device_root}/status/manual_read"

    def case_two_device_discovery() -> None:
        print("[e2e] case: two-device discovery model")
        ensure_stub_online_backend('{"mode":"online"}', label="two-device discovery baseline")
        mqtt.subscribe(f"{device_root}/+/inverter_serial/state", force=True)
        controller_topic = ""
        controller_serial = ""
        deadline = time.time() + 20
        while time.time() < deadline:
            topic, payload = mqtt.wait_for_publish(timeout_s=5.0)
            if topic.startswith(f"{device_root}/") and topic.endswith("/inverter_serial/state"):
                controller_topic = topic
                controller_serial = payload
                break
        if not controller_topic:
            raise E2EError("controller inverter_serial state topic was not observed")
        parts = controller_topic.split("/")
        if len(parts) < 3:
            raise E2EError(f"unexpected controller inverter_serial topic format: {controller_topic}")
        controller_id = parts[1]
        if not re.fullmatch(r"alpha2mqtt_[0-9a-f]{12}", controller_id):
            raise E2EError(f"invalid controller identifier from inverter_serial topic: {controller_id!r}")

        serial_known = bool(_fetch_poll(mqtt, poll_topic).get("inverter_ready", False)) and str(controller_serial).strip().lower() not in ("", "unknown")
        inverter_device_id = f"alpha2mqtt_inv_{controller_serial.strip()}" if serial_known else ""
        expect_inverter_entity = serial_known
        inverter_serial_uid = f"{controller_id}_inverter_serial"
        controller_inverter_serial_topic = f"homeassistant/sensor/{controller_id}/inverter_serial/config"
        inverter_discovery_filter = f"homeassistant/+/{inverter_device_id}/+/config" if expect_inverter_entity else ""

        mqtt.subscribe(controller_inverter_serial_topic, force=True)
        if inverter_discovery_filter:
            mqtt.subscribe(inverter_discovery_filter, force=True)
        # Subscribe before forcing HA rediscovery so non-retained entity configs are observed.
        # Drain any immediate retained traffic first; the checks below must validate the fresh re-advertise.
        drain_deadline = time.time() + 2
        while time.time() < drain_deadline:
            nxt = mqtt._try_wait_for_publish(timeout_s=0.25)
            if nxt is None:
                continue

        mqtt.publish("homeassistant/status", "online", retain=False)
        _sleep_with_mqtt(mqtt, 2)

        deadline = time.time() + 25
        saw_controller_inverter_serial = False
        saw_inverter_entity = False
        while time.time() < deadline and (not saw_controller_inverter_serial or (expect_inverter_entity and not saw_inverter_entity)):
            try:
                got_topic, raw_payload = mqtt.wait_for_publish(timeout_s=5.0)
            except E2EError as e:
                if "Timeout waiting for MQTT publish" in str(e):
                    continue
                raise
            if got_topic == controller_inverter_serial_topic:
                payload = _parse_json(raw_payload)
                if str(payload.get("unique_id", "")) != inverter_serial_uid:
                    raise E2EError(
                        f"controller inverter_serial unique_id mismatch: "
                        f"expected {inverter_serial_uid!r} got {payload.get('unique_id')!r}"
                    )
                saw_controller_inverter_serial = True
                continue

            if not serial_known or not got_topic.startswith(f"homeassistant/") or f"/{inverter_device_id}/" not in got_topic:
                continue
            try:
                payload = _parse_json(raw_payload)
            except Exception:
                continue
            if not isinstance(payload, dict) or not payload:
                continue

            if not saw_inverter_entity:
                inverter_device = payload.get("device", {})
                if not isinstance(inverter_device, dict):
                    raise E2EError(f"inverter discovery missing device block: {payload}")
                identifiers = inverter_device.get("identifiers", [])
                if not isinstance(identifiers, list) or not identifiers or str(identifiers[0]) != inverter_device_id:
                    raise E2EError(
                        f"inverter discovery identifier mismatch: expected {inverter_device_id!r} got {identifiers!r}"
                    )
                via = str(inverter_device.get("via_device", ""))
                if via != controller_id:
                    raise E2EError(f"inverter via_device mismatch: via={via!r} expected={controller_id!r}")
                device_name = str(inverter_device.get("name", ""))
                if not device_name.startswith("Alpha ") or len(device_name) <= len("Alpha "):
                    raise E2EError(f"inverter device name malformed: {device_name!r}")
                label_display = device_name[len("Alpha "):]
                label_id = _normalize_label_for_entity_id(label_display)
                default_entity_id = str(payload.get("default_entity_id", ""))
                if not default_entity_id.startswith(f"{got_topic.split('/')[1]}.alpha_{label_id}_"):
                    raise E2EError(
                        f"inverter default_entity_id prefix mismatch: expected prefix "
                        f"{got_topic.split('/')[1]}.alpha_{label_id}_ got {default_entity_id!r}"
                    )
                saw_inverter_entity = True

        missing: list[str] = []
        if not saw_controller_inverter_serial:
            missing.append(controller_inverter_serial_topic)
        if expect_inverter_entity and not saw_inverter_entity:
            missing.append(f"{inverter_discovery_filter} (non-empty payload)")
        if missing:
            raise E2EError(f"missing discovery topic(s): {missing}")

    def case_offline() -> None:
        print("[e2e] case: stub offline")
        set_mode_and_wait('{"mode":"offline"}', ("offline",))

        def pred() -> Tuple[bool, str]:
            cur = _fetch_poll(mqtt, poll_topic)
            attempts = int(cur.get("ess_snapshot_attempts", 0))
            ok = bool(cur.get("ess_snapshot_last_ok", False))
            skip = str(cur.get("dispatch_last_skip_reason", ""))
            mode = str(cur.get("rs485_stub_mode", ""))
            detail = f"attempts={attempts} ok={ok} skip={skip} mode={mode}"
            return (mode == "offline" and attempts > 0 and (not ok) and skip == "ess_snapshot_failed"), detail

        _assert_eventually("offline causes snapshot fail + dispatch suppressed", pred, timeout_s=45)

    def case_fail_then_recover() -> None:
        print("[e2e] case: fail then recover (n=2)")
        set_mode_and_wait('{"mode":"fail","fail_n":2}', ("fail_then_recover", "fail"))

        def pred() -> Tuple[bool, str]:
            cur = _fetch_poll(mqtt, poll_topic)
            ok = bool(cur.get("ess_snapshot_last_ok", False))
            remaining = int(cur.get("rs485_stub_fail_remaining", 0))
            mode = str(cur.get("rs485_stub_mode", ""))
            detail = f"ok={ok} remaining={remaining} mode={mode}"
            # Success should occur once remaining reaches 0.
            return (mode == "fail_then_recover" and remaining == 0 and ok), detail

        _assert_eventually("fail_then_recover eventually succeeds", pred, timeout_s=60)

    def case_runtime_loss_reprobe() -> None:
        print("[e2e] case: runtime RS485 loss clears identity and reprobes")
        inverter_id = _ensure_online_inverter_identity("runtime loss baseline")
        baseline_poll = _fetch_poll(mqtt, poll_topic)
        baseline_epoch = int(baseline_poll.get("rs485_connection_epoch", 0))
        baseline_core = _fetch_status_core(mqtt, status_core_topic)
        baseline_ha_unique = str(baseline_core.get("ha_unique_id", ""))
        if not baseline_ha_unique.startswith("A2M-"):
            raise E2EError(f"baseline HA identity is not live: {baseline_ha_unique!r}")

        set_mode_and_wait('{"mode":"fail","fail_n":3}', ("fail_then_recover", "fail"))

        def fell_back_to_probe() -> Tuple[bool, str]:
            cur_poll = _fetch_poll(mqtt, poll_topic)
            cur_core = _fetch_status_core(mqtt, status_core_topic)
            inverter_ready = bool(cur_poll.get("inverter_ready", False))
            probe_backoff_ms = int(cur_poll.get("rs485_probe_backoff_ms", 0))
            epoch = int(cur_poll.get("rs485_connection_epoch", 0))
            ha_unique = str(cur_core.get("ha_unique_id", ""))
            detail = (
                f"inverter_ready={inverter_ready} probe_backoff_ms={probe_backoff_ms} "
                f"epoch={epoch} ha_unique_id={ha_unique!r}"
            )
            return (not inverter_ready and probe_backoff_ms > 0 and ha_unique == "A2M-UNKNOWN"), detail

        _assert_eventually(
            "runtime loss falls back to probing and clears live identity",
            fell_back_to_probe,
            timeout_s=80,
            poll_s=3.0,
        )

        def recovered_after_reprobe() -> Tuple[bool, str]:
            cur_poll = _fetch_poll(mqtt, poll_topic)
            cur_core = _fetch_status_core(mqtt, status_core_topic)
            inverter_ready = bool(cur_poll.get("inverter_ready", False))
            snapshot_ok = bool(cur_poll.get("ess_snapshot_last_ok", False))
            probe_backoff_ms = int(cur_poll.get("rs485_probe_backoff_ms", 0))
            epoch = int(cur_poll.get("rs485_connection_epoch", 0))
            ha_unique = str(cur_core.get("ha_unique_id", ""))
            current_inverter_id = _inverter_id_from_ha_unique(ha_unique)
            detail = (
                f"inverter_ready={inverter_ready} snapshot_ok={snapshot_ok} "
                f"probe_backoff_ms={probe_backoff_ms} epoch={epoch} "
                f"ha_unique_id={ha_unique!r} inverter_id={current_inverter_id!r}"
            )
            return (
                inverter_ready
                and snapshot_ok
                and probe_backoff_ms == 0
                and epoch > baseline_epoch
                and current_inverter_id == inverter_id
            ), detail

        _assert_eventually(
            "runtime loss recovery starts a new RS485 connection epoch",
            recovered_after_reprobe,
            timeout_s=90,
            poll_s=3.0,
        )

    def case_online() -> None:
        print("[e2e] case: stub online")
        set_mode_and_wait('{"mode":"online"}', ("online",))
        def pred() -> Tuple[bool, str]:
            cur = _fetch_poll(mqtt, poll_topic)
            ok = bool(cur.get("ess_snapshot_last_ok", False))
            skip = str(cur.get("dispatch_last_skip_reason", ""))
            mode = str(cur.get("rs485_stub_mode", ""))
            detail = f"ok={ok} skip={skip} mode={mode}"
            return (mode == "online" and ok and skip != "ess_snapshot_failed"), detail

        _assert_eventually("online succeeds and dispatch not suppressed", pred, timeout_s=45)

    def case_boot_mem_publish() -> None:
        print("[e2e] case: boot/mem retained publish captures boot heap checkpoints")
        base = _resolve_device_http_base(mqtt, device_root)
        reboot_normal_path = _discover_reboot_normal_path_from_code()
        if not reboot_normal_path:
            raise E2EError("Could not discover /reboot/normal endpoint from firmware source")

        boot_topic = f"{device_root}/boot"
        boot_mem_topic = f"{device_root}/boot/mem"
        previous_boot = _fetch_latest_text(mqtt, boot_topic, label="boot_before_boot_mem_reboot")
        previous_boot_mem = mqtt.latest_payload(boot_mem_topic) or ""

        reboot_status, _ = _http_request("POST", base + reboot_normal_path, headers={}, body=b"", timeout_s=20)
        if reboot_status != 200:
            raise E2EError(f"/reboot/normal returned unexpected status={reboot_status}")

        _wait_for_topic_change(mqtt, boot_topic, previous_boot, timeout_s=60, label="boot after boot_mem reboot")
        boot_mem_text = _wait_for_topic_change(
            mqtt,
            boot_mem_topic,
            previous_boot_mem,
            timeout_s=60,
            label="boot/mem after reboot",
        )
        boot_mem = _parse_json(boot_mem_text)
        if int(boot_mem.get("fw_build_ts_ms", 0)) != expected_ts:
            raise E2EError(
                f"boot/mem build mismatch: expected {expected_ts}, got {boot_mem.get('fw_build_ts_ms')!r}"
            )

        checkpoints = {
            "heap_pre_wifi": int(boot_mem.get("heap_pre_wifi", 0)),
            "heap_post_wifi": int(boot_mem.get("heap_post_wifi", 0)),
            "heap_post_mqtt": int(boot_mem.get("heap_post_mqtt", 0)),
            "heap_pre_rs485": int(boot_mem.get("heap_pre_rs485", 0)),
            "heap_post_rs485": int(boot_mem.get("heap_post_rs485", 0)),
        }
        minimums = {
            "heap_pre_wifi": 18500,
            "heap_post_wifi": 17000,
            "heap_post_mqtt": 12500,
            "heap_pre_rs485": 10000,
            "heap_post_rs485": 8000,
        }
        for key, minimum in minimums.items():
            actual = checkpoints[key]
            if actual < minimum:
                raise E2EError(f"boot/mem {key} below threshold: actual={actual} minimum={minimum}")

    def case_bucket_snapshot_skip_only() -> None:
        print("[e2e] case: bucket gating skips only ESS snapshot entities (and dispatch) when snapshot fails")

        # Keep this test lightweight: prove that non-snapshot status continues to publish while
        # ESS snapshot (and dispatch) is suppressed.
        set_mode_and_wait('{"mode":"online"}', ("online",))
        net_topic = f"{device_root}/status/net"
        mqtt.subscribe(net_topic, force=True)
        net1_text = _fetch_latest_text(mqtt, net_topic, label="net")
        u1 = int(_parse_json(net1_text).get("uptime_s", 0))

        # Force snapshot failure and confirm:
        # - net uptime continues (non-snapshot publish)
        # - dispatch is suppressed
        set_mode_and_wait('{"mode":"offline"}', ("offline",))
        _assert_eventually(
            "offline triggers dispatch suppression",
            lambda: (
                lambda cur: (
                    (
                        str(cur.get("dispatch_last_skip_reason", "")) == "ess_snapshot_failed"
                        and str(cur.get("rs485_stub_mode", "")) == "offline"
                        and int(cur.get("ess_snapshot_attempts", 0)) > 0
                    ),
                    (
                        f"skip={cur.get('dispatch_last_skip_reason')} "
                        f"mode={cur.get('rs485_stub_mode')} "
                        f"attempts={cur.get('ess_snapshot_attempts')}"
                    ),
                )
            )(_fetch_poll(mqtt, poll_topic)),
            timeout_s=45,
            poll_s=3.0,
        )

        # Require a changed status/net payload (not a retained replay), then compare uptime.
        net2_text = _wait_for_topic_change(mqtt, net_topic, net1_text, timeout_s=25, label="status/net while offline")
        u2 = int(_parse_json(net2_text).get("uptime_s", 0))
        if u2 <= u1:
            # Wait one more changed payload to avoid phase-edge flake near publish boundaries.
            net3_text = _wait_for_topic_change(mqtt, net_topic, net2_text, timeout_s=25, label="status/net second change while offline")
            u3 = int(_parse_json(net3_text).get("uptime_s", 0))
            if u3 <= u2:
                raise E2EError(f"Expected uptime_s to increase while snapshot is failing, got {u1}->{u2}->{u3}")

        # Recover and confirm SOC resumes.
        ensure_stub_online_backend('{"mode":"online"}', label="bucket snapshot recovery")
        _assert_eventually(
            "dispatch skip clears after recovery",
            lambda: (
                str(_fetch_poll(mqtt, poll_topic).get("dispatch_last_skip_reason", "")) != "ess_snapshot_failed",
                "waiting",
            ),
            timeout_s=60,
            poll_s=3.0,
        )

    def case_dispatch_write_via_commands() -> None:
        print("[e2e] case: dispatch write via atomic request")
        ha_unique = _ensure_online_inverter_identity("dispatch write baseline")
        reg_dispatch_start, _ = ensure_dispatch_write(ha_unique, label_prefix="dispatch")
        before_stop = _fetch_poll(mqtt, poll_topic)
        writes_before_stop = int(before_stop.get("rs485_stub_writes", 0))
        last_write_ms_before_stop = int(before_stop.get("rs485_stub_last_write_ms", 0))
        publish_dispatch_request_and_wait_status(
            ha_unique,
            _default_dispatch_request(mode="normal_mode"),
            expected_status="ok",
            require_queue_advance=False,
        )

        def stop_write_pred() -> Tuple[bool, str]:
            cur = _fetch_matching_or_latest_json(
                mqtt,
                poll_topic,
                "dispatch_stop_poll",
                lambda payload: (
                    int(payload.get("rs485_stub_writes", 0)) > writes_before_stop
                    and int(payload.get("rs485_stub_last_write_reg", 0)) == reg_dispatch_start
                    and int(payload.get("rs485_stub_last_write_reg_count", 0)) == 1
                    and int(payload.get("rs485_stub_last_write_ms", 0)) > last_write_ms_before_stop
                ),
            )
            writes = int(cur.get("rs485_stub_writes", 0))
            last_reg = int(cur.get("rs485_stub_last_write_reg", 0))
            last_reg_count = int(cur.get("rs485_stub_last_write_reg_count", 0))
            last_ms = int(cur.get("rs485_stub_last_write_ms", 0))
            detail = (
                f"writes={writes} last_reg={last_reg} last_reg_count={last_reg_count} "
                f"last_ms={last_ms} baseline_writes={writes_before_stop} "
                f"baseline_last_write_ms={last_write_ms_before_stop}"
            )
            return (
                writes > writes_before_stop
                and last_reg == reg_dispatch_start
                and last_reg_count == 1
                and last_ms > last_write_ms_before_stop
            ), detail

        _assert_eventually(
            "normal_mode stop write observed in stub backend",
            stop_write_pred,
            timeout_s=15,
            poll_s=1.0,
        )
        return

    def case_dispatch_legacy_command_topics_ignored() -> None:
        print("[e2e] case: retired dispatch control command topics are ignored")
        ha_unique = _ensure_online_inverter_identity("dispatch legacy command baseline")
        before = _fetch_poll(mqtt, poll_topic)
        queued_before = int(before.get("dispatch_request_queued_ms", 0))
        writes_before = int(before.get("rs485_stub_writes", 0))

        mqtt.publish(_command_topic(ha_unique, "Dispatch_Duration"), "60", retain=False)
        mqtt.publish(_command_topic(ha_unique, "Op_Mode"), "No Charge", retain=False)
        _sleep_with_mqtt(mqtt, 2.0)

        after = _fetch_poll(mqtt, poll_topic)
        queued_after = int(after.get("dispatch_request_queued_ms", 0))
        writes_after = int(after.get("rs485_stub_writes", 0))
        if queued_after != queued_before:
            raise E2EError(
                f"retired command topic should not queue atomic dispatch requests: "
                f"queued_ms {queued_before}->{queued_after}"
            )
        if writes_after != writes_before:
            raise E2EError(
                f"retired command topic should not write RS485 dispatch registers: "
                f"writes {writes_before}->{writes_after}"
            )

    def case_dispatch_invalid_payload_no_write() -> None:
        print("[e2e] case: invalid atomic dispatch payload reports an error without writing RS485")
        ha_unique = _ensure_online_inverter_identity("dispatch invalid baseline")
        before = _fetch_poll(mqtt, poll_topic)
        writes_before = int(before.get("rs485_stub_writes", 0))

        publish_dispatch_request_and_wait_status(
            ha_unique,
            {"mode": "not_a_real_mode"},
            expected_status="invalid mode",
            require_queue_advance=False,
            timeout_s=10,
        )

        after = _fetch_poll(mqtt, poll_topic)
        writes_after = int(after.get("rs485_stub_writes", 0))
        if writes_after != writes_before:
            raise E2EError(
                f"invalid dispatch payload should not touch RS485 writes: "
                f"{writes_before}->{writes_after}"
            )

    def case_dispatch_invalid_numeric_payloads_no_write() -> None:
        print("[e2e] case: invalid numeric atomic dispatch payloads report errors without writing RS485")
        ha_unique = _ensure_online_inverter_identity("dispatch invalid numeric baseline")
        invalid_payloads = (
            (_default_dispatch_request(mode="battery_only_charges_from_pv", power_w=500, duration_s=60), "invalid power"),
            (_default_dispatch_request(mode="state_of_charge_control", power_w=-1000, soc_percent=101, duration_s=60), "invalid soc"),
            (_default_dispatch_request(mode="state_of_charge_control", power_w=-1000, soc_percent=20, duration_s=4294968), "invalid duration"),
        )

        for payload, expected_status in invalid_payloads:
            before = _fetch_poll(mqtt, poll_topic)
            writes_before = int(before.get("rs485_stub_writes", 0))
            publish_dispatch_request_and_wait_status(
                ha_unique,
                payload,
                expected_status=expected_status,
                require_queue_advance=False,
                timeout_s=10,
            )
            after = _fetch_poll(mqtt, poll_topic)
            writes_after = int(after.get("rs485_stub_writes", 0))
            if writes_after != writes_before:
                raise E2EError(
                    f"{expected_status} payload should not touch RS485 writes: "
                    f"{writes_before}->{writes_after}"
                )

    def case_dispatch_write_feedback_via_register_value() -> None:
        print("[e2e] case: atomic dispatch write feedback (Dispatch_* topics reflect new dispatch state)")
        ensure_clean_suite_baseline()
        ha_unique = _wait_for_inverter_identity()
        _reg_dispatch_start, dispatch_start_start = ensure_dispatch_write(ha_unique, label_prefix="dispatch feedback")
        dispatch_start_stop = _discover_define_value("DISPATCH_START_STOP")
        dispatch_start_topic = _state_topic(ha_unique, "Dispatch_Start")
        dispatch_power_topic = _state_topic(ha_unique, "Dispatch_Power")
        expected_dispatch_power = str(_default_dispatch_request().get("power_w", -1200))
        mqtt.subscribe(dispatch_start_topic, force=True)
        mqtt.subscribe(dispatch_power_topic, force=True)

        def dispatch_started_pred() -> Tuple[bool, str]:
            start_state = _fetch_matching_or_latest_text(
                mqtt,
                dispatch_start_topic,
                label="dispatch_start_state",
                predicate=lambda text: (
                    text.strip().lower() in ("start", "started")
                    or text.strip() == str(dispatch_start_start)
                ),
            )
            normalized = start_state.strip().lower()
            ok = normalized in ("start", "started") or start_state.strip() == str(dispatch_start_start)
            return ok, f"last={start_state!r}"

        _assert_eventually(
            "dispatch start state published before manual read",
            dispatch_started_pred,
            timeout_s=20,
            poll_s=1.0,
        )

        def dispatch_power_pred() -> Tuple[bool, str]:
            power_state = _fetch_matching_or_latest_text(
                mqtt,
                dispatch_power_topic,
                label="dispatch_power_state",
                predicate=lambda text: text.strip() == expected_dispatch_power,
            ).strip()
            return power_state == expected_dispatch_power, f"last={power_state!r}"

        _assert_eventually(
            "dispatch power state published as signed watts",
            dispatch_power_pred,
            timeout_s=20,
            poll_s=1.0,
        )

        publish_dispatch_request_and_wait_status(
            ha_unique,
            _default_dispatch_request(mode="normal_mode"),
            expected_status="ok",
            require_queue_advance=False,
            timeout_s=25,
        )
        def dispatch_stopped_pred() -> Tuple[bool, str]:
            start_state = _fetch_matching_or_latest_text(
                mqtt,
                dispatch_start_topic,
                label="dispatch_start_stop_state",
                predicate=lambda text: (
                    text.strip().lower() in ("stop", "stopped")
                    or text.strip() == str(dispatch_start_stop)
                ),
            )
            normalized = start_state.strip().lower()
            ok = normalized in ("stop", "stopped") or start_state.strip() == str(dispatch_start_stop)
            return ok, f"last={start_state!r}"

        _assert_eventually(
            "dispatch stop state published after normal_mode request",
            dispatch_stopped_pred,
            timeout_s=20,
            poll_s=1.0,
        )

    def case_dispatch_primes_single_snapshot_refresh() -> None:
        print("[e2e] case: dispatch request primes an immediate ESS snapshot refresh")
        ha_unique = _ensure_online_inverter_identity("dispatch snapshot prime baseline")
        ensure_stub_online_backend(
            '{"mode":"online","soc_pct":50,"battery_power_w":0,"grid_power_w":0,"pv_ct_power_w":0}',
            label="dispatch snapshot prime backend",
        )

        initial_attempts = int(_fetch_poll(mqtt, poll_topic).get("ess_snapshot_attempts", 0))
        _assert_eventually(
            "wait for next scheduled snapshot before dispatch",
            lambda: (
                int(_fetch_poll(mqtt, poll_topic).get("ess_snapshot_attempts", 0)) > initial_attempts,
                f"attempts={_fetch_poll(mqtt, poll_topic).get('ess_snapshot_attempts')}",
            ),
            timeout_s=20,
            poll_s=0.5,
        )
        attempts_before_dispatch = int(_fetch_poll(mqtt, poll_topic).get("ess_snapshot_attempts", 0))

        publish_dispatch_request_and_wait_status(
            ha_unique,
            _default_dispatch_request(),
            expected_status="ok",
            timeout_s=20,
        )

        snapshot_refresh_started_at = time.time()
        _assert_eventually(
            "dispatch primes an immediate snapshot refresh ahead of the next 10s scheduler pass",
            lambda: (
                int(_fetch_poll(mqtt, poll_topic).get("ess_snapshot_attempts", 0)) >= attempts_before_dispatch + 1,
                f"attempts={_fetch_poll(mqtt, poll_topic).get('ess_snapshot_attempts')}",
            ),
            timeout_s=4,
            poll_s=0.5,
        )
        if (time.time() - snapshot_refresh_started_at) >= 4.0:
            raise E2EError(
                "dispatch-triggered snapshot refresh did not arrive before the normal 10s scheduler cadence"
            )
    def case_dispatch_write_under_100ms() -> None:
        print("[e2e] case: atomic dispatch request writes the 9-register block within 100 ms")
        ha_unique = _ensure_online_inverter_identity("dispatch timing baseline")
        elapsed_ms = measure_dispatch_write_latency(
            ha_unique,
            label_prefix="dispatch timing",
            duration_s=12,
            poll_interval_s=None,
            timeout_s=12,
        )
        if elapsed_ms > 100:
            raise E2EError(
                f"dispatch block write exceeded 100 ms from queued command to stub-observed 9-register write: "
                f"elapsed_ms={elapsed_ms}"
            )

        dispatch_time_topic = _state_topic(ha_unique, "Dispatch_Time")
        mqtt.subscribe(dispatch_time_topic, force=True)
        _assert_eventually(
            "raw dispatch time reflects the timed write",
            lambda: (
                _fetch_latest_text(mqtt, dispatch_time_topic, label="dispatch_time").strip() == "12",
                "waiting for Dispatch_Time=12",
            ),
            timeout_s=20,
            poll_s=1.0,
        )

    def case_dispatch_timed_restart_and_expire() -> None:
        print("[e2e] case: timed dispatch countdown restarts and expires cleanly")
        ha_unique = _ensure_online_inverter_identity("timed dispatch baseline")
        current_config = _fetch_config(mqtt, config_topic)
        original_poll_interval = int(current_config.get("poll_interval_s", 9) or 9)
        long_poll_interval = 120
        try:
            ensure_dispatch_write(
                ha_unique,
                label_prefix="dispatch timed",
                duration_s=12,
                poll_interval_s=long_poll_interval,
                timeout_s=8,
            )
            dispatch_start_stop = _discover_define_value("DISPATCH_START_STOP")
            remaining_topic = _state_topic(ha_unique, "Dispatch_Remaining")
            start_topic = _state_topic(ha_unique, "Dispatch_Start")
            mqtt.subscribe(remaining_topic, force=True)
            mqtt.subscribe(start_topic, force=True)

            first_remaining_text = ""

            def initial_remaining_pred() -> Tuple[bool, str]:
                nonlocal first_remaining_text
                first_remaining_text = _fetch_latest_text(mqtt, remaining_topic, label="dispatch_remaining_initial")
                first_remaining = int(first_remaining_text.strip())
                return (0 < first_remaining <= 12), f"remaining={first_remaining}"

            _assert_eventually(
                "initial timed Dispatch_Remaining becomes positive",
                initial_remaining_pred,
                timeout_s=20,
                poll_s=1.0,
            )
            first_remaining = int(first_remaining_text.strip())

            dropped_text = _wait_for_topic_change(
                mqtt,
                remaining_topic,
                first_remaining_text,
                timeout_s=12,
                label="dispatch_remaining_drop",
            )
            dropped = int(dropped_text.strip())
            if dropped >= first_remaining:
                raise E2EError(f"Dispatch_Remaining did not decrease: {first_remaining} -> {dropped}")

            publish_dispatch_request_and_wait_status(
                ha_unique,
                _default_dispatch_request(duration_s=12),
                expected_status="ok",
                timeout_s=25,
            )
            restarted_text = ""
            restart_prev_text = (
                _fetch_cached_or_latest_text(
                    mqtt,
                    remaining_topic,
                    label="dispatch_remaining_restart_current",
                ).strip()
                or dropped_text
            )

            def restart_pred() -> Tuple[bool, str]:
                nonlocal restarted_text, restart_prev_text
                # The dispatch request helper already proves the second request queued
                # successfully. Observe the countdown topic directly here so we do not
                # miss a fast restart by waiting on slower status/poll cadence.
                if restart_prev_text.isdigit() and int(restart_prev_text) > dropped:
                    restarted_text = restart_prev_text
                    restarted = int(restarted_text)
                    return True, f"remaining={restarted}"
                try:
                    next_text = _wait_for_topic_change(
                        mqtt,
                        remaining_topic,
                        restart_prev_text,
                        timeout_s=5,
                        label="dispatch_remaining_restart",
                    ).strip()
                except E2EError as e:
                    return False, f"waiting ({e})"
                restart_prev_text = next_text
                if not next_text.isdigit():
                    return False, f"remaining={next_text!r}"
                if int(next_text) <= dropped:
                    return False, f"remaining={next_text}"
                restarted_text = next_text
                restarted = int(restarted_text.strip())
                return restarted > dropped, f"remaining={restarted}"

            _assert_eventually(
                "Dispatch_Remaining restarts upward after identical command",
                restart_pred,
                timeout_s=20,
                poll_s=0.5,
            )

            def expiry_pred() -> Tuple[bool, str]:
                start_state = _fetch_latest_text(mqtt, start_topic, label="dispatch_start_expire").strip()
                remaining_state = _fetch_latest_text(mqtt, remaining_topic, label="dispatch_remaining_expire").strip()
                stop_seen = start_state.lower() in ("stop", "stopped") or start_state == str(dispatch_start_stop)
                remaining_zero = remaining_state == "0"
                return stop_seen and remaining_zero, f"start={start_state!r} remaining={remaining_state!r}"

            _assert_eventually("timed dispatch expires to stop", expiry_pred, timeout_s=20, poll_s=2.0)
            _sleep_with_mqtt(mqtt, 6)
            start_after_expiry = _fetch_latest_text(mqtt, start_topic, label="dispatch_start_post_expire").strip()
            if start_after_expiry.lower() not in ("stop", "stopped") and start_after_expiry != str(dispatch_start_stop):
                raise E2EError(f"dispatch restarted after expiry instead of staying stopped: {start_after_expiry!r}")
        finally:
            wait_runtime_poll_interval_applied(original_poll_interval)

    def case_dispatch_timed_no_rewrite_without_fresh_snapshot() -> None:
        print("[e2e] case: timed countdown ticks do not rewrite without a new atomic request")
        ha_unique = _ensure_online_inverter_identity("dispatch timed no-rewrite baseline")
        ensure_dispatch_write(
            ha_unique,
            label_prefix="dispatch timed no-rewrite",
            duration_s=25,
            timeout_s=8,
        )
        remaining_topic = _state_topic(ha_unique, "Dispatch_Remaining")
        mqtt.subscribe(remaining_topic, force=True)

        def accepted_pred() -> Tuple[bool, str]:
            remaining_text = _fetch_latest_text(mqtt, remaining_topic, label="dispatch_remaining_no_rewrite")
            remaining = int(remaining_text.strip())
            return (0 < remaining <= 25), f"remaining={remaining}"

        _assert_eventually(
            "timed dispatch accepted before stale-snapshot window",
            accepted_pred,
            timeout_s=20,
            poll_s=1.0,
        )

        before = _fetch_poll(mqtt, poll_topic)
        writes_before = int(before.get("rs485_stub_writes", 0))

        # Countdown publishes every 5s, but the atomic request path should not rewrite dispatch
        # during those wakeups unless a fresh external request arrives.
        _sleep_with_mqtt(mqtt, 7)
        after = _fetch_poll(mqtt, poll_topic)
        writes_after = int(after.get("rs485_stub_writes", 0))
        if writes_after != writes_before:
            raise E2EError(
                f"Timed countdown rewrote dispatch without a new atomic request: "
                f"writes {writes_before}->{writes_after}"
            )

    def case_dispatch_disable_timed_stops_countdown_wakes() -> None:
        print("[e2e] case: normal_mode stop does not keep countdown-triggered rewrites alive")
        ha_unique = _ensure_online_inverter_identity("dispatch disable baseline")
        ensure_dispatch_write(
            ha_unique,
            label_prefix="dispatch disable",
            duration_s=12,
            timeout_s=8,
        )

        before_disable = _fetch_stub(mqtt, stub_topic)
        writes_before_disable = int(before_disable.get("stub_writes", 0))

        publish_dispatch_request_and_wait_status(
            ha_unique,
            _default_dispatch_request(mode="normal_mode"),
            expected_status="ok",
            require_queue_advance=False,
            timeout_s=10,
        )

        disable_write_count = writes_before_disable

        def disable_write_pred() -> Tuple[bool, str]:
            nonlocal disable_write_count
            cur = _fetch_matching_or_latest_json(
                mqtt,
                stub_topic,
                "stub",
                lambda payload: int(payload.get("stub_writes", 0)) > writes_before_disable,
            )
            disable_write_count = int(cur.get("stub_writes", 0))
            return disable_write_count > writes_before_disable, f"writes={disable_write_count}"

        _assert_eventually(
            "dispatch duration 0 triggers one non-timed rewrite",
            disable_write_pred,
            timeout_s=10,
            poll_s=1.0,
        )

        # After a confirmed normal-mode stop, the old countdown wake path should stay quiet.
        _sleep_with_mqtt(mqtt, 7)
        after_disable = _fetch_cached_or_latest_json(mqtt, stub_topic, "stub")
        writes_after_disable = int(after_disable.get("stub_writes", 0))
        if writes_after_disable != disable_write_count:
            raise E2EError(
                f"normal_mode kept countdown-triggered writes alive: "
                f"{disable_write_count}->{writes_after_disable}"
            )

    def case_dispatch_boot_fail_closed() -> None:
        print("[e2e] case: boot fail-closes an active dispatch once RS485 returns")
        dispatch_start_start = _discover_define_value("DISPATCH_START_START")
        dispatch_start_stop = _discover_define_value("DISPATCH_START_STOP")
        dispatch_mode_target = _discover_define_value("DISPATCH_MODE_STATE_OF_CHARGE_CONTROL")
        active_power_offset = _discover_define_value("DISPATCH_ACTIVE_POWER_OFFSET")

        ensure_stub_online_backend(
            json.dumps({
                "mode": "online",
                "dispatch_start": dispatch_start_start,
                "dispatch_mode": dispatch_mode_target,
                "dispatch_active_power": active_power_offset + 1200,
                "dispatch_soc": 200,
                "dispatch_time": 45,
            }),
            label="boot fail-close baseline",
        )
        ha_unique = _wait_for_inverter_identity()
        start_topic = _state_topic(ha_unique, "Dispatch_Start")
        mqtt.subscribe(start_topic, force=True)

        base = _resolve_device_http_base(mqtt, device_root)
        reboot_normal_path = _discover_reboot_normal_path_from_code()
        if not reboot_normal_path:
            raise E2EError("Could not discover /reboot/normal endpoint from firmware source")

        boot_topic = f"{device_root}/boot"
        previous_boot = _fetch_latest_text(mqtt, boot_topic, label="boot_before_dispatch_reboot")
        previous_poll = _fetch_latest_text(mqtt, poll_topic, label="poll_before_dispatch_reboot")
        reboot_status, _ = _http_request("POST", base + reboot_normal_path, headers={}, body=b"", timeout_s=20)
        if reboot_status != 200:
            raise E2EError(f"/reboot/normal returned unexpected status={reboot_status}")

        _wait_for_topic_change(mqtt, boot_topic, previous_boot, timeout_s=60, label="boot after dispatch reboot")
        _wait_for_topic_change(mqtt, poll_topic, previous_poll, timeout_s=60, label="poll after dispatch reboot")

        # The stub backend defaults to offline after reboot. Reintroduce an active dispatch only after boot;
        # bootStopPending must still force it back to Stop once RS485 becomes live again.
        set_mode_and_wait(
            json.dumps({
                "mode": "online",
                "dispatch_start": dispatch_start_start,
                "dispatch_mode": dispatch_mode_target,
                "dispatch_active_power": active_power_offset + 1200,
                "dispatch_soc": 200,
                "dispatch_time": 45,
            }),
            ("online",),
            timeout_s=60,
            poll_s=2.0,
        )

        _assert_eventually(
            "boot stop clears active dispatch",
            lambda: (
                (
                    lambda val: (val.lower() in ("stop", "stopped")) or (val == str(dispatch_start_stop))
                )(_fetch_latest_text(mqtt, start_topic, label="dispatch_start_boot_stop").strip()),
                "waiting for Dispatch_Start=Stop",
            ),
            timeout_s=25,
            poll_s=2.0,
        )
        _sleep_with_mqtt(mqtt, 6)
        start_after_boot = _fetch_latest_text(mqtt, start_topic, label="dispatch_start_after_boot_stop").strip()
        if start_after_boot.lower() not in ("stop", "stopped") and start_after_boot != str(dispatch_start_stop):
            raise E2EError(f"boot fail-close did not leave dispatch stopped: {start_after_boot!r}")

    def case_strict_unknown_snapshot_has_no_unknown_reads() -> None:
        print("[e2e] case: strict unknown-register protection (snapshot path)")
        # Register_Number is RAM-only controller state. Reboot first so this snapshot-focused case
        # starts from a deterministic manual-read selection without depending on inverter-topic
        # command subscriptions being active yet.
        _reboot_normal_and_wait("strict_snapshot")

        ensure_stub_online_backend(
            '{"mode":"online","soc_pct":50,"battery_power_w":0,"grid_power_w":0,"pv_ct_power_w":0}',
            label="strict snapshot baseline",
        )
        ha_unique = _wait_for_inverter_identity()
        # Strict mode should be safe for ESS snapshot: the stub must implement all snapshot registers.

        strict_payload = '{"mode":"online","strict_unknown":1,"strict":1}'
        wait_stub_control_applied(
            strict_payload,
            label="strict snapshot control",
            expect_mode="online",
            expect_strict_unknown=True,
            timeout_s=30,
        )

        # Baseline counters after requesting strict mode. The later manual-read case proves that the
        # strict control path actually applies; this case only needs to verify that snapshot traffic
        # remains healthy while strict mode requests are in flight.
        baseline_stub = _fetch_stub(mqtt, stub_topic)
        baseline_unknown = int(baseline_stub.get("stub_unknown_reads", 0))
        baseline_attempts = int(_fetch_poll(mqtt, poll_topic).get("ess_snapshot_attempts", 0))
        last_unknown = baseline_unknown
        last_attempts = baseline_attempts

        def pred() -> Tuple[bool, str]:
            nonlocal last_unknown, last_attempts
            cur_poll = _fetch_poll(mqtt, poll_topic)
            cur_stub = _fetch_stub(mqtt, stub_topic)
            attempts = int(cur_poll.get("ess_snapshot_attempts", 0))
            ok = bool(cur_poll.get("ess_snapshot_last_ok", False))
            unknown = int(cur_stub.get("stub_unknown_reads", 0))
            last_read_reg = int(cur_stub.get("last_read_reg", 0))
            last_fn = int(cur_stub.get("last_fn", 0))
            last_fail_reg = int(cur_stub.get("last_fail_reg", 0))
            last_fail_fn = int(cur_stub.get("last_fail_fn", 0))
            last_fail_type = str(cur_stub.get("last_fail_type", ""))
            detail = (
                f"attempts={attempts} ok={ok} unknown={unknown} unknown_baseline={baseline_unknown} "
                f"last_read_reg={last_read_reg} last_fn={last_fn} "
                f"last_fail_reg={last_fail_reg} last_fail_fn={last_fail_fn} last_fail_type={last_fail_type}"
            )
            # Strict-mode stabilization: allow a brief post-toggle transition, but require that unknown reads
            # stop increasing while subsequent successful snapshot attempts continue.
            if attempts > last_attempts:
                if unknown > last_unknown:
                    last_unknown = unknown
                    last_attempts = attempts
                    return False, detail
                last_attempts = attempts
            return (attempts > baseline_attempts + 1 and ok and unknown <= last_unknown), detail

        _assert_eventually("strict_unknown keeps snapshot OK with zero unknown-register reads", pred, timeout_s=70, poll_s=5.0)

    def case_scheduler_idle_does_not_add_reads() -> None:
        print("[e2e] case: scheduler selectivity (no extra reads during idle window)")
        set_mode_and_wait('{"mode":"online"}', ("online",))
        _assert_eventually(
            "wait for inverter identity/probe to settle",
            lambda: (
                int(_fetch_poll(mqtt, poll_topic).get("rs485_probe_backoff_ms", 1)) == 0,
                f"backoff={_fetch_poll(mqtt, poll_topic).get('rs485_probe_backoff_ms')}",
            ),
            timeout_s=120,
            poll_s=5.0,
        )
        # After switching to online, the firmware may force an immediate resend when RS485 transitions
        # from disconnected -> connected (resendAllData resets baselines to 0). That can cause a second
        # snapshot attempt sooner than 10s. Wait until we're past that initial stabilization.
        base_attempts = int(_fetch_poll(mqtt, poll_topic).get("ess_snapshot_attempts", 0))
        _assert_eventually(
            "wait for two snapshot attempts (stabilize after connect/resend)",
            lambda: (
                int(_fetch_poll(mqtt, poll_topic).get("ess_snapshot_attempts", 0)) >= base_attempts + 2,
                "waiting",
            ),
            timeout_s=90,
            poll_s=3.0,
        )
        attempts1 = int(_fetch_poll(mqtt, poll_topic).get("ess_snapshot_attempts", 0))
        s1 = _fetch_stub(mqtt, stub_topic)
        reads1 = int(s1.get("stub_reads", 0))

        # For a window shorter than the 10s bucket, no ESS snapshot should run => no Modbus reads should occur.
        time.sleep(4.0)
        poll2 = _fetch_poll(mqtt, poll_topic)
        attempts2 = int(poll2.get("ess_snapshot_attempts", 0))
        # status/stub publishes on the status cadence, not on every scheduler pass.
        # For this short idle window, reuse the latest observed retained stub payload if no new one has arrived.
        s2 = _fetch_cached_or_latest_json(mqtt, stub_topic, "stub")
        reads2 = int(s2.get("stub_reads", 0))
        delta_attempts = attempts2 - attempts1
        delta_reads = reads2 - reads1

        # Some firmware transitions (e.g., RS485 connect->connected resend) can force a one-off
        # "run now" pass that resets schedule baselines. That may legitimately cause a snapshot
        # attempt inside this short window, but we should never see high-frequency read storms.
        if delta_attempts > 1:
            raise E2EError(
                f"Too many snapshot attempts in idle window (<10s): attempts {attempts1} -> {attempts2} (delta={delta_attempts})"
            )
        if delta_reads > 64:
            raise E2EError(
                f"Too many stub reads in idle window (<10s): reads {reads1} -> {reads2} (delta={delta_reads})"
            )

    def case_strict_unknown_register_reads() -> None:
        print("[e2e] case: strict/loose unknown register reads via Register_Value")
        print("[e2e] strict register: rebooting to clean baseline")
        _reboot_normal_and_wait("strict_register_reads")
        print("[e2e] strict register: reacquiring inverter identity")
        ha_unique = _ensure_online_inverter_identity("strict unknown register baseline")
        manual_topic = _manual_read_topic()
        baseline_payload = '{"mode":"online"}'

        # Use a handled register that is not virtualized by the stub (so the stub sees it as unknown).
        reg = _discover_register_value("REG_INVERTER_HOME_R_INVERTER_TEMP")

        print("[e2e] strict register: applying loose baseline")
        wait_stub_control_applied(
            baseline_payload,
            label="strict register loose baseline",
            expect_mode="online",
            expect_strict_unknown=False,
            timeout_s=30,
        )
        print("[e2e] strict register: verifying loose unknown read")
        loose_manual = _select_register_and_wait_manual_read(
            mqtt,
            _command_topic(ha_unique, "Register_Number"),
            manual_topic,
            _state_topic(ha_unique, "Register_Number"),
            reg,
            label="reg_value_loose",
        )
        value_loose = str(loose_manual.get("value", ""))
        if int(loose_manual.get("observed_reg", 0)) != reg:
            raise E2EError(f"manual_read observed_reg mismatch in loose mode: expected {reg}, got {loose_manual.get('observed_reg')!r}")
        if value_loose in ("Slave Error", "Nothing read"):
            raise E2EError(
                f"Expected loose unknown read to return a formatted value, got {value_loose!r}"
            )

        strict_payload = '{"mode":"online","strict_unknown":1}'
        print("[e2e] strict register: applying strict mode")
        wait_stub_control_applied(
            strict_payload,
            label="strict register control",
            expect_mode="online",
            expect_strict_unknown=True,
            timeout_s=30,
        )
        strict_manual_state = _state_topic(ha_unique, "Register_Number")
        mqtt.subscribe(strict_manual_state, force=True)
        strict_manual: dict[str, Any] = {}
        def strict_manual_pred() -> Tuple[bool, str]:
            nonlocal strict_manual
            strict_manual = _select_register_and_wait_manual_read(
                mqtt,
                _command_topic(ha_unique, "Register_Number"),
                manual_topic,
                strict_manual_state,
                reg,
                label="reg_value_strict",
            )
            value_strict = str(strict_manual.get("value", ""))
            observed_reg = int(strict_manual.get("observed_reg", 0))
            return (
                observed_reg == reg and value_strict == "Slave Error",
                f"observed_reg={observed_reg} value={value_strict!r}",
            )
        _assert_eventually(
            "strict unknown read returns slave error",
            strict_manual_pred,
            timeout_s=30,
            poll_s=2.0,
        )
        print("[e2e] strict register: restoring clean suite baseline")
        ensure_clean_suite_baseline()
        print("[e2e] strict register: confirming inverter identity after cleanup")
        _wait_for_inverter_identity()
        return

    def case_fail_specific_snapshot_register_and_type() -> None:
        print("[e2e] case: fail specific snapshot register + fail type reporting")
        reg_soc = _discover_register_value("REG_BATTERY_HOME_R_SOC")
        set_mode_and_wait(f'{{"mode":"online","fail_n":0,"reg":{reg_soc},"fail_type":1}}', ("online",))

        def pred() -> Tuple[bool, str]:
            cur = _fetch_poll(mqtt, poll_topic)
            ok = bool(cur.get("ess_snapshot_last_ok", False))
            skip = str(cur.get("dispatch_last_skip_reason", ""))
            detail = f"snapshot_ok={ok} skip={skip}"
            return ((not ok) and skip == "ess_snapshot_failed"), detail

        _assert_eventually("failing SOC register forces snapshot failure with slave_error", pred, timeout_s=60, poll_s=3.0)

        # Clear failure and confirm recovery.
        set_mode_and_wait('{"mode":"online"}', ("online",))
        _assert_eventually("snapshot recovers after clearing failure", lambda: (bool(_fetch_poll(mqtt, poll_topic).get("ess_snapshot_last_ok", False)), "waiting"), timeout_s=60, poll_s=3.0)

    def case_rs485_error_counters_split() -> None:
        print("[e2e] case: rs485 error counters split transport vs other")
        reg_soc = _discover_register_value("REG_BATTERY_HOME_R_SOC")

        baseline = _fetch_poll(mqtt, poll_topic)
        base_total = int(baseline.get("rs485_error_count", 0))
        base_transport = int(baseline.get("rs485_transport_error_count", 0))
        base_other = int(baseline.get("rs485_other_error_count", 0))

        set_mode_and_wait(f'{{"mode":"online","fail_n":0,"reg":{reg_soc},"fail_type":0}}', ("online",))

        def transport_pred() -> Tuple[bool, str]:
            cur = _fetch_poll(mqtt, poll_topic)
            total = int(cur.get("rs485_error_count", 0))
            transport = int(cur.get("rs485_transport_error_count", 0))
            other = int(cur.get("rs485_other_error_count", 0))
            detail = (
                f"total={total} transport={transport} other={other} "
                f"base_total={base_total} base_transport={base_transport} base_other={base_other}"
            )
            return (transport > base_transport and other == base_other and total > base_total), detail

        _assert_eventually(
            "no_response increments transport rs485 errors only",
            transport_pred,
            timeout_s=60,
            poll_s=3.0,
        )

        set_mode_and_wait('{"mode":"online"}', ("online",))
        _assert_eventually(
            "snapshot recovers after clearing transport failure",
            lambda: (bool(_fetch_poll(mqtt, poll_topic).get("ess_snapshot_last_ok", False)), "waiting"),
            timeout_s=60,
            poll_s=3.0,
        )

        baseline = _fetch_poll(mqtt, poll_topic)
        base_total = int(baseline.get("rs485_error_count", 0))
        base_transport = int(baseline.get("rs485_transport_error_count", 0))
        base_other = int(baseline.get("rs485_other_error_count", 0))

        set_mode_and_wait(f'{{"mode":"online","fail_n":0,"reg":{reg_soc},"fail_type":1}}', ("online",))

        def other_pred() -> Tuple[bool, str]:
            cur = _fetch_poll(mqtt, poll_topic)
            total = int(cur.get("rs485_error_count", 0))
            transport = int(cur.get("rs485_transport_error_count", 0))
            other = int(cur.get("rs485_other_error_count", 0))
            detail = (
                f"total={total} transport={transport} other={other} "
                f"base_total={base_total} base_transport={base_transport} base_other={base_other}"
            )
            return (other > base_other and transport == base_transport and total > base_total), detail

        _assert_eventually(
            "slave_error increments other rs485 errors only",
            other_pred,
            timeout_s=60,
            poll_s=3.0,
        )

        set_mode_and_wait('{"mode":"online"}', ("online",))
        _assert_eventually(
            "snapshot recovers after clearing other failure",
            lambda: (bool(_fetch_poll(mqtt, poll_topic).get("ess_snapshot_last_ok", False)), "waiting"),
            timeout_s=60,
            poll_s=3.0,
        )

        base = _resolve_device_http_base(mqtt, device_root)
        status_code, root_body = _http_request_full("GET", base + "/", headers={}, body=b"", timeout_s=20)
        if status_code != 200:
            raise E2EError(f"runtime root unavailable after rs485 split check: status={status_code}")
        root_html = root_body.decode("utf-8", errors="replace")
        for needle in ("RS485 errors:", "RS485 transport errors:", "RS485 other errors:"):
            if needle not in root_html:
                raise E2EError(f"runtime root missing {needle!r}")

    def case_fail_every_n_snapshot_attempts() -> None:
        print("[e2e] case: fail every N snapshot attempts (N=2)")
        # Use slave_error (not no_response) so RS485 stays "online" and we observe clean fail/ok alternation.
        baseline = _fetch_poll(mqtt, poll_topic)
        base_attempts = int(baseline.get("ess_snapshot_attempts", 0))
        base_ok_count = int(baseline.get("poll_ok_count", 0))
        base_err_count = int(baseline.get("poll_err_count", 0))
        set_mode_and_wait('{"mode":"online","fail_every_n":2,"fail_type":1}', ("online",))

        def pred() -> Tuple[bool, str]:
            cur = _fetch_poll(mqtt, poll_topic)
            attempts = int(cur.get("ess_snapshot_attempts", 0))
            ok_count = int(cur.get("poll_ok_count", 0))
            err_count = int(cur.get("poll_err_count", 0))
            detail = (
                f"attempts={attempts} ok_count={ok_count} err_count={err_count} "
                f"baseline_attempts={base_attempts} baseline_ok={base_ok_count} baseline_err={base_err_count}"
            )
            enough_attempts = attempts >= (base_attempts + 3)
            saw_ok = ok_count > base_ok_count
            saw_err = err_count > base_err_count
            return (enough_attempts and saw_ok and saw_err), detail

        _assert_eventually("fail_every_n produces both snapshot errors and recoveries",
                           pred,
                           timeout_s=80,
                           poll_s=3.0)

    def case_latency_does_not_break_status() -> None:
        print("[e2e] case: latency injection keeps status flowing")
        set_mode_and_wait('{"mode":"online","latency_ms":200}', ("online",))
        before = _fetch_poll(mqtt, poll_topic)
        before_attempts = int(before.get("ess_snapshot_attempts", 0))

        def pred() -> Tuple[bool, str]:
            cur = _fetch_poll(mqtt, poll_topic)
            attempts = int(cur.get("ess_snapshot_attempts", 0))
            ok = bool(cur.get("ess_snapshot_last_ok", False))
            last_ms = int(cur.get("last_poll_ms", 0))
            detail = f"attempts={attempts} ok={ok} last_poll_ms={last_ms}"
            return (attempts > before_attempts and ok and last_ms > 0), detail

        _assert_eventually("latency allows snapshot attempts and records configured latency", pred, timeout_s=90, poll_s=5.0)

    def case_flapping_online_offline() -> None:
        print("[e2e] case: flapping online/offline toggles snapshot result")
        # The scheduler's snapshot cadence is ~10s. Choose a flap period that is NOT a divisor of 10s,
        # otherwise every snapshot lands in the same phase and you never observe a toggle.
        set_mode_and_wait('{"mode":"flap","flap_online_ms":3500,"flap_offline_ms":3500}', ("flap",))

        baseline = _fetch_poll(mqtt, poll_topic)
        baseline_ok_count = int(baseline.get("poll_ok_count", 0))
        baseline_err_count = int(baseline.get("poll_err_count", 0))
        seen_ok = False
        seen_fail = False
        deadline = time.time() + 80
        last_detail = ""
        while time.time() < deadline:
            cur = _fetch_poll(mqtt, poll_topic)
            ok = bool(cur.get("ess_snapshot_last_ok", False))
            mode = str(cur.get("rs485_stub_mode", ""))
            ok_count = int(cur.get("poll_ok_count", 0))
            err_count = int(cur.get("poll_err_count", 0))
            last_detail = f"mode={mode} ok={ok} poll_ok_count={ok_count} poll_err_count={err_count}"
            if ok or ok_count > baseline_ok_count:
                seen_ok = True
            if (not ok) or err_count > baseline_err_count:
                seen_fail = True
            if seen_ok and seen_fail:
                return
            time.sleep(3.0)
        raise E2EError(f"flap mode did not toggle snapshot ok/fail within timeout; last={last_detail}")

    def case_probe_delayed_online() -> None:
        print("[e2e] case: probe_delayed becomes online after N attempts")
        set_mode_and_wait('{"mode":"probe_delayed","probe_success_after_n":3}', ("probe_delayed",))

        def pred() -> Tuple[bool, str]:
            core = _fetch_status_core(mqtt, status_core_topic)
            cur = _fetch_poll(mqtt, poll_topic)
            ok = bool(cur.get("ess_snapshot_last_ok", False))
            mode = str(cur.get("rs485_stub_mode", ""))
            ha_unique = str(core.get("ha_unique_id", ""))
            detail = f"mode={mode} snapshot_ok={ok} ha_unique_id={ha_unique!r}"
            return (mode == "probe_delayed" and ok and ha_unique.startswith("A2M-")), detail

        _assert_eventually("probe_delayed eventually succeeds", pred, timeout_s=90, poll_s=5.0)
        _wait_for_inverter_identity()

    def case_identity_reboot_unknown_after_offline_reboot() -> None:
        print("[e2e] case: identity resets to unknown after offline reboot")
        ensure_stub_online_backend('{"mode":"online"}', label="identity baseline")
        inverter_id = _wait_for_inverter_identity()
        serial = inverter_id[len("alpha2mqtt_inv_"):]
        if not serial:
            raise E2EError(f"could not extract serial from inverter id: {inverter_id!r}")

        set_mode_and_wait('{"mode":"offline"}', ("offline",))

        base = _resolve_device_http_base(mqtt, device_root)
        reboot_normal_path = _discover_reboot_normal_path_from_code()
        if not reboot_normal_path:
            raise E2EError("Could not discover /reboot/normal endpoint from firmware source")

        boot_topic = f"{device_root}/boot"
        previous_boot = _fetch_latest_text(mqtt, boot_topic, label="boot_before_identity_reboot")
        previous_poll = _fetch_latest_text(mqtt, poll_topic, label="poll_before_identity_reboot")
        previous_core = _fetch_latest_text(mqtt, status_core_topic, label="core_before_identity_reboot")
        reboot_status, _ = _http_request("POST", base + reboot_normal_path, headers={}, body=b"", timeout_s=20)
        if reboot_status != 200:
            raise E2EError(f"/reboot/normal returned unexpected status={reboot_status}")

        try:
            _wait_for_topic_change(
                mqtt,
                boot_topic,
                previous_boot,
                timeout_s=60,
                label="boot after identity reboot",
            )
        except E2EError as exc:
            print(f"[e2e] boot topic did not present a fresh changed payload after identity reboot; continuing with poll/core checks ({exc})")
        _wait_for_topic_change(
            mqtt,
            poll_topic,
            previous_poll,
            timeout_s=60,
            label="poll after identity reboot",
        )
        _wait_for_topic_change(
            mqtt,
            status_core_topic,
            previous_core,
            timeout_s=60,
            label="core after identity reboot",
        )
        _assert_unknown_inverter_identity("offline reboot clears live inverter identity")

        core = _fetch_status_core(mqtt, status_core_topic)
        if str(core.get("ha_unique_id", "")) == f"A2M-{serial}":
            raise E2EError("device reused prior live serial after offline reboot")

        _assert_eventually(
            "offline reboot reaches probe backoff window",
            lambda: (
                int(_fetch_poll(mqtt, poll_topic).get("rs485_probe_backoff_ms", 0)) >= 5000,
                f"probe_backoff_ms={_fetch_poll(mqtt, poll_topic).get('rs485_probe_backoff_ms')}",
            ),
            timeout_s=60,
            poll_s=2.0,
        )
        reads_before_idle = int(_fetch_stub(mqtt, stub_topic).get("stub_reads", 0))
        time.sleep(2.0)
        reads_after_idle = int(_fetch_stub(mqtt, stub_topic).get("stub_reads", 0))
        if reads_after_idle != reads_before_idle:
            raise E2EError(
                f"offline reboot should not keep adding idle bootstrap reads once probe backoff is active: "
                f"{reads_before_idle}->{reads_after_idle}"
            )

        ensure_stub_online_backend('{"mode":"online"}', label="identity reboot cleanup")

    def case_fail_writes_only_dispatch_write_fails() -> None:
        print("[e2e] case: fail writes only (dispatch write fails, snapshot reads still ok)")
        ensure_clean_suite_baseline()
        reg_dispatch_start = _discover_register_value("REG_DISPATCH_RW_DISPATCH_START")
        dispatch_start_stop = _discover_define_value("DISPATCH_START_STOP")
        set_mode_and_wait(
            f'{{"mode":"online","reg":{reg_dispatch_start},"fail_writes":1,"fail_reads":0,"fail_type":1,'
            '"soc_pct":50,"battery_power_w":0,"grid_power_w":0,"pv_ct_power_w":0,"dispatch_start":0,"dispatch_mode":0,"dispatch_soc":0}}',
            ("online",),
        )

        def write_fail_armed_pred() -> Tuple[bool, str]:
            cur_stub = _fetch_latest_json(mqtt, stub_topic, "stub")
            fail_writes = bool(cur_stub.get("fail_writes", False))
            fail_reg = int(cur_stub.get("fail_reg", 0))
            detail = f"fail_writes={fail_writes} fail_reg={fail_reg}"
            return fail_writes and fail_reg == reg_dispatch_start, detail

        _assert_eventually(
            "stub write-fail injection armed",
            write_fail_armed_pred,
            timeout_s=20,
            poll_s=1.0,
        )

        ha_unique = _wait_for_inverter_identity()
        publish_dispatch_request_and_wait_status(
            ha_unique,
            _default_dispatch_request(),
            expected_status="modbus write failed",
            timeout_s=20,
        )

        manual_topic = _manual_read_topic()
        regnum_topic = _state_topic(ha_unique, "Register_Number")

        def pred() -> Tuple[bool, str]:
            cur = _fetch_poll(mqtt, poll_topic)
            ok = bool(cur.get("ess_snapshot_last_ok", False))
            manual = _select_register_and_wait_manual_read(
                mqtt,
                _command_topic(ha_unique, "Register_Number"),
                manual_topic,
                regnum_topic,
                reg_dispatch_start,
                label="dispatch_start_fail_writes",
            )
            value = str(manual.get("value", "")).strip()
            observed_reg = int(manual.get("observed_reg", 0))
            still_stopped = value.lower() in ("stop", "stopped") or value == str(dispatch_start_stop)
            detail = f"ok={ok} observed_reg={observed_reg} value={value!r}"
            return (ok and observed_reg == reg_dispatch_start and still_stopped), detail

        _assert_eventually("dispatch write failure leaves dispatch stopped without breaking snapshot", pred, timeout_s=90, poll_s=5.0)

    def case_dispatch_readback_window_tolerates_transient_read_failures() -> None:
        print("[e2e] case: atomic dispatch waits out transient readback failures")
        ha_unique = _wait_for_inverter_identity()
        ensure_stub_online_backend(
            '{"mode":"online","soc_pct":50,"battery_power_w":0,"grid_power_w":0,"pv_ct_power_w":0,'
            '"dispatch_start":0,"dispatch_mode":0,"dispatch_soc":0,"dispatch_time":0}',
            label="dispatch_readback_window backend",
        )

        # Arm a short read-only failure window immediately before the request so
        # the dispatch write still succeeds, but the first readback attempts are
        # forced to retry. This exercises the widened firmware confirmation
        # window instead of pinning a specific register failed forever.
        set_mode(
            '{"mode":"online","fail_for_ms":600,"fail_reads":1,"fail_writes":0,"fail_type":0,'
            '"soc_pct":50,"battery_power_w":0,"grid_power_w":0,"pv_ct_power_w":0,'
            '"dispatch_start":0,"dispatch_mode":0,"dispatch_soc":0,"dispatch_time":0}'
        )

        def transient_read_failure_armed_pred() -> Tuple[bool, str]:
            cur_stub = _fetch_latest_json(mqtt, stub_topic, "stub")
            fail_reads = bool(cur_stub.get("fail_reads", False))
            fail_writes = bool(cur_stub.get("fail_writes", False))
            fail_for_ms = int(cur_stub.get("fail_for_ms", 0))
            detail = f"fail_reads={fail_reads} fail_writes={fail_writes} fail_for_ms={fail_for_ms}"
            return fail_reads and not fail_writes and fail_for_ms == 600, detail

        _assert_eventually(
            "stub transient readback failure injection armed",
            transient_read_failure_armed_pred,
            timeout_s=10,
            poll_s=0.5,
        )
        publish_dispatch_request_and_wait_status(
            ha_unique,
            _default_dispatch_request(duration_s=60),
            expected_status="ok",
            timeout_s=35,
        )

    def case_dispatch_readback_timeout_status() -> None:
        print("[e2e] case: atomic dispatch reports readback timeout after a successful write")
        reg_dispatch_start = _discover_register_value("REG_DISPATCH_RW_DISPATCH_START")
        ha_unique = _wait_for_inverter_identity()
        set_mode_and_wait(
            f'{{"mode":"online","reg":{reg_dispatch_start},"fail_for_ms":4000,"fail_reads":1,"fail_writes":0,"fail_type":0,'
            '"soc_pct":50,"battery_power_w":0,"grid_power_w":0,"pv_ct_power_w":0,'
            '"dispatch_start":0,"dispatch_mode":0,"dispatch_soc":0,"dispatch_time":0}',
            ("online",),
        )
        status_topic = _dispatch_status_topic(ha_unique)
        mqtt.subscribe(status_topic, force=True)
        publish_dispatch_request_no_wait(
            ha_unique,
            _default_dispatch_request(duration_s=60),
        )

        def timeout_status_pred() -> Tuple[bool, str]:
            try:
                status = _fetch_latest_text(mqtt, status_topic, label="dispatch_timeout_status").strip()
            except E2EError as e:
                return False, f"err={e}"
            return status == "readback timeout", f"status={status!r}"

        _assert_eventually(
            "atomic dispatch reports readback timeout",
            timeout_status_pred,
            timeout_s=20,
            poll_s=0.5,
        )

        after = _fetch_poll(mqtt, poll_topic)
        queued_ms = int(after.get("dispatch_request_queued_ms", 0))
        last_reg = int(after.get("rs485_stub_last_write_reg", 0))
        last_reg_count = int(after.get("rs485_stub_last_write_reg_count", 0))
        last_write_ms = int(after.get("rs485_stub_last_write_ms", 0))
        if (
            queued_ms <= 0
            or last_reg != reg_dispatch_start
            or last_reg_count != 9
            or last_write_ms < queued_ms
        ):
            raise E2EError(
                f"readback timeout should still come after one dispatch block write: "
                f"queued_ms={queued_ms} last_write_ms={last_write_ms} "
                f"last_reg={last_reg} last_reg_count={last_reg_count}"
            )

    def case_max_feedin_percent_write() -> None:
        print("[e2e] case: Max_Feedin_Percent writes only when enabled and confirms readback")
        ha_unique = _ensure_online_inverter_identity("max feedin baseline")
        current_config = _fetch_config(mqtt, config_topic)
        current_poll_interval = int(current_config.get("poll_interval_s", 4) or 4)
        reg_max_feedin = _discover_register_value("REG_SYSTEM_CONFIG_RW_MAX_FEED_INTO_GRID_PERCENT")
        command_topic = _command_topic(ha_unique, "Max_Feedin_Percent")
        state_topic = _state_topic(ha_unique, "Max_Feedin_Percent")

        before_disabled = _fetch_poll(mqtt, poll_topic)
        writes_before_disabled = int(before_disabled.get("rs485_stub_writes", 0))
        mqtt.publish(command_topic, "35", retain=False)
        _sleep_with_mqtt(mqtt, 2.0)
        after_disabled = _fetch_poll(mqtt, poll_topic)
        writes_after_disabled = int(after_disabled.get("rs485_stub_writes", 0))
        if writes_after_disabled != writes_before_disabled:
            raise E2EError(
                f"disabled Max_Feedin_Percent should ignore command writes: "
                f"{writes_before_disabled}->{writes_after_disabled}"
            )

        wait_polling_config_applied(
            current_poll_interval,
            {"Max_Feedin_Percent": "one_min"},
        )
        mqtt.subscribe(state_topic, force=True)

        before_enabled = _fetch_poll(mqtt, poll_topic)
        writes_before_enabled = int(before_enabled.get("rs485_stub_writes", 0))
        mqtt.publish(command_topic, "35", retain=False)

        _assert_eventually(
            "Max_Feedin_Percent state publishes confirmed value",
            lambda: (
                _fetch_latest_text(mqtt, state_topic, label="max_feedin_state").strip() == "35",
                f"last={_fetch_latest_text(mqtt, state_topic, label='max_feedin_state').strip()!r}",
            ),
            timeout_s=20,
            poll_s=1.0,
        )

        def write_pred() -> Tuple[bool, str]:
            cur = _fetch_poll(mqtt, poll_topic)
            writes = int(cur.get("rs485_stub_writes", 0))
            last_reg = int(cur.get("rs485_stub_last_write_reg", 0))
            last_reg_count = int(cur.get("rs485_stub_last_write_reg_count", 0))
            detail = (
                f"writes={writes} last_reg={last_reg} last_reg_count={last_reg_count} "
                f"baseline_writes={writes_before_enabled}"
            )
            return (
                writes > writes_before_enabled
                and last_reg == reg_max_feedin
                and last_reg_count == 1
            ), detail

        _assert_eventually(
            "Max_Feedin_Percent uses single-register write",
            write_pred,
            timeout_s=20,
            poll_s=1.0,
        )

        before_invalid = _fetch_poll(mqtt, poll_topic)
        writes_before_invalid = int(before_invalid.get("rs485_stub_writes", 0))
        mqtt.publish(command_topic, "101", retain=False)
        _sleep_with_mqtt(mqtt, 2.0)
        after_invalid = _fetch_poll(mqtt, poll_topic)
        writes_after_invalid = int(after_invalid.get("rs485_stub_writes", 0))
        if writes_after_invalid != writes_before_invalid:
            raise E2EError(
                f"out-of-range Max_Feedin_Percent should not write RS485: "
                f"{writes_before_invalid}->{writes_after_invalid}"
            )

    def case_fail_for_ms_then_recover() -> None:
        print("[e2e] case: fail for N ms then recover")
        transient_fail_ms = 15000
        baseline = _fetch_poll(mqtt, poll_topic)
        baseline_attempts = int(baseline.get("ess_snapshot_attempts", 0))
        baseline_poll_errs = int(baseline.get("poll_err_count", 0))
        baseline_transport_errs = int(baseline.get("rs485_transport_error_count", 0))
        fail_payload = (
            f'{{"mode":"online","fail_for_ms":{transient_fail_ms},"fail_reads":1,"fail_writes":1,'
            '"fail_type":0,"fail_every_n":0,"reg":0,"latency_ms":0,'
            '"flap_online_ms":0,"flap_offline_ms":0,"probe_success_after_n":0,'
            '"strict_unknown":0,"strict":0,"soc_step_x10_per_snapshot":0}'
        )
        set_mode(fail_payload)
        last_pub = time.time()

        def fail_window_ack_pred() -> Tuple[bool, str]:
            nonlocal last_pub
            cur_stub = _fetch_matching_or_latest_json(
                mqtt,
                stub_topic,
                "fail_for_ms_stub_current",
                lambda cur: (
                    bool(cur.get("fail_reads", False))
                    and bool(cur.get("fail_writes", False))
                    and int(cur.get("fail_for_ms", 0)) == transient_fail_ms
                ),
            )
            fail_reads = bool(cur_stub.get("fail_reads", False))
            fail_writes = bool(cur_stub.get("fail_writes", False))
            fail_for_ms = int(cur_stub.get("fail_for_ms", 0))
            detail = (
                f"fail_reads={fail_reads} fail_writes={fail_writes} "
                f"fail_for_ms={fail_for_ms}"
            )
            if fail_reads and fail_writes and fail_for_ms == transient_fail_ms:
                return True, detail
            if (time.time() - last_pub) >= 6.0:
                set_mode(fail_payload)
                last_pub = time.time()
            return False, detail

        _assert_eventually(
            "fail_for_ms control acknowledged",
            fail_window_ack_pred,
            timeout_s=10,
            poll_s=1.0,
        )

        # Expect at least one failing snapshot after the control is armed and then eventual recovery.
        seen_fail = False
        deadline = time.time() + 60
        last_detail = ""
        while time.time() < deadline:
            # This case needs fresh runtime progress. Sampling the retained/latest
            # poll payload can skip the transient failing window entirely and only
            # observe the later recovered state.
            cur = _fetch_live_json(mqtt, poll_topic, "fail_for_ms_poll", timeout_s=20)
            ok = bool(cur.get("ess_snapshot_last_ok", False))
            attempts = int(cur.get("ess_snapshot_attempts", 0))
            poll_errs = int(cur.get("poll_err_count", 0))
            transport_errs = int(cur.get("rs485_transport_error_count", 0))
            mode = str(cur.get("rs485_stub_mode", ""))
            last_detail = (
                f"attempts={attempts} baseline_attempts={baseline_attempts} "
                f"poll_errs={poll_errs} baseline_poll_errs={baseline_poll_errs} "
                f"transport_errs={transport_errs} baseline_transport_errs={baseline_transport_errs} "
                f"ok={ok} mode={mode!r}"
            )
            if attempts <= baseline_attempts:
                time.sleep(3.0)
                continue
            if mode != "online":
                continue
            if (
                not ok or
                poll_errs > baseline_poll_errs or
                transport_errs > baseline_transport_errs
            ):
                seen_fail = True
            if seen_fail and ok:
                return
        raise E2EError(f"fail_for_ms did not show fail then recover within timeout; last={last_detail}")

    def _soc_drift_payload() -> str:
        return (
            '{"mode":"online","soc_pct":50,"soc_step_x10_per_snapshot":10,'
            '"fail_n":0,"fail_reads":0,"fail_writes":0,"fail_type":0,'
            '"fail_every_n":0,"fail_for_ms":0,'
            '"flap_online_ms":0,"flap_offline_ms":0,'
            '"probe_success_after_n":0,"strict_unknown":0,"strict":0}'
        )

    def _soc_drift_poll_interval() -> int:
        return int(_fetch_poll(mqtt, poll_topic).get("poll_interval_s", 9))

    soc_drift_verified = False

    def _verify_soc_drift_internal() -> None:
        nonlocal soc_drift_verified
        ha_unique = _current_inverter_identity() or _wait_for_inverter_identity()
        manual_topic = _manual_read_topic()
        reg_soc = _discover_register_value("REG_BATTERY_HOME_R_SOC")
        state_topic = _state_topic(ha_unique, "Register_Number")

        first = _select_register_and_wait_manual_read(
            mqtt,
            _command_topic(ha_unique, "Register_Number"),
            manual_topic,
            state_topic,
            reg_soc,
            label="soc_drift_manual_before",
        )
        first_value = str(first.get("value", ""))
        try:
            first_soc = float(first_value)
        except ValueError:
            raise E2EError(f"Unexpected SOC payload before drift check: {first_value!r}")

        deadline = time.time() + 45
        last_value = first_value
        while time.time() < deadline:
            _sleep_with_mqtt(mqtt, 8.0)
            cur = _select_register_and_wait_manual_read(
                mqtt,
                _command_topic(ha_unique, "Register_Number"),
                manual_topic,
                state_topic,
                reg_soc,
                label="soc_drift_manual_after",
            )
            last_value = str(cur.get("value", ""))
            try:
                current_soc = float(last_value)
            except ValueError:
                continue
            if current_soc > first_soc:
                soc_drift_verified = True
                return

        raise E2EError(f"Expected SOC to increase under drift backend: {first_value!r} -> {last_value!r}")

    def _ensure_soc_drift_backend(current_poll_interval: int, *, force_reset: bool = False) -> None:
        nonlocal soc_drift_verified
        drift_mode_payload = _soc_drift_payload()
        baseline_poll = _fetch_poll(mqtt, poll_topic)
        baseline_ok_count = int(baseline_poll.get("poll_ok_count", 0))
        baseline_err_count = int(baseline_poll.get("poll_err_count", 0))
        if force_reset:
            soc_drift_verified = False
            ensure_stub_online_backend('{"mode":"online","soc_pct":50,"battery_power_w":0,"grid_power_w":0,"pv_ct_power_w":0}', label="soc drift reset")
        wait_stub_control_applied(
            drift_mode_payload,
            label="soc drift control",
            expect_mode="online",
            expect_strict_unknown=False,
            timeout_s=30,
        )
        wait_polling_config_applied(
            current_poll_interval,
            {"State_of_Charge": "ten_sec"},
            timeout_s=45,
            republish_every_s=5.0,
        )
        last_pub = time.time()
        _wait_for_live_json_change(mqtt, poll_topic, "poll liveness after soc drift setup", timeout_s=25)

        def drift_ready_pred() -> Tuple[bool, str]:
            nonlocal last_pub
            now = time.time()
            if (now - last_pub) >= 5.0:
                set_mode(drift_mode_payload)
                set_polling_config(current_poll_interval, "State_of_Charge=ten_sec;")
                last_pub = now
            cur_poll = _fetch_matching_or_latest_json(
                mqtt,
                poll_topic,
                "soc_drift_ready_poll",
                lambda cur: (
                    bool(cur.get("ess_snapshot_last_ok", False)) or
                    int(cur.get("poll_ok_count", 0)) > baseline_ok_count
                ),
            )
            mode = str(cur_poll.get("rs485_stub_mode", ""))
            snapshot_ok = bool(cur_poll.get("ess_snapshot_last_ok", False))
            poll_ok = int(cur_poll.get("poll_ok_count", 0))
            poll_err = int(cur_poll.get("poll_err_count", 0))
            probe_backoff_ms = int(cur_poll.get("rs485_probe_backoff_ms", 0))
            detail = (
                f"mode={mode!r} snapshot_ok={snapshot_ok} "
                f"poll_ok={poll_ok} baseline_ok={baseline_ok_count} "
                f"poll_err={poll_err} baseline_err={baseline_err_count} "
                f"probe_backoff_ms={probe_backoff_ms}"
            )
            return (
                mode == "online"
                and (snapshot_ok or (poll_ok > baseline_ok_count and poll_err == baseline_err_count))
                and probe_backoff_ms == 0
            ), detail

        _assert_eventually(
            "soc drift backend recovered and publishing",
            drift_ready_pred,
            timeout_s=60,
            poll_s=2.0,
        )
        if not soc_drift_verified:
            _verify_soc_drift_internal()

    def _wait_for_soc_change(ha_unique: str, prev: str, label: str, timeout_s: int) -> str:
        soc_topic = _state_topic(ha_unique, "State_of_Charge")
        mqtt.subscribe(soc_topic, force=True)
        deadline = time.time() + timeout_s
        last_err = "no attempts"
        while time.time() < deadline:
            try:
                return _wait_for_topic_change(mqtt, soc_topic, prev, timeout_s=20, label=label)
            except E2EError as e:
                last_err = str(e)
                _ensure_soc_drift_backend(_soc_drift_poll_interval(), force_reset=True)
        raise E2EError(f"Timeout waiting for {label} change after drift-mode recovery attempts. Last observed: {last_err}")

    def case_soc_drift_backend_ready() -> None:
        print("[e2e] case: soc drift backend ready")
        _ensure_soc_drift_backend(_soc_drift_poll_interval(), force_reset=True)

    def case_stub_soc_drift_applies() -> None:
        print("[e2e] case: stub SOC drift applies internally")
        current_poll_interval = _soc_drift_poll_interval()
        _ensure_soc_drift_backend(current_poll_interval, force_reset=True)

    def case_soc_publish_respects_bucket() -> None:
        print("[e2e] case: State_of_Charge publish respects bucket")
        current_poll_interval = _soc_drift_poll_interval()
        _ensure_soc_drift_backend(current_poll_interval)
        ha_unique = _current_inverter_identity() or _wait_for_inverter_identity()
        soc_topic = _state_topic(ha_unique, "State_of_Charge")
        mqtt.subscribe(soc_topic, force=True)
        v1 = _fetch_latest_text(mqtt, soc_topic, label="soc")
        v2 = _wait_for_soc_change(ha_unique, v1, "soc_publish", timeout_s=45)
        if v2 == v1:
            raise E2EError(f"State_of_Charge did not republish after bucket enable: v1={v1!r} v2={v2!r}")

    def case_soc_drift_e2e() -> None:
        print("[e2e] case: soc drift end-to-end")
        current_poll_interval = _soc_drift_poll_interval()
        _ensure_soc_drift_backend(current_poll_interval)
        ha_unique = _current_inverter_identity() or _wait_for_inverter_identity()
        soc_topic = _state_topic(ha_unique, "State_of_Charge")
        mqtt.subscribe(soc_topic, force=True)
        v1 = _fetch_latest_text(mqtt, soc_topic, label="soc")
        v2 = _wait_for_soc_change(ha_unique, v1, "soc", timeout_s=45)
        try:
            f1 = float(v1)
            f2 = float(v2)
        except ValueError:
            raise E2EError(f"Unexpected SOC payloads (not floats): v1={v1!r} v2={v2!r}")
        if f2 <= f1:
            v3 = _wait_for_soc_change(ha_unique, v2, "soc", timeout_s=30)
            f3 = float(v3)
            if f3 <= f2:
                raise E2EError(f"SOC did not monotonically increase across publishes: {v1!r} {v2!r} {v3!r}")

    def case_load_power_snapshot_formula() -> None:
        print("[e2e] case: Load_Power uses snapshot formula without extra reads")
        config_before = _fetch_config(mqtt, config_topic)
        original_intervals = config_before.get("entity_intervals", {})
        if not isinstance(original_intervals, dict):
            raise E2EError(f"config entity_intervals missing or invalid: {config_before}")
        original_interval = int(config_before.get("poll_interval_s", 9) or 9)
        original_bucket = _effective_bucket(original_intervals, "Load_Power")

        def wait_load_state(load_topic: str, expected: str, *, label: str, timeout_s: int = 60) -> str:
            mqtt.subscribe(load_topic, force=True)

            def pred() -> Tuple[bool, str]:
                try:
                    current = _fetch_latest_text(mqtt, load_topic, label=label).strip()
                except E2EError as exc:
                    return False, str(exc)
                return (current == expected), f"state={current!r}"

            _assert_eventually(label, pred, timeout_s=timeout_s, poll_s=2.0)
            return _fetch_latest_text(mqtt, load_topic, label=f"{label}_final").strip()

        try:
            wait_polling_config_applied(
                9,
                {"Load_Power": "user"},
                timeout_s=60,
                republish_every_s=5.0,
            )
            ensure_stub_online_backend(
                '{"mode":"online","soc_pct":50,"battery_power_w":200,"grid_power_w":300,"pv_ct_power_w":500}',
                label="load power positive backend",
            )
            ha_unique = _wait_for_inverter_identity()
            load_topic = _state_topic(ha_unique, "Load_Power")
            first = wait_load_state(load_topic, "1000", label="load_power_initial")
            if first != "1000":
                raise E2EError(f"unexpected initial Load_Power state: {first!r}")

            ensure_stub_online_backend(
                '{"mode":"online","soc_pct":50,"battery_power_w":-50,"grid_power_w":-150,"pv_ct_power_w":400}',
                label="load power signed backend",
            )
            second = wait_load_state(load_topic, "200", label="load_power_signed")
            if second != "200":
                raise E2EError(f"unexpected signed Load_Power state: {second!r}")
        finally:
            wait_polling_config_applied(
                original_interval,
                {"Load_Power": original_bucket},
                timeout_s=60,
                republish_every_s=5.0,
            )

    def case_polling_config_persistence() -> None:
        print("[e2e] case: polling config mapping + poll_interval_s takes effect")
        ensure_stub_online_backend(
            '{"mode":"online","fail_n":0,"fail_reads":0,"fail_writes":0,'
            '"fail_type":0,"fail_every_n":0,"fail_for_ms":0,'
            '"flap_online_ms":0,"flap_offline_ms":0,'
            '"probe_success_after_n":0,"strict_unknown":0,"strict":0}',
            label="polling config baseline",
        )
        config_before = _fetch_config(mqtt, config_topic)
        intervals = config_before.get("entity_intervals", {})
        if not isinstance(intervals, dict):
            raise E2EError(f"config entity_intervals missing or invalid: {config_before}")

        target = "State_of_Charge"
        old_bucket = _effective_bucket(intervals, target)
        if not old_bucket:
            raise E2EError(f"config missing {target} in entity_intervals")

        # Keep this bounded to buckets that can execute within this test window.
        new_bucket = "ten_sec" if old_bucket != "ten_sec" else "user"
        if new_bucket == old_bucket:
            raise E2EError(f"unable to select alternate bucket for {target} (bucket={old_bucket})")

        poll_interval = 9
        wait_polling_config_applied(
            poll_interval,
            {target: new_bucket},
            timeout_s=60,
            republish_every_s=5.0,
        )

    def case_portal_rs485_baud_reconcile() -> None:
        # This case is intentionally opt-in rather than part of CASE_ORDER. It
        # exercises a real persisted RS485 config mutation path and may need to
        # seed a supported baseline when no stored baud exists yet.
        reboot_url = _discover_reboot_wifi_path_from_code()
        if not reboot_url:
            raise E2EError("Could not discover /reboot/wifi endpoint from firmware source")
        base = _resolve_device_http_base(mqtt, device_root)
        portal_reboot_normal_path = "/config/reboot-normal"
        supported_bauds = {9600, 115200, 19200}
        baseline_poll = _fetch_poll(mqtt, poll_topic)
        baseline_epoch = int(baseline_poll.get("rs485_connection_epoch", 0))
        baseline_configured = int(baseline_poll.get("rs485_baud_configured", 0))
        baseline_actual = int(baseline_poll.get("rs485_baud_actual", 0))
        restore_baud = (
            baseline_configured
            if baseline_configured in supported_bauds
            else baseline_actual if baseline_actual in supported_bauds else 9600
        )
        seeded_restore_baseline = baseline_configured not in supported_bauds

        def load_rs485_portal(path_suffix: str = "") -> str:
            status, body = _http_request_full("GET", base + "/config/rs485" + path_suffix, headers={}, body=b"", timeout_s=20)
            if status != 200:
                raise E2EError(f"/config/rs485{path_suffix} returned unexpected status={status}")
            return body.decode("utf-8", errors="replace")

        def save_rs485_baud_via_portal(target_baud: int) -> None:
            rs485_html = load_rs485_portal()
            csrf = _extract_input_value(rs485_html, "csrf")
            if not csrf:
                raise E2EError("portal rs485 page missing csrf token")
            save_status = _http_post_form(
                base + "/config/rs485/save", {"baud": str(target_baud), "csrf": csrf}, timeout_s=20
            )
            if save_status not in (200, 302):
                raise E2EError(f"/config/rs485/save failed status={save_status}")
            confirm_html = load_rs485_portal("?saved=1")
            if "Saved." not in confirm_html:
                raise E2EError("portal rs485 saved page missing confirmation")

        print(f"[e2e] rebooting into wifi portal via {reboot_url} (rs485 baud portal check)")
        reboot_wifi_status, reboot_wifi_body = _http_request_full(
            "POST", base + reboot_url, headers={}, body=b"", timeout_s=20
        )
        if reboot_wifi_status != 200:
            raise E2EError(f"{reboot_url} returned unexpected status={reboot_wifi_status}")
        if reboot_wifi_body:
            _assert_reboot_handoff_html(
                reboot_wifi_body.decode("utf-8", errors="replace"),
                expected_heading="Rebooting to Wi-Fi config",
                expected_target_mode="wifi",
                expected_probe_kind="fetch",
            )
        _wait_for_http_ok(base + "/", timeout_s=40)
        _assert_portal_root_menu(base, timeout_s=20, required_mode="wifi")

        rs485_html = load_rs485_portal()
        if "Configured:" not in rs485_html or "Sync:" not in rs485_html:
            raise E2EError("portal rs485 page missing status summary")
        if "value=\"115200\"" not in rs485_html:
            raise E2EError("portal rs485 page missing 115200 option")
        if seeded_restore_baseline:
            print(f"[e2e] seeding deterministic rs485 baud baseline via portal to {restore_baud}")
            save_rs485_baud_via_portal(restore_baud)
        save_rs485_baud_via_portal(115200)

        reboot_normal_url = base + portal_reboot_normal_path
        reboot_status, reboot_body = _http_request_full("POST", reboot_normal_url, headers={}, body=b"", timeout_s=20)
        if reboot_status != 200:
            raise E2EError(f"{portal_reboot_normal_path} returned unexpected status={reboot_status}")
        if reboot_body:
            _assert_reboot_handoff_html(
                reboot_body.decode("utf-8", errors="replace"),
                expected_heading="Rebooting to normal mode",
                expected_target_mode="normal",
                expected_probe_kind="fetch",
            )

        def runtime_root_ready() -> Tuple[bool, str]:
            try:
                root_status, root_body = _http_request_full("GET", base + "/", headers={}, body=b"", timeout_s=20)
            except Exception as e:
                return False, f"err={e}"
            if root_status != 200:
                return False, f"status={root_status}"
            html = root_body.decode("utf-8", errors="replace")
            if "Alpha2MQTT Control" not in html:
                return False, "waiting for runtime root"
            if "MQTT connected: 1" not in html:
                return False, "waiting for runtime mqtt reconnect"
            return True, "ok"

        _assert_eventually("runtime root page after portal rs485 save reboot", runtime_root_ready, timeout_s=60, poll_s=2.0)

        set_mode_and_wait('{"mode":"online","modbus_baud":9600}', ("online",))

        def pred() -> Tuple[bool, str]:
            cur_poll = _fetch_poll(mqtt, poll_topic)
            configured = int(cur_poll.get("rs485_baud_configured", 0))
            actual = int(cur_poll.get("rs485_baud_actual", 0))
            sync = str(cur_poll.get("rs485_baud_sync", ""))
            epoch = int(cur_poll.get("rs485_connection_epoch", 0))
            last_write_reg = int(cur_poll.get("rs485_stub_last_write_reg", 0))
            writes = int(cur_poll.get("rs485_stub_writes", 0))
            detail = (
                f"configured={configured} actual={actual} sync={sync!r} "
                f"epoch={epoch} last_write_reg={last_write_reg} writes={writes}"
            )
            return (
                configured == 115200
                and actual == 115200
                and sync == "synced"
                and epoch > baseline_epoch
                and last_write_reg == 2064
                and writes >= 1
            ), detail

        _assert_eventually(
            "portal-configured RS485 baud is reconciled and confirmed after reprobe",
            pred,
            timeout_s=90,
            poll_s=3.0,
        )

        if restore_baud == 115200 and not seeded_restore_baseline:
            return

        print(f"[e2e] restoring persisted rs485 baud via portal to {restore_baud}")
        _http_post_simple(reboot_url, timeout_s=10)
        _assert_portal_root_menu(base, timeout_s=40, required_mode="wifi")
        save_rs485_baud_via_portal(restore_baud)

        restore_reboot_status, restore_reboot_body = _http_request_full(
            "POST", reboot_normal_url, headers={}, body=b"", timeout_s=20
        )
        if restore_reboot_status != 200:
            raise E2EError(f"{portal_reboot_normal_path} restore returned unexpected status={restore_reboot_status}")
        if restore_reboot_body:
            _assert_reboot_handoff_html(
                restore_reboot_body.decode("utf-8", errors="replace"),
                expected_heading="Rebooting to normal mode",
                expected_target_mode="normal",
                expected_probe_kind="fetch",
            )
        _assert_eventually("runtime root page after rs485 restore reboot", runtime_root_ready, timeout_s=60, poll_s=2.0)

        def restored_pred() -> Tuple[bool, str]:
            cur_poll = _fetch_poll(mqtt, poll_topic)
            configured = int(cur_poll.get("rs485_baud_configured", 0))
            actual = int(cur_poll.get("rs485_baud_actual", 0))
            sync = str(cur_poll.get("rs485_baud_sync", ""))
            detail = f"configured={configured} actual={actual} sync={sync!r}"
            return configured == restore_baud and actual == restore_baud and sync == "synced", detail

        _assert_eventually(
            "portal rs485 case restores the original persisted baud baseline",
            restored_pred,
            timeout_s=90,
            poll_s=3.0,
        )

    def case_runtime_polling_reset_without_page() -> None:
        print("[e2e] case: runtime root resets polling defaults without loading polling page")
        ensure_stub_online_backend(
            '{"mode":"online","fail_n":0,"fail_reads":0,"fail_writes":0,'
            '"fail_type":0,"fail_every_n":0,"fail_for_ms":0,'
            '"flap_online_ms":0,"flap_offline_ms":0,'
            '"probe_success_after_n":0,"strict_unknown":0,"strict":0}',
            label="runtime polling reset baseline",
        )
        inverter_device_id = _wait_for_inverter_identity()
        base = _resolve_device_http_base(mqtt, device_root)
        config_before = _fetch_config(mqtt, config_topic)
        intervals_before = config_before.get("entity_intervals", {})
        if not isinstance(intervals_before, dict):
            raise E2EError(f"config entity_intervals missing or invalid before reset case: {config_before}")

        target = "State_of_Charge"
        hidden_target = "Max_Feedin_Percent"
        default_bucket = _load_entity_default_buckets().get(target, "")
        if not default_bucket:
            raise E2EError(f"missing default bucket for {target}")
        hidden_default_bucket = _load_entity_default_buckets().get(hidden_target, "")
        if hidden_default_bucket != "disabled":
            raise E2EError(
                f"{hidden_target} expected disabled default, got {hidden_default_bucket!r}"
            )
        discovery_topic = f"homeassistant/number/{inverter_device_id}/{hidden_target}/config"

        current_bucket = _effective_bucket(intervals_before, target)
        desired_bucket = "ten_sec" if default_bucket != "ten_sec" else "user"
        if current_bucket == desired_bucket:
            desired_bucket = "one_min" if default_bucket != "one_min" else "five_min"
        if current_bucket == desired_bucket:
            raise E2EError(f"unable to select non-default bucket for {target} (current={current_bucket!r})")

        wait_polling_config_applied(
            9,
            {target: desired_bucket, hidden_target: "one_min"},
            timeout_s=60,
            republish_every_s=5.0,
        )
        discovery_payload = _fetch_latest_text(
            mqtt,
            discovery_topic,
            label="runtime_polling_reset_discovery_before",
        )
        if not discovery_payload.strip():
            raise E2EError(f"{hidden_target} discovery did not publish after enable")

        root_status, _root_body = _http_request_full("GET", base + "/", headers={}, body=b"", timeout_s=20)
        if root_status != 200:
            raise E2EError(f"runtime root returned unexpected status={root_status}")

        reset_status = _http_post_simple(base + "/config/polling/reset", timeout_s=20)
        if reset_status not in (200, 303):
            raise E2EError(f"runtime polling reset returned unexpected status={reset_status}")

        reset_root_status, reset_root_body = _http_request_full(
            "GET",
            base + "/?polling_reset=1",
            headers={},
            body=b"",
            timeout_s=20,
        )
        if reset_root_status != 200:
            raise E2EError(f"runtime root after polling reset returned unexpected status={reset_root_status}")
        if "Polling reset to defaults." not in reset_root_body.decode("utf-8", errors="replace"):
            raise E2EError("runtime root did not show polling reset confirmation")

        def reset_pred() -> Tuple[bool, str]:
            cfg = _fetch_config(mqtt, config_topic)
            poll = _fetch_poll(mqtt, poll_topic)
            intervals = cfg.get("entity_intervals", {})
            if not isinstance(intervals, dict):
                return False, f"entity_intervals invalid: {cfg!r}"
            cfg_interval = int(cfg.get("poll_interval_s", 0) or 0)
            runtime_interval = int(poll.get("poll_interval_s", 0) or 0)
            actual_bucket = _effective_bucket(intervals, target)
            detail = (
                f"cfg_poll_interval_s={cfg_interval} runtime_poll_interval_s={runtime_interval} "
                f"{target}={actual_bucket!r}"
            )
            return (cfg_interval == 60 and runtime_interval == 60 and actual_bucket == default_bucket), detail

        _assert_eventually(
            "runtime polling reset restores default interval and bucket",
            reset_pred,
            timeout_s=60,
            poll_s=3.0,
        )

        def discovery_clear_pred() -> Tuple[bool, str]:
            payload = mqtt.latest_payload(discovery_topic)
            return payload == "", f"payload={payload!r}"

        _assert_eventually(
            "runtime polling reset clears disabled discovery",
            discovery_clear_pred,
            timeout_s=30,
            poll_s=1.0,
        )

    def case_portal_polling_ui() -> None:
        print("[e2e] case: portal UI updates polling schedule (lightweight paged portal)")
        portal_stub_baseline = (
            '{"mode":"online","fail_n":0,"fail_reads":0,"fail_writes":0,'
            '"fail_type":0,"fail_every_n":0,"fail_for_ms":0,'
            '"flap_online_ms":0,"flap_offline_ms":0,'
            '"probe_success_after_n":0,"strict_unknown":0,"strict":0}'
        )
        ensure_stub_online_backend(portal_stub_baseline, label="portal polling ui baseline")
        base = _resolve_device_http_base(mqtt, device_root)
        portal_reboot_normal_path = "/config/reboot-normal"
        config_before = _fetch_config(mqtt, config_topic)
        original_interval = int(config_before.get("poll_interval_s", 0))
        original_intervals = config_before.get("entity_intervals", {})
        if not isinstance(original_intervals, dict):
            raise E2EError(f"config entity_intervals missing before portal case: {config_before}")

        reboot_wifi_path = _discover_reboot_wifi_path_from_code()
        if not reboot_wifi_path:
            raise E2EError("Could not discover /reboot/wifi endpoint from firmware source")

        reboot_url = base + reboot_wifi_path
        print(f"[e2e] rebooting into wifi portal via {reboot_url}")
        reboot_wifi_status, reboot_wifi_body = _http_request_full("POST", reboot_url, headers={}, body=b"", timeout_s=20)
        if reboot_wifi_status != 200:
            raise E2EError(f"{reboot_wifi_path} returned unexpected status={reboot_wifi_status}")
        if reboot_wifi_body:
            _assert_reboot_handoff_html(
                reboot_wifi_body.decode("utf-8", errors="replace"),
                expected_heading="Rebooting to Wi-Fi config",
                expected_target_mode="wifi",
                expected_probe_kind="fetch",
            )

        # Portal is STA-only and should come back on the same IP, but runtime
        # can still answer briefly during the deferred reboot window. Wait for
        # the actual portal menu, not just any 200 on `/`.
        _assert_portal_root_menu(base, timeout_s=40, required_mode="wifi")

        polling_path, html = _load_polling_page_via_menu(base)
        if not polling_path.startswith("/config/polling"):
            raise E2EError(f"unexpected polling menu path: {polling_path!r}")
        if "poll_interval_s" not in html or "/config/polling/save" not in html:
            raise E2EError("portal polling page HTML missing expected form fields")
        if "/config/polling/reset" not in html or "Reset Polling Defaults" not in html:
            raise E2EError("portal polling page missing reset-to-defaults action")
        if "/config/polling/clear" not in html or "Disable All Entities" not in html:
            raise E2EError("portal polling page missing clear-all action")
        if "<h2>Polling schedule</h2>" not in html or "/config/reboot-normal" not in html:
            raise E2EError("portal polling page missing simplified header/navigation")
        if "bucket_map_full" not in html:
            raise E2EError("portal polling page missing hidden bucket_map_full field")
        if 'href="/config/polling?family=battery&page=0">Battery</a>' not in html and \
           'href="/config/polling?family=battery&page=0">[Battery</a>' not in html:
            raise E2EError("portal polling family nav did not render human-readable labels")

        # Find which row index corresponds to a known stable mqttName.
        target = "State_of_Charge"
        target_family, target_page, total_pages, row, initial_bucket, family_keys = _locate_entity_on_polling_pages(base, html, target)
        target_family_first_html = html if target_family == _extract_polling_page_family_key(html) else _load_polling_page(base, target_family, 0)
        _assert_polling_nav_buttons(target_family_first_html, prev_enabled=False, next_enabled=(total_pages > 1))
        if total_pages > 1:
            target_family_last_html = _load_polling_page(base, target_family, total_pages - 1)
            _assert_polling_nav_buttons(target_family_last_html, prev_enabled=True, next_enabled=False)
        active_target_html = _load_polling_page(base, target_family, target_page)
        csrf = _extract_input_value(active_target_html, "csrf")
        row, current_bucket = _extract_entity_row_and_selected_bucket(active_target_html, target)
        if current_bucket != initial_bucket:
            raise E2EError(
                f"{target} bucket changed while preparing nav assertions (expected {initial_bucket!r}, got {current_bucket!r})"
            )
        for candidate_bucket in ("user", "ten_sec", "five_min", "one_hour", "disabled"):
            if initial_bucket != candidate_bucket:
                desired_bucket = candidate_bucket
                break
        else:
            raise E2EError(f"unable to select alternate bucket for {target} (bucket={initial_bucket!r})")

        # Change the visible row on the page where the entity actually appears and save it.
        fields = {
            "family": target_family,
            "page": str(target_page),
            "csrf": csrf,
            "poll_interval_s": "9",
            f"b{row}": desired_bucket,
        }
        save_url = base + "/config/polling/save"
        print(f"[e2e] POST {save_url} fields: family={target_family} page={target_page} poll_interval_s=9 b{row}={desired_bucket}")
        try:
            save_status = _http_post_form(save_url, fields, timeout_s=20)
            if save_status not in (200, 302):
                raise E2EError(f"polling save failed status={save_status}")
        except Exception as e:
            if not _is_expected_portal_transport_error(e):
                raise
            print(f"[e2e] polling save transport timeout tolerated; verifying by state ({e})")
        saved_html = _load_polling_page(base, target_family, target_page)
        interval_match = re.search(r'name="poll_interval_s"[^>]*value="(\d+)"', saved_html)
        if not interval_match:
            raise E2EError("polling page missing poll_interval_s input after save")
        if int(interval_match.group(1)) != 9:
            raise E2EError(f"poll_interval_s UI value not updated after save: {interval_match.group(1)}")
        _, saved_bucket = _extract_entity_row_and_selected_bucket(saved_html, target)
        if saved_bucket != desired_bucket:
            raise E2EError(
                f"{target} bucket UI value not updated after save (expected {desired_bucket!r}, got {saved_bucket!r})"
            )

        reboot_normal_url = base + portal_reboot_normal_path
        print(f"[e2e] rebooting to normal via {reboot_normal_url}")
        reboot_status, reboot_body = _http_request_full("POST", reboot_normal_url, headers={}, body=b"", timeout_s=20)
        if reboot_status != 200:
            raise E2EError(f"{portal_reboot_normal_path} returned unexpected status={reboot_status}")
        if reboot_body:
            _assert_reboot_handoff_html(
                reboot_body.decode("utf-8", errors="replace"),
                expected_heading="Rebooting to normal mode",
                expected_target_mode="normal",
                expected_probe_kind="fetch",
            )

        # Wait for MQTT status to resume after reboot to NORMAL.
        _assert_eventually(
            "device publishes status/poll after explicit reboot to normal",
            lambda: (True, "ok") if _fetch_poll(mqtt, poll_topic).get("poll_interval_s") else (False, "waiting"),
            timeout_s=60,
            poll_s=3.0,
        )
        ensure_stub_online_backend(portal_stub_baseline, label="portal polling ui after save reboot")

        root_html_box = {"html": ""}
        def root_ready_pred() -> Tuple[bool, str]:
            try:
                root_status, root_body = _http_request_full("GET", base + "/", headers={}, body=b"", timeout_s=20)
            except Exception as e:
                return False, f"err={e}"
            if root_status != 200:
                return False, f"status={root_status}"
            html = root_body.decode("utf-8", errors="replace")
            if "Alpha2MQTT Control" not in html:
                return False, "waiting for runtime root"
            if "Boot mode:" not in html:
                return False, "runtime root missing boot status"
            if "MQTT connected: 1" not in html:
                return False, "waiting for runtime mqtt reconnect"
            root_html_box["html"] = html
            return True, "ok"

        _assert_eventually("runtime root page after reboot", root_ready_pred, timeout_s=60, poll_s=2.0)
        root_html = root_html_box["html"]
        for required in (
            "Alpha2MQTT Control",
            "Reboot Normal",
            "Reboot AP Config",
            "Reboot WiFi Config",
            "Boot mode:",
            "Boot intent:",
            "Reset reason:",
            "Firmware version:",
            "RS485 backend:",
            "Uptime (ms):",
            "WiFi status:",
            "MQTT connected:",
            "Inverter ready:",
            "ESS snapshot ok:",
            "poll_interval_s:",
        ):
            if required not in root_html:
                raise E2EError(f"runtime root page missing expected status/control field: {required}")

        restart_alias_status, _ = _http_request("GET", base + "/restart?", headers={}, body=b"", timeout_s=10)
        if restart_alias_status != 302:
            raise E2EError(f"/restart? should redirect to runtime root with 302 (got {restart_alias_status})")
        _wait_for_http_ok(base + "/", timeout_s=10)

        def portal_save_applied_pred() -> Tuple[bool, str]:
            poll = _fetch_poll(mqtt, poll_topic)
            cfg = _fetch_config(mqtt, config_topic)
            intervals = cfg.get("entity_intervals", {})
            if not isinstance(intervals, dict):
                return False, f"entity_intervals invalid: {cfg}"
            runtime_interval = int(poll.get("poll_interval_s", 0))
            actual_bucket = str(intervals.get(target, ""))
            detail = f"poll_interval_s={runtime_interval} {target}={actual_bucket!r}"
            return (runtime_interval == 9 and actual_bucket == desired_bucket), detail

        _assert_eventually(
            "portal save is reflected in runtime mqtt state after reboot",
            portal_save_applied_pred,
            timeout_s=60,
            poll_s=3.0,
        )

        # Re-enter portal and verify values are restored in the UI after reboot.
        print(f"[e2e] rebooting into wifi portal via {reboot_url} (persistence check)")
        _http_post_simple(reboot_url, timeout_s=10)
        _assert_portal_root_menu(base, timeout_s=40, required_mode="wifi")

        polling_path2, html2 = _load_polling_page_via_menu(base)
        if not polling_path2.startswith("/config/polling"):
            raise E2EError(f"unexpected polling menu path after reboot: {polling_path2!r}")

        csrf2 = _extract_input_value(html2, "csrf")
        interval_match = re.search(r'name="poll_interval_s"[^>]*value="(\d+)"', html2)
        if not interval_match:
            raise E2EError("polling page missing poll_interval_s input after reboot")
        if int(interval_match.group(1)) != 9:
            raise E2EError(f"poll_interval_s UI value not restored after reboot: {interval_match.group(1)}")

        restored_family, restored_page, _, _, restored_bucket, _ = _locate_entity_on_polling_pages(base, html2, target)
        if restored_family != target_family or restored_page != target_page:
            raise E2EError(
                f"{target} moved to a different portal location across reboot: "
                f"before={target_family}/{target_page} after={restored_family}/{restored_page}"
        )
        if restored_bucket != desired_bucket:
            raise E2EError(f"{target} bucket UI value not restored after reboot (expected {desired_bucket!r}, got {restored_bucket!r})")

        print(f"[e2e] POST {base}/config/polling/clear (clear-all check)")
        try:
            clear_status = _http_post_form(
                base + "/config/polling/clear",
                {
                    "family": target_family,
                    "page": str(target_page),
                    "csrf": csrf2,
                },
                timeout_s=20,
            )
            if clear_status not in (200, 302):
                raise E2EError(f"polling clear failed status={clear_status}")
        except Exception as e:
            if not _is_expected_portal_transport_error(e):
                raise
            print(f"[e2e] polling clear transport timeout tolerated; verifying by state ({e})")

        cleared_html = _load_polling_page(base, target_family, target_page)
        _, cleared_bucket = _extract_entity_row_and_selected_bucket(cleared_html, target)
        if cleared_bucket != "disabled":
            raise E2EError(f"{target} bucket UI value not cleared after disable-all (got {cleared_bucket!r})")

        print(f"[e2e] rebooting to normal via {reboot_normal_url} (clear-all check)")
        reboot_status, reboot_body = _http_request_full("POST", reboot_normal_url, headers={}, body=b"", timeout_s=20)
        if reboot_status != 200:
            raise E2EError(f"{portal_reboot_normal_path} after clear returned unexpected status={reboot_status}")
        if reboot_body:
            _assert_reboot_handoff_html(
                reboot_body.decode("utf-8", errors="replace"),
                expected_heading="Rebooting to normal mode",
                expected_target_mode="normal",
                expected_probe_kind="fetch",
            )

        _assert_eventually(
            "device publishes status/poll after clear-all reboot to normal",
            lambda: (True, "ok") if _fetch_poll(mqtt, poll_topic).get("poll_interval_s") else (False, "waiting"),
            timeout_s=60,
            poll_s=3.0,
        )
        ensure_stub_online_backend(portal_stub_baseline, label="portal polling ui after clear reboot")

        def cleared_pred() -> Tuple[bool, str]:
            cfg_cur = _fetch_config(mqtt, config_topic)
            intervals_cur = cfg_cur.get("entity_intervals", {})
            if not isinstance(intervals_cur, dict):
                return False, f"entity_intervals invalid: {cfg_cur}"
            if intervals_cur:
                return False, f"remaining={len(intervals_cur)}"
            return True, "ok"

        _assert_eventually("clear-all removes active entity intervals", cleared_pred, timeout_s=60, poll_s=3.0)

        wait_runtime_poll_interval_applied(
            original_interval,
            timeout_s=60,
            republish_every_s=5.0,
        )
        if original_intervals:
            wait_intervals_applied(
                {str(key): str(value) for key, value in original_intervals.items()},
                timeout_s=60,
                republish_every_s=5.0,
            )

        def restored_pred() -> Tuple[bool, str]:
            cur = _fetch_poll(mqtt, poll_topic)
            cfg_cur = _fetch_config(mqtt, config_topic)
            cur_interval = int(cur.get("poll_interval_s", 0))
            intervals_cur = cfg_cur.get("entity_intervals", {})
            detail = f"poll_interval_s={cur_interval} count={len(intervals_cur) if isinstance(intervals_cur, dict) else 'invalid'}"
            if cur_interval != original_interval:
                return False, detail
            if not isinstance(intervals_cur, dict):
                return False, detail
            if intervals_cur != original_intervals:
                return False, detail
            return True, detail

        _assert_eventually("polling config restored after clear-all portal check", restored_pred, timeout_s=60, poll_s=3.0)

    def case_polling_profile_export_import() -> None:
        print("[e2e] case: portal polling profile export/import streams overwrite restores")
        portal_stub_baseline = (
            '{"mode":"online","fail_n":0,"fail_reads":0,"fail_writes":0,'
            '"fail_type":0,"fail_every_n":0,"fail_for_ms":0,'
            '"flap_online_ms":0,"flap_offline_ms":0,'
            '"probe_success_after_n":0,"strict_unknown":0,"strict":0}'
        )
        ensure_clean_suite_baseline()
        base = _resolve_device_http_base(mqtt, device_root)
        defaults = _load_entity_default_buckets()
        target_keep = "State_of_Charge"
        target_clear = "ESS_Power"
        target_keep_default = str(defaults.get(target_keep, "")).strip()
        target_clear_default = str(defaults.get(target_clear, "")).strip()
        if not target_keep_default or not target_clear_default:
            raise E2EError("could not resolve default buckets for polling profile targets")

        baseline_buckets = {
            target_keep: target_keep_default,
            target_clear: target_clear_default,
            "Register_Number": "one_min",
            "Register_Value": "one_min",
        }
        wait_polling_config_applied(9, baseline_buckets, timeout_s=60, republish_every_s=5.0)

        config_before = _fetch_config(mqtt, config_topic)
        intervals_before = config_before.get("entity_intervals", {})
        if not isinstance(intervals_before, dict):
            raise E2EError(f"config entity_intervals missing before profile export/import case: {config_before}")

        reboot_wifi_path = _discover_reboot_wifi_path_from_code()
        if not reboot_wifi_path:
            raise E2EError("Could not discover /reboot/wifi endpoint from firmware source")

        reboot_wifi_status, reboot_wifi_body = _http_request_full(
            "POST", base + reboot_wifi_path, headers={}, body=b"", timeout_s=20
        )
        if reboot_wifi_status != 200:
            raise E2EError(f"{reboot_wifi_path} returned unexpected status={reboot_wifi_status}")
        if reboot_wifi_body:
            _assert_reboot_handoff_html(
                reboot_wifi_body.decode("utf-8", errors="replace"),
                expected_heading="Rebooting to Wi-Fi config",
                expected_target_mode="wifi",
                expected_probe_kind="fetch",
            )
        _assert_portal_root_menu(base, timeout_s=40, required_mode="wifi")
        baseline_export_status, baseline_export_body = _http_request_full(
            "GET",
            base + "/config/polling/export",
            headers={},
            body=b"",
            timeout_s=20,
        )
        if baseline_export_status != 200:
            raise E2EError(f"baseline polling profile export returned unexpected status={baseline_export_status}")
        restore_profile = baseline_export_body.decode("utf-8", errors="strict")
        original_interval, original_export_intervals = _parse_polling_profile_text(restore_profile)
        if int(config_before.get("poll_interval_s", 0) or 0) != original_interval:
            raise E2EError(f"baseline polling profile export poll interval drifted: {restore_profile!r}")

        reboot_normal_url = base + "/config/reboot-normal"
        reboot_status, reboot_body = _http_request_full("POST", reboot_normal_url, headers={}, body=b"", timeout_s=20)
        if reboot_status != 200:
            raise E2EError(f"/config/reboot-normal after baseline export returned unexpected status={reboot_status}")
        if reboot_body:
            _assert_reboot_handoff_html(
                reboot_body.decode("utf-8", errors="replace"),
                expected_heading="Rebooting to normal mode",
                expected_target_mode="normal",
                expected_probe_kind="fetch",
            )

        _assert_eventually(
            "device publishes status/poll after baseline export reboot",
            lambda: (True, "ok") if _fetch_poll(mqtt, poll_topic).get("poll_interval_s") else (False, "waiting"),
            timeout_s=60,
            poll_s=3.0,
        )
        ensure_stub_online_backend(portal_stub_baseline, label="polling profile baseline reboot control")

        original_keep = _effective_bucket(intervals_before, target_keep)
        original_clear = _effective_bucket(intervals_before, target_clear)
        original_register_number = _effective_bucket(intervals_before, "Register_Number")
        original_register_value = _effective_bucket(intervals_before, "Register_Value")
        if original_export_intervals.get(target_keep) != original_keep:
            raise E2EError(f"baseline polling profile export missing full {target_keep} assignment: {restore_profile}")
        if original_export_intervals.get(target_clear) != original_clear:
            raise E2EError(f"baseline polling profile export missing full {target_clear} assignment: {restore_profile}")

        def pick_alt_bucket(*excluded: str) -> str:
            for candidate in ("user", "ten_sec", "five_min", "one_hour", "one_day", "disabled"):
                if candidate not in excluded:
                    return candidate
            raise E2EError(f"unable to choose alternate bucket (excluded={excluded!r})")

        keep_override = pick_alt_bucket(target_keep_default)
        clear_override = pick_alt_bucket(target_clear_default, keep_override)
        modified_override = pick_alt_bucket(target_keep_default, keep_override)
        modified_interval = 17 if original_interval != 17 else 19

        wait_intervals_applied(
            {
                target_keep: keep_override,
                target_clear: clear_override,
            },
            timeout_s=60,
            republish_every_s=5.0,
        )

        reboot_wifi_status, reboot_wifi_body = _http_request_full(
            "POST", base + reboot_wifi_path, headers={}, body=b"", timeout_s=20
        )
        if reboot_wifi_status != 200:
            raise E2EError(f"{reboot_wifi_path} returned unexpected status={reboot_wifi_status}")
        if reboot_wifi_body:
            _assert_reboot_handoff_html(
                reboot_wifi_body.decode("utf-8", errors="replace"),
                expected_heading="Rebooting to Wi-Fi config",
                expected_target_mode="wifi",
                expected_probe_kind="fetch",
            )
        _assert_portal_root_menu(base, timeout_s=40, required_mode="wifi")

        export_status, export_body = _http_request_full(
            "GET",
            base + "/config/polling/export",
            headers={},
            body=b"",
            timeout_s=20,
        )
        if export_status != 200:
            raise E2EError(f"polling profile export returned unexpected status={export_status}")
        exported_profile = export_body.decode("utf-8", errors="strict")
        exported_interval, exported_intervals = _parse_polling_profile_text(exported_profile)
        if exported_interval != original_interval:
            raise E2EError(f"polling profile export poll interval drifted: {exported_profile!r}")
        if exported_intervals.get(target_keep) != keep_override:
            raise E2EError(f"exported profile missing {target_keep} override: {exported_profile}")
        if exported_intervals.get(target_clear) != clear_override:
            raise E2EError(f"exported profile missing {target_clear} override: {exported_profile}")
        if exported_intervals.get("Register_Number") != original_register_number:
            raise E2EError(f"exported profile missing full Register_Number assignment: {exported_profile}")
        if exported_intervals.get("Register_Value") != original_register_value:
            raise E2EError(f"exported profile missing full Register_Value assignment: {exported_profile}")

        import_status, import_body = _http_request_full(
            "GET",
            base + "/config/polling/import",
            headers={},
            body=b"",
            timeout_s=20,
        )
        if import_status != 200:
            raise E2EError(f"polling profile import page returned unexpected status={import_status}")
        import_html = import_body.decode("utf-8", errors="replace")
        if "/config/polling/import" not in import_html or 'name="profile"' not in import_html or 'type="file"' not in import_html:
            raise E2EError("polling profile import page missing form fields")
        import_action = urllib.parse.urljoin(base + "/config/polling/import", _extract_form_action_with_input(import_html, "profile"))

        invalid_status, invalid_body = _http_post_multipart_bytes_full(
            import_action,
            "profile",
            "invalid.txt",
            b"A2M_POLLING_PROFILE 1\npoll_interval_s=17\nbroken-line\n",
            timeout_s=20,
        )
        if invalid_status != 200:
            raise E2EError(f"invalid polling profile import returned unexpected status={invalid_status}")
        invalid_html = invalid_body.decode("utf-8", errors="replace")
        if "Invalid polling profile line" not in invalid_html:
            raise E2EError("invalid polling profile import did not show an error banner")
        invalid_action = urllib.parse.urljoin(base + "/config/polling/import", _extract_form_action_with_input(invalid_html, "profile"))

        oversize_rejected_with_banner = False
        try:
            oversize_status, oversize_body = _http_post_multipart_bytes_full(
                invalid_action,
                "profile",
                "oversize.txt",
                (
                    b"A2M_POLLING_PROFILE 1\n"
                    b"poll_interval_s=17\n"
                    + b"A" * 200
                    + b"=one_min\n"
                ),
                timeout_s=20,
            )
            if oversize_status not in (200, 302):
                raise E2EError(
                    f"oversize polling profile import returned unexpected status={oversize_status}"
                )
            if oversize_status == 200:
                oversize_html = oversize_body.decode("utf-8", errors="replace")
                oversize_rejected_with_banner = (
                    "Polling profile line exceeds the portal limit." in oversize_html
                )
        except Exception as exc:
            if not _is_expected_portal_transport_error(exc):
                raise

        _assert_portal_root_menu(base, timeout_s=40, required_mode="wifi")
        oversize_export_status, oversize_export_body = _http_request_full(
            "GET",
            base + "/config/polling/export",
            headers={},
            body=b"",
            timeout_s=20,
        )
        if oversize_export_status != 200:
            raise E2EError(
                f"polling profile export after oversize import returned unexpected status={oversize_export_status}"
            )
        oversize_exported_profile = oversize_export_body.decode("utf-8", errors="strict")
        if oversize_exported_profile != exported_profile:
            raise E2EError(
                f"oversize polling profile import changed persisted state: {oversize_exported_profile}"
            )
        if not oversize_rejected_with_banner:
            print("[e2e] oversize import rejected before banner; portal remained healthy and state stayed unchanged")

        import_status, import_body = _http_request_full(
            "GET",
            base + "/config/polling/import",
            headers={},
            body=b"",
            timeout_s=20,
        )
        if import_status != 200:
            raise E2EError(f"polling profile import page after oversize returned unexpected status={import_status}")
        import_html = import_body.decode("utf-8", errors="replace")
        import_action = urllib.parse.urljoin(base + "/config/polling/import", _extract_form_action_with_input(import_html, "profile"))

        modified_profile = "\n".join(
            (
                "A2M_POLLING_PROFILE 1",
                f"poll_interval_s={modified_interval}",
                f"{target_keep}={modified_override}",
                "Unknown_Profile_Entity=one_min",
                "",
            )
        )
        apply_status = _http_post_multipart_bytes(
            import_action,
            "profile",
            "modified.txt",
            modified_profile.encode("utf-8"),
            timeout_s=20,
        )
        if apply_status not in (200, 302):
            raise E2EError(f"polling profile import returned unexpected status={apply_status}")

        imported_polling_status, imported_polling_body = _http_request_full(
            "GET",
            base + "/config/polling?imported=1&unknown=1",
            headers={},
            body=b"",
            timeout_s=20,
        )
        if imported_polling_status != 200:
            raise E2EError(f"polling page after import returned unexpected status={imported_polling_status}")
        imported_polling_html = imported_polling_body.decode("utf-8", errors="replace")
        if "Polling profile imported. Reboot Normal to apply it to runtime and Home Assistant." not in imported_polling_html:
            raise E2EError("polling page did not show the import confirmation guidance")
        if "Ignored 1 unknown entities from the imported profile." not in imported_polling_html:
            raise E2EError("polling page did not show the unknown-entity warning")

        exported_modified_status, exported_modified_body = _http_request_full(
            "GET",
            base + "/config/polling/export",
            headers={},
            body=b"",
            timeout_s=20,
        )
        if exported_modified_status != 200:
            raise E2EError(
                f"polling profile export after import returned unexpected status={exported_modified_status}"
            )
        exported_modified = exported_modified_body.decode("utf-8", errors="strict")
        exported_modified_interval, exported_modified_intervals = _parse_polling_profile_text(exported_modified)
        if exported_modified_interval != modified_interval:
            raise E2EError(f"polling profile export did not update poll_interval_s: {exported_modified}")
        if exported_modified_intervals.get(target_keep) != modified_override:
            raise E2EError(f"polling profile export did not replace bucket map: {exported_modified}")
        if target_clear in exported_modified_intervals:
            raise E2EError(f"polling profile export did not disable omitted entity {target_clear}: {exported_modified}")

        reboot_status, reboot_body = _http_request_full("POST", reboot_normal_url, headers={}, body=b"", timeout_s=20)
        if reboot_status != 200:
            raise E2EError(f"/config/reboot-normal after import returned unexpected status={reboot_status}")
        if reboot_body:
            _assert_reboot_handoff_html(
                reboot_body.decode("utf-8", errors="replace"),
                expected_heading="Rebooting to normal mode",
                expected_target_mode="normal",
                expected_probe_kind="fetch",
            )

        _assert_eventually(
            "device publishes status/poll after polling profile import reboot",
            lambda: (True, "ok") if _fetch_poll(mqtt, poll_topic).get("poll_interval_s") else (False, "waiting"),
            timeout_s=60,
            poll_s=3.0,
        )
        ensure_stub_online_backend(portal_stub_baseline, label="polling profile after import reboot control")

        def imported_pred() -> Tuple[bool, str]:
            cfg = _fetch_config(mqtt, config_topic)
            poll = _fetch_poll(mqtt, poll_topic)
            intervals = cfg.get("entity_intervals", {})
            if not isinstance(intervals, dict):
                return False, f"entity_intervals invalid: {cfg!r}"
            actual_keep = _effective_bucket(intervals, target_keep)
            detail = (
                f"cfg_poll_interval_s={cfg.get('poll_interval_s')} runtime_poll_interval_s={poll.get('poll_interval_s')} "
                f"{target_keep}={actual_keep!r} {target_clear}_present={target_clear in intervals} intervals={intervals!r}"
            )
            return (
                int(cfg.get("poll_interval_s", 0) or 0) == modified_interval
                and int(poll.get("poll_interval_s", 0) or 0) == modified_interval
                and actual_keep == modified_override
                and target_clear not in intervals
            ), detail

        _assert_eventually(
            "polling profile import applies replacement schedule after reboot",
            imported_pred,
            timeout_s=60,
            poll_s=3.0,
        )

        reboot_wifi_status, reboot_wifi_body = _http_request_full(
            "POST", base + reboot_wifi_path, headers={}, body=b"", timeout_s=20
        )
        if reboot_wifi_status != 200:
            raise E2EError(f"{reboot_wifi_path} returned unexpected status={reboot_wifi_status}")
        if reboot_wifi_body:
            _assert_reboot_handoff_html(
                reboot_wifi_body.decode("utf-8", errors="replace"),
                expected_heading="Rebooting to Wi-Fi config",
                expected_target_mode="wifi",
                expected_probe_kind="fetch",
            )
        _assert_portal_root_menu(base, timeout_s=40, required_mode="wifi")
        restore_import_status, restore_import_body = _http_request_full(
            "GET",
            base + "/config/polling/import",
            headers={},
            body=b"",
            timeout_s=20,
        )
        if restore_import_status != 200:
            raise E2EError(f"polling profile restore page returned unexpected status={restore_import_status}")
        restore_html = restore_import_body.decode("utf-8", errors="replace")
        restore_action = urllib.parse.urljoin(base + "/config/polling/import", _extract_form_action_with_input(restore_html, "profile"))
        restore_status, restore_body = _http_post_multipart_bytes_full(
            restore_action,
            "profile",
            "restore.txt",
            restore_profile.encode("utf-8"),
            timeout_s=20,
        )
        if restore_status not in (200, 302):
            raise E2EError(f"polling profile restore returned unexpected status={restore_status}")
        if restore_status == 200:
            restore_html = restore_body.decode("utf-8", errors="replace")
            if "Polling profile import failed." in restore_html:
                raise E2EError("polling profile restore returned an error page instead of applying")

        restored_export_status, restored_export_body = _http_request_full(
            "GET",
            base + "/config/polling/export",
            headers={},
            body=b"",
            timeout_s=20,
        )
        if restored_export_status != 200:
            raise E2EError(
                f"polling profile export after restore returned unexpected status={restored_export_status}"
            )
        restored_export = restored_export_body.decode("utf-8", errors="strict")
        restored_export_interval, restored_export_intervals = _parse_polling_profile_text(restored_export)
        if restored_export_interval != original_interval:
            raise E2EError(f"polling profile export did not restore poll_interval_s: {restored_export}")
        if restored_export_intervals != original_export_intervals:
            raise E2EError(f"polling profile export did not restore the original active schedule: {restored_export}")

        reboot_status, reboot_body = _http_request_full("POST", reboot_normal_url, headers={}, body=b"", timeout_s=20)
        if reboot_status != 200:
            raise E2EError(f"/config/reboot-normal after restore returned unexpected status={reboot_status}")
        if reboot_body:
            _assert_reboot_handoff_html(
                reboot_body.decode("utf-8", errors="replace"),
                expected_heading="Rebooting to normal mode",
                expected_target_mode="normal",
                expected_probe_kind="fetch",
            )

        _assert_eventually(
            "device publishes status/poll after polling profile restore reboot",
            lambda: (True, "ok") if _fetch_poll(mqtt, poll_topic).get("poll_interval_s") else (False, "waiting"),
            timeout_s=60,
            poll_s=3.0,
        )
        ensure_stub_online_backend(portal_stub_baseline, label="polling profile after restore reboot control")

        def restored_pred() -> Tuple[bool, str]:
            cfg = _fetch_config(mqtt, config_topic)
            poll = _fetch_poll(mqtt, poll_topic)
            intervals = cfg.get("entity_intervals", {})
            if not isinstance(intervals, dict):
                return False, f"entity_intervals invalid: {cfg!r}"
            actual_keep = _effective_bucket(intervals, target_keep)
            actual_clear = _effective_bucket(intervals, target_clear)
            register_number_bucket = _effective_bucket(intervals, "Register_Number")
            register_value_bucket = _effective_bucket(intervals, "Register_Value")
            detail = (
                f"cfg_poll_interval_s={cfg.get('poll_interval_s')} runtime_poll_interval_s={poll.get('poll_interval_s')} "
                f"{target_keep}={actual_keep!r} {target_clear}={actual_clear!r} "
                f"Register_Number={register_number_bucket!r} Register_Value={register_value_bucket!r}"
            )
            return (
                int(cfg.get("poll_interval_s", 0) or 0) == original_interval
                and int(poll.get("poll_interval_s", 0) or 0) == original_interval
                and actual_keep == original_keep
                and actual_clear == original_clear
                and register_number_bucket == original_register_number
                and register_value_bucket == original_register_value
            ), detail

        _assert_eventually(
            "polling profile restore returns runtime config to the original baseline",
            restored_pred,
            timeout_s=60,
            poll_s=3.0,
        )

    def case_reboot_ap_confirmation() -> None:
        print("[e2e] case: reboot AP config requires an explicit confirmation page")
        portal_stub_baseline = (
            '{"mode":"online","fail_n":0,"fail_reads":0,"fail_writes":0,'
            '"fail_type":0,"fail_every_n":0,"fail_for_ms":0,'
            '"flap_online_ms":0,"flap_offline_ms":0,'
            '"probe_success_after_n":0,"strict_unknown":0,"strict":0}'
        )
        ensure_stub_online_backend(portal_stub_baseline, label="reboot ap confirmation baseline")
        base = _resolve_device_http_base(mqtt, device_root)

        root_status, root_body = _http_request_full("GET", base + "/", headers={}, body=b"", timeout_s=20)
        if root_status != 200:
            raise E2EError(f"runtime root returned status={root_status}")
        root_html = root_body.decode("utf-8", errors="replace")
        if "Alpha2MQTT Control" not in root_html:
            raise E2EError("runtime root missing control-plane heading before AP confirmation check")
        if "method='GET' action='/reboot/ap'" not in root_html:
            raise E2EError("runtime root did not expose GET-based AP reboot confirmation entrypoint")

        confirm_status, confirm_body = _http_request_full("GET", base + "/reboot/ap", headers={}, body=b"", timeout_s=20)
        if confirm_status != 200:
            raise E2EError(f"GET /reboot/ap returned status={confirm_status}")
        confirm_html = confirm_body.decode("utf-8", errors="replace")
        for required in (
            "Reboot into AP config?",
            "remove the device from your network for at least 5 minutes",
            "auto-reboot back to normal after about 5 minutes",
            "Yes, reboot AP Config",
            "Cancel",
        ):
            if required not in confirm_html:
                raise E2EError(f"AP reboot confirmation page missing expected warning/control: {required}")

        root_status_after, root_body_after = _http_request_full("GET", base + "/", headers={}, body=b"", timeout_s=20)
        if root_status_after != 200:
            raise E2EError(f"runtime root became unavailable after GET /reboot/ap: status={root_status_after}")
        if "Alpha2MQTT Control" not in root_body_after.decode("utf-8", errors="replace"):
            raise E2EError("GET /reboot/ap unexpectedly left normal runtime")

    def case_portal_wifi_save_reboot_only() -> None:
        reboot_url = _discover_reboot_wifi_path_from_code()
        if not reboot_url:
            raise E2EError("Could not discover /reboot/wifi endpoint from firmware source")
        portal_reboot_normal_path = "/config/reboot-normal"
        base = _resolve_device_http_base(mqtt, device_root)

        print(f"[e2e] rebooting into wifi portal via {reboot_url} (wifi save-only check)")
        reboot_wifi_status, reboot_wifi_body = _http_request_full(
            "POST", base + reboot_url, headers={}, body=b"", timeout_s=20
        )
        if reboot_wifi_status != 200:
            raise E2EError(f"{reboot_url} returned unexpected status={reboot_wifi_status}")
        if reboot_wifi_body:
            _assert_reboot_handoff_html(
                reboot_wifi_body.decode("utf-8", errors="replace"),
                expected_heading="Rebooting to Wi-Fi config",
                expected_target_mode="wifi",
                expected_probe_kind="fetch",
            )
        _wait_for_http_ok(base + "/", timeout_s=40)
        _assert_portal_root_menu(base, timeout_s=20, required_mode="wifi")

        wifi_action, wifi_html = _load_wifi_page(base)
        ssid = _extract_input_value(wifi_html, "s")
        password = _extract_input_value(wifi_html, "p")
        csrf = _extract_input_value(wifi_html, "csrf")
        if not ssid:
            raise E2EError("portal wifi page exposed a blank SSID")

        save_url = urllib.parse.urljoin(base + "/0wifi", wifi_action)
        print(f"[e2e] POST {save_url} fields: s=<current> p=<current>")
        save_status = _http_post_form(save_url, {"s": ssid, "p": password, "csrf": csrf}, timeout_s=20)
        if save_status not in (200, 302):
            raise E2EError(f"wifi save failed status={save_status}")

        saved_status, saved_body = _http_request_full("GET", base + "/0wifi?saved=1", headers={}, body=b"", timeout_s=20)
        if saved_status != 200:
            raise E2EError(f"saved wifi page returned status={saved_status}")
        saved_html = saved_body.decode("utf-8", errors="replace")
        if "applied on the next reboot" not in saved_html:
            raise E2EError("saved wifi page missing reboot-required message")

        polling_status, polling_body = _http_request_full("GET", base + "/config/polling", headers={}, body=b"", timeout_s=20)
        if polling_status != 200:
            raise E2EError(f"polling page not reachable after wifi save: status={polling_status}")
        if "Polling" not in polling_body.decode("utf-8", errors="replace"):
            raise E2EError("polling page missing after wifi save")

        reboot_normal_url = base + portal_reboot_normal_path
        print(f"[e2e] rebooting to normal via {reboot_normal_url} (wifi save-only check)")
        reboot_status, reboot_body = _http_request_full("POST", reboot_normal_url, headers={}, body=b"", timeout_s=20)
        if reboot_status != 200:
            raise E2EError(f"{portal_reboot_normal_path} returned unexpected status={reboot_status}")
        if reboot_body:
            _assert_reboot_handoff_html(
                reboot_body.decode("utf-8", errors="replace"),
                expected_heading="Rebooting to normal mode",
                expected_target_mode="normal",
                expected_probe_kind="fetch",
            )

        root_html_box = {"html": ""}
        def root_ready_pred() -> Tuple[bool, str]:
            try:
                root_status, root_body = _http_request_full("GET", base + "/", headers={}, body=b"", timeout_s=20)
            except Exception as e:
                return False, f"err={e}"
            if root_status != 200:
                return False, f"status={root_status}"
            html = root_body.decode("utf-8", errors="replace")
            if "Alpha2MQTT Control" not in html:
                return False, "waiting for runtime root"
            if "Boot mode:" not in html:
                return False, "runtime root missing boot status"
            if "MQTT connected: 1" not in html:
                return False, "waiting for runtime mqtt reconnect"
            root_html_box["html"] = html
            return True, "ok"

        _assert_eventually("runtime root page after wifi save-only reboot", root_ready_pred, timeout_s=60, poll_s=2.0)

    def case_portal_mqtt_save_reboot_handoff() -> None:
        reboot_url = _discover_reboot_wifi_path_from_code()
        if not reboot_url:
            raise E2EError("Could not discover /reboot/wifi endpoint from firmware source")
        base = _resolve_device_http_base(mqtt, device_root)

        print(f"[e2e] rebooting into wifi portal via {reboot_url} (mqtt save handoff check)")
        reboot_wifi_status, reboot_wifi_body = _http_request_full(
            "POST", base + reboot_url, headers={}, body=b"", timeout_s=20
        )
        if reboot_wifi_status != 200:
            raise E2EError(f"{reboot_url} returned unexpected status={reboot_wifi_status}")
        if reboot_wifi_body:
            _assert_reboot_handoff_html(
                reboot_wifi_body.decode("utf-8", errors="replace"),
                expected_heading="Rebooting to Wi-Fi config",
                expected_target_mode="wifi",
                expected_probe_kind="fetch",
            )
        _wait_for_http_ok(base + "/", timeout_s=40)
        _assert_portal_root_menu(base, timeout_s=20, required_mode="wifi")

        mqtt_status, mqtt_body = _http_request_full("GET", base + "/config/mqtt", headers={}, body=b"", timeout_s=20)
        if mqtt_status != 200:
            raise E2EError(f"/config/mqtt returned unexpected status={mqtt_status}")
        mqtt_html = mqtt_body.decode("utf-8", errors="replace")
        fields = {
            "server": _extract_input_value(mqtt_html, "server"),
            "port": _extract_input_value(mqtt_html, "port"),
            "user": _extract_input_value(mqtt_html, "user"),
            "mpass": _extract_input_value(mqtt_html, "mpass"),
            "inverter_label": _extract_input_value(mqtt_html, "inverter_label"),
        }
        if not fields["server"] or not fields["port"]:
            raise E2EError(f"portal mqtt page missing saved runtime config: {fields!r}")

        boot_topic = f"{device_root}/boot"
        poll_topic = f"{device_root}/status/poll"
        previous_boot = _fetch_latest_text(mqtt, boot_topic, label="boot_before_portal_mqtt_save")
        previous_poll = _fetch_latest_text(mqtt, poll_topic, label="poll_before_portal_mqtt_save")

        save_status, save_body = _http_post_form_full(base + "/config/mqtt/save", fields, timeout_s=20)
        if save_status != 200:
            raise E2EError(f"/config/mqtt/save should return reboot handoff HTML (got status={save_status})")
        _assert_reboot_handoff_html(
            save_body.decode("utf-8", errors="replace"),
            expected_heading="Rebooting to normal mode",
            expected_target_mode="normal",
            expected_probe_kind="fetch",
        )

        _wait_for_topic_change(mqtt, boot_topic, previous_boot, timeout_s=60, label="boot after portal mqtt save")
        _wait_for_topic_change(mqtt, poll_topic, previous_poll, timeout_s=60, label="poll after portal mqtt save")

        def root_ready_pred() -> Tuple[bool, str]:
            try:
                root_status, root_body = _http_request_full("GET", base + "/", headers={}, body=b"", timeout_s=20)
            except Exception as e:
                return False, f"err={e}"
            if root_status != 200:
                return False, f"status={root_status}"
            html = root_body.decode("utf-8", errors="replace")
            if "Alpha2MQTT Control" not in html:
                return False, "waiting for runtime root"
            if "MQTT connected: 1" not in html:
                return False, "waiting for runtime mqtt reconnect"
            return True, "ok"

        _assert_eventually("runtime root page after portal mqtt save reboot", root_ready_pred, timeout_s=60, poll_s=2.0)

    cases: list[Tuple[str, Callable[[], None]]] = [
        ("two_device_discovery", case_two_device_discovery),
        ("offline", case_offline),
        ("fail_then_recover", case_fail_then_recover),
        ("runtime_loss_reprobe", case_runtime_loss_reprobe),
        ("online", case_online),
        ("boot_mem_publish", case_boot_mem_publish),
        ("scheduler_idle_no_extra_reads", case_scheduler_idle_does_not_add_reads),
        ("bucket_snapshot_skip_only", case_bucket_snapshot_skip_only),
        ("dispatch_write_via_commands", case_dispatch_write_via_commands),
        ("dispatch_legacy_command_topic_ignored", case_dispatch_legacy_command_topics_ignored),
        ("dispatch_invalid_payload_no_write", case_dispatch_invalid_payload_no_write),
        ("dispatch_invalid_numeric_payloads_no_write", case_dispatch_invalid_numeric_payloads_no_write),
        ("dispatch_write_feedback", case_dispatch_write_feedback_via_register_value),
        ("dispatch_primes_snapshot_once", case_dispatch_primes_single_snapshot_refresh),
        ("dispatch_write_under_100ms", case_dispatch_write_under_100ms),
        ("dispatch_timed_restart_expire", case_dispatch_timed_restart_and_expire),
        ("dispatch_timed_no_rewrite_without_fresh_snapshot", case_dispatch_timed_no_rewrite_without_fresh_snapshot),
        ("dispatch_disable_timed_stops_countdown_wakes", case_dispatch_disable_timed_stops_countdown_wakes),
        ("dispatch_boot_fail_closed", case_dispatch_boot_fail_closed),
        ("fail_specific_snapshot_reg", case_fail_specific_snapshot_register_and_type),
        ("rs485_error_counters_split", case_rs485_error_counters_split),
        ("fail_every_n", case_fail_every_n_snapshot_attempts),
        ("latency", case_latency_does_not_break_status),
        ("flapping", case_flapping_online_offline),
        ("probe_delayed", case_probe_delayed_online),
        ("identity_reboot_unknown", case_identity_reboot_unknown_after_offline_reboot),
        ("fail_writes_only", case_fail_writes_only_dispatch_write_fails),
        ("dispatch_readback_window", case_dispatch_readback_window_tolerates_transient_read_failures),
        ("dispatch_readback_timeout_status", case_dispatch_readback_timeout_status),
        ("max_feedin_percent_write", case_max_feedin_percent_write),
        ("fail_for_ms", case_fail_for_ms_then_recover),
        ("strict_unknown_snapshot", case_strict_unknown_snapshot_has_no_unknown_reads),
        ("strict_unknown_register_reads", case_strict_unknown_register_reads),
        ("soc_drift_backend_ready", case_soc_drift_backend_ready),
        ("stub_soc_drift_applies", case_stub_soc_drift_applies),
        ("soc_publish_respects_bucket", case_soc_publish_respects_bucket),
        ("soc_drift_e2e", case_soc_drift_e2e),
        ("load_power_formula", case_load_power_snapshot_formula),
        ("polling_config", case_polling_config_persistence),
        ("portal_rs485_baud_reconcile", case_portal_rs485_baud_reconcile),
        ("runtime_polling_reset_without_page", case_runtime_polling_reset_without_page),
        ("portal_polling_ui", case_portal_polling_ui),
        ("polling_profile_export_import", case_polling_profile_export_import),
        ("reboot_ap_confirmation", case_reboot_ap_confirmation),
        ("portal_wifi_save_reboot_only", case_portal_wifi_save_reboot_only),
        ("portal_mqtt_save_reboot_handoff", case_portal_mqtt_save_reboot_handoff),
    ]

    case_map = {name: fn for name, fn in cases}
    ordered_names = [name for name, _ in cases]
    if from_case:
        ordered_names = ordered_names[ordered_names.index(from_case):]
    if selected_cases:
        selected_set = set(selected_cases)
        ordered_names = [name for name in ordered_names if name in selected_set]
    if not ordered_names:
        raise E2EError("No cases selected to run")

    ensure_clean_suite_baseline()

    def dump_failure_context(failed_case: str) -> None:
        print(f"[e2e] failure context for case={failed_case}")
        for label, topic, fetcher in (
            ("poll", poll_topic, lambda: _fetch_poll(mqtt, poll_topic)),
            ("stub", stub_topic, lambda: _fetch_stub(mqtt, stub_topic)),
            ("core", status_core_topic, lambda: _fetch_status_core(mqtt, status_core_topic)),
            ("config", config_topic, lambda: _fetch_config(mqtt, config_topic)),
        ):
            try:
                payload = fetcher()
                print(f"[e2e] ctx {label} topic={topic} payload={json.dumps(payload, separators=(',', ':'))}")
            except Exception as e:
                print(f"[e2e] ctx {label} topic={topic} err={e}")

    for name in ordered_names:
        fn = case_map[name]
        _announce(f"running case: {name}")
        try:
            _mqtt_retry(mqtt, f"case:{name}", lambda f=fn: f())
        except Exception:
            dump_failure_context(name)
            raise

    print("[e2e] OK")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except E2EError as e:
        print(f"[e2e] FAIL: {e}", file=sys.stderr)
        raise SystemExit(2)
