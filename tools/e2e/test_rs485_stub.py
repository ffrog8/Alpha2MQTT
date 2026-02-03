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


def _log(msg: str) -> None:
    if VERBOSE:
        print(f"[e2e] {msg}")


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

def _firmware_main_cpp() -> Path:
    # Repo layout: <repo>/Alpha2MQTT/src/main.cpp
    return _repo_root() / "Alpha2MQTT" / "src" / "main.cpp"

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

def _discover_reboot_wifi_path_from_code() -> Optional[str]:
    data = _firmware_main_cpp().read_text(encoding="utf-8", errors="replace")
    # NORMAL-mode HTTP control plane route.
    if 'httpServer.on("/reboot/wifi"' in data:
        return "/reboot/wifi"
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
        _log(f"mqtt publish: topic={topic} retain={int(retain)} bytes={len(payload)}")

    def subscribe(self, topic_filter: str) -> None:
        if not self.sock:
            raise E2EError("MQTT not connected")
        pid = self._next_packet_id()
        _log(f"mqtt subscribe: filter={topic_filter} pid={pid}")
        vh = pid.to_bytes(2, "big")
        payload = _encode_utf8(topic_filter) + bytes([0x00])  # QoS 0
        body = vh + payload
        self.sock.sendall(bytes([0x82]) + _encode_varint(len(body)) + body)

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
            return

        raise E2EError("Timeout waiting for SUBACK")

    def wait_for_publish(self, timeout_s: int) -> Tuple[str, str]:
        if self._pending_publishes:
            return self._pending_publishes.pop(0)
        if not self.sock:
            raise E2EError("MQTT not connected")
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            pkt_type, _flags, data = self._read_packet(timeout_s=5)
            if pkt_type == 3:  # PUBLISH
                topic, payload = self._decode_publish(data)
                _log(f"mqtt rx: topic={topic} bytes={len(payload)}")
                return topic, payload
            # Ignore other packet types.
        raise E2EError("Timeout waiting for MQTT publish")

    def _read_packet(self, timeout_s: int) -> Tuple[int, int, bytes]:
        if not self.sock:
            raise E2EError("MQTT not connected")
        old_timeout = self.sock.gettimeout()
        self.sock.settimeout(timeout_s)
        try:
            b1 = _read_exact(self.sock, 1)[0]
            pkt_type = b1 >> 4
            flags = b1 & 0x0F
            rl = _read_varint(self.sock)
            data = _read_exact(self.sock, rl)
            return pkt_type, flags, data
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
    while time.time() < deadline:
        ok, detail = fn()
        last_detail = detail
        if ok:
            return
        time.sleep(poll_s)
    raise E2EError(f"Timeout waiting for: {name}. Last observed: {last_detail}")


def _fetch_poll(mqtt: MqttClient, topic: str) -> dict[str, Any]:
    mqtt.subscribe(topic)
    deadline = time.time() + 15
    last = ""
    while time.time() < deadline:
        got_topic, payload = mqtt.wait_for_publish(timeout_s=10)
        last = f"topic={got_topic} payload={payload!r}"
        if got_topic != topic:
            _log(f"poll wait: ignoring other topic={got_topic}")
            continue
        try:
            _log(f"poll wait: got bytes={len(payload)}")
            return _parse_json(payload)
        except Exception as e:
            raise E2EError(f"Status payload was not valid JSON on {topic}: {payload!r} ({e})")
    raise E2EError(f"Timeout waiting for status JSON on {topic}. Last observed: {last}")

def _fetch_boot(mqtt: MqttClient, boot_topic: str) -> dict[str, Any]:
    mqtt.subscribe(boot_topic)
    deadline = time.time() + 15
    last = ""
    while time.time() < deadline:
        got_topic, payload = mqtt.wait_for_publish(timeout_s=10)
        last = f"topic={got_topic} payload={payload!r}"
        if got_topic != boot_topic:
            _log(f"boot wait: ignoring other topic={got_topic}")
            continue
        try:
            _log(f"boot wait: got bytes={len(payload)}")
            return _parse_json(payload)
        except Exception as e:
            raise E2EError(f"Boot payload was not valid JSON on {boot_topic}: {payload!r} ({e})")
    raise E2EError(f"Timeout waiting for boot JSON on {boot_topic}. Last observed: {last}")

def _device_is_latest_stub(
    mqtt: MqttClient,
    device_root: str,
    expected_build_ts_ms: int,
    status_poll_suffix: str,
) -> Tuple[bool, str]:
    boot = _fetch_boot(mqtt, f"{device_root}/boot")
    fw_ts = boot.get("fw_build_ts_ms")
    if not isinstance(fw_ts, int):
        return False, f"boot.fw_build_ts_ms missing/invalid (keys={sorted(boot.keys())})"
    if fw_ts != expected_build_ts_ms:
        return False, f"fw_build_ts_ms={fw_ts} expected={expected_build_ts_ms}"

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
    time.sleep(8)

    print(f"[e2e] POST {upload_path} (upload firmware: {firmware_path.name})")
    status = _http_post_multipart(upload_url, field_name=field_name, file_path=firmware_path, timeout_s=120)
    print(f"[e2e] upload HTTP status={status}")
    if status < 200 or status >= 400:
        raise E2EError(f"OTA upload failed (HTTP {status})")

    print("[e2e] waiting for device to reboot and report new fw_build_ts_ms over MQTT...")
    def pred() -> Tuple[bool, str]:
        ok2, det2 = _device_is_latest_stub(mqtt, device_root, expected_ts, status_poll_suffix)
        return ok2, det2

    _assert_eventually("device reports latest stub after OTA", pred, timeout_s=120, poll_s=5.0)


def main() -> int:
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
    mqtt = MqttClient(host=host, port=port, user=user, password=password)
    mqtt.connect()

    control_suffix = _discover_control_suffix_from_code()
    status_poll_suffix = _discover_status_poll_suffix_from_code()
    device_root = _discover_device_topic(mqtt, status_poll_suffix)

    poll_topic = f"{device_root}{status_poll_suffix}"
    control_topic = f"{device_root}{control_suffix}"

    print(f"[e2e] device_root={device_root}")
    print(f"[e2e] poll_topic={poll_topic}")
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

    def set_mode(payload: str) -> None:
        mqtt.publish(control_topic, payload, retain=False)

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

    case_offline()
    case_fail_then_recover()
    case_online()

    print("[e2e] OK")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except E2EError as e:
        print(f"[e2e] FAIL: {e}", file=sys.stderr)
        raise SystemExit(2)
