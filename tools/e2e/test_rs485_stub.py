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
and asserts on keys emitted by the firmware:
  rs485_backend, rs485_stub_mode, rs485_stub_fail_remaining,
  ess_snapshot_attempts, ess_snapshot_last_ok,
  dispatch_last_run_ms, dispatch_last_skip_reason.

Usage
  python3 tools/e2e/test_rs485_stub.py
  python3 tools/e2e/test_rs485_stub.py --ota
  python3 tools/e2e/test_rs485_stub.py --ensure-stub
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
PROGRESS_INTERVAL_S = 10.0


def _log(msg: str) -> None:
    if VERBOSE:
        print(f"[e2e] {msg}")


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
        return int(m.group(1)), resp_body
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

def _fetch_boot(mqtt: MqttClient, boot_topic: str) -> dict[str, Any]:
    # Boot is retained, but if the broker has restarted (no retained state), we may need to
    # wait for a device reboot to republish it.
    return _fetch_latest_json(mqtt, boot_topic, label="boot", timeout_s=60)

def _fetch_stub(mqtt: MqttClient, topic: str) -> dict[str, Any]:
    return _fetch_latest_json(mqtt, topic, label="stub")

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
    return True, "ok"

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
    def pred() -> Tuple[bool, str]:
        ok2, det2 = _device_is_latest_stub(mqtt, device_root, expected_ts, status_poll_suffix)
        return ok2, det2

    _assert_eventually("device reports latest stub after OTA", pred, timeout_s=120, poll_s=5.0)


def main() -> int:
    _load_env_file_defaults(_default_env_file())
    ap = argparse.ArgumentParser()
    ap.add_argument("--ota", action="store_true", help="Perform one OTA upload before running tests (requires DEVICE_HTTP_BASE).")
    ap.add_argument("--ensure-stub", action="store_true", help="Ensure the device is running the latest stub firmware (OTA if needed), then run tests.")
    ap.add_argument("--verbose", action="store_true", help="Verbose logging (MQTT rx/tx and filtering).")
    args = ap.parse_args()
    global VERBOSE
    VERBOSE = args.verbose

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

    if args.ota or args.ensure_stub:
        http_base = _require_env("DEVICE_HTTP_BASE")
        firmware_path = Path(os.environ.get("FIRMWARE_BIN") or _latest_stub_firmware_path())
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
    for k in (
        "sched_10s_last_run_ms",
        "sched_1m_last_run_ms",
        "sched_5m_last_run_ms",
        "sched_1h_last_run_ms",
        "sched_1d_last_run_ms",
        "sched_user_last_run_ms",
        "poll_interval_s",
    ):
        if k not in first:
            raise E2EError(f"Missing expected scheduler observability field in /status/poll: {k} (keys={sorted(first.keys())})")

    def set_mode(payload: str) -> None:
        mqtt.publish(control_topic, payload, retain=False)

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

    def set_polling_config(poll_interval_s: int, bucket_map: str) -> None:
        # Config-set parser expects string values, not numbers.
        payload = json.dumps({
            "poll_interval_s": str(poll_interval_s),
            "bucket_map": bucket_map,
        })
        mqtt.publish(f"{device_root}/config/set", payload, retain=False)

    def _wait_for_inverter_identity() -> str:
        def pred() -> Tuple[bool, str]:
            core = _fetch_status_core(mqtt, status_core_topic)
            ha_unique = str(core.get("ha_unique_id", ""))
            detail = f"ha_unique_id={ha_unique!r}"
            if ha_unique and ha_unique != "A2M-UNKNOWN":
                return True, detail
            return False, detail
        _assert_eventually("inverter identity available (ha_unique_id != A2M-UNKNOWN)", pred, timeout_s=60, poll_s=3.0)
        core = _fetch_status_core(mqtt, status_core_topic)
        return str(core.get("ha_unique_id", "A2M-UNKNOWN"))

    def _state_topic(ha_unique: str, name: str) -> str:
        return f"{device_root}/{ha_unique}/{name}/state"

    def _command_topic(ha_unique: str, name: str) -> str:
        return f"{device_root}/{ha_unique}/{name}/command"

    def case_offline() -> None:
        print("[e2e] case: stub offline")
        before = _fetch_poll(mqtt, poll_topic)
        before_attempts = int(before.get("ess_snapshot_attempts", 0))
        set_mode('{"mode":"offline"}')

        def pred() -> Tuple[bool, str]:
            cur = _fetch_poll(mqtt, poll_topic)
            attempts = int(cur.get("ess_snapshot_attempts", 0))
            ok = bool(cur.get("ess_snapshot_last_ok", False))
            skip = str(cur.get("dispatch_last_skip_reason", ""))
            detail = f"attempts={attempts} ok={ok} skip={skip} mode={cur.get('rs485_stub_mode')}"
            return (attempts > before_attempts and (not ok) and skip == "ess_snapshot_failed"), detail

        _assert_eventually("offline causes snapshot fail + dispatch suppressed", pred, timeout_s=45)

    def case_fail_then_recover() -> None:
        print("[e2e] case: fail then recover (n=2)")
        set_mode('{"mode":"fail","fail_n":2}')
        # Expect at least one fail then eventually success.
        _assert_eventually(
            "fail_then_recover mode applied",
            lambda: (
                str(_fetch_poll(mqtt, poll_topic).get("rs485_stub_mode", "")) in ("fail_then_recover", "fail"),
                f"mode={_fetch_poll(mqtt, poll_topic).get('rs485_stub_mode')}",
            ),
            timeout_s=30,
            poll_s=2.0,
        )

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
        set_mode('{"mode":"online"}')
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
        set_mode('{"mode":"online"}')
        net_topic = f"{device_root}/status/net"
        mqtt.subscribe(net_topic)
        net1 = _fetch_latest_json(mqtt, net_topic, label="net")
        u1 = int(net1.get("uptime_s", 0))

        # Force snapshot failure and confirm:
        # - net uptime continues (non-snapshot publish)
        # - dispatch is suppressed
        set_mode('{"mode":"offline"}')
        _assert_eventually(
            "offline triggers dispatch suppression",
            lambda: (
                str(_fetch_poll(mqtt, poll_topic).get("dispatch_last_skip_reason", "")) == "ess_snapshot_failed",
                f"skip={_fetch_poll(mqtt, poll_topic).get('dispatch_last_skip_reason')}",
            ),
            timeout_s=45,
            poll_s=3.0,
        )

        net2 = _fetch_latest_json(mqtt, net_topic, label="net")
        u2 = int(net2.get("uptime_s", 0))
        if u2 <= u1:
            # Wait one more cycle to avoid flaking on same-sample reads.
            net3 = _fetch_latest_json(mqtt, net_topic, label="net")
            u3 = int(net3.get("uptime_s", 0))
            if u3 <= u2:
                raise E2EError(f"Expected uptime_s to increase while snapshot is failing, got {u1}->{u2}->{u3}")

        # Recover and confirm SOC resumes.
        set_mode('{"mode":"online"}')
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
        set_mode('{"mode":"online","soc_pct":50,"battery_power_w":0,"grid_power_w":0,"pv_ct_power_w":0}')

        ha_unique = _wait_for_inverter_identity()
        base = f"{device_root}/{ha_unique}"

        # Ensure we can assert on the actual dispatch start register value.
        reg_dispatch_start = _discover_register_value("REG_DISPATCH_RW_DISPATCH_START")
        dispatch_start_start = _discover_define_value("DISPATCH_START_START")

        # Also validate write -> read feedback via the existing Register_Number/Register_Value entities.
        # Re-enable and speed up Register_Value in case prior runs changed intervals.
        set_intervals({"Register_Value": "freqTenSec"})
        regnum_topic = _state_topic(ha_unique, "Register_Number")
        val_topic = _state_topic(ha_unique, "Register_Value")
        mqtt.subscribe(regnum_topic, force=True)
        mqtt.subscribe(val_topic, force=True)

        # Point Register_Value at the dispatch-start register, and wait until the firmware confirms it.
        def ensure_regnum_set() -> None:
            deadline = time.time() + 30
            last_seen = ""
            while time.time() < deadline:
                mqtt.publish(f"{base}/Register_Number/command", str(reg_dispatch_start), retain=False)
                try:
                    last_seen = _fetch_latest_text(mqtt, regnum_topic, label="reg_number_state")
                    if last_seen.strip() == str(reg_dispatch_start):
                        return
                except E2EError:
                    pass
                time.sleep(1.0)
            raise E2EError(f"Register_Number did not update to {reg_dispatch_start}; last_seen={last_seen!r}")

        ensure_regnum_set()
        before_val = _fetch_latest_text(mqtt, val_topic, label="dispatch_start_before")

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
        set_mode('{"mode":"online","dispatch_start":0,"dispatch_mode":65535,"dispatch_active_power":0,"dispatch_soc":0}')

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

        # Now assert that subsequent reads/publishes reflect the effect of the write.
        deadline = time.time() + 60
        last_val = before_val
        while time.time() < deadline:
            last_val = _fetch_latest_text(mqtt, val_topic, label="dispatch_start_after")
            normalized = last_val.strip().lower()
            if normalized in ("start", "started") or last_val.strip() == str(dispatch_start_start):
                return
            time.sleep(1.0)
        raise E2EError(
            f"Register_Value did not reflect dispatch_start change; expected 'Start' (or {dispatch_start_start}) "
            f"but got last={last_val!r} initial={before_val!r}"
        )

    def case_dispatch_write_feedback_via_register_value() -> None:
        print("[e2e] case: dispatch write feedback (Register_Value reflects new dispatch state)")
        # Coverage is now handled in case_dispatch_write_via_commands() to avoid duplicated, flaky sequencing.
        return

    def case_strict_unknown_snapshot_has_no_unknown_reads() -> None:
        print("[e2e] case: strict unknown-register protection (snapshot path)")
        _wait_for_inverter_identity()
        # Strict mode should be safe for ESS snapshot: the stub must implement all snapshot registers.
        # To keep this test scoped to the snapshot path (and not other entities that perform Modbus reads),
        # temporarily disable known non-snapshot entities that read Modbus directly.
        set_intervals(
            {
                "Inverter_version": "freqDisabled",
                "Inverter_SN": "freqDisabled",
                "EMS_version": "freqDisabled",
                "EMS_SN": "freqDisabled",
                "ESS_Energy_Charge": "freqDisabled",
                "ESS_Energy_Discharge": "freqDisabled",
                "Grid_Regulation": "freqDisabled",
                "Grid_Energy_To": "freqDisabled",
                "Grid_Energy_From": "freqDisabled",
                "Battery_Capacity": "freqDisabled",
                "Inverter_Temp": "freqDisabled",
                "Battery_Temp": "freqDisabled",
                "Battery_Faults": "freqDisabled",
                "Battery_Warnings": "freqDisabled",
                "Inverter_Faults": "freqDisabled",
                "Inverter_Warnings": "freqDisabled",
                "Solar_Energy": "freqDisabled",
                "Frequency": "freqDisabled",
                "System_Faults": "freqDisabled",
                "Register_Value": "freqDisabled",
            }
        )

        set_mode('{"mode":"online","strict_unknown":1,"strict":1}')
        # Wait until strict_unknown is actually applied (status is asynchronous).
        _assert_eventually(
            "strict_unknown applied",
            lambda: (
                bool(_fetch_stub(mqtt, stub_topic).get("strict_unknown", False)),
                "waiting",
            ),
            timeout_s=30,
            poll_s=2.0,
        )

        # Baseline counters after strict mode is applied (firmware may reset counters on apply).
        baseline_stub = _fetch_stub(mqtt, stub_topic)
        baseline_unknown = int(baseline_stub.get("stub_unknown_reads", 0))
        baseline_attempts = int(_fetch_poll(mqtt, poll_topic).get("ess_snapshot_attempts", 0))

        def pred() -> Tuple[bool, str]:
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
            # Wait until at least one new snapshot attempt occurred and succeeded, and unknown reads did not increase.
            return (attempts > baseline_attempts and ok and unknown == baseline_unknown), detail

        _assert_eventually("strict_unknown keeps snapshot OK with zero unknown-register reads", pred, timeout_s=70, poll_s=5.0)

    def case_scheduler_idle_does_not_add_reads() -> None:
        print("[e2e] case: scheduler selectivity (no extra reads during idle window)")
        set_mode('{"mode":"online"}')
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

        # Prior cases may temporarily disable Register_Value to keep snapshot tests scoped; re-enable it here
        # and speed it up so we don't wait a full minute.
        set_intervals({"Register_Value": "freqTenSec"})

        # Use a handled register that is not virtualized by the stub (so the stub sees it as unknown).
        reg = _discover_register_value("REG_INVERTER_HOME_R_INVERTER_TEMP")

        # Make sure Register_Number is actually applied before we assert on Register_Value output.
        regnum_topic = _state_topic(ha_unique, "Register_Number")
        mqtt.subscribe(regnum_topic, force=True)

        val_topic = _state_topic(ha_unique, "Register_Value")
        mqtt.subscribe(val_topic, force=True)

        def ensure_regnum_set() -> None:
            deadline = time.time() + 30
            last_seen = ""
            while time.time() < deadline:
                mqtt.publish(_command_topic(ha_unique, "Register_Number"), str(reg), retain=False)
                try:
                    last_seen = _fetch_latest_text(mqtt, regnum_topic, label="reg_number_state")
                    if last_seen.strip() == str(reg):
                        return
                except E2EError:
                    pass
                time.sleep(1.0)
            raise E2EError(f"Register_Number did not update to {reg}; last_seen={last_seen!r}")

        ensure_regnum_set()

        before_stub = _fetch_stub(mqtt, stub_topic)
        before_unknown = int(before_stub.get("stub_unknown_reads", 0))

        set_mode('{"mode":"online","strict_unknown":0}')
        value_loose = _fetch_latest_text(mqtt, val_topic, label="reg_value")
        after_loose = _fetch_stub(mqtt, stub_topic)

        unknown_after = int(after_loose.get("stub_unknown_reads", 0))
        strict_after = bool(after_loose.get("strict_unknown", False))
        if value_loose in ("Slave Error", "Nothing read") or unknown_after <= before_unknown or strict_after:
            raise E2EError(
                f"Expected loose unknown read (not Slave Error) and unknown_reads to increase. "
                f"value={value_loose!r} unknown_before={before_unknown} unknown_after={unknown_after} strict={strict_after}"
            )

        set_mode('{"mode":"online","strict_unknown":1}')
        value_strict = _wait_for_topic_change(mqtt, val_topic, value_loose, timeout_s=30, label="reg_value")
        if value_strict != "Slave Error":
            raise E2EError(f"Expected Slave Error in strict mode but got: {value_strict!r}")
        # `status/stub` is periodic/retained telemetry; allow time for it to reflect the last fail.
        deadline = time.time() + 30
        last = {}
        while time.time() < deadline:
            last = _fetch_stub(mqtt, stub_topic)
            if bool(last.get("strict_unknown", False)) and str(last.get("last_fail_type", "")) == "slave_error":
                return
            time.sleep(1.0)
        raise E2EError(f"Expected strict_unknown=true and last_fail_type=slave_error, got: {last}")

    def case_fail_specific_snapshot_register_and_type() -> None:
        print("[e2e] case: fail specific snapshot register + fail type reporting")
        reg_soc = _discover_register_value("REG_BATTERY_HOME_R_SOC")
        set_mode(f'{{"mode":"online","fail_n":0,"reg":{reg_soc},"fail_type":1}}')

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
        set_mode('{"mode":"online"}')
        _assert_eventually("snapshot recovers after clearing failure", lambda: (bool(_fetch_poll(mqtt, poll_topic).get("ess_snapshot_last_ok", False)), "waiting"), timeout_s=60, poll_s=3.0)

    def case_fail_every_n_snapshot_attempts() -> None:
        print("[e2e] case: fail every N snapshot attempts (N=2)")
        # Use slave_error (not no_response) so RS485 stays "online" and we observe clean fail/ok alternation.
        set_mode('{"mode":"online","fail_every_n":2,"fail_type":1}')

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
        set_mode('{"mode":"online","latency_ms":200}')
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
        set_mode('{"mode":"flap","flap_online_ms":3500,"flap_offline_ms":3500}')

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
        set_mode('{"mode":"probe_delayed","probe_success_after_n":3}')
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
        set_mode(
            f'{{"mode":"online","reg":{reg_dispatch_start},"fail_writes":1,"fail_reads":0,"fail_type":1,'
            '"soc_pct":50,"battery_power_w":0,"grid_power_w":0,"pv_ct_power_w":0,"dispatch_start":0,"dispatch_mode":0,"dispatch_soc":0}}'
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
        set_mode('{"mode":"online","fail_for_ms":5000}')
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

    def case_soc_drift_over_time() -> None:
        print("[e2e] case: virtual SOC drifts per snapshot attempt")
        set_mode('{"mode":"online","soc_pct":50,"soc_step_x10_per_snapshot":10}')
        ha_unique = _wait_for_inverter_identity()
        set_intervals({"State_of_Charge": "freqTenSec"})
        soc_topic = _state_topic(ha_unique, "State_of_Charge")
        mqtt.subscribe(soc_topic)

        v1 = _fetch_latest_text(mqtt, soc_topic, label="soc")
        v2 = _wait_for_topic_change(mqtt, soc_topic, v1, timeout_s=45, label="soc")
        try:
            f1 = float(v1)
            f2 = float(v2)
        except ValueError:
            raise E2EError(f"Unexpected SOC payloads (not floats): v1={v1!r} v2={v2!r}")
        if f2 <= f1:
            v3 = _wait_for_topic_change(mqtt, soc_topic, v2, timeout_s=45, label="soc")
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

        new_bucket = "user" if old_bucket != "user" else "one_hour"
        if new_bucket == old_bucket:
            raise E2EError(f"unable to select alternate bucket for {target} (bucket={old_bucket})")

        poll_before = _fetch_poll(mqtt, poll_topic)
        bucket_to_count = {
            "ten_sec": "sched_10s_count",
            "one_min": "sched_1m_count",
            "five_min": "sched_5m_count",
            "one_hour": "sched_1h_count",
            "one_day": "sched_1d_count",
            "user": "sched_user_count",
        }
        old_field = bucket_to_count.get(old_bucket)
        new_field = bucket_to_count.get(new_bucket)
        old_count = int(poll_before.get(old_field, 0)) if old_field else None
        new_count = int(poll_before.get(new_field, 0)) if new_field else None

        poll_interval = 13
        set_polling_config(poll_interval, f"{target}={new_bucket};")

        def poll_pred() -> Tuple[bool, str]:
            cur = _fetch_poll(mqtt, poll_topic)
            cur_interval = int(cur.get("poll_interval_s", 0))
            detail = f"poll_interval_s={cur_interval}"
            if cur_interval != poll_interval:
                return False, detail
            if new_field:
                cur_new = int(cur.get(new_field, 0))
                detail += f" new={cur_new} expected>={new_count + 1}"
                if cur_new < new_count + 1:
                    return False, detail
            return True, detail

        _assert_eventually("bucket counts updated", poll_pred, timeout_s=60, poll_s=3.0)

    cases: list[Tuple[str, Callable[[], None]]] = [
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
        ("soc_drift", case_soc_drift_over_time),
        ("polling_config", case_polling_config_persistence),
    ]

    for name, fn in cases:
        _announce(f"running case: {name}")
        _mqtt_retry(mqtt, f"case:{name}", lambda f=fn: f())

    print("[e2e] OK")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except E2EError as e:
        print(f"[e2e] FAIL: {e}", file=sys.stderr)
        raise SystemExit(2)
