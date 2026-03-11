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
  - "offline" / {"mode":"offline"}
  - "online"  / {"mode":"online"}
  - "fail 2"  / {"mode":"fail","fail_n":2}
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
    "online",
    "scheduler_idle_no_extra_reads",
    "bucket_snapshot_skip_only",
    "dispatch_write_via_commands",
    "dispatch_write_feedback",
    "strict_unknown_snapshot",
    "strict_unknown_register_reads",
    "fail_specific_snapshot_reg",
    "fail_every_n",
    "latency",
    "flapping",
    "probe_delayed",
    "fail_writes_only",
    "fail_for_ms",
    "soc_drift_backend_ready",
    "stub_soc_drift_applies",
    "soc_publish_respects_bucket",
    "soc_drift_e2e",
    "polling_config",
    "portal_polling_ui",
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
    if 'httpServer.on("/reboot/wifi"' in data:
        return "/reboot/wifi"
    return None

def _discover_reboot_normal_path_from_code() -> Optional[str]:
    data = _firmware_main_cpp().read_text(encoding="utf-8", errors="replace")
    if 'httpServer.on("/reboot/normal"' in data:
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
        self._pending_publishes: list[Tuple[str, str]] = []
        self._subscriptions: set[str] = set()
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
                self._pending_publishes.append((topic, payload_str))
                _log(f"mqtt rx buffered: topic={topic} bytes={len(payload_str)}")
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

    def _try_wait_for_publish(self, timeout_s: float) -> Optional[Tuple[str, str]]:
        try:
            return self.wait_for_publish(timeout_s=timeout_s)
        except E2EError as e:
            if "Timeout waiting for MQTT publish" in str(e):
                return None
            raise

    def wait_for_publish(self, timeout_s: float) -> Tuple[str, str]:
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
            pkt_type, _flags, data = self._read_packet(timeout_s=remaining)
            if pkt_type == 3:  # PUBLISH
                topic, payload = self._decode_publish(data)
                _log(f"mqtt rx: topic={topic} bytes={len(payload)}")
                return topic, payload
            # Ignore other packet types.
        raise E2EError("Timeout waiting for MQTT publish")

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

    status, _ = _http_request("POST", url, headers=headers, body=body, timeout_s=timeout_s)
    return status

def _http_request_full(method: str, url: str, headers: dict[str, str], body: bytes, timeout_s: int = 20, max_bytes: int = 65536) -> Tuple[int, bytes]:
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

        body_bytes = resp_body
        while len(body_bytes) < max_bytes:
            try:
                chunk = conn.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            body_bytes += chunk
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

def _wait_for_http_ok(url: str, timeout_s: int = 30) -> None:
    def pred() -> Tuple[bool, str]:
        try:
            status, _ = _http_request("GET", url, headers={}, body=b"", timeout_s=5)
            return (status in (200, 302)), f"status={status}"
        except Exception as e:
            return False, f"err={e}"
    _assert_eventually(f"HTTP reachable: {url}", pred, timeout_s=timeout_s, poll_s=2.0)

def _discover_polling_menu_path(menu_html: str) -> str:
    # Accept both the root menu form button and setup-page anchor forms of the link.
    m = re.search(r"""(?:action|href)=['"](/config/polling(?:\?[^'"]*)?)['"]""", menu_html, flags=re.IGNORECASE)
    if not m:
        raise E2EError("portal menu missing Polling entry/link")
    return m.group(1)

def _load_polling_page_via_menu(base: str) -> tuple[str, str]:
    direct_path = "/config/polling?page=0"
    status_poll, poll_body = _http_request_full("GET", base + direct_path, headers={}, body=b"", timeout_s=20)
    if status_poll == 200:
        return direct_path, poll_body.decode("utf-8", errors="replace")

    deadline = time.time() + 25
    polling_path = ""
    last_detail = "not checked"
    while time.time() < deadline:
        for menu_path in ("/", "/param"):
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
        time.sleep(1.0)
    if not polling_path:
        raise E2EError(f"portal menu missing Polling entry/link ({last_detail})")

    status_poll, poll_body = _http_request_full("GET", base + polling_path, headers={}, body=b"", timeout_s=20)
    if status_poll != 200:
        raise E2EError(f"portal polling page not reachable via menu link {polling_path}: status={status_poll}")
    poll_html = poll_body.decode("utf-8", errors="replace")
    return polling_path, poll_html

def _extract_polling_page_bounds(poll_html: str) -> tuple[int, int]:
    m = re.search(r'<p class="hint">Page\s+(\d+)\s+of\s+(\d+)</p>', poll_html)
    if not m:
        raise E2EError("could not locate polling page bounds")
    page = int(m.group(1)) - 1
    total = int(m.group(2))
    if page < 0 or total <= 0 or page >= total:
        raise E2EError(f"invalid polling page bounds page={page} total={total}")
    return page, total

def _load_polling_page(base: str, page: int) -> str:
    status_poll, poll_body = _http_request_full(
        "GET",
        f"{base}/config/polling?page={page}",
        headers={},
        body=b"",
        timeout_s=20,
    )
    if status_poll != 200:
        raise E2EError(f"portal polling page {page} not reachable: status={status_poll}")
    return poll_body.decode("utf-8", errors="replace")

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

def _locate_entity_on_polling_pages(base: str, first_page_html: str, entity_name: str) -> tuple[int, int, str, str]:
    first_page, total_pages = _extract_polling_page_bounds(first_page_html)
    pages = [first_page] + [page for page in range(total_pages) if page != first_page]
    for page in pages:
        page_html = first_page_html if page == first_page else _load_polling_page(base, page)
        try:
            row, bucket = _extract_entity_row_and_selected_bucket(page_html, entity_name)
            return page, total_pages, row, bucket
        except E2EError:
            continue
    raise E2EError(f"could not locate {entity_name} row on any polling page")


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

def _mqtt_retry(mqtt: MqttClient, name: str, fn: Callable[[], Any]) -> Any:
    """
    Some broker/network setups will occasionally drop a TCP connection mid-test.
    Reconnect once and retry so the E2E remains robust and bounded.
    """
    for attempt in range(5):
        try:
            return fn()
        except E2EError as e:
            if not _is_socket_closed_error(e) or attempt == 4:
                raise
            print(f"[e2e] mqtt socket closed during {name}; reconnecting...")
            mqtt.close()
            time.sleep(0.5)
            mqtt.connect()
            # Broker-side subscription state is lost on reconnect.
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


def _fetch_poll(mqtt: MqttClient, topic: str) -> dict[str, Any]:
    return _fetch_latest_json(mqtt, topic, label="poll")

def _fetch_config(mqtt: MqttClient, topic: str) -> dict[str, Any]:
    return _fetch_latest_json(mqtt, topic, label="config")

def _fetch_status_core(mqtt: MqttClient, topic: str) -> dict[str, Any]:
    return _fetch_latest_json(mqtt, topic, label="core")

def _fetch_status_net(mqtt: MqttClient, topic: str) -> dict[str, Any]:
    return _fetch_latest_json(mqtt, topic, label="net")

def _fetch_boot(mqtt: MqttClient, boot_topic: str) -> dict[str, Any]:
    # Boot is retained, but if the broker has restarted (no retained state), we may need to
    # wait for a device reboot to republish it.
    return _fetch_latest_json(mqtt, boot_topic, label="boot", timeout_s=60)

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
    mqtt.subscribe(boot_topic, force=True)
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

def _fetch_stub(mqtt: MqttClient, topic: str) -> dict[str, Any]:
    return _fetch_latest_json(mqtt, topic, label="stub")

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

def _select_register_and_wait_manual_read(
    mqtt: MqttClient,
    command_topic: str,
    manual_topic: str,
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
    try:
        boot = _fetch_boot(mqtt, f"{device_root}/boot")
        fw_ts = boot.get("fw_build_ts_ms")
        if not isinstance(fw_ts, int):
            return False, f"boot.fw_build_ts_ms missing/invalid (keys={sorted(boot.keys())})"
        if fw_ts != expected_build_ts_ms:
            return False, f"fw_build_ts_ms={fw_ts} expected={expected_build_ts_ms}"
    except E2EError as e:
        # If the broker doesn't have retained boot state (e.g. broker restart), we can't verify build id
        # until the device republishes /boot (usually on reboot). Treat as "not verified".
        return False, f"boot_unavailable ({e})"

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
        return False, f"status_net_stale ({e})"
    return True, "ok"

def _resolve_device_http_base(mqtt: MqttClient, device_root: str) -> str:
    configured = os.environ.get("DEVICE_HTTP_BASE", "").strip()
    net_topic = f"{device_root}/status/net"
    try:
        net = _fetch_status_net(mqtt, net_topic)
        ip = str(net.get("ip", "")).strip()
        if ip:
            live_base = f"http://{ip}"
            if configured and configured != live_base:
                _announce(f"DEVICE_HTTP_BASE override ignored; using live status/net IP {live_base}")
            return live_base
    except E2EError:
        pass

    if configured:
        return configured
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
    upload_path = os.environ.get("DEVICE_OTA_UPLOAD_PATH", "/u")
    field_name = os.environ.get("DEVICE_OTA_FIELD_NAME", "update")

    reboot_url = http_base.rstrip("/") + reboot_path
    upload_url = http_base.rstrip("/") + upload_path

    print(f"[e2e] POST {reboot_path} (reboot into Wi-Fi config portal)")
    try:
        status = _http_post_simple(reboot_url, timeout_s=10)
    except (TimeoutError, OSError) as e:
        raise E2EError(
            f"HTTP request to device failed ({e}). "
            "Check DEVICE_HTTP_BASE is reachable from this host and the device is on the LAN."
        )
    print(f"[e2e] reboot HTTP status={status}")
    if status == 404:
        print("[e2e] reboot endpoint not found (old firmware or not in NORMAL); continuing with direct upload")

    # Give the device time to reboot and bring the portal up.
    _sleep_with_mqtt(mqtt, 8)

    print(f"[e2e] POST {upload_path} (upload firmware: {firmware_path.name})")
    status: Optional[int] = None
    try:
        status = _http_post_multipart(upload_url, field_name=field_name, file_path=firmware_path, timeout_s=240)
        print(f"[e2e] upload HTTP status={status}")
        if status < 200 or status >= 400:
            raise E2EError(f"OTA upload failed (HTTP {status})")
    except TimeoutError:
        # Some OTA implementations reboot before responding, or hold the connection open without a response.
        # Treat this as ambiguous and confirm success via MQTT build timestamp instead.
        print("[e2e] upload timed out waiting for HTTP response; verifying success via MQTT...")

    print("[e2e] waiting for device to reboot and report new fw_build_ts_ms over MQTT...")
    # Ensure we observe a *new boot publish* with the new build id; otherwise the retained /boot
    # message can remain stale during this run and make verification flaky.
    _wait_for_boot_fw_build_ts_ms(mqtt, f"{device_root}/boot", expected_ts, timeout_s=120)

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

    # Verify stub backend is actually active (after optional ensure/update).
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

    def set_mode(payload: str) -> None:
        mqtt.publish(control_topic, payload, retain=False)

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
            cur = _fetch_poll(mqtt, poll_topic)
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
                actual = str(intervals.get(key, ""))
                if actual != expected:
                    mismatches.append(f"{key}={actual!r}")
            return (not mismatches), ", ".join(mismatches) if mismatches else "ok"

        _assert_eventually(
            "intervals applied",
            pred,
            timeout_s=timeout_s,
            poll_s=2.0,
        )

    def set_polling_config(poll_interval_s: int, bucket_map: str) -> None:
        # Config-set parser expects string values, not numbers.
        payload = json.dumps({
            "poll_interval_s": str(poll_interval_s),
            "bucket_map": bucket_map,
        })
        mqtt.publish(f"{device_root}/config/set", payload, retain=False)

    def _wait_for_inverter_identity() -> str:
        def inverter_id_from_ha_unique(ha_unique: str) -> str:
            if not ha_unique.startswith("A2M-"):
                return ""
            serial = ha_unique[4:]
            if not serial or serial.lower() == "unknown":
                return ""
            return f"alpha2mqtt_inv_{serial}"

        def pred() -> Tuple[bool, str]:
            core = _fetch_status_core(mqtt, status_core_topic)
            ha_unique = str(core.get("ha_unique_id", ""))
            inverter_id = inverter_id_from_ha_unique(ha_unique)
            detail = f"ha_unique_id={ha_unique!r} inverter_id={inverter_id!r}"
            return (inverter_id != ""), detail
        _assert_eventually("inverter identity available", pred, timeout_s=60, poll_s=3.0)
        core = _fetch_status_core(mqtt, status_core_topic)
        return inverter_id_from_ha_unique(str(core.get("ha_unique_id", "")))

    def _state_topic(inverter_device_id: str, name: str) -> str:
        return f"{device_root}/{inverter_device_id}/{name}/state"

    def _command_topic(inverter_device_id: str, name: str) -> str:
        return f"{device_root}/{inverter_device_id}/{name}/command"

    def _manual_read_topic() -> str:
        return f"{device_root}/status/manual_read"

    def case_two_device_discovery() -> None:
        print("[e2e] case: two-device discovery model")
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

        # Force a discovery re-advertise so checks are against current firmware behavior, not stale retained topics.
        mqtt.publish("homeassistant/status", "online", retain=False)
        _sleep_with_mqtt(mqtt, 2)

        serial_known = str(controller_serial).strip().lower() not in ("", "unknown")
        inverter_device_id = _wait_for_inverter_identity() if serial_known else ""
        inverter_serial_uid = f"{controller_id}_inverter_serial"
        expected_topics = {
            f"homeassistant/sensor/{controller_id}/inverter_serial/config",
            f"homeassistant/sensor/{controller_id}/A2M_version/config",
        }
        if serial_known:
            expected_topics.add(f"homeassistant/sensor/{inverter_device_id}/Inverter_SN/config")

        seen_topics: set[str] = set()
        mqtt.subscribe("homeassistant/+/+/+/config", force=True)
        deadline = time.time() + 25
        while time.time() < deadline and seen_topics != expected_topics:
            try:
                got_topic, raw_payload = mqtt.wait_for_publish(timeout_s=5.0)
            except E2EError as e:
                if "Timeout waiting for MQTT publish" in str(e):
                    continue
                raise
            if got_topic not in expected_topics:
                continue
            try:
                payload = _parse_json(raw_payload)
            except Exception:
                continue
            if got_topic.endswith("/inverter_serial/config"):
                if str(payload.get("unique_id", "")) != inverter_serial_uid:
                    raise E2EError(
                        f"controller inverter_serial unique_id mismatch: "
                        f"expected {inverter_serial_uid!r} got {payload.get('unique_id')!r}"
                    )
            elif got_topic.endswith("/A2M_version/config"):
                if not str(payload.get("unique_id", "")).startswith(f"{controller_id}_"):
                    raise E2EError("controller discovery entities not found (unique_id prefix mismatch)")
            elif got_topic.endswith("/Inverter_SN/config"):
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
            seen_topics.add(got_topic)

        missing = sorted(expected_topics - seen_topics)
        if missing:
            raise E2EError(f"missing discovery topic(s): {missing}")

    def case_offline() -> None:
        print("[e2e] case: stub offline")
        before = _fetch_poll(mqtt, poll_topic)
        before_attempts = int(before.get("ess_snapshot_attempts", 0))
        set_mode_and_wait('{"mode":"offline"}', ("offline",))

        def pred() -> Tuple[bool, str]:
            cur = _fetch_poll(mqtt, poll_topic)
            attempts = int(cur.get("ess_snapshot_attempts", 0))
            ok = bool(cur.get("ess_snapshot_last_ok", False))
            skip = str(cur.get("dispatch_last_skip_reason", ""))
            mode = str(cur.get("rs485_stub_mode", ""))
            detail = f"attempts={attempts} ok={ok} skip={skip} mode={mode}"
            return (mode == "offline" and attempts > before_attempts and (not ok) and skip == "ess_snapshot_failed"), detail

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

    def case_bucket_snapshot_skip_only() -> None:
        print("[e2e] case: bucket gating skips only ESS snapshot entities (and dispatch) when snapshot fails")

        # Keep this test lightweight: prove that non-snapshot status continues to publish while
        # ESS snapshot (and dispatch) is suppressed.
        set_mode_and_wait('{"mode":"online"}', ("online",))
        net_topic = f"{device_root}/status/net"
        mqtt.subscribe(net_topic, force=True)
        net1_text = _fetch_latest_text(mqtt, net_topic, label="net")
        u1 = int(_parse_json(net1_text).get("uptime_s", 0))
        before = _fetch_poll(mqtt, poll_topic)
        before_attempts = int(before.get("ess_snapshot_attempts", 0))

        # Force snapshot failure and confirm:
        # - net uptime continues (non-snapshot publish)
        # - dispatch is suppressed
        set_mode_and_wait('{"mode":"offline"}', ("offline",))
        _assert_eventually(
            "offline triggers dispatch suppression",
            lambda: (
                (
                    str(_fetch_poll(mqtt, poll_topic).get("dispatch_last_skip_reason", "")) == "ess_snapshot_failed"
                    and str(_fetch_poll(mqtt, poll_topic).get("rs485_stub_mode", "")) == "offline"
                    and int(_fetch_poll(mqtt, poll_topic).get("ess_snapshot_attempts", 0)) > before_attempts
                ),
                (
                    f"skip={_fetch_poll(mqtt, poll_topic).get('dispatch_last_skip_reason')} "
                    f"mode={_fetch_poll(mqtt, poll_topic).get('rs485_stub_mode')} "
                    f"attempts={_fetch_poll(mqtt, poll_topic).get('ess_snapshot_attempts')}"
                ),
            ),
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
        set_mode_and_wait('{"mode":"online"}', ("online",))
        _assert_eventually(
            "snapshot recovers after online",
            lambda: (bool(_fetch_poll(mqtt, poll_topic).get("ess_snapshot_last_ok", False)), "waiting"),
            timeout_s=60,
            poll_s=3.0,
        )
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
        print("[e2e] case: dispatch write via command topics (virtual inverter)")
        # Make sure stub is online and in a known inverter state.
        set_mode_and_wait('{"mode":"online","soc_pct":50,"battery_power_w":0,"grid_power_w":0,"pv_ct_power_w":0}', ("online",))

        ha_unique = _wait_for_inverter_identity()
        base = f"{device_root}/{ha_unique}"

        # Ensure we can assert on the actual dispatch start register value.
        reg_dispatch_start = _discover_register_value("REG_DISPATCH_RW_DISPATCH_START")
        dispatch_start_start = _discover_define_value("DISPATCH_START_START")
        manual_topic = _manual_read_topic()
        manual_before = _select_register_and_wait_manual_read(
            mqtt,
            _command_topic(ha_unique, "Register_Number"),
            manual_topic,
            reg_dispatch_start,
            label="dispatch_start_before",
        )
        before_val = str(manual_before.get("value", ""))
        if before_val == "":
            raise E2EError("manual_read missing value for dispatch_start_before")

        op_mode_target = _discover_define_string("OP_MODE_DESC_TARGET")

        # Publish all required command knobs so the firmware considers A2M "ready".
        # Note: inverter command topics are only subscribed once inverter identity is known; to avoid a race where
        # we publish before subscriptions exist, we republish periodically while waiting.
        def publish_ready_commands() -> None:
            mqtt.publish(f"{base}/Op_Mode/command", op_mode_target, retain=False)
            mqtt.publish(f"{base}/SOC_Target/command", "80", retain=False)
            mqtt.publish(f"{base}/Charge_Power/command", "1200", retain=False)
            mqtt.publish(f"{base}/Discharge_Power/command", "1200", retain=False)
            mqtt.publish(f"{base}/Push_Power/command", "500", retain=False)

        # Subscribe to the state topics too, and wait until the firmware echoes these values back.
        # This makes the test deterministic: it proves the callback/subscription path is active before we expect a dispatch write.
        def wait_for_state(name: str, expected: str) -> None:
            st = _state_topic(ha_unique, name)
            mqtt.subscribe(st, force=True)
            cmd = f"{base}/{name}/command"
            deadline = time.time() + 35
            last_seen = ""
            while time.time() < deadline:
                mqtt.publish(cmd, expected, retain=False)
                try:
                    last_seen = _fetch_latest_text(mqtt, st, label=f"{name}_state")
                    if last_seen.strip().lower() == expected.strip().lower():
                        return
                except E2EError:
                    pass
                time.sleep(1.0)
            raise E2EError(f"{name} did not update to {expected!r}; last_seen={last_seen!r}")

        # Capture baseline before publishing any readiness commands so we don't miss a fast dispatch write.
        before = _fetch_poll(mqtt, poll_topic)
        before_writes = int(before.get("rs485_stub_writes", 0))
        before_last_ms = int(before.get("rs485_stub_last_write_ms", 0))

        # Force a dispatch mismatch AFTER baseline so a new write is required.
        set_mode_and_wait('{"mode":"online","dispatch_start":0,"dispatch_mode":65535,"dispatch_active_power":0,"dispatch_soc":0}', ("online",))

        publish_ready_commands()
        wait_for_state("Op_Mode", op_mode_target)
        wait_for_state("SOC_Target", "80")
        wait_for_state("Charge_Power", "1200")
        wait_for_state("Discharge_Power", "1200")
        wait_for_state("Push_Power", "500")
        last_pub = time.time()

        def pred() -> Tuple[bool, str]:
            cur = _fetch_poll(mqtt, poll_topic)
            writes = int(cur.get("rs485_stub_writes", 0))
            last_reg = int(cur.get("rs485_stub_last_write_reg", 0))
            last_ms = int(cur.get("rs485_stub_last_write_ms", 0))
            detail = f"writes={writes} last_reg={last_reg} last_ms={last_ms} expect_reg={reg_dispatch_start}"
            nonlocal last_pub
            if time.time() - last_pub > 8.0:
                publish_ready_commands()
                last_pub = time.time()
            wrote_after_baseline = (writes > before_writes) or (last_ms > before_last_ms)
            return (wrote_after_baseline and last_reg == reg_dispatch_start and last_ms > 0), detail

        _assert_eventually("dispatch write observed in stub backend", pred, timeout_s=45, poll_s=3.0)

        manual_after = _select_register_and_wait_manual_read(
            mqtt,
            _command_topic(ha_unique, "Register_Number"),
            manual_topic,
            reg_dispatch_start,
            label="dispatch_start_after",
        )
        observed_reg = int(manual_after.get("observed_reg", 0))
        if observed_reg != reg_dispatch_start:
            raise E2EError(f"manual_read observed_reg mismatch: expected {reg_dispatch_start}, got {observed_reg}")
        last_val = str(manual_after.get("value", ""))
        normalized = last_val.strip().lower()
        if normalized not in ("start", "started") and last_val.strip() != str(dispatch_start_start):
            raise E2EError(
                f"manual_read did not reflect dispatch_start change; expected 'Start' (or {dispatch_start_start}) "
                f"but got last={last_val!r} initial={before_val!r}"
            )
        return

    def case_dispatch_write_feedback_via_register_value() -> None:
        print("[e2e] case: dispatch write feedback (Register_Value reflects new dispatch state)")
        # Coverage is now handled in case_dispatch_write_via_commands() to avoid duplicated, flaky sequencing.
        return

    def case_strict_unknown_snapshot_has_no_unknown_reads() -> None:
        print("[e2e] case: strict unknown-register protection (snapshot path)")
        ha_unique = _wait_for_inverter_identity()
        # Strict mode should be safe for ESS snapshot: the stub must implement all snapshot registers.

        # Ensure Register_Value is pointed at a known register, so stale unknown-register selection from
        # previous runs cannot inflate unknown-read counters during this snapshot-focused check.
        safe_reg = _discover_register_value("REG_BATTERY_HOME_R_SOC")
        regnum_topic = _state_topic(ha_unique, "Register_Number")
        mqtt.subscribe(regnum_topic, force=True)
        deadline = time.time() + 30
        last_seen = ""
        while time.time() < deadline:
            mqtt.publish(_command_topic(ha_unique, "Register_Number"), str(safe_reg), retain=False)
            try:
                last_seen = _fetch_latest_text(mqtt, regnum_topic, label="reg_number_state")
                if last_seen.strip() == str(safe_reg):
                    break
            except E2EError:
                pass
            time.sleep(1.0)
        else:
            raise E2EError(f"Register_Number did not update to {safe_reg}; last_seen={last_seen!r}")

        # When already in online mode, mode-only acknowledgement is insufficient; keep publishing
        # until the strict flag itself is observed in status/stub.
        strict_payload = '{"mode":"online","strict_unknown":1,"strict":1}'
        last_pub = 0.0
        def strict_applied_pred() -> Tuple[bool, str]:
            nonlocal last_pub
            now = time.time()
            if (now - last_pub) >= 6.0:
                set_mode(strict_payload)
                last_pub = now

            cur_poll = _fetch_poll(mqtt, poll_topic)
            cur_stub = _fetch_stub(mqtt, stub_topic)
            mode = str(cur_poll.get("rs485_stub_mode", ""))
            strict_val = cur_stub.get("strict_unknown", False)
            strict_on = strict_val is True or strict_val == 1 or str(strict_val).lower() == "true"
            detail = f"mode={mode} strict={strict_val!r}"
            return (mode == "online" and strict_on), detail

        _assert_eventually(
            "strict_unknown applied",
            strict_applied_pred,
            timeout_s=45,
            poll_s=2.0,
        )

        # Baseline counters after strict mode is applied (firmware may reset counters on apply).
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
        s2 = _fetch_stub(mqtt, stub_topic)
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
        ha_unique = _wait_for_inverter_identity()
        manual_topic = _manual_read_topic()

        # Use a handled register that is not virtualized by the stub (so the stub sees it as unknown).
        reg = _discover_register_value("REG_INVERTER_HOME_R_INVERTER_TEMP")

        before_stub = _fetch_stub(mqtt, stub_topic)
        before_unknown = int(before_stub.get("stub_unknown_reads", 0))

        set_mode_and_wait('{"mode":"online","strict_unknown":0}', ("online",))
        _assert_eventually(
            "strict_unknown disabled while backend stays online",
            lambda: (
                str(_fetch_poll(mqtt, poll_topic).get("rs485_stub_mode", "")) == "online"
                and not bool(_fetch_stub(mqtt, stub_topic).get("strict_unknown", False)),
                f"mode={_fetch_poll(mqtt, poll_topic).get('rs485_stub_mode')} strict={_fetch_stub(mqtt, stub_topic).get('strict_unknown')}",
            ),
            timeout_s=30,
            poll_s=2.0,
        )
        loose_manual = _select_register_and_wait_manual_read(
            mqtt,
            _command_topic(ha_unique, "Register_Number"),
            manual_topic,
            reg,
            label="reg_value_loose",
        )
        value_loose = str(loose_manual.get("value", ""))
        if int(loose_manual.get("observed_reg", 0)) != reg:
            raise E2EError(f"manual_read observed_reg mismatch in loose mode: expected {reg}, got {loose_manual.get('observed_reg')!r}")
        def loose_recorded_pred() -> Tuple[bool, str]:
            cur = _fetch_stub(mqtt, stub_topic)
            unknown_now = int(cur.get("stub_unknown_reads", 0))
            return (unknown_now > before_unknown), f"unknown_reads={unknown_now}"
        _assert_eventually(
            "loose unknown register read recorded in stub status",
            loose_recorded_pred,
            timeout_s=25,
            poll_s=1.5,
        )
        after_loose = _fetch_stub(mqtt, stub_topic)

        unknown_after = int(after_loose.get("stub_unknown_reads", 0))
        strict_after = bool(after_loose.get("strict_unknown", False))
        if value_loose in ("Slave Error", "Nothing read") or unknown_after <= before_unknown or strict_after:
            raise E2EError(
                f"Expected loose unknown read (not Slave Error) and unknown_reads to increase. "
                f"value={value_loose!r} unknown_before={before_unknown} unknown_after={unknown_after} strict={strict_after}"
            )

        set_mode_and_wait('{"mode":"online","strict_unknown":1}', ("online",))
        _assert_eventually(
            "strict_unknown enabled while backend stays online",
            lambda: (
                str(_fetch_poll(mqtt, poll_topic).get("rs485_stub_mode", "")) == "online"
                and bool(_fetch_stub(mqtt, stub_topic).get("strict_unknown", False)),
                f"mode={_fetch_poll(mqtt, poll_topic).get('rs485_stub_mode')} strict={_fetch_stub(mqtt, stub_topic).get('strict_unknown')}",
            ),
            timeout_s=30,
            poll_s=2.0,
        )
        strict_manual = _select_register_and_wait_manual_read(
            mqtt,
            _command_topic(ha_unique, "Register_Number"),
            manual_topic,
            reg,
            label="reg_value_strict",
        )
        value_strict = str(strict_manual.get("value", ""))
        if int(strict_manual.get("observed_reg", 0)) != reg:
            raise E2EError(f"manual_read observed_reg mismatch in strict mode: expected {reg}, got {strict_manual.get('observed_reg')!r}")
        if value_strict != "Slave Error":
            raise E2EError(f"Expected Slave Error in strict mode but got: {value_strict!r}")
        def strict_recorded_pred() -> Tuple[bool, str]:
            cur = _fetch_stub(mqtt, stub_topic)
            strict_val = bool(cur.get("strict_unknown", False))
            last_fail_type = str(cur.get("last_fail_type", ""))
            return (strict_val and last_fail_type == "slave_error"), f"strict={strict_val} last_fail_type={last_fail_type}"
        _assert_eventually(
            "strict unknown register read recorded in stub status",
            strict_recorded_pred,
            timeout_s=25,
            poll_s=1.5,
        )
        return

    def case_fail_specific_snapshot_register_and_type() -> None:
        print("[e2e] case: fail specific snapshot register + fail type reporting")
        reg_soc = _discover_register_value("REG_BATTERY_HOME_R_SOC")
        set_mode_and_wait(f'{{"mode":"online","fail_n":0,"reg":{reg_soc},"fail_type":1}}', ("online",))

        def pred() -> Tuple[bool, str]:
            cur = _fetch_poll(mqtt, poll_topic)
            ok = bool(cur.get("ess_snapshot_last_ok", False))
            skip = str(cur.get("dispatch_last_skip_reason", ""))
            cur_stub = _fetch_stub(mqtt, stub_topic)
            last_fail_reg = int(cur_stub.get("last_fail_reg", 0))
            last_fail_type = str(cur_stub.get("last_fail_type", ""))
            detail = f"snapshot_ok={ok} skip={skip} last_fail_reg={last_fail_reg} last_fail_type={last_fail_type}"
            return ((not ok) and skip == "ess_snapshot_failed" and last_fail_reg == reg_soc and last_fail_type == "slave_error"), detail

        _assert_eventually("failing SOC register forces snapshot failure with slave_error", pred, timeout_s=60, poll_s=3.0)

        # Clear failure and confirm recovery.
        set_mode_and_wait('{"mode":"online"}', ("online",))
        _assert_eventually("snapshot recovers after clearing failure", lambda: (bool(_fetch_poll(mqtt, poll_topic).get("ess_snapshot_last_ok", False)), "waiting"), timeout_s=60, poll_s=3.0)

    def case_fail_every_n_snapshot_attempts() -> None:
        print("[e2e] case: fail every N snapshot attempts (N=2)")
        # Use slave_error (not no_response) so RS485 stays "online" and we observe clean fail/ok alternation.
        set_mode_and_wait('{"mode":"online","fail_every_n":2,"fail_type":1}', ("online",))

        # Observe a fail at least once (2nd attempt) and a success afterwards.
        seen_fail = False
        seen_ok_after_fail = False
        deadline = time.time() + 80
        last_detail = ""
        while time.time() < deadline:
            cur = _fetch_poll(mqtt, poll_topic)
            attempts = int(cur.get("ess_snapshot_attempts", 0))
            ok = bool(cur.get("ess_snapshot_last_ok", False))
            last_detail = f"attempts={attempts} ok={ok}"
            if not ok:
                seen_fail = True
            if seen_fail and ok:
                seen_ok_after_fail = True
                break
            time.sleep(3.0)
        if not (seen_fail and seen_ok_after_fail):
            raise E2EError(f"fail_every_n did not produce fail->ok within timeout; last={last_detail}")

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
            cur_stub = _fetch_stub(mqtt, stub_topic)
            lat = int(cur_stub.get("latency_ms", 0))
            detail = f"attempts={attempts} ok={ok} last_poll_ms={last_ms} latency_ms={lat}"
            return (attempts > before_attempts and ok and lat == 200 and last_ms > 0), detail

        _assert_eventually("latency allows snapshot attempts and records configured latency", pred, timeout_s=90, poll_s=5.0)

    def case_flapping_online_offline() -> None:
        print("[e2e] case: flapping online/offline toggles snapshot result")
        # The scheduler's snapshot cadence is ~10s. Choose a flap period that is NOT a divisor of 10s,
        # otherwise every snapshot lands in the same phase and you never observe a toggle.
        set_mode_and_wait('{"mode":"flap","flap_online_ms":3500,"flap_offline_ms":3500}', ("flap",))

        seen_ok = False
        seen_fail = False
        deadline = time.time() + 80
        last_detail = ""
        while time.time() < deadline:
            cur = _fetch_poll(mqtt, poll_topic)
            ok = bool(cur.get("ess_snapshot_last_ok", False))
            mode = str(cur.get("rs485_stub_mode", ""))
            last_detail = f"mode={mode} ok={ok}"
            if ok:
                seen_ok = True
            else:
                seen_fail = True
            if seen_ok and seen_fail:
                return
            time.sleep(3.0)
        raise E2EError(f"flap mode did not toggle snapshot ok/fail within timeout; last={last_detail}")

    def case_probe_delayed_online() -> None:
        print("[e2e] case: probe_delayed becomes online after N attempts")
        set_mode_and_wait('{"mode":"probe_delayed","probe_success_after_n":3}', ("probe_delayed",))
        before_stub = _fetch_stub(mqtt, stub_topic)
        before_attempts = int(before_stub.get("probe_attempts", 0))

        def pred() -> Tuple[bool, str]:
            cur_stub = _fetch_stub(mqtt, stub_topic)
            probe_attempts = int(cur_stub.get("probe_attempts", 0))
            cur = _fetch_poll(mqtt, poll_topic)
            ok = bool(cur.get("ess_snapshot_last_ok", False))
            mode = str(cur.get("rs485_stub_mode", ""))
            detail = f"mode={mode} probe_attempts={probe_attempts} snapshot_ok={ok}"
            return (mode == "probe_delayed" and probe_attempts >= before_attempts + 3 and ok), detail

        _assert_eventually("probe_delayed reaches N attempts then snapshot succeeds", pred, timeout_s=90, poll_s=5.0)

    def case_fail_writes_only_dispatch_write_fails() -> None:
        print("[e2e] case: fail writes only (dispatch write fails, snapshot reads still ok)")
        reg_dispatch_start = _discover_register_value("REG_DISPATCH_RW_DISPATCH_START")
        set_mode_and_wait(
            f'{{"mode":"online","reg":{reg_dispatch_start},"fail_writes":1,"fail_reads":0,"fail_type":1,'
            '"soc_pct":50,"battery_power_w":0,"grid_power_w":0,"pv_ct_power_w":0,"dispatch_start":0,"dispatch_mode":0,"dispatch_soc":0}}',
            ("online",),
        )
        ha_unique = _wait_for_inverter_identity()
        base = f"{device_root}/{ha_unique}"

        def publish_ready_commands() -> None:
            mqtt.publish(f"{base}/Op_Mode/command", "target", retain=False)
            mqtt.publish(f"{base}/SOC_Target/command", "80", retain=False)
            mqtt.publish(f"{base}/Charge_Power/command", "1200", retain=False)
            mqtt.publish(f"{base}/Discharge_Power/command", "1200", retain=False)
            mqtt.publish(f"{base}/Push_Power/command", "500", retain=False)

        publish_ready_commands()
        last_pub = time.time()

        def pred() -> Tuple[bool, str]:
            cur = _fetch_poll(mqtt, poll_topic)
            ok = bool(cur.get("ess_snapshot_last_ok", False))
            stub = _fetch_stub(mqtt, stub_topic)
            fail_reg = int(stub.get("last_fail_reg", 0))
            fail_fn = int(stub.get("last_fail_fn", 0))
            fail_type = str(stub.get("last_fail_type", ""))
            detail = f"ok={ok} last_fail_reg={fail_reg} last_fail_fn={fail_fn} last_fail_type={fail_type}"
            nonlocal last_pub
            if time.time() - last_pub > 8.0:
                publish_ready_commands()
                last_pub = time.time()
            # A dispatch write failure isn't necessarily reflected in poll_err_count (which is primarily snapshot-driven).
            return (ok and fail_reg == reg_dispatch_start and fail_fn == 16 and fail_type == "slave_error"), detail

        _assert_eventually("dispatch write failure observed (write fn) without breaking snapshot", pred, timeout_s=90, poll_s=5.0)

    def case_fail_for_ms_then_recover() -> None:
        print("[e2e] case: fail for N ms then recover")
        set_mode_and_wait('{"mode":"online","fail_for_ms":5000}', ("online",))
        # Expect at least one fail early and eventual recovery.
        seen_fail = False
        deadline = time.time() + 40
        last_detail = ""
        while time.time() < deadline:
            cur = _fetch_poll(mqtt, poll_topic)
            ok = bool(cur.get("ess_snapshot_last_ok", False))
            attempts = int(cur.get("ess_snapshot_attempts", 0))
            last_detail = f"attempts={attempts} ok={ok}"
            if not ok:
                seen_fail = True
            if seen_fail and ok:
                return
            time.sleep(3.0)
        raise E2EError(f"fail_for_ms did not show fail then recover within timeout; last={last_detail}")

    def _soc_drift_payload() -> str:
        return '{"mode":"online","soc_pct":50,"soc_step_x10_per_snapshot":10}'

    def _soc_drift_poll_interval() -> int:
        return int(_fetch_poll(mqtt, poll_topic).get("poll_interval_s", 13))

    def _ensure_soc_drift_backend(current_poll_interval: int) -> None:
        drift_mode_payload = _soc_drift_payload()
        set_mode_and_wait(drift_mode_payload, ("online",))
        set_polling_config(current_poll_interval, "State_of_Charge=ten_sec;")
        last_pub = time.time()
        _wait_for_live_json_change(mqtt, poll_topic, "poll liveness after soc drift setup", timeout_s=25)

        def drift_ready_pred() -> Tuple[bool, str]:
            nonlocal last_pub
            now = time.time()
            if (now - last_pub) >= 5.0:
                set_mode(drift_mode_payload)
                set_polling_config(current_poll_interval, "State_of_Charge=ten_sec;")
                last_pub = now
            cur_poll = _fetch_poll(mqtt, poll_topic)
            cur_stub = _fetch_stub(mqtt, stub_topic)
            cfg = _fetch_config(mqtt, config_topic)
            intervals = cfg.get("entity_intervals", {})
            bucket = str(intervals.get("State_of_Charge", "")) if isinstance(intervals, dict) else ""
            mode = str(cur_poll.get("rs485_stub_mode", ""))
            snapshot_ok = bool(cur_poll.get("ess_snapshot_last_ok", False))
            probe_backoff_ms = int(cur_poll.get("rs485_probe_backoff_ms", 0))
            stub_reads = int(cur_stub.get("stub_reads", 0))
            step = int(cur_stub.get("soc_step_x10_per_snapshot", 0))
            soc_x10 = int(cur_stub.get("soc_x10", 0))
            detail = (
                f"mode={mode!r} bucket={bucket!r} snapshot_ok={snapshot_ok} "
                f"probe_backoff_ms={probe_backoff_ms} stub_reads={stub_reads} soc_step={step} soc_x10={soc_x10}"
            )
            return (
                mode == "online"
                and bucket == "ten_sec"
                and snapshot_ok
                and probe_backoff_ms == 0
                and stub_reads > 0
                and step == 10
                and soc_x10 >= 500
            ), detail

        _assert_eventually(
            "soc drift backend recovered and publishing",
            drift_ready_pred,
            timeout_s=60,
            poll_s=2.0,
        )

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
                _ensure_soc_drift_backend(_soc_drift_poll_interval())
        raise E2EError(f"Timeout waiting for {label} change after drift-mode recovery attempts. Last observed: {last_err}")

    def case_soc_drift_backend_ready() -> None:
        print("[e2e] case: soc drift backend ready")
        _wait_for_inverter_identity()
        _ensure_soc_drift_backend(_soc_drift_poll_interval())

    def case_stub_soc_drift_applies() -> None:
        print("[e2e] case: stub SOC drift applies internally")
        _wait_for_inverter_identity()
        current_poll_interval = _soc_drift_poll_interval()
        _ensure_soc_drift_backend(current_poll_interval)
        before = _fetch_stub(mqtt, stub_topic)
        before_reads = int(before.get("stub_reads", 0))
        before_soc_x10 = int(before.get("soc_x10", 0))

        def drift_pred() -> Tuple[bool, str]:
            cur_poll = _fetch_poll(mqtt, poll_topic)
            cur = _fetch_stub(mqtt, stub_topic)
            cur_mode = str(cur_poll.get("rs485_stub_mode", ""))
            cur_reads = int(cur.get("stub_reads", 0))
            cur_soc_x10 = int(cur.get("soc_x10", 0))
            if cur_mode != "online" or cur_reads < before_reads or cur_soc_x10 < before_soc_x10:
                raise E2EError(
                    "stub SOC drift state reset while waiting: "
                    f"mode={cur_mode!r} stub_reads={cur_reads} soc_x10={cur_soc_x10} "
                    f"before_reads={before_reads} before_soc_x10={before_soc_x10}"
                )
            detail = (
                f"mode={cur_mode!r} stub_reads={cur_reads} soc_x10={cur_soc_x10} "
                f"before_reads={before_reads} before_soc_x10={before_soc_x10}"
            )
            return (cur_reads > before_reads and cur_soc_x10 > before_soc_x10), detail

        _assert_eventually("stub SOC drifts internally", drift_pred, timeout_s=45, poll_s=3.0)

    def case_soc_publish_respects_bucket() -> None:
        print("[e2e] case: State_of_Charge publish respects bucket")
        ha_unique = _wait_for_inverter_identity()
        current_poll_interval = _soc_drift_poll_interval()
        _ensure_soc_drift_backend(current_poll_interval)
        soc_topic = _state_topic(ha_unique, "State_of_Charge")
        mqtt.subscribe(soc_topic, force=True)
        v1 = _fetch_latest_text(mqtt, soc_topic, label="soc")
        v2 = _wait_for_soc_change(ha_unique, v1, "soc_publish", timeout_s=45)
        if v2 == v1:
            raise E2EError(f"State_of_Charge did not republish after bucket enable: v1={v1!r} v2={v2!r}")

    def case_soc_drift_e2e() -> None:
        print("[e2e] case: soc drift end-to-end")
        ha_unique = _wait_for_inverter_identity()
        current_poll_interval = _soc_drift_poll_interval()
        _ensure_soc_drift_backend(current_poll_interval)
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

    def case_polling_config_persistence() -> None:
        print("[e2e] case: polling config mapping + poll_interval_s takes effect")
        config_before = _fetch_config(mqtt, config_topic)
        intervals = config_before.get("entity_intervals", {})
        if not isinstance(intervals, dict):
            raise E2EError(f"config entity_intervals missing or invalid: {config_before}")

        target = "State_of_Charge"
        old_bucket = str(intervals.get(target, ""))
        if not old_bucket:
            raise E2EError(f"config missing {target} in entity_intervals")

        # Keep this bounded to buckets that can execute within this test window.
        new_bucket = "ten_sec" if old_bucket != "ten_sec" else "user"
        if new_bucket == old_bucket:
            raise E2EError(f"unable to select alternate bucket for {target} (bucket={old_bucket})")

        poll_interval = 13
        set_polling_config(poll_interval, f"{target}={new_bucket};")

        def applied_pred() -> Tuple[bool, str]:
            cur = _fetch_poll(mqtt, poll_topic)
            cfg = _fetch_config(mqtt, config_topic)
            cur_interval = int(cur.get("poll_interval_s", 0))
            intervals_cur = cfg.get("entity_intervals", {})
            mapped = str(intervals_cur.get(target, "")) if isinstance(intervals_cur, dict) else ""
            detail = f"poll_interval_s={cur_interval} mapped={mapped!r}"
            if cur_interval != poll_interval:
                return False, detail
            if mapped != new_bucket:
                return False, detail
            return True, detail

        _assert_eventually("polling config applied", applied_pred, timeout_s=60, poll_s=3.0)

    def case_portal_polling_ui() -> None:
        print("[e2e] case: portal UI updates polling schedule (WiFiManager, paged)")
        base = _resolve_device_http_base(mqtt, device_root)

        reboot_wifi_path = _discover_reboot_wifi_path_from_code()
        if not reboot_wifi_path:
            raise E2EError("Could not discover /reboot/wifi endpoint from firmware source")

        reboot_url = base + reboot_wifi_path
        print(f"[e2e] rebooting into wifi portal via {reboot_url}")
        _http_post_simple(reboot_url, timeout_s=10)

        # Portal is STA-only and should come back on the same IP.
        _wait_for_http_ok(base + "/", timeout_s=40)

        polling_path, html = _load_polling_page_via_menu(base)
        if not polling_path.startswith("/config/polling"):
            raise E2EError(f"unexpected polling menu path: {polling_path!r}")
        if "poll_interval_s" not in html or "/config/polling/save" not in html:
            raise E2EError("portal polling page HTML missing expected form fields")
        if "class=\"wrap\"" not in html or "<h1>Setup</h1>" not in html:
            raise E2EError("portal polling page missing shared portal wrapper/header style")
        if "Unsaved polling changes will be lost. Continue?" not in html:
            raise E2EError("portal polling page missing unsaved-changes warning text")

        # Find which row index corresponds to a known stable mqttName.
        target = "State_of_Charge"
        target_page, total_pages, row, initial_bucket = _locate_entity_on_polling_pages(base, html, target)
        desired_bucket = "user" if initial_bucket != "user" else "ten_sec"

        # Navigate away without save and ensure no persistence happened.
        nav_page = 0 if target_page != 0 else (1 if total_pages > 1 else 0)
        _http_request_full("GET", f"{base}/config/polling?page={nav_page}", headers={}, body=b"", timeout_s=20)
        html_back_text = _load_polling_page(base, target_page)
        _, after_nav_bucket = _extract_entity_row_and_selected_bucket(html_back_text, target)
        if after_nav_bucket != initial_bucket:
            raise E2EError(f"unsaved bucket changed after page navigation: before={initial_bucket!r} after={after_nav_bucket!r}")

        # Change poll_interval_s and move target to an alternate bucket.
        fields = {
            "page": str(target_page),
            "poll_interval_s": "13",
            f"b{row}": desired_bucket,
        }
        save_url = base + "/config/polling/save"
        print(f"[e2e] POST {save_url} fields: page={target_page} poll_interval_s=13 b{row}={desired_bucket}")
        save_status = _http_post_form(save_url, fields, timeout_s=20)
        if save_status not in (200, 302):
            raise E2EError(f"polling save failed status={save_status}")

        # Save should not reboot out of portal; page should remain reachable after save.
        _wait_for_http_ok(f"{base}/config/polling?page={target_page}", timeout_s=10)
        _sleep_with_mqtt(mqtt, 3)
        _wait_for_http_ok(f"{base}/config/polling?page={target_page}", timeout_s=10)
        html_saved_text = _load_polling_page(base, target_page)
        interval_match_saved = re.search(r'name="poll_interval_s"[^>]*value="(\d+)"', html_saved_text)
        if not interval_match_saved or int(interval_match_saved.group(1)) != 13:
            raise E2EError("poll_interval_s UI value not updated immediately after save")
        _, saved_bucket = _extract_entity_row_and_selected_bucket(html_saved_text, target)
        if saved_bucket != desired_bucket:
            raise E2EError(f"{target} bucket UI value not updated immediately after save (expected {desired_bucket!r}, got {saved_bucket!r})")

        reboot_normal_path = _discover_reboot_normal_path_from_code()
        if not reboot_normal_path:
            raise E2EError("Could not discover /reboot/normal endpoint from firmware source")

        reboot_normal_url = base + reboot_normal_path
        print(f"[e2e] rebooting to normal via {reboot_normal_url}")
        reboot_status, reboot_body = _http_request("POST", reboot_normal_url, headers={}, body=b"", timeout_s=20)
        if reboot_status != 200:
            raise E2EError(f"/reboot/normal returned unexpected status={reboot_status}")
        if reboot_body:
            reboot_html = reboot_body.decode("utf-8", errors="replace")
            if "Rebooting into normal runtime" not in reboot_html:
                raise E2EError("/reboot/normal response missing reboot-to-normal heading")
            if "Alpha2MQTT Control" not in reboot_html:
                raise E2EError("/reboot/normal response missing runtime probe marker")

        # Wait for MQTT status to resume after reboot to NORMAL.
        _assert_eventually(
            "device publishes status/poll after explicit reboot to normal",
            lambda: (True, "ok") if _fetch_poll(mqtt, poll_topic).get("poll_interval_s") else (False, "waiting"),
            timeout_s=60,
            poll_s=3.0,
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

        poll = _fetch_poll(mqtt, poll_topic)
        if int(poll.get("poll_interval_s", 0)) != 13:
            raise E2EError(f"poll_interval_s did not persist via portal UI: {poll.get('poll_interval_s')}")

        cfg = _fetch_config(mqtt, config_topic)
        intervals = cfg.get("entity_intervals", {})
        if not isinstance(intervals, dict):
            raise E2EError(f"config entity_intervals missing: {cfg}")
        if str(intervals.get(target, "")) != desired_bucket:
            raise E2EError(f"{target} bucket not updated via portal UI (expected {desired_bucket}): {intervals.get(target)!r}")

        # Re-enter portal and verify values are restored in the UI after reboot.
        print(f"[e2e] rebooting into wifi portal via {reboot_url} (persistence check)")
        _http_post_simple(reboot_url, timeout_s=10)
        _wait_for_http_ok(base + "/", timeout_s=40)

        polling_path2, html2 = _load_polling_page_via_menu(base)
        if not polling_path2.startswith("/config/polling"):
            raise E2EError(f"unexpected polling menu path after reboot: {polling_path2!r}")

        interval_match = re.search(r'name="poll_interval_s"[^>]*value="(\d+)"', html2)
        if not interval_match:
            raise E2EError("polling page missing poll_interval_s input after reboot")
        if int(interval_match.group(1)) != 13:
            raise E2EError(f"poll_interval_s UI value not restored after reboot: {interval_match.group(1)}")

        restored_page, _, _, restored_bucket = _locate_entity_on_polling_pages(base, html2, target)
        if restored_page != target_page:
            raise E2EError(f"{target} moved to a different portal page across reboot: before={target_page} after={restored_page}")
        if restored_bucket != desired_bucket:
            raise E2EError(f"{target} bucket UI value not restored after reboot (expected {desired_bucket!r}, got {restored_bucket!r})")

    cases: list[Tuple[str, Callable[[], None]]] = [
        ("two_device_discovery", case_two_device_discovery),
        ("offline", case_offline),
        ("fail_then_recover", case_fail_then_recover),
        ("online", case_online),
        ("scheduler_idle_no_extra_reads", case_scheduler_idle_does_not_add_reads),
        ("bucket_snapshot_skip_only", case_bucket_snapshot_skip_only),
        ("dispatch_write_via_commands", case_dispatch_write_via_commands),
        ("dispatch_write_feedback", case_dispatch_write_feedback_via_register_value),
        ("strict_unknown_snapshot", case_strict_unknown_snapshot_has_no_unknown_reads),
        ("strict_unknown_register_reads", case_strict_unknown_register_reads),
        ("fail_specific_snapshot_reg", case_fail_specific_snapshot_register_and_type),
        ("fail_every_n", case_fail_every_n_snapshot_attempts),
        ("latency", case_latency_does_not_break_status),
        ("flapping", case_flapping_online_offline),
        ("probe_delayed", case_probe_delayed_online),
        ("fail_writes_only", case_fail_writes_only_dispatch_write_fails),
        ("fail_for_ms", case_fail_for_ms_then_recover),
        ("soc_drift_backend_ready", case_soc_drift_backend_ready),
        ("stub_soc_drift_applies", case_stub_soc_drift_applies),
        ("soc_publish_respects_bucket", case_soc_publish_respects_bucket),
        ("soc_drift_e2e", case_soc_drift_e2e),
        ("polling_config", case_polling_config_persistence),
        ("portal_polling_ui", case_portal_polling_ui),
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
