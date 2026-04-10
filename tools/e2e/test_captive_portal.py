#!/usr/bin/env python3
"""
Real-device captive-portal verification.

This script starts from a true virgin device state by fully erasing flash over
serial, flashing the latest real firmware, onboarding WiFi + MQTT through the
portal via a remote Pi, and then verifying the normal-mode WiFi portal can save
polling bucket changes.

It is intentionally separate from the RS485 stub E2E suite because it depends
on extra lab infrastructure:
- USB serial access to the target device
- SSH access to a Pi with LAN + WiFi
- real WiFi credentials
"""

from __future__ import annotations

import argparse
import base64
import html as html_lib
import ipaddress
import json
import os
import re
import shlex
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any, Callable, Optional, Tuple


class PortalTestError(Exception):
    pass


ESP8266_SERIAL_BAUD = 115200


VERBOSE = False
TRACE_HTTP = False
TRACE_SSH = False
CASE_ORDER: tuple[str, ...] = (
    "wifi_only",
    "ap_bad_wifi_retains_saved_ssid",
    "wifi_plus_mqtt",
    "normal_to_wifi_portal_set_mqtt",
    "normal_to_wifi_portal_bad_wifi_recovery_cycle",
)


def _announce(msg: str) -> None:
    print(f"[portal-e2e] {msg}", flush=True)


def _log(msg: str) -> None:
    if VERBOSE:
        print(f"[portal-e2e] {msg}")


def _ssh_log(msg: str) -> None:
    if TRACE_SSH:
        print(f"[portal-e2e][ssh] {msg}")


def _http_log(msg: str) -> None:
    if TRACE_HTTP:
        print(f"[portal-e2e][http] {msg}")


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _default_json_file() -> Path:
    return Path(__file__).resolve().parent / "e2e.local.json"


def _default_env_file() -> Path:
    return Path(__file__).resolve().parent / "e2e.local.env"


def _default_secrets_file() -> Path:
    return _repo_root() / ".secrets"


def _load_json_run_defaults(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {}
    data = json.loads(path.read_text(encoding="utf-8", errors="replace"))
    if not isinstance(data, dict):
        return {}
    run = data.get("run", {})
    return run if isinstance(run, dict) else {}


def _load_json_file_defaults(path: Path) -> None:
    if not path.exists():
        return
    data = json.loads(path.read_text(encoding="utf-8", errors="replace"))
    if not isinstance(data, dict):
        raise PortalTestError(f"Expected JSON object in {path}")
    env_map = data.get("env", data)
    if not isinstance(env_map, dict):
        raise PortalTestError(f"Expected object for env map in {path}")
    for key, value in env_map.items():
        if not isinstance(key, str) or not key or key in os.environ:
            continue
        if isinstance(value, bool):
            os.environ[key] = "1" if value else "0"
        elif isinstance(value, (int, float)):
            os.environ[key] = str(value)
        elif isinstance(value, str):
            os.environ[key] = value


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


def _load_env_file_defaults(path: Path) -> None:
    if not path.exists():
        return
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if not key or key in os.environ:
            continue
        os.environ[key] = value


def _shell_value(raw: str) -> str:
    raw = raw.strip()
    if not raw:
        return ""
    lexer = shlex.shlex(raw, posix=True)
    lexer.whitespace_split = True
    lexer.commenters = ""
    parts = list(lexer)
    return parts[0] if parts else ""


def _load_secrets_defaults(path: Path) -> None:
    if not path.exists():
        return
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[len("export "):].strip()
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        if not key or key in os.environ:
            continue
        os.environ[key] = _shell_value(value)


def _require_env(name: str) -> str:
    value = os.environ.get(name, "")
    if not value:
        raise PortalTestError(f"Missing required env var: {name}")
    return value


def _read_mqtt_password() -> str:
    passfile = os.environ.get("MQTT_PASSFILE", "").strip()
    if passfile:
        return Path(passfile).read_text(encoding="utf-8").strip()
    return _require_env("MQTT_PASS").strip()


def _run(cmd: list[str], timeout_s: int, *, input_text: Optional[str] = None, env: Optional[dict[str, str]] = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        input=input_text,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout_s,
        env=env,
    )


def _run_checked(cmd: list[str], timeout_s: int, *, input_text: Optional[str] = None, env: Optional[dict[str, str]] = None, redact: Optional[str] = None) -> str:
    cp = _run(cmd, timeout_s, input_text=input_text, env=env)
    if cp.returncode != 0:
        out = cp.stdout or ""
        if redact:
            out = out.replace(redact, "<redacted>")
        raise PortalTestError(out.strip() or f"Command failed with exit={cp.returncode}")
    return cp.stdout


class MqttClient:
    """Minimal MQTT 3.1.1 client (QoS 0 only)."""

    def __init__(self, host: str, port: int, user: str, password: str, client_id: str = "a2m-portal-e2e"):
        self.host = host
        self.port = port
        self.user = user
        self.password = password
        self.client_id = f"{client_id}-{int(time.time()*1000)}"
        self.sock: Optional[socket.socket] = None
        self._packet_id = 1
        self._pending: list[Tuple[str, str]] = []
        self._subs: set[str] = set()
        self._last_tx = time.time()
        self._last_rx = time.time()

    def connect(self, timeout_s: int = 10) -> None:
        sock = socket.create_connection((self.host, self.port), timeout=timeout_s)
        sock.settimeout(5.0)

        proto = _encode_utf8("MQTT") + bytes([0x04])
        flags = 0x02
        if self.user:
            flags |= 0x80
        if self.password:
            flags |= 0x40
        keepalive = (300).to_bytes(2, "big")

        payload = _encode_utf8(self.client_id)
        if self.user:
            payload += _encode_utf8(self.user)
        if self.password:
            payload += _encode_utf8(self.password)

        vh = proto + bytes([flags]) + keepalive
        body = vh + payload
        sock.sendall(bytes([0x10]) + _encode_varint(len(body)) + body)

        fixed = _read_exact(sock, 1)
        if fixed[0] != 0x20:
            raise PortalTestError(f"Unexpected MQTT CONNACK header: 0x{fixed[0]:02x}")
        rl = _read_varint(sock)
        data = _read_exact(sock, rl)
        if len(data) != 2 or data[1] != 0:
            raise PortalTestError(f"MQTT connect refused rc={data[1] if len(data) > 1 else 'unknown'}")

        self.sock = sock
        now = time.time()
        self._last_tx = now
        self._last_rx = now

    def close(self) -> None:
        if self.sock:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def subscribe(self, topic_filter: str, *, force: bool = False) -> None:
        if topic_filter in self._subs and not force:
            return
        if not self.sock:
            raise PortalTestError("MQTT not connected")
        pid = self._next_packet_id()
        vh = pid.to_bytes(2, "big")
        payload = _encode_utf8(topic_filter) + bytes([0x00])
        body = vh + payload
        self.sock.sendall(bytes([0x82]) + _encode_varint(len(body)) + body)
        self._last_tx = time.time()

        deadline = time.time() + 10
        while time.time() < deadline:
            pkt_type, _flags, data = self._read_packet(timeout_s=5)
            if pkt_type == 3:
                self._pending.append(self._decode_publish(data))
                continue
            if pkt_type != 9:
                continue
            if len(data) < 3 or data[0:2] != pid.to_bytes(2, "big"):
                continue
            if any(rc == 0x80 for rc in data[2:]):
                raise PortalTestError("Subscription refused")
            self._subs.add(topic_filter)
            return
        raise PortalTestError("Timeout waiting for SUBACK")

    def wait_for_publish(self, timeout_s: float) -> Tuple[str, str]:
        if self._pending:
            return self._pending.pop(0)
        if not self.sock:
            raise PortalTestError("MQTT not connected")
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            try:
                pkt_type, _flags, data = self._read_packet(timeout_s=max(0.1, min(5.0, deadline - time.time())))
            except TimeoutError:
                continue
            if pkt_type == 3:
                return self._decode_publish(data)
        raise PortalTestError("Timeout waiting for MQTT publish")

    def publish(self, topic: str, payload: str, retain: bool = False) -> None:
        if not self.sock:
            raise PortalTestError("MQTT not connected")
        flags = 0x01 if retain else 0x00
        fixed = 0x30 | flags
        body = _encode_utf8(topic) + payload.encode("utf-8")
        self.sock.sendall(bytes([fixed]) + _encode_varint(len(body)) + body)
        self._last_tx = time.time()

    def _next_packet_id(self) -> int:
        pid = self._packet_id
        self._packet_id = (self._packet_id % 0xFFFF) + 1
        return pid

    def _read_packet(self, timeout_s: float) -> Tuple[int, int, bytes]:
        if not self.sock:
            raise PortalTestError("MQTT not connected")
        old_timeout = self.sock.gettimeout()
        self.sock.settimeout(timeout_s)
        try:
            first = _read_exact(self.sock, 1)[0]
            rl = _read_varint(self.sock)
            data = _read_exact(self.sock, rl)
            self._last_rx = time.time()
            return (first >> 4), (first & 0x0F), data
        finally:
            self.sock.settimeout(old_timeout)

    def _decode_publish(self, data: bytes) -> Tuple[str, str]:
        if len(data) < 2:
            raise PortalTestError("Malformed MQTT PUBLISH")
        topic_len = int.from_bytes(data[0:2], "big")
        if len(data) < 2 + topic_len:
            raise PortalTestError("Malformed MQTT topic length")
        topic = data[2:2 + topic_len].decode("utf-8", errors="replace")
        payload = data[2 + topic_len:].decode("utf-8", errors="replace")
        return topic, payload


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
            raise PortalTestError("MQTT socket closed")
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
            raise PortalTestError("MQTT remaining length varint too large")


def _parse_json(payload: str) -> dict[str, Any]:
    data = json.loads(payload)
    if not isinstance(data, dict):
        raise PortalTestError("Expected JSON object")
    return data


def _fetch_latest_json(mqtt: MqttClient, topic: str, label: str, *, timeout_s: int = 20) -> dict[str, Any]:
    mqtt.subscribe(topic, force=True)
    deadline = time.time() + timeout_s
    last_observed = ""
    while time.time() < deadline:
        got_topic, payload = mqtt.wait_for_publish(timeout_s=12.0)
        last_observed = f"topic={got_topic}"
        if got_topic != topic:
            continue
        try:
            return _parse_json(payload)
        except Exception as exc:
            raise PortalTestError(f"{label} payload was not valid JSON on {topic}: {exc}")
    raise PortalTestError(f"Timeout waiting for {label} JSON on {topic}. Last observed: {last_observed}")


def _wait_for_config_interval(
    mqtt: MqttClient,
    *,
    device_root: str,
    entity_name: str,
    expected_bucket: str,
    expected_poll_interval_s: str,
    timeout_s: int = 60,
) -> dict[str, Any]:
    deadline = time.time() + timeout_s
    last_config: dict[str, Any] = {}
    while time.time() < deadline:
        config = _fetch_latest_json(mqtt, f"{device_root}/config", "config", timeout_s=min(20, max(5, int(deadline - time.time()))))
        last_config = config
        intervals = config.get("entity_intervals", {})
        if str(config.get("entity_intervals_encoding", "")) == "bucket_map_chunks":
            merged: dict[str, str] = {}
            chunk_count = int(config.get("entity_intervals_chunks", 0))
            for idx in range(chunk_count):
                chunk = _fetch_latest_json(mqtt, f"{device_root}/config/entity_intervals/{idx}", f"config-chunk-{idx}", timeout_s=15)
                if isinstance(chunk.get("entity_intervals"), dict):
                    for key, value in chunk["entity_intervals"].items():
                        merged[str(key)] = str(value)
                    continue
                raw_map = str(chunk.get("active_bucket_map", ""))
                for token in raw_map.split(";"):
                    token = token.strip()
                    if not token or "=" not in token:
                        continue
                    key, value = token.split("=", 1)
                    key = key.strip()
                    value = value.strip()
                    if key and value:
                        merged[key] = value
            intervals = merged
        if str(config.get("poll_interval_s", "")) == expected_poll_interval_s and str(intervals.get(entity_name, "")) == expected_bucket:
            return config
        time.sleep(2.0)
    raise PortalTestError(
        f"Polling config did not persist {entity_name}={expected_bucket} poll_interval_s={expected_poll_interval_s}: {last_config!r}"
    )


def _wait_for_boot_fw_build_ts_ms(mqtt: MqttClient, boot_topic: str, expected_build_ts_ms: int, *, timeout_s: int) -> dict[str, Any]:
    mqtt.subscribe(boot_topic, force=True)
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        got_topic, payload = mqtt.wait_for_publish(timeout_s=12.0)
        if got_topic != boot_topic:
            continue
        try:
            parsed = _parse_json(payload)
        except Exception:
            continue
        if int(parsed.get("fw_build_ts_ms", 0)) == expected_build_ts_ms:
            return parsed
    raise PortalTestError(f"Timeout waiting for boot fw_build_ts_ms={expected_build_ts_ms} on {boot_topic}")


def _wait_for_topic_change(mqtt: MqttClient, topic: str, previous_payload: str, *, timeout_s: int, label: str) -> str:
    mqtt.subscribe(topic, force=True)
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        got_topic, payload = mqtt.wait_for_publish(timeout_s=12.0)
        if got_topic != topic:
            continue
        if payload != previous_payload:
            return payload
    raise PortalTestError(f"Timeout waiting for fresh {label} on {topic}")


def _latest_payload_for_topic(mqtt: MqttClient, topic: str, *, timeout_s: float = 3.0) -> str:
    mqtt.subscribe(topic, force=True)
    deadline = time.time() + timeout_s
    latest = ""
    while time.time() < deadline:
        try:
            got_topic, payload = mqtt.wait_for_publish(timeout_s=min(1.0, max(0.1, deadline - time.time())))
        except PortalTestError as exc:
            if "Timeout waiting for MQTT publish" in str(exc):
                break
            raise
        if got_topic == topic:
            latest = payload
    return latest


def _wait_for_runtime_ip_via_mqtt(
    mqtt: MqttClient,
    *,
    device_root: str,
    expected_build_ts: int,
    previous_boot_payload: str,
    previous_net_payload: str,
    timeout_s: int,
) -> str:
    boot_payload = _wait_for_topic_change(
        mqtt,
        f"{device_root}/boot",
        previous_boot_payload,
        timeout_s=timeout_s,
        label="boot",
    )
    boot = _parse_json(boot_payload)
    if int(boot.get("fw_build_ts_ms", 0)) != expected_build_ts:
        raise PortalTestError(f"Fresh boot payload reported unexpected fw_build_ts_ms: {boot!r}")

    net_payload = _wait_for_topic_change(
        mqtt,
        f"{device_root}/status/net",
        previous_net_payload,
        timeout_s=30,
        label="status/net",
    )
    net = _parse_json(net_payload)
    runtime_ip = str(net.get("ip", "")).strip()
    if not runtime_ip:
        raise PortalTestError(f"Fresh status/net payload did not expose runtime IP: {net!r}")
    return runtime_ip


def _collect_homeassistant_topics_for_device(mqtt: MqttClient, *, device_root: str, timeout_s: int) -> list[tuple[str, str]]:
    mqtt.subscribe("homeassistant/#", force=True)
    deadline = time.time() + timeout_s
    matches: list[tuple[str, str]] = []
    while time.time() < deadline:
        try:
            got_topic, payload = mqtt.wait_for_publish(timeout_s=min(2.0, max(0.1, deadline - time.time())))
        except PortalTestError as exc:
            if "Timeout waiting for MQTT publish" in str(exc):
                break
            raise
        if not got_topic.startswith("homeassistant/"):
            continue
        if device_root not in payload:
            continue
        matches.append((got_topic, payload))
    return matches


def _clear_homeassistant_topics_for_device(mqtt: MqttClient, *, device_root: str) -> None:
    seen = _collect_homeassistant_topics_for_device(mqtt, device_root=device_root, timeout_s=3)
    for topic, _payload in seen:
        mqtt.publish(topic, "", retain=True)
    if seen:
        time.sleep(1.0)


def _wait_for_controller_discovery(mqtt: MqttClient, *, device_root: str, timeout_s: int = 60) -> tuple[str, str]:
    mqtt.subscribe("homeassistant/#", force=True)
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        got_topic, payload = mqtt.wait_for_publish(timeout_s=min(12.0, max(0.5, deadline - time.time())))
        if not got_topic.endswith("/MQTT_Config/config"):
            continue
        if device_root not in payload:
            continue
        return got_topic, payload
    raise PortalTestError(f"Timeout waiting for controller HA discovery for {device_root}")


def _wait_for_homeassistant_quiet(mqtt: MqttClient, *, quiet_s: float = 2.0, timeout_s: float = 20.0) -> None:
    mqtt.subscribe("homeassistant/#", force=True)
    deadline = time.time() + timeout_s
    quiet_deadline = time.time() + quiet_s
    while time.time() < deadline:
        remaining_quiet = max(0.1, quiet_deadline - time.time())
        try:
            got_topic, _payload = mqtt.wait_for_publish(timeout_s=min(remaining_quiet, 2.0))
        except PortalTestError as exc:
            if "Timeout waiting for MQTT publish" in str(exc):
                return
            raise
        if got_topic.startswith("homeassistant/"):
            quiet_deadline = time.time() + quiet_s
    raise PortalTestError("Timed out waiting for Home Assistant discovery traffic to go quiet")


def _http_request(method: str, url: str, *, body: Optional[bytes] = None, headers: Optional[dict[str, str]] = None, timeout_s: int = 20) -> Tuple[int, bytes]:
    req = urllib.request.Request(url, data=body, headers=headers or {}, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout_s) as resp:
            return int(resp.getcode()), resp.read()
    except urllib.error.HTTPError as e:  # type: ignore[name-defined]
        return int(e.code), e.read()


def _latest_real_firmware_path() -> Path:
    override = os.environ.get("A2M_FIRMWARE_PATH", "").strip()
    if override:
        fw_path = Path(override).expanduser().resolve()
        if not fw_path.exists():
            raise PortalTestError(f"Firmware override does not exist: {fw_path}")
        return fw_path
    latest = _repo_root() / "Alpha2MQTT" / "build" / "firmware" / "Alpha2MQTT_latest_real.txt"
    if not latest.exists():
        raise PortalTestError(f"Missing {latest} (run firmware build first)")
    fw_name = latest.read_text(encoding="utf-8").strip()
    fw_path = _repo_root() / "Alpha2MQTT" / "build" / "firmware" / fw_name
    if not fw_path.exists():
        raise PortalTestError(f"Firmware referenced by {latest} does not exist: {fw_path}")
    return fw_path


def _firmware_build_ts_ms_from_filename(path: Path) -> int:
    m = re.search(r"Alpha2MQTT_(\d+)_real\.bin$", path.name)
    if not m:
        raise PortalTestError(f"Cannot extract build timestamp from firmware filename: {path.name}")
    return int(m.group(1))


def _build_container_name() -> str:
    return os.environ.get("A2M_BUILD_CONTAINER", "arduino-cli-build").strip() or "arduino-cli-build"


def _container_firmware_path(path: Path, *, container_name: str) -> str:
    try:
        rel = path.relative_to(_repo_root())
        rel_posix = rel.as_posix()
        # Support both the legacy container layout (/project/...) and the current
        # bind-mounted repo layout (/project/Alpha2MQTT/...).
        for candidate in (f"/project/{rel_posix}", f"/project/Alpha2MQTT/{rel_posix}"):
            result = subprocess.run(
                ["docker", "exec", container_name, "test", "-f", candidate],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )
            if result.returncode == 0:
                return candidate
    except ValueError:
        pass
    staged = f"/tmp/{path.name}"
    _run_checked(["docker", "cp", str(path), f"{container_name}:{staged}"], timeout_s=60)
    return staged


def _find_esptool_path(container_name: str) -> str:
    out = _run_checked(
        [
            "bash",
            "-lc",
            f"tail -f /dev/null | docker exec -i {shlex.quote(container_name)} bash -lc 'find /root/.arduino15 -maxdepth 8 -type f | grep esptool.py | head -n 1'",
        ],
        timeout_s=30,
    )
    path = out.strip().splitlines()[-1].strip() if out.strip() else ""
    if not path:
        raise PortalTestError(f"Could not find esptool.py inside {container_name}")
    return path


def _start_serial_runtime_ip_watch(*, serial_port: str, timeout_s: int = 120) -> subprocess.Popen[str]:
    container_name = _build_container_name()
    script = f"""
import json, re, serial, sys, time
PORT = {serial_port!r}
TIMEOUT_S = {int(timeout_s)}
PATTERNS = [
    re.compile(r"WiFi connected, IP is ([0-9]+(?:\\.[0-9]+){{3}})"),
    re.compile(r"STA portal: connected IP=([0-9]+(?:\\.[0-9]+){{3}})"),
    re.compile(r"STA portal URL: http://([0-9]+(?:\\.[0-9]+){{3}})/"),
    re.compile(r"WiFi connected: SSID=.*? IP=([0-9]+(?:\\.[0-9]+){{3}})"),
]

deadline = time.time() + TIMEOUT_S
buffer = ""
while time.time() < deadline:
    try:
        ser = serial.Serial(PORT, {ESP8266_SERIAL_BAUD}, timeout=0.25)
    except Exception as exc:
        time.sleep(0.5)
        continue
    try:
        while time.time() < deadline:
            try:
                data = ser.read(512)
            except Exception:
                break
            if not data:
                continue
            text = data.decode("utf-8", errors="replace")
            buffer = (buffer + text)[-4096:]
            for pattern in PATTERNS:
                match = pattern.search(buffer)
                if match:
                    print(json.dumps({{"ip": match.group(1)}}))
                    raise SystemExit(0)
    finally:
        try:
            ser.close()
        except Exception:
            pass
    time.sleep(0.25)
print(json.dumps({{"error": "runtime ip not seen on serial"}}))
raise SystemExit(2)
"""
    cmd = [
        "bash",
        "-lc",
        f"tail -f /dev/null | docker exec -i {shlex.quote(container_name)} "
        f"python3 -u -c {shlex.quote(script)}",
    ]
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def _start_serial_capture(*, serial_port: str, timeout_s: int) -> subprocess.Popen[str]:
    container_name = _build_container_name()
    script = f"""
import serial, sys, time
PORT = {serial_port!r}
TIMEOUT_S = {int(timeout_s)}

deadline = time.time() + TIMEOUT_S
while time.time() < deadline:
    try:
        ser = serial.Serial(PORT, {ESP8266_SERIAL_BAUD}, timeout=0.25)
        break
    except Exception:
        time.sleep(0.5)
else:
    raise SystemExit("serial capture: open failed")

try:
    while time.time() < deadline:
        data = ser.read(512)
        if data:
            sys.stdout.write(data.decode("utf-8", errors="replace"))
            sys.stdout.flush()
finally:
    ser.close()
"""
    cmd = [
        "bash",
        "-lc",
        f"tail -f /dev/null | docker exec -i {shlex.quote(container_name)} "
        f"python3 -u -c {shlex.quote(script)}",
    ]
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def _finish_serial_runtime_ip_watch(proc: subprocess.Popen[str], *, wait_s: int) -> tuple[str, str]:
    try:
        out, _ = proc.communicate(timeout=wait_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()
    lines = [line.strip() for line in (out or "").splitlines() if line.strip()]
    for line in reversed(lines):
        try:
            parsed = json.loads(line)
        except json.JSONDecodeError:
            continue
        runtime_ip = str(parsed.get("ip", "")).strip()
        if runtime_ip:
            return runtime_ip, out or ""
    return "", out or ""


def _finish_serial_capture(proc: subprocess.Popen[str], *, wait_s: int) -> str:
    try:
        out, _ = proc.communicate(timeout=wait_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()
    return out or ""


def _serial_line_toggle(container_name: str, *, serial_port: str, bootloader: bool) -> None:
    if bootloader:
        script = (
            "import serial,time;"
            f"ser=serial.Serial({serial_port!r},115200,timeout=0.25);"
            "ser.setDTR(False);"
            "ser.setRTS(True);"
            "time.sleep(0.1);"
            "ser.setDTR(True);"
            "ser.setRTS(False);"
            "time.sleep(0.05);"
            "ser.setDTR(False);"
            "time.sleep(0.2);"
            "ser.close()"
        )
    else:
        script = (
            "import serial,time;"
            f"ser=serial.Serial({serial_port!r},115200,timeout=0.25);"
            "ser.setDTR(False);"
            "ser.setRTS(True);"
            "time.sleep(0.1);"
            "ser.setRTS(False);"
            "time.sleep(0.2);"
            "ser.close()"
        )
    cmd = (
        f"tail -f /dev/null | docker exec -i {shlex.quote(container_name)} "
        f"python3 -c {shlex.quote(script)}"
    )
    _run_checked(["bash", "-lc", cmd], timeout_s=30)


def _run_esptool_with_bootloader_retry(
    container_name: str,
    *,
    serial_port: str,
    cmd: str,
    timeout_s: int,
    attempts: int = 4,
) -> None:
    last_exc: PortalTestError | None = None
    for attempt in range(1, attempts + 1):
        _serial_line_toggle(container_name, serial_port=serial_port, bootloader=True)
        try:
            _run_checked(["bash", "-lc", cmd], timeout_s=timeout_s)
            return
        except PortalTestError as exc:
            last_exc = exc
            if attempt == attempts:
                break
            _announce(f"manual bootloader attempt {attempt}/{attempts} failed; retrying")
            time.sleep(1.0)
    assert last_exc is not None
    raise last_exc


def _erase_and_flash_real_firmware(fw_path: Path, *, serial_port: str, baud: str) -> None:
    container_name = _build_container_name()
    esptool = _find_esptool_path(container_name)
    fw_in_container = _container_firmware_path(fw_path, container_name=container_name)
    erase_cmd = (
        f"tail -f /dev/null | docker exec -i {shlex.quote(container_name)} bash -lc "
        f"\"python3 {shlex.quote(esptool)} --chip esp8266 --port {shlex.quote(serial_port)} "
        f"--baud {shlex.quote(baud)} --before default_reset --after hard_reset erase_flash\""
    )
    flash_cmd = (
        f"tail -f /dev/null | docker exec -i {shlex.quote(container_name)} bash -lc "
        f"\"python3 {shlex.quote(esptool)} --chip esp8266 --port {shlex.quote(serial_port)} "
        f"--baud {shlex.quote(baud)} --before default_reset --after hard_reset "
        f"write_flash 0x0 {shlex.quote(fw_in_container)}\""
    )
    _announce(f"erase flash on {serial_port}")
    try:
        _run_checked(["bash", "-lc", erase_cmd], timeout_s=240)
        _announce(f"flash real firmware {fw_path.name}")
        _run_checked(["bash", "-lc", flash_cmd], timeout_s=240)
        return
    except PortalTestError as exc:
        _announce(f"default_reset flash path failed; retrying manual bootloader entry ({exc})")

    manual_erase_cmd = (
        f"tail -f /dev/null | docker exec -i {shlex.quote(container_name)} bash -lc "
        f"\"python3 {shlex.quote(esptool)} --chip esp8266 --port {shlex.quote(serial_port)} "
        f"--baud {shlex.quote(baud)} --before no_reset --after no_reset erase_flash\""
    )
    manual_flash_cmd = (
        f"tail -f /dev/null | docker exec -i {shlex.quote(container_name)} bash -lc "
        f"\"python3 {shlex.quote(esptool)} --chip esp8266 --port {shlex.quote(serial_port)} "
        f"--baud {shlex.quote(baud)} --before no_reset --after no_reset "
        f"write_flash 0x0 {shlex.quote(fw_in_container)}\""
    )
    _run_esptool_with_bootloader_retry(
        container_name,
        serial_port=serial_port,
        cmd=manual_erase_cmd,
        timeout_s=240,
    )
    _announce(f"flash real firmware {fw_path.name}")
    _run_esptool_with_bootloader_retry(
        container_name,
        serial_port=serial_port,
        cmd=manual_flash_cmd,
        timeout_s=240,
    )
    _serial_line_toggle(container_name, serial_port=serial_port, bootloader=False)


class PiSsh:
    def __init__(self, host: str, user: str, password: str):
        self.host = host
        self.user = user
        self.password = password
        self._known_hosts = tempfile.NamedTemporaryFile(prefix="a2m_pi_known_hosts_", delete=False)
        self._known_hosts.close()
        self._askpass = tempfile.NamedTemporaryFile(prefix="a2m_pi_askpass_", suffix=".sh", delete=False, mode="w", encoding="utf-8")
        self._askpass.write("#!/bin/sh\nprintf '%s' " + shlex.quote(password) + "\n")
        self._askpass.close()
        os.chmod(self._askpass.name, 0o700)

    def close(self) -> None:
        for path in (self._known_hosts.name, self._askpass.name):
            try:
                os.unlink(path)
            except FileNotFoundError:
                pass

    def _base_cmd(self) -> list[str]:
        return [
            "ssh",
            "-o", "PreferredAuthentications=password",
            "-o", "PubkeyAuthentication=no",
            "-o", "KbdInteractiveAuthentication=no",
            "-o", "BatchMode=no",
            "-o", "LogLevel=ERROR",
            "-o", "StrictHostKeyChecking=no",
            "-o", f"UserKnownHostsFile={self._known_hosts.name}",
            f"{self.user}@{self.host}",
        ]

    def _env(self) -> dict[str, str]:
        env = os.environ.copy()
        env["DISPLAY"] = "codex-ssh"
        env["SSH_ASKPASS"] = self._askpass.name
        env["SSH_ASKPASS_REQUIRE"] = "force"
        return env

    def run_python(self, script: str, *, timeout_s: int) -> str:
        _ssh_log(f"remote python len={len(script)}")
        cp = _run(self._base_cmd() + ["python3", "-"], timeout_s=timeout_s, input_text=script, env=self._env())
        if cp.returncode != 0:
            raise PortalTestError(cp.stdout.strip() or f"SSH command failed with exit={cp.returncode}")
        return cp.stdout


def _pi_join_esp_ap(ssh: PiSsh, *, iface: str, ssid_prefix: str) -> dict[str, Any]:
    script = f"""
import json, re, subprocess, sys, time
iface = {json.dumps(iface)}
ssid_prefix = {json.dumps(ssid_prefix)}
sudo_pw = {json.dumps(ssh.password)}

def run(cmd, *, input_text=None, check=True):
    cp = subprocess.run(cmd, input=input_text, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if check and cp.returncode != 0:
        raise SystemExit(json.dumps({{"error": cp.stdout.strip() or "command failed", "cmd": cmd}}))
    return cp.stdout

def sudo(cmd):
    return run(["sudo", "-S", "-p", ""] + cmd, input_text=sudo_pw + "\\n")

def soft_reset_iface():
    run(["nmcli", "device", "disconnect", iface], check=False)
    sudo(["nmcli", "device", "set", iface, "managed", "no"])
    time.sleep(1)
    sudo(["nmcli", "device", "set", iface, "managed", "yes"])
    time.sleep(2)

def scan_wifi():
    run(["nmcli", "radio", "wifi", "on"], check=False)
    run(["nmcli", "device", "disconnect", iface], check=False)
    run(["nmcli", "device", "wifi", "rescan", "ifname", iface], check=False)
    scan = run([
        "nmcli", "-t", "-f", "IN-USE,SSID,BSSID,SIGNAL,SECURITY",
        "device", "wifi", "list", "ifname", iface
    ], check=False)
    return scan

def pick_best(scan):
    best = None
    for raw in scan.splitlines():
        if not raw:
            continue
        parts = re.split(r'(?<!\\\\):', raw, maxsplit=4)
        if len(parts) < 5:
            continue
        _in_use, ssid, bssid, signal, security = parts
        bssid = bssid.replace("\\:", ":")
        if not ssid.startswith(ssid_prefix):
            continue
        try:
            score = int(signal)
        except ValueError:
            score = -1
        cand = {{"ssid": ssid, "bssid": bssid, "signal": score, "security": security}}
        if best is None or cand["signal"] > best["signal"]:
            best = cand
    return best

last_scan = ""
scan_history = []
last_error = ""
best = None
run(["nmcli", "radio", "wifi", "on"], check=False)
run(["nmcli", "device", "disconnect", iface], check=False)
for attempt in range(18):
    if attempt in (6, 12):
        soft_reset_iface()
    scan = scan_wifi()
    last_scan = scan
    scan_history.append(scan)
    if len(scan_history) > 4:
        scan_history = scan_history[-4:]
    best = pick_best(scan)
    if best is not None:
        break
    time.sleep(5)

if best is None:
    show = run(["nmcli", "-t", "-f", "GENERAL.STATE,GENERAL.CONNECTION", "device", "show", iface], check=False)
    raise SystemExit(json.dumps({{
        "error": "no matching AP",
        "scan": last_scan,
        "recent_scans": scan_history,
        "device": show,
    }}))

run(["sudo", "-S", "-p", "", "nmcli", "device", "disconnect", iface], input_text=sudo_pw + "\\n", check=False)
for attempt in range(10):
    if attempt in (3, 7):
        soft_reset_iface()
    scan = scan_wifi()
    last_scan = scan
    scan_history.append(scan)
    if len(scan_history) > 4:
        scan_history = scan_history[-4:]
    refreshed = pick_best(scan)
    if refreshed is None:
        last_error = "matching AP disappeared before connect"
        time.sleep(3)
        continue
    best = refreshed
    connect_variants = [
        ["sudo", "-S", "-p", "", "nmcli", "device", "wifi", "connect", best["bssid"], "ifname", iface],
        ["sudo", "-S", "-p", "", "nmcli", "device", "wifi", "connect", best["ssid"], "ifname", iface, "bssid", best["bssid"]],
        ["sudo", "-S", "-p", "", "nmcli", "device", "wifi", "connect", best["ssid"], "ifname", iface],
    ]
    for cmd in connect_variants:
        cp = subprocess.run(
            cmd,
            input=sudo_pw + "\\n",
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if cp.returncode == 0:
            show = run(["nmcli", "-t", "-f", "GENERAL.STATE,IP4.ADDRESS,IP4.GATEWAY", "device", "show", iface], check=False)
            print(json.dumps({{"ssid": best["ssid"], "bssid": best["bssid"], "signal": best["signal"], "show": show}}))
            sys.exit(0)
        last_error = cp.stdout.strip()
    time.sleep(3)

raise SystemExit(json.dumps({{"error": last_error or "connect failed", "ssid": best["ssid"], "bssid": best["bssid"], "scan": last_scan}}))
"""
    out = ssh.run_python(script, timeout_s=90).strip()
    return json.loads(out.splitlines()[-1])


def _wait_for_ap_ssid_cycle(ssh: PiSsh, *, iface: str, ssid: str, timeout_s: int) -> None:
    script = f"""
import json, re, subprocess, time
iface = {json.dumps(iface)}
ssid = {json.dumps(ssid)}
timeout_s = {int(timeout_s)}

def run(cmd, *, check=True):
    cp = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if check and cp.returncode != 0:
        raise SystemExit(json.dumps({{"error": cp.stdout.strip() or "command failed", "cmd": cmd}}))
    return cp.stdout

def scan_for_target():
    run(["nmcli", "radio", "wifi", "on"], check=False)
    run(["nmcli", "device", "wifi", "rescan", "ifname", iface], check=False)
    scan = run([
        "nmcli", "-t", "-f", "SSID,BSSID,SIGNAL",
        "device", "wifi", "list", "ifname", iface,
    ], check=False)
    present = False
    for raw in scan.splitlines():
        if not raw:
            continue
        parts = re.split(r'(?<!\\\\):', raw, maxsplit=2)
        if parts and parts[0] == ssid:
            present = True
            break
    return present, scan

run(["nmcli", "device", "disconnect", iface], check=False)
deadline = time.time() + timeout_s
saw_absent = False
last_scan = ""
while time.time() < deadline:
    present, scan = scan_for_target()
    last_scan = scan
    if not saw_absent:
        if not present:
            saw_absent = True
    elif present:
        print(json.dumps({{"ssid": ssid, "cycled": True}}))
        raise SystemExit(0)
    time.sleep(2.0)

raise SystemExit(json.dumps({{
    "error": "ssid did not disappear and reappear in time",
    "ssid": ssid,
    "saw_absent": saw_absent,
    "scan": last_scan,
}}))
"""
    out = ssh.run_python(script, timeout_s=max(60, timeout_s + 15)).strip()
    try:
        parsed = json.loads(out.splitlines()[-1])
    except json.JSONDecodeError as exc:
        raise PortalTestError(f"Could not parse AP cycle result: {out[:300]!r}") from exc
    if not parsed.get("cycled"):
        raise PortalTestError(f"AP cycle wait failed: {parsed!r}")


def _pi_http_request(ssh: PiSsh, *, method: str, url: str, form: Optional[dict[str, str]] = None, timeout_s: int = 20) -> dict[str, Any]:
    script = f"""
import json, subprocess, urllib.parse
url = {json.dumps(url)}
form = json.loads({json.dumps(json.dumps(form))})
cmd = [
    "curl",
    "-sS",
    "--http1.0",
    "--connect-timeout", str(min(10, max(1, int({int(timeout_s)})))),
    "--max-time", str(int({int(timeout_s)})),
    "--output", "-",
    "--write-out", "\\n__A2M_STATUS__%{{http_code}}",
    "-X", {json.dumps(method)},
    "-H", "Connection: close",
]
if form is not None:
    cmd.extend([
        "-H", "Content-Type: application/x-www-form-urlencoded",
        "--data-binary", urllib.parse.urlencode(form),
    ])
cmd.append(url)
cp = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False)
marker = "\\n__A2M_STATUS__"
body, _, status = cp.stdout.rpartition(marker)
if not status:
    if cp.returncode != 0:
        raise SystemExit(json.dumps({{
            "error": cp.stderr.strip() or cp.stdout.strip() or f"curl rc={{cp.returncode}}",
            "url": url,
            "method": {json.dumps(method)},
        }}))
    raise SystemExit(json.dumps({{"error": "missing curl status marker", "url": url, "method": {json.dumps(method)}}}))
print(json.dumps({{"status": int(status.strip()), "body": body}}))
"""
    out = ssh.run_python(script, timeout_s=timeout_s + 15).strip()
    return json.loads(out.splitlines()[-1])


def _assert_contains(text: str, needle: str, context: str) -> None:
    if needle not in text:
        raise PortalTestError(f"{context}: missing {needle!r}")


def _assert_contains_any(text: str, needles: list[str], context: str) -> None:
    for needle in needles:
        if needle in text:
            return
    raise PortalTestError(f"{context}: missing any of {needles!r}")


def _assert_portal_root_menu(body: str, context: str) -> None:
    for needle in ("/0wifi", "/config/mqtt", "/config/polling", "/config/polling/reset", "/config/update", "/status", "/config/reboot-normal"):
        _assert_contains(body, needle, context)


def _assert_wifi_portal_root_contract(body: str, context: str) -> None:
    _assert_portal_root_menu(body, context)
    _assert_button_theme(body, "wifi", context)


def _assert_normal_runtime_root_contract(body: str, context: str) -> None:
    _assert_contains(body, "Alpha2MQTT Control", context)
    _assert_contains(body, "Boot mode: normal", context)
    _assert_contains(body, "/reboot/wifi", context)
    _assert_button_theme(body, "normal", context)


def _assert_button_theme(body: str, mode: str, context: str) -> None:
    accents = {
        "ap": "#1c6bcf",
        "wifi": "#c57a00",
        "normal": "#1d8c4b",
    }
    accent = accents[mode]
    _assert_contains(body, 'meta name="a2m-ui" content="buttons-v1"', context)
    _assert_contains(body, f'meta name="a2m-mode" content="{mode}"', context)
    _assert_contains(body, f'meta name="a2m-accent" content="{accent}"', context)
    _assert_contains(body, "border-radius:14px", context)
    _assert_contains(body, "min-height:52px", context)


def _assert_reboot_handoff_contract(body: str, *, heading: str, target_mode: str, context: str) -> None:
    for needle in (
        heading,
        'id="reboot-handoff"',
        'id="reboot-status"',
        f'data-target-mode="{target_mode}"',
        'data-start-ms="10000"',
        'data-retry-ms="5000"',
        'data-timeout-ms="300000"',
        "Auto-refresh starts in 10 seconds",
        "5 minutes",
    ):
        _assert_contains(body, needle, context)


def _extract_runtime_ip_from_status(body: str) -> str:
    m = re.search(r"SSID:\s*[^<]+<br>IP:\s*([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)", body, flags=re.IGNORECASE)
    if not m:
        raise PortalTestError("Portal status page did not expose a runtime IP after WiFi connected")
    return m.group(1)


def _wait_for_portal_status_connected(ssh: PiSsh, *, timeout_s: int) -> Tuple[dict[str, Any], str]:
    deadline = time.time() + timeout_s
    last = ""
    while time.time() < deadline:
        try:
            resp = _pi_http_request(ssh, method="GET", url="http://192.168.4.1/status", timeout_s=10)
        except Exception as exc:
            last = f"exc={exc}"
            time.sleep(2.0)
            continue
        body = str(resp.get("body", ""))
        last = body
        if "WiFi Status: Connected" in body or "STA status: Connected" in body:
            return resp, _extract_runtime_ip_from_status(body)
        time.sleep(2.0)
    raise PortalTestError(f"Timed out waiting for portal connected status. Last body snippet: {last[:300]!r}")


def _wait_for_runtime_ready(
    mqtt: MqttClient,
    *,
    device_root: str,
    expected_build_ts: int,
    previous_boot_payload: str,
    previous_status_payload: str,
    previous_net_payload: str,
    timeout_s: int,
) -> Tuple[dict[str, Any], dict[str, Any]]:
    boot_payload = _wait_for_topic_change(
        mqtt,
        f"{device_root}/boot",
        previous_boot_payload,
        timeout_s=timeout_s,
        label="boot",
    )
    boot = _parse_json(boot_payload)
    if int(boot.get("fw_build_ts_ms", 0)) != expected_build_ts:
        raise PortalTestError(f"Fresh boot payload reported unexpected fw_build_ts_ms: {boot!r}")

    status_payload = _wait_for_topic_change(
        mqtt,
        f"{device_root}/status",
        previous_status_payload,
        timeout_s=30,
        label="status",
    )
    net_payload = _wait_for_topic_change(
        mqtt,
        f"{device_root}/status/net",
        previous_net_payload,
        timeout_s=30,
        label="status/net",
    )
    return _parse_json(status_payload), _parse_json(net_payload)


def _wait_for_runtime_root(ssh: PiSsh, base_url: str, *, timeout_s: int) -> str:
    deadline = time.time() + timeout_s
    last = ""
    while time.time() < deadline:
        try:
            resp = _pi_http_request(ssh, method="GET", url=base_url + "/", timeout_s=10)
        except Exception as exc:
            last = f"exc={exc}"
            time.sleep(2.0)
            continue
        status = int(resp.get("status", 0))
        last = str(resp.get("body", ""))
        if status == 200 and "Alpha2MQTT Control" in last:
            return last
        time.sleep(2.0)
    raise PortalTestError(f"Timed out waiting for runtime root. Last={last[:300]!r}")


def _discover_runtime_root_local(*, previous_ip: str, hostname: str, timeout_s: int) -> tuple[str, str]:
    def probe_url(url: str) -> tuple[str, str] | None:
        try:
            with urllib.request.urlopen(url, timeout=1.5) as resp:
                body = resp.read().decode("utf-8", errors="replace")
                if int(resp.getcode()) == 200 and "Alpha2MQTT Control" in body and "Boot mode:" in body:
                    return url.rstrip("/"), body
        except Exception:
            return None
        return None

    urls: list[str] = []
    seen: set[str] = set()

    def add_url(url: str) -> None:
        if url not in seen:
            seen.add(url)
            urls.append(url)

    if hostname:
        add_url(f"http://{hostname}/")

    if previous_ip:
        try:
            network = ipaddress.ip_network(previous_ip + "/24", strict=False)
            preferred = [ipaddress.ip_address(previous_ip)]
            others = [ip for ip in network.hosts() if ip != preferred[0]]
            for ip in preferred + others:
                add_url(f"http://{ip}/")
        except ValueError:
            add_url(f"http://{previous_ip}/")
    else:
        try:
            addr_data = json.loads(
                subprocess.check_output(["ip", "-json", "-4", "addr", "show", "up"], text=True)
            )
        except Exception:
            addr_data = []
        for iface in addr_data:
            for info in iface.get("addr_info", []):
                if info.get("family") != "inet" or info.get("scope") != "global":
                    continue
                local = info.get("local")
                prefixlen = int(info.get("prefixlen", 24))
                if not local:
                    continue
                try:
                    network = ipaddress.ip_network(f"{local}/{max(prefixlen, 24)}", strict=False)
                except ValueError:
                    continue
                for ip in network.hosts():
                    add_url(f"http://{ip}/")

    with concurrent.futures.ThreadPoolExecutor(max_workers=32) as ex:
        futures = [ex.submit(probe_url, url) for url in urls]
        for fut in concurrent.futures.as_completed(futures, timeout=max(30, timeout_s)):
            result = fut.result()
            if result:
                return result
    raise PortalTestError(
        f"local runtime root not found previous_ip={previous_ip!r} hostname={hostname!r}"
    )


def _wait_for_runtime_root_contains(ssh: PiSsh, base_url: str, needle: str, *, timeout_s: int) -> str:
    deadline = time.time() + timeout_s
    last = ""
    while time.time() < deadline:
        body = _wait_for_runtime_root(ssh, base_url, timeout_s=min(20, max(5, int(deadline - time.time()))))
        last = body
        if needle in body:
            return body
        time.sleep(2.0)
    raise PortalTestError(f"runtime root: missing {needle!r}; last={last[:300]!r}")


def _wait_for_runtime_root_stable_markers(
    ssh: PiSsh,
    *,
    base_url: str,
    timeout_s: int,
    samples: int = 3,
) -> str:
    deadline = time.time() + timeout_s
    consecutive = 0
    last_uptime_ms = -1
    last_body = ""
    while time.time() < deadline:
        resp = _pi_http_request(ssh, method="GET", url=base_url + "/", timeout_s=10)
        body = str(resp.get("body", ""))
        last_body = body
        if int(resp.get("status", 0)) != 200:
            consecutive = 0
            last_uptime_ms = -1
            time.sleep(1.0)
            continue
        if "Boot mode: normal" not in body or "MQTT connected: 1" not in body:
            consecutive = 0
            last_uptime_ms = -1
            time.sleep(1.0)
            continue
        uptime_match = re.search(r"Uptime \(ms\): (\d+)", body)
        if not uptime_match:
            consecutive = 0
            last_uptime_ms = -1
            time.sleep(1.0)
            continue
        current_uptime_ms = int(uptime_match.group(1))
        if last_uptime_ms >= 0 and current_uptime_ms + 500 < last_uptime_ms:
            consecutive = 0
            last_uptime_ms = -1
            time.sleep(1.0)
            continue
        last_uptime_ms = current_uptime_ms
        consecutive += 1
        if consecutive >= max(1, samples):
            return body
        time.sleep(1.0)
    raise PortalTestError(f"runtime root did not stabilize; last={last_body[:300]!r}")


def _wait_for_runtime_uptime_at_least(
    ssh: PiSsh,
    *,
    base_url: str,
    min_uptime_ms: int,
    timeout_s: int,
) -> str:
    deadline = time.time() + timeout_s
    last_body = ""
    while time.time() < deadline:
        body = _wait_for_runtime_root(ssh, base_url, timeout_s=min(20, max(5, int(deadline - time.time()))))
        last_body = body
        uptime_match = re.search(r"Uptime \(ms\): (\d+)", body)
        if uptime_match and int(uptime_match.group(1)) >= min_uptime_ms:
            return body
        time.sleep(1.0)
    raise PortalTestError(f"runtime root did not reach uptime>={min_uptime_ms}ms; last={last_body[:300]!r}")


def _assert_runtime_root_stable_under_load(
    ssh: PiSsh,
    *,
    base_url: str,
    duration_s: int = 12,
) -> None:
    initial_body = _wait_for_runtime_root_stable_markers(ssh, base_url=base_url, timeout_s=30, samples=3)
    initial_match = re.search(r"Uptime \(ms\): (\d+)", initial_body)
    if not initial_match:
        raise PortalTestError(f"Runtime root missing uptime marker under load: {initial_body[:300]!r}")
    initial_uptime_ms = int(initial_match.group(1))

    deadline = time.time() + duration_s
    last_body = ""
    last_uptime_ms = initial_uptime_ms
    successes = 0
    consecutive_failures = 0
    while time.time() < deadline:
        try:
            resp = _pi_http_request(ssh, method="GET", url=base_url + "/", timeout_s=5)
        except Exception as exc:
            consecutive_failures += 1
            last_body = f"exc={exc}"
            if consecutive_failures >= 4:
                raise PortalTestError(f"Runtime GET / repeatedly failed under load: last={last_body[:300]!r}")
            time.sleep(1.0)
            continue
        last_body = str(resp.get("body", ""))
        if int(resp.get("status", 0)) != 200:
            consecutive_failures += 1
            if consecutive_failures >= 4:
                raise PortalTestError(f"Runtime GET / repeatedly failed under load with status={resp.get('status')}")
            time.sleep(1.0)
            continue
        if "Boot mode: normal" not in last_body or "MQTT connected: 1" not in last_body:
            consecutive_failures += 1
            if consecutive_failures >= 4:
                raise PortalTestError(f"Runtime root repeatedly lost expected markers under load: {last_body[:300]!r}")
            time.sleep(1.0)
            continue
        uptime_match = re.search(r"Uptime \(ms\): (\d+)", last_body)
        if not uptime_match:
            consecutive_failures += 1
            if consecutive_failures >= 4:
                raise PortalTestError(f"Runtime root repeatedly missed uptime marker under load: {last_body[:300]!r}")
            time.sleep(1.0)
            continue
        current_uptime_ms = int(uptime_match.group(1))
        if current_uptime_ms + 500 < last_uptime_ms:
            raise PortalTestError(
                f"Runtime uptime moved backwards under HTTP load: previous={last_uptime_ms}ms current={current_uptime_ms}ms"
            )
        consecutive_failures = 0
        successes += 1
        last_uptime_ms = current_uptime_ms
        time.sleep(1.0)

    min_successes = max(2, duration_s // 6)
    if successes < min_successes:
        raise PortalTestError(f"Runtime root had too few good under-load samples: successes={successes} last={last_body[:300]!r}")
    min_expected_ms = initial_uptime_ms + max(5000, (duration_s * 1000) // 2)
    if last_uptime_ms < min_expected_ms:
        raise PortalTestError(
            f"Runtime uptime did not advance enough under HTTP load: start={initial_uptime_ms}ms end={last_uptime_ms}ms expected>={min_expected_ms}ms"
        )


def _discover_runtime_root(ssh: PiSsh, *, previous_ip: str, hostname: str, timeout_s: int) -> tuple[str, str]:
    try:
        return _discover_runtime_root_local(previous_ip=previous_ip, hostname=hostname, timeout_s=min(timeout_s, 90))
    except Exception:
        pass

    direct_url = f"http://{previous_ip}"
    try:
        return direct_url, _wait_for_runtime_root(ssh, direct_url, timeout_s=max(20, min(timeout_s, 60)))
    except Exception:
        pass

    script = f"""
import concurrent.futures, ipaddress, json, subprocess, urllib.request
previous_ip = {json.dumps(previous_ip)}
hostname = {json.dumps(hostname)}
timeout_s = {int(timeout_s)}

def probe_url(url: str):
    try:
        with urllib.request.urlopen(url, timeout=1.5) as resp:
            body = resp.read().decode('utf-8', errors='replace')
            if int(resp.getcode()) == 200 and 'Alpha2MQTT Control' in body and 'Boot mode:' in body:
                return {{"url": url.rstrip('/'), "body": body}}
    except Exception:
        return None
    return None

urls = []
if hostname:
    urls.append(f"http://{{hostname}}/")

seen = set(urls)

def add_url(url: str):
    if url not in seen:
        seen.add(url)
        urls.append(url)

if previous_ip:
    network = ipaddress.ip_network(previous_ip + "/24", strict=False)
    preferred = [ipaddress.ip_address(previous_ip)]
    others = [ip for ip in network.hosts() if ip != preferred[0]]
    ordered = preferred + others
    for ip in ordered:
        add_url(f"http://{{ip}}/")
else:
    try:
        addr_data = json.loads(subprocess.check_output(["ip", "-json", "-4", "addr", "show", "up"], text=True))
    except Exception:
        addr_data = []
    for iface in addr_data:
        for info in iface.get("addr_info", []):
            if info.get("family") != "inet" or info.get("scope") != "global":
                continue
            local = info.get("local")
            prefixlen = int(info.get("prefixlen", 24))
            if not local:
                continue
            try:
                addr = ipaddress.ip_address(local)
            except ValueError:
                continue
            network = ipaddress.ip_network(f"{{local}}/{{max(prefixlen, 24)}}", strict=False)
            for ip in network.hosts():
                add_url(f"http://{{ip}}/")

with concurrent.futures.ThreadPoolExecutor(max_workers=32) as ex:
    futures = [ex.submit(probe_url, url) for url in urls]
    for fut in concurrent.futures.as_completed(futures, timeout=max(30, timeout_s)):
        result = fut.result()
        if result:
            print(json.dumps(result))
            raise SystemExit(0)

raise SystemExit(json.dumps({{"error": "runtime root not found", "previous_ip": previous_ip, "hostname": hostname}}))
"""
    out = ssh.run_python(script, timeout_s=max(60, timeout_s + 15)).strip()
    last_line = out.splitlines()[-1]
    try:
        parsed = json.loads(last_line)
    except json.JSONDecodeError as exc:
        raise PortalTestError(f"Could not parse runtime discovery result: {last_line[:300]!r}") from exc
    if "url" not in parsed or "body" not in parsed:
        raise PortalTestError(f"Runtime discovery failed: {parsed!r}")
    return str(parsed["url"]), str(parsed["body"])


def _wait_for_sta_portal(ssh: PiSsh, base_url: str, *, timeout_s: int) -> str:
    deadline = time.time() + timeout_s
    last = ""
    while time.time() < deadline:
        try:
            resp = _pi_http_request(ssh, method="GET", url=base_url + "/", timeout_s=10)
        except Exception as exc:
            last = f"exc={exc}"
            time.sleep(2.0)
            continue
        status = int(resp.get("status", 0))
        last = str(resp.get("body", ""))
        if status == 200 and all(
            token in last
            for token in (
                "/config/mqtt",
                "/config/polling",
                "/config/reboot-normal",
                'meta name="a2m-mode" content="wifi"',
            )
        ):
            return last
        time.sleep(2.0)
    raise PortalTestError(f"Timed out waiting for STA portal root menu. Last={last[:300]!r}")


def _rediscover_sta_portal_after_reboot_wifi(ssh: PiSsh, base_url: str, *, timeout_s: int) -> str:
    previous_ip = urllib.parse.urlparse(base_url).hostname or ""
    discovered_base_url, _portal_body = _discover_sta_portal_base(
        ssh,
        previous_ip=previous_ip,
        hostname="",
        timeout_s=timeout_s,
    )
    return discovered_base_url


def _discover_sta_portal_base(
    ssh: PiSsh,
    *,
    previous_ip: str,
    hostname: str,
    timeout_s: int,
) -> tuple[str, str]:
    if previous_ip:
        direct_url = f"http://{previous_ip}"
        try:
            return direct_url, _wait_for_sta_portal(ssh, direct_url, timeout_s=max(20, min(timeout_s, 60)))
        except Exception:
            pass

    script = f"""
import concurrent.futures, ipaddress, json, subprocess, urllib.request
previous_ip = {json.dumps(previous_ip)}
hostname = {json.dumps(hostname)}
timeout_s = {int(timeout_s)}

def probe_url(url: str):
    try:
        with urllib.request.urlopen(url, timeout=1.5) as resp:
            body = resp.read().decode('utf-8', errors='replace')
            if int(resp.getcode()) == 200 and '/config/mqtt' in body and '/config/polling' in body and '/config/reboot-normal' in body and 'meta name="a2m-mode" content="wifi"' in body:
                return {{"url": url[:-1], "body": body}}
    except Exception:
        return None
    return None

urls = []
if hostname:
    urls.append(f"http://{{hostname}}/")

seen = set(urls)

def add_url(url: str):
    if url not in seen:
        seen.add(url)
        urls.append(url)

if previous_ip:
    network = ipaddress.ip_network(previous_ip + "/24", strict=False)
    preferred = [ipaddress.ip_address(previous_ip)]
    others = [ip for ip in network.hosts() if ip != preferred[0]]
    ordered = preferred + others
    for ip in ordered:
        add_url(f"http://{{ip}}/")
else:
    try:
        addr_data = json.loads(subprocess.check_output(["ip", "-json", "-4", "addr", "show", "up"], text=True))
    except Exception:
        addr_data = []
    for iface in addr_data:
        for info in iface.get("addr_info", []):
            if info.get("family") != "inet" or info.get("scope") != "global":
                continue
            local = info.get("local")
            prefixlen = int(info.get("prefixlen", 24))
            if not local:
                continue
            try:
                addr = ipaddress.ip_address(local)
            except ValueError:
                continue
            network = ipaddress.ip_network(f"{{local}}/{{max(prefixlen, 24)}}", strict=False)
            for ip in network.hosts():
                add_url(f"http://{{ip}}/")

with concurrent.futures.ThreadPoolExecutor(max_workers=32) as ex:
    futures = [ex.submit(probe_url, url) for url in urls]
    for fut in concurrent.futures.as_completed(futures, timeout=max(30, timeout_s)):
        result = fut.result()
        if result:
            print(json.dumps(result))
            raise SystemExit(0)

raise SystemExit(json.dumps({{"error": "sta portal not found", "previous_ip": previous_ip, "hostname": hostname}}))
"""
    out = ssh.run_python(script, timeout_s=max(60, timeout_s + 15)).strip()
    last_line = out.splitlines()[-1]
    try:
        parsed = json.loads(last_line)
    except json.JSONDecodeError as exc:
        raise PortalTestError(f"Could not parse STA portal discovery result: {last_line[:300]!r}") from exc
    if "url" not in parsed or "body" not in parsed:
        raise PortalTestError(f"STA portal discovery failed: {parsed!r}")
    return str(parsed["url"]), str(parsed["body"])


def _extract_form_action(body: str) -> str:
    m = re.search(r"<form[^>]*action=['\"]([^'\"]+)['\"]", body, flags=re.IGNORECASE)
    if not m:
        raise PortalTestError("Could not find form action in HTML")
    return m.group(1)


def _extract_input_names(body: str) -> set[str]:
    return set(re.findall(r"name=['\"]([^'\"]+)['\"]", body, flags=re.IGNORECASE))


def _extract_form_action_with_input(body: str, required_input: str) -> str:
    for match in re.finditer(r"<form[^>]*action=['\"]([^'\"]+)['\"][^>]*>(.*?)</form>", body, flags=re.IGNORECASE | re.DOTALL):
        action = match.group(1)
        form_body = match.group(2)
        input_names = _extract_input_names(form_body)
        if required_input in input_names:
            return action
    raise PortalTestError(f"Could not find form action for input {required_input!r}")


def _extract_input_value(body: str, name: str) -> str:
    match = re.search(rf'name=["\']{re.escape(name)}["\'][^>]*value=["\']([^"\']*)["\']', body, flags=re.IGNORECASE)
    if not match:
        raise PortalTestError(f"Could not find input value for {name!r}")
    return html_lib.unescape(match.group(1))


def _wait_for_polling_page_persisted(
    ssh: PiSsh,
    *,
    base_url: str,
    family: str,
    page: str,
    entity_name: str,
    expected_bucket: str,
    expected_poll_interval_s: str,
    timeout_s: int = 30,
) -> str:
    deadline = time.time() + timeout_s
    last = ""
    url = f"{base_url}/config/polling?family={urllib.parse.quote(family)}&page={urllib.parse.quote(page)}&saved=1"
    while time.time() < deadline:
        try:
            resp = _pi_http_request(ssh, method="GET", url=url, timeout_s=10)
        except Exception as exc:
            last = f"exc={exc}"
            time.sleep(1.0)
            continue
        if int(resp.get("status", 0)) != 200:
            last = f"status={resp.get('status')}"
            time.sleep(1.0)
            continue
        body = str(resp.get("body", ""))
        last = body
        poll_ok = (
            'name="poll_interval_s"' in body and
            f'value="{expected_poll_interval_s}"' in body and
            'max="120"' in body
        )
        row_match = re.search(
            rf'<tr[^>]*data-entity="{re.escape(entity_name)}"[^>]*>(.*?)</tr>',
            body,
            flags=re.IGNORECASE | re.DOTALL,
        )
        row_ok = False
        if row_match:
            row_html = row_match.group(1)
            row_ok = re.search(
                rf'<option value="{re.escape(expected_bucket)}"[^>]*selected',
                row_html,
                flags=re.IGNORECASE,
            ) is not None
        if poll_ok and row_ok:
            return body
        time.sleep(1.0)
    raise PortalTestError(
        f"Polling page did not reflect persisted {entity_name}={expected_bucket} poll_interval_s={expected_poll_interval_s}: {last[:500]!r}"
    )


def _prime_runtime_topics(mqtt: "MqttClient", device_root: str) -> None:
    # Keep subscriptions active for later diagnostics/config checks, but do not use retained
    # MQTT state as the onboarding->runtime phase gate. That gate must be tied to current HTTP state.
    for topic in ("boot", "status", "status/net"):
        mqtt.subscribe(f"{device_root}/{topic}", force=True)
        try:
            mqtt.wait_for_publish(timeout_s=2.0)
        except Exception:
            pass


def _join_ap_and_load_wifi_page(ssh: "PiSsh", mqtt: "MqttClient", *, iface: str) -> tuple[str, str, str]:
    join = _pi_join_esp_ap(ssh, iface=iface, ssid_prefix="Alpha2MQTT-")
    ap_ssid = str(join.get("ssid", ""))
    if not ap_ssid.startswith("Alpha2MQTT-"):
        raise PortalTestError(f"Unexpected AP SSID: {ap_ssid!r}")
    _announce(f"joined portal AP {ap_ssid}")
    _prime_runtime_topics(mqtt, ap_ssid)

    root_resp = _pi_http_request(ssh, method="GET", url="http://192.168.4.1/")
    if int(root_resp.get("status", 0)) != 200:
        raise PortalTestError(f"Portal GET / returned {root_resp.get('status')}")
    root_body = str(root_resp.get("body", ""))
    _assert_portal_root_menu(root_body, "portal root")
    _assert_button_theme(root_body, "ap", "portal root")

    deadline = time.time() + 20
    wifi_body = ""
    wifi_action = ""
    input_names: set[str] = set()
    while time.time() < deadline:
        wifi_resp = _pi_http_request(ssh, method="GET", url="http://192.168.4.1/0wifi")
        if int(wifi_resp.get("status", 0)) != 200:
            time.sleep(1.0)
            continue
        wifi_body = str(wifi_resp.get("body", ""))
        input_names = _extract_input_names(wifi_body)
        try:
            wifi_action = _extract_form_action_with_input(wifi_body, "s")
        except PortalTestError:
            wifi_action = ""
        if wifi_action and "s" in input_names and "p" in input_names:
            break
        time.sleep(1.0)
    if not wifi_action or "s" not in input_names or "p" not in input_names:
        raise PortalTestError(f"wifi form missing ssid/password inputs: names={sorted(input_names)!r}")
    _assert_button_theme(wifi_body, "ap", "portal wifi page")
    return ap_ssid, wifi_action, wifi_body


def _save_wifi_and_wait_connected(
    ssh: "PiSsh",
    *,
    serial_port: str,
    wifi_ssid: str,
    wifi_pwd: str,
    wifi_action: str,
    wifi_body: str,
) -> str:
    serial_watch = _start_serial_runtime_ip_watch(serial_port=serial_port, timeout_s=120)
    csrf = _extract_input_value(wifi_body, "csrf")
    save_wifi = _pi_http_request(
        ssh,
        method="POST",
        url=urllib.parse.urljoin("http://192.168.4.1/0wifi", wifi_action),
        form={"s": wifi_ssid, "p": wifi_pwd, "csrf": csrf},
        timeout_s=30,
    )
    save_wifi_status = int(save_wifi.get("status", 0))
    save_wifi_body = str(save_wifi.get("body", ""))
    if save_wifi_status not in (0, 200, 302):
        raise PortalTestError(f"Portal POST /wifisave returned {save_wifi_status}")
    if save_wifi_status != 200:
        _http_log(
            "portal wifi save transport did not complete cleanly; "
            f"continuing with connected-state discovery status={save_wifi_status} "
            f"body={save_wifi_body[:300]!r}"
        )
    try:
        _connected_status, portal_runtime_ip = _wait_for_portal_status_connected(ssh, timeout_s=20)
    except PortalTestError as exc:
        _http_log(
            "portal wifi save did not expose AP connected status; "
            f"continuing with discovery save_body={save_wifi_body[:300]!r} err={exc}"
        )
        portal_runtime_ip, serial_output = _finish_serial_runtime_ip_watch(serial_watch, wait_s=90)
        if portal_runtime_ip:
            _announce(f"serial runtime ip: {portal_runtime_ip}")
        else:
            tail_lines = [line.strip() for line in serial_output.splitlines() if line.strip()][-12:]
            if tail_lines:
                _http_log("serial runtime-ip watch saw no IP; tail=" + " | ".join(tail_lines))
        return portal_runtime_ip
    serial_watch.kill()
    try:
        serial_watch.communicate(timeout=5)
    except Exception:
        pass
    _announce(f"portal connected via http://{portal_runtime_ip}")
    return portal_runtime_ip


def _load_param_page(ssh: "PiSsh", *, base_url: str, timeout_s: int = 20) -> tuple[str, str]:
    deadline = time.time() + timeout_s
    last_status = 0
    last_body = ""
    while time.time() < deadline:
        try:
            param_resp = _pi_http_request(ssh, method="GET", url=base_url + "/config/mqtt", timeout_s=10)
        except Exception as exc:
            last_status = 0
            last_body = f"exc={exc}"
            time.sleep(1.0)
            continue
        param_status = int(param_resp.get("status", 0))
        param_body = str(param_resp.get("body", ""))
        last_status = param_status
        last_body = param_body
        if param_status == 200 and all(token in param_body for token in ("server", "port", "user", "mpass")):
            param_action = _extract_form_action_with_input(param_body, "server")
            return param_body, param_action
        time.sleep(1.0)
    raise PortalTestError(f"Portal GET /config/mqtt did not become ready; last status={last_status} body={last_body[:300]!r}")


def _load_sta_wifi_page(ssh: "PiSsh", *, base_url: str, timeout_s: int = 20) -> tuple[str, str]:
    deadline = time.time() + timeout_s
    last_status = 0
    last_body = ""
    while time.time() < deadline:
        try:
            wifi_resp = _pi_http_request(ssh, method="GET", url=base_url + "/0wifi", timeout_s=10)
        except Exception as exc:
            last_status = 0
            last_body = f"exc={exc}"
            time.sleep(1.0)
            continue
        wifi_status = int(wifi_resp.get("status", 0))
        wifi_body = str(wifi_resp.get("body", ""))
        last_status = wifi_status
        last_body = wifi_body
        if wifi_status == 200:
            input_names = _extract_input_names(wifi_body)
            if "s" in input_names and "p" in input_names:
                wifi_action = _extract_form_action_with_input(wifi_body, "s")
                return wifi_body, wifi_action
        time.sleep(1.0)
    raise PortalTestError(f"Portal GET /0wifi did not become ready; last status={last_status} body={last_body[:300]!r}")


def _wait_for_page_contains(ssh: "PiSsh", *, url: str, needle: str, timeout_s: int = 20) -> str:
    deadline = time.time() + timeout_s
    last = ""
    while time.time() < deadline:
        try:
            resp = _pi_http_request(ssh, method="GET", url=url, timeout_s=10)
        except Exception as exc:
            last = f"exc={exc}"
            time.sleep(1.0)
            continue
        status = int(resp.get("status", 0))
        body = str(resp.get("body", ""))
        last = body
        if status == 200 and needle in body:
            return body
        time.sleep(1.0)
    raise PortalTestError(f"{url}: missing {needle!r}; last={last[:300]!r}")


def _extract_uptime_ms(status_body: str) -> int:
    match = re.search(r"Uptime \(ms\):\s*(\d+)", status_body, flags=re.IGNORECASE)
    if not match:
        raise PortalTestError(f"Could not parse uptime from status page: {status_body[:300]!r}")
    return int(match.group(1))


def _literal_ip_from_base_url(base_url: str) -> str:
    host = urllib.parse.urlparse(base_url).hostname or ""
    if not host:
        return ""
    try:
        ipaddress.ip_address(host)
    except ValueError:
        return ""
    return host


def _resolve_http_base_url(
    ssh: "PiSsh",
    *,
    candidates: list[str],
    path: str,
    required_token: str | None = None,
    timeout_s: int = 20,
) -> str:
    deadline = time.time() + timeout_s
    last_error = ""
    while time.time() < deadline:
        for base_url in candidates:
            try:
                resp = _pi_http_request(ssh, method="GET", url=base_url + path, timeout_s=10)
            except Exception as exc:
                last_error = f"{base_url}{path}: {exc}"
                continue
            status = int(resp.get("status", 0))
            body = str(resp.get("body", ""))
            if status == 200 and (required_token is None or required_token in body):
                return base_url
            last_error = f"{base_url}{path}: status={status} body={body[:200]!r}"
        time.sleep(1.0)
    raise PortalTestError(f"Could not resolve reachable base URL for {path}: {last_error}")


def _resolve_portal_base_url(ssh: "PiSsh", *, candidates: list[str], timeout_s: int = 60) -> str:
    deadline = time.time() + timeout_s
    last_errors: dict[str, str] = {}
    while time.time() < deadline:
        for base_url in candidates:
            try:
                _load_param_page(ssh, base_url=base_url, timeout_s=5)
                return base_url
            except PortalTestError as exc:
                last_errors[base_url] = str(exc)
                continue
        time.sleep(1.0)
    detail = "; ".join(f"{base_url}: {msg}" for base_url, msg in last_errors.items())
    raise PortalTestError(f"Could not resolve reachable portal base URL: {detail}")


def _save_mqtt_params(
    ssh: "PiSsh",
    *,
    base_url: str,
    mqtt_host: str,
    mqtt_port: int,
    mqtt_user: str,
    mqtt_pass: str,
) -> dict[str, Any]:
    _param_body, param_action = _load_param_page(ssh, base_url=base_url)
    save_param_resp = _pi_http_request(
        ssh,
        method="POST",
        url=urllib.parse.urljoin(base_url + "/config/mqtt", param_action),
        form={
            "server": mqtt_host,
            "port": str(mqtt_port),
            "user": mqtt_user,
            "mpass": mqtt_pass,
            "inverter_label": "",
        },
        timeout_s=30,
    )
    save_param_status = int(save_param_resp.get("status", 0))
    if save_param_status not in (200, 302):
        raise PortalTestError(f"Portal POST /config/mqtt/save returned {save_param_status}")
    return save_param_resp


def _reboot_normal_and_tolerate_disconnect(ssh: "PiSsh", *, url: str, context: str) -> None:
    try:
        reboot_resp = _pi_http_request(ssh, method="POST", url=url, timeout_s=15)
        reboot_status = int(reboot_resp.get("status", 0))
        if reboot_status != 200:
            raise PortalTestError(f"{context} returned unexpected status={reboot_status}")
    except Exception as exc:
        _http_log(f"{context} transport did not complete cleanly: {exc}")


def _run_wifi_only_case(
    ssh: "PiSsh",
    mqtt: "MqttClient",
    *,
    iface: str,
    fw_path: Path,
    serial_port: str,
    flash_baud: str,
    wifi_ssid: str,
    wifi_pwd: str,
) -> str:
    _announce("case: fresh blank flash -> wifi only -> explicit normal reboot")
    _erase_and_flash_real_firmware(fw_path, serial_port=serial_port, baud=flash_baud)
    time.sleep(8)

    ap_ssid, wifi_action, wifi_body = _join_ap_and_load_wifi_page(ssh, mqtt, iface=iface)
    expected_build_ts = _firmware_build_ts_ms_from_filename(fw_path)
    previous_boot_payload = _latest_payload_for_topic(mqtt, f"{ap_ssid}/boot")
    previous_net_payload = _latest_payload_for_topic(mqtt, f"{ap_ssid}/status/net")
    portal_runtime_ip = _save_wifi_and_wait_connected(
        ssh,
        serial_port=serial_port,
        wifi_ssid=wifi_ssid,
        wifi_pwd=wifi_pwd,
        wifi_action=wifi_action,
        wifi_body=wifi_body,
    )
    if not portal_runtime_ip:
        try:
            portal_runtime_ip = _wait_for_runtime_ip_via_mqtt(
                mqtt,
                device_root=ap_ssid,
                expected_build_ts=expected_build_ts,
                previous_boot_payload=previous_boot_payload,
                previous_net_payload=previous_net_payload,
                timeout_s=180,
            )
        except Exception as exc:
            _http_log(f"MQTT runtime-IP fallback failed in wifi-only case: {exc}")
    try:
        portal_base_url, portal_root_body = _discover_sta_portal_base(
            ssh,
            previous_ip=portal_runtime_ip,
            hostname=ap_ssid,
            timeout_s=180,
        )
    except Exception:
        # Local builds may inject MQTT defaults at compile time. In that case a
        # blank-flash AP onboarding run becomes fully configured as soon as WiFi
        # is saved and the device can boot straight into normal runtime.
        base_url, root_page = _discover_runtime_root(
            ssh,
            previous_ip=portal_runtime_ip,
            hostname=ap_ssid,
            timeout_s=180,
        )
        _assert_normal_runtime_root_contract(root_page, "runtime root (wifi-only direct-normal path)")
        return base_url
    _assert_wifi_portal_root_contract(portal_root_body, "sta portal root (wifi-only case)")
    portal_runtime_ip = urllib.parse.urlparse(portal_base_url).hostname or portal_runtime_ip

    _reboot_normal_and_tolerate_disconnect(
        ssh,
        url=portal_base_url + "/config/reboot-normal",
        context="portal /config/reboot-normal",
    )

    base_url, root_page = _discover_runtime_root(
        ssh,
        previous_ip=portal_runtime_ip,
        hostname=ap_ssid,
        timeout_s=180,
    )
    _assert_normal_runtime_root_contract(root_page, "runtime root (wifi-only case)")
    return base_url


def _run_ap_bad_wifi_retains_saved_ssid_case(
    ssh: "PiSsh",
    mqtt: "MqttClient",
    *,
    iface: str,
    fw_path: Path,
    serial_port: str,
    flash_baud: str,
    wifi_ssid: str,
    wifi_pwd: str,
) -> None:
    _announce("case: fresh blank flash -> AP save bad wifi -> AP wifi page retains submitted ssid")
    _erase_and_flash_real_firmware(fw_path, serial_port=serial_port, baud=flash_baud)
    time.sleep(8)

    _ap_ssid, wifi_action, wifi_body = _join_ap_and_load_wifi_page(ssh, mqtt, iface=iface)
    csrf = _extract_input_value(wifi_body, "csrf")
    bad_ssid = f"{wifi_ssid}-bad-{int(time.time())}"
    save_wifi = _pi_http_request(
        ssh,
        method="POST",
        url=urllib.parse.urljoin("http://192.168.4.1/0wifi", wifi_action),
        form={"s": bad_ssid, "p": wifi_pwd, "csrf": csrf},
        timeout_s=20,
    )
    save_wifi_status = int(save_wifi.get("status", 0))
    if save_wifi_status not in (0, 200, 302):
        raise PortalTestError(f"Portal POST /wifisave returned {save_wifi_status}")

    deadline = time.time() + 15
    last_wifi_body = ""
    while time.time() < deadline:
        wifi_resp = _pi_http_request(ssh, method="GET", url="http://192.168.4.1/0wifi", timeout_s=10)
        if int(wifi_resp.get("status", 0)) != 200:
            time.sleep(1.0)
            continue
        last_wifi_body = str(wifi_resp.get("body", ""))
        try:
            saved_ssid = _extract_input_value(last_wifi_body, "s")
        except PortalTestError:
            time.sleep(1.0)
            continue
        if saved_ssid == bad_ssid:
            _assert_button_theme(last_wifi_body, "ap", "ap wifi page after bad wifi save")
            return
        time.sleep(1.0)

    _announce("bad wifi save did not remain on AP immediately; waiting for AP cycle back from wifi_config")
    _wait_for_ap_ssid_cycle(ssh, iface=iface, ssid=_ap_ssid, timeout_s=90)
    _ap_ssid_after, _wifi_action_after, last_wifi_body = _join_ap_and_load_wifi_page(ssh, mqtt, iface=iface)
    if _ap_ssid_after != _ap_ssid:
        raise PortalTestError(f"AP SSID changed across bad-wifi recovery: before={_ap_ssid!r} after={_ap_ssid_after!r}")
    saved_ssid = _extract_input_value(last_wifi_body, "s")
    if saved_ssid == bad_ssid:
        _assert_button_theme(last_wifi_body, "ap", "ap wifi page after bad wifi recovery")
        return

    raise PortalTestError(
        f"AP wifi page did not retain submitted SSID {bad_ssid!r}; last body={last_wifi_body[:300]!r}"
    )


def _run_wifi_plus_mqtt_case(
    ssh: "PiSsh",
    mqtt: "MqttClient",
    *,
    iface: str,
    fw_path: Path,
    serial_port: str,
    flash_baud: str,
    wifi_ssid: str,
    wifi_pwd: str,
    mqtt_host: str,
    mqtt_port: int,
    mqtt_user: str,
    mqtt_pass: str,
) -> str:
    _announce("case: fresh blank flash -> wifi plus mqtt -> runtime -> sta portal")
    _erase_and_flash_real_firmware(fw_path, serial_port=serial_port, baud=flash_baud)
    time.sleep(8)

    device_root, wifi_action, wifi_body = _join_ap_and_load_wifi_page(ssh, mqtt, iface=iface)
    _clear_homeassistant_topics_for_device(mqtt, device_root=device_root)
    expected_build_ts = _firmware_build_ts_ms_from_filename(fw_path)
    previous_boot_payload = _latest_payload_for_topic(mqtt, f"{device_root}/boot")
    previous_net_payload = _latest_payload_for_topic(mqtt, f"{device_root}/status/net")
    portal_runtime_ip = _save_wifi_and_wait_connected(
        ssh,
        serial_port=serial_port,
        wifi_ssid=wifi_ssid,
        wifi_pwd=wifi_pwd,
        wifi_action=wifi_action,
        wifi_body=wifi_body,
    )
    if not portal_runtime_ip:
        try:
            portal_runtime_ip = _wait_for_runtime_ip_via_mqtt(
                mqtt,
                device_root=device_root,
                expected_build_ts=expected_build_ts,
                previous_boot_payload=previous_boot_payload,
                previous_net_payload=previous_net_payload,
                timeout_s=180,
            )
        except Exception as exc:
            _http_log(f"MQTT runtime-IP fallback failed in wifi+mqtt case: {exc}")
    try:
        portal_base_url, _portal_page = _discover_sta_portal_base(
            ssh,
            previous_ip=portal_runtime_ip,
            hostname=device_root,
            timeout_s=180,
        )
        _announce(f"wifi+mqtt: discovered sta portal at {portal_base_url}")
    except Exception:
        base_url, root_page = _discover_runtime_root(
            ssh,
            previous_ip=portal_runtime_ip,
            hostname=device_root,
            timeout_s=180,
        )
        _announce(f"wifi+mqtt: fell straight into runtime at {base_url}")
        _assert_normal_runtime_root_contract(root_page, "runtime root (wifi+mqtt default path)")
        root_page = _wait_for_runtime_root_contains(ssh, base_url, "MQTT connected: 1", timeout_s=180)
        _assert_normal_runtime_root_contract(root_page, "runtime root (wifi+mqtt default path)")
        return base_url
    portal_root_body = _wait_for_page_contains(ssh, url=portal_base_url + "/", needle="/config/mqtt", timeout_s=20)
    _assert_wifi_portal_root_contract(portal_root_body, "sta portal root (wifi+mqtt case)")
    literal_portal_ip = _literal_ip_from_base_url(portal_base_url)
    if literal_portal_ip:
        portal_runtime_ip = literal_portal_ip

    save_mqtt_resp = _save_mqtt_params(
        ssh,
        base_url=portal_base_url,
        mqtt_host=mqtt_host,
        mqtt_port=mqtt_port,
        mqtt_user=mqtt_user,
        mqtt_pass=mqtt_pass,
    )
    if int(save_mqtt_resp.get("status", 0)) != 200:
        raise PortalTestError(f"/config/mqtt/save should return reboot handoff HTML once reboot is armed (got {save_mqtt_resp.get('status')})")
    _assert_reboot_handoff_contract(
        str(save_mqtt_resp.get("body", "")),
        heading="Rebooting to normal mode",
        target_mode="normal",
        context="wifi+mqtt mqtt save handoff",
    )
    _announce("wifi+mqtt: mqtt parameters saved")

    base_url, root_page = _discover_runtime_root(
        ssh,
        previous_ip=portal_runtime_ip,
        hostname=device_root,
        timeout_s=180,
    )
    _announce(f"wifi+mqtt: discovered runtime at {base_url}")
    _assert_contains(root_page, "Boot mode: normal", "runtime root")
    root_page = _wait_for_runtime_root_contains(ssh, base_url, "MQTT connected: 1", timeout_s=180)
    _assert_contains(root_page, "Boot mode: normal", "runtime root")
    _assert_button_theme(root_page, "normal", "runtime root")
    _wait_for_controller_discovery(mqtt, device_root=device_root, timeout_s=60)
    _wait_for_homeassistant_quiet(mqtt)
    runtime_serial = _start_serial_capture(serial_port=serial_port, timeout_s=90)
    try:
        _wait_for_runtime_uptime_at_least(ssh, base_url=base_url, min_uptime_ms=30000, timeout_s=60)
        _assert_runtime_root_stable_under_load(ssh, base_url=base_url)
    except Exception:
        serial_output = _finish_serial_capture(runtime_serial, wait_s=5)
        tail_lines = [line.strip() for line in serial_output.splitlines() if line.strip()][-20:]
        if tail_lines:
            _http_log("runtime under-load serial tail: " + " | ".join(tail_lines))
        raise
    else:
        _finish_serial_capture(runtime_serial, wait_s=5)
    _announce("wifi+mqtt: runtime stable under load")

    reboot_wifi_resp = _pi_http_request(ssh, method="POST", url=base_url + "/reboot/wifi", timeout_s=15)
    reboot_wifi_status = int(reboot_wifi_resp.get("status", 0))
    if reboot_wifi_status != 200:
        raise PortalTestError(f"/reboot/wifi returned unexpected status={reboot_wifi_status}")

    base_url = _rediscover_sta_portal_after_reboot_wifi(ssh, base_url, timeout_s=90)
    _announce(f"wifi+mqtt: rediscovered sta portal after /reboot/wifi at {base_url}")
    literal_portal_ip = _literal_ip_from_base_url(base_url)
    if literal_portal_ip:
        portal_runtime_ip = literal_portal_ip

    bucket_map = "State_of_Charge=ten_sec;"
    polling_page_resp = _pi_http_request(ssh, method="GET", url=base_url + "/config/polling", timeout_s=10)
    if int(polling_page_resp.get("status", 0)) != 200:
        raise PortalTestError(f"/config/polling returned unexpected status={polling_page_resp.get('status')}")
    polling_csrf = _extract_input_value(str(polling_page_resp.get("body", "")), "csrf")
    save_resp = _pi_http_request(
        ssh,
        method="POST",
        url=base_url + "/config/polling/save",
        form={
            "family": "battery",
            "page": "0",
            "poll_interval_s": "9",
            "bucket_map_full": bucket_map,
            "csrf": polling_csrf,
        },
        timeout_s=20,
    )
    save_status = int(save_resp.get("status", 0))
    if save_status not in (200, 302):
        raise PortalTestError(f"/config/polling/save returned unexpected status={save_status}")
    save_text = str(save_resp.get("body", ""))
    if "saved=1" not in save_text and "Rebooting to normal mode" not in save_text and "Runtime will now restart" not in save_text:
        _http_log(f"polling save body: {save_text[:500]}")

    _wait_for_polling_page_persisted(
        ssh,
        base_url=base_url,
        family="battery",
        page="0",
        entity_name="State_of_Charge",
        expected_bucket="ten_sec",
        expected_poll_interval_s="9",
        timeout_s=30,
    )
    _announce("wifi+mqtt: polling save persisted")

    try:
        reboot_normal_resp = _pi_http_request(ssh, method="POST", url=base_url + "/config/reboot-normal", timeout_s=15)
        reboot_normal_status = int(reboot_normal_resp.get("status", 0))
        if reboot_normal_status != 200:
            raise PortalTestError(f"/config/reboot-normal returned unexpected status={reboot_normal_status}")
    except Exception as exc:
        _http_log(f"/config/reboot-normal transport did not complete cleanly: {exc}")

    base_url, final_root_page = _discover_runtime_root(
        ssh,
        previous_ip=portal_runtime_ip,
        hostname=device_root,
        timeout_s=60,
    )
    _announce(f"wifi+mqtt: final runtime rediscovered at {base_url}")
    final_root_page = _wait_for_runtime_root_contains(ssh, base_url, "MQTT connected: 1", timeout_s=60)
    _assert_contains(final_root_page, "Boot mode: normal", "final runtime root")
    _wait_for_config_interval(
        mqtt,
        device_root=device_root,
        entity_name="State_of_Charge",
        expected_bucket="ten_sec",
        expected_poll_interval_s="9",
        timeout_s=60,
    )
    return base_url


def _run_normal_to_wifi_portal_set_mqtt_case(
    ssh: "PiSsh",
    mqtt: "MqttClient",
    *,
    iface: str,
    fw_path: Path,
    serial_port: str,
    flash_baud: str,
    wifi_ssid: str,
    wifi_pwd: str,
    mqtt_host: str,
    mqtt_port: int,
    mqtt_user: str,
    mqtt_pass: str,
) -> None:
    _announce("case: fresh blank flash -> wifi only -> normal -> sta portal -> set mqtt")
    base_url = _run_wifi_only_case(
        ssh,
        mqtt,
        iface=iface,
        fw_path=fw_path,
        serial_port=serial_port,
        flash_baud=flash_baud,
        wifi_ssid=wifi_ssid,
        wifi_pwd=wifi_pwd,
    )

    reboot_wifi_resp = _pi_http_request(ssh, method="POST", url=base_url + "/reboot/wifi", timeout_s=15)
    reboot_wifi_status = int(reboot_wifi_resp.get("status", 0))
    if reboot_wifi_status != 200:
        raise PortalTestError(f"/reboot/wifi returned unexpected status={reboot_wifi_status}")

    base_url = _rediscover_sta_portal_after_reboot_wifi(ssh, base_url, timeout_s=90)
    save_mqtt_resp = _save_mqtt_params(
        ssh,
        base_url=base_url,
        mqtt_host=mqtt_host,
        mqtt_port=mqtt_port,
        mqtt_user=mqtt_user,
        mqtt_pass=mqtt_pass,
    )
    if int(save_mqtt_resp.get("status", 0)) != 200:
        raise PortalTestError(f"/config/mqtt/save should return reboot handoff HTML once reboot is armed (got {save_mqtt_resp.get('status')})")
    _assert_reboot_handoff_contract(
        str(save_mqtt_resp.get("body", "")),
        heading="Rebooting to normal mode",
        target_mode="normal",
        context="normal->wifi portal mqtt save handoff",
    )

    final_root_page = _wait_for_runtime_root_contains(ssh, base_url, "MQTT connected: 1", timeout_s=60)
    _assert_contains(final_root_page, "Boot mode: normal", "final runtime root (sta mqtt case)")


def _run_normal_to_wifi_portal_bad_wifi_falls_back_to_ap_case(
    ssh: "PiSsh",
    mqtt: "MqttClient",
    *,
    iface: str,
    fw_path: Path,
    serial_port: str,
    flash_baud: str,
    wifi_ssid: str,
    wifi_pwd: str,
    mqtt_host: str,
    mqtt_port: int,
    mqtt_user: str,
    mqtt_pass: str,
) -> None:
    _announce("case: fresh blank flash -> wifi plus mqtt -> sta portal -> save bad wifi -> AP recovery cycle")
    base_url = _run_wifi_plus_mqtt_case(
        ssh,
        mqtt,
        iface=iface,
        fw_path=fw_path,
        serial_port=serial_port,
        flash_baud=flash_baud,
        wifi_ssid=wifi_ssid,
        wifi_pwd=wifi_pwd,
        mqtt_host=mqtt_host,
        mqtt_port=mqtt_port,
        mqtt_user=mqtt_user,
        mqtt_pass=mqtt_pass,
    )
    reboot_wifi_resp = _pi_http_request(ssh, method="POST", url=base_url + "/reboot/wifi", timeout_s=15)
    reboot_wifi_status = int(reboot_wifi_resp.get("status", 0))
    if reboot_wifi_status != 200:
        raise PortalTestError(f"/reboot/wifi returned unexpected status={reboot_wifi_status}")

    base_url = _rediscover_sta_portal_after_reboot_wifi(ssh, base_url, timeout_s=90)
    wifi_body, wifi_action = _load_sta_wifi_page(ssh, base_url=base_url, timeout_s=30)
    wifi_csrf = _extract_input_value(wifi_body, "csrf")
    bad_ssid = f"{wifi_ssid}-bad-{int(time.time())}"
    save_wifi = _pi_http_request(
        ssh,
        method="POST",
        url=urllib.parse.urljoin(base_url + "/0wifi", wifi_action),
        form={"s": bad_ssid, "p": wifi_pwd, "csrf": wifi_csrf},
        timeout_s=20,
    )
    save_wifi_status = int(save_wifi.get("status", 0))
    if save_wifi_status not in (200, 302):
        raise PortalTestError(f"Portal POST /wifisave returned {save_wifi_status}")

    saved_wifi_page = _pi_http_request(ssh, method="GET", url=base_url + "/0wifi?saved=1", timeout_s=10)
    if int(saved_wifi_page.get("status", 0)) != 200:
        raise PortalTestError(f"STA portal saved WiFi page returned {saved_wifi_page.get('status')}")
    _assert_contains(str(saved_wifi_page.get("body", "")), "applied on the next reboot", "sta wifi saved page")

    polling_page = _pi_http_request(ssh, method="GET", url=base_url + "/config/polling", timeout_s=10)
    if int(polling_page.get("status", 0)) != 200:
        raise PortalTestError(f"Polling page not reachable after STA WiFi save: {polling_page.get('status')}")
    polling_body = str(polling_page.get("body", ""))
    _assert_contains(polling_body, "Polling", "sta portal after wifi save")
    if 'href="/config/polling?family=battery&page=0">Battery</a>' not in polling_body and \
       'href="/config/polling?family=battery&page=0">[Battery</a>' not in polling_body:
        raise PortalTestError("sta portal polling nav did not render human-readable family labels")

    _reboot_normal_and_tolerate_disconnect(
        ssh,
        url=base_url + "/config/reboot-normal",
        context="sta portal /config/reboot-normal after wifi save",
    )

    ap_ssid, _wifi_action, _wifi_body = _join_ap_and_load_wifi_page(ssh, mqtt, iface=iface)
    status_resp = _pi_http_request(ssh, method="GET", url="http://192.168.4.1/status", timeout_s=10)
    if int(status_resp.get("status", 0)) != 200:
        raise PortalTestError(f"AP fallback /status returned {status_resp.get('status')}")
    status_body = str(status_resp.get("body", ""))
    _assert_contains(status_body, "Boot mode: ap_config", "ap fallback status")
    _assert_contains(status_body, bad_ssid, "ap fallback target ssid")
    mqtt_page, _mqtt_action = _load_param_page(ssh, base_url="http://192.168.4.1", timeout_s=20)
    _assert_contains(mqtt_page, "MQTT Setup", "ap fallback mqtt page")
    polling_resp = _pi_http_request(ssh, method="GET", url="http://192.168.4.1/config/polling", timeout_s=10)
    if int(polling_resp.get("status", 0)) != 200:
        raise PortalTestError(f"AP fallback polling page returned {polling_resp.get('status')}")
    polling_fallback_body = str(polling_resp.get("body", ""))
    _assert_contains(polling_fallback_body, "Polling", "ap fallback polling page")
    if 'href="/config/polling?family=battery&page=0">Battery</a>' not in polling_fallback_body and \
       'href="/config/polling?family=battery&page=0">[Battery</a>' not in polling_fallback_body:
        raise PortalTestError("ap fallback polling nav did not render human-readable family labels")
    _wait_for_page_contains(ssh, url="http://192.168.4.1/", needle="/config/update", timeout_s=20)

    _announce("waiting for AP idle timeout to cycle back through normal retry")
    ap_idle_recovery_wait_s = int(os.environ.get("A2M_AP_IDLE_RECOVERY_WAIT_S", "0") or "0")
    if ap_idle_recovery_wait_s > 0:
        time.sleep(ap_idle_recovery_wait_s)
    else:
        _wait_for_ap_ssid_cycle(ssh, iface=iface, ssid=ap_ssid, timeout_s=420)
    ap_ssid, _wifi_action, _wifi_body = _join_ap_and_load_wifi_page(ssh, mqtt, iface=iface)
    status_resp = _pi_http_request(ssh, method="GET", url="http://192.168.4.1/status", timeout_s=10)
    if int(status_resp.get("status", 0)) != 200:
        raise PortalTestError(f"AP recovery-cycle /status returned {status_resp.get('status')}")
    status_body = str(status_resp.get("body", ""))
    _assert_contains(status_body, "Boot mode: ap_config", "ap recovery-cycle status")
    _assert_contains(status_body, bad_ssid, "ap recovery-cycle target ssid")
    if ap_idle_recovery_wait_s > 0:
        recovery_uptime_ms = _extract_uptime_ms(status_body)
        max_recovery_uptime_ms = int(
            os.environ.get(
                "A2M_AP_IDLE_RECOVERY_MAX_UPTIME_MS",
                str(max(10000, max(1, ap_idle_recovery_wait_s - 20) * 1000)),
            )
        )
        if recovery_uptime_ms > max_recovery_uptime_ms:
            raise PortalTestError(
                "AP recovery-cycle uptime was too high to prove reboot: "
                f"uptime_ms={recovery_uptime_ms} threshold_ms={max_recovery_uptime_ms}"
            )

    # Keep the final portal case self-cleaning. This case intentionally stages bad
    # WiFi to prove AP fallback, but the suite should finish with the device back on
    # the normal LAN so follow-up checks and manual access do not inherit AP mode.
    portal_runtime_ip = _save_wifi_and_wait_connected(
        ssh,
        serial_port=serial_port,
        wifi_ssid=wifi_ssid,
        wifi_pwd=wifi_pwd,
        wifi_action=_wifi_action,
        wifi_body=_wifi_body,
    )
    recovered_base_url, recovered_root = _discover_runtime_root(
        ssh,
        previous_ip=portal_runtime_ip,
        hostname=ap_ssid,
        timeout_s=180,
    )
    _assert_contains(recovered_root, "Alpha2MQTT Control", "runtime root after AP fallback recovery")
    recovered_root = _wait_for_runtime_root_contains(
        ssh,
        recovered_base_url,
        "MQTT connected: 1",
        timeout_s=180,
    )
    _assert_contains(recovered_root, "Boot mode: normal", "runtime root after AP fallback recovery")


def main() -> int:
    global VERBOSE, TRACE_HTTP, TRACE_SSH

    json_cfg_path = _default_json_file()
    run_cfg = _load_json_run_defaults(json_cfg_path)
    _load_json_file_defaults(json_cfg_path)
    _load_env_file_defaults(_default_env_file())
    _load_secrets_defaults(_default_secrets_file())

    default_verbose = _env_bool("E2E_VERBOSE", _as_bool(run_cfg.get("verbose", False)))
    default_trace_http = _env_bool("E2E_TRACE_HTTP", _as_bool(run_cfg.get("trace_http", False)))
    default_trace_ssh = _env_bool("E2E_TRACE_SSH", _as_bool(run_cfg.get("trace_ssh", False)))
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

    ap = argparse.ArgumentParser(description="Real-device captive portal verification")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--trace-http", action="store_true")
    ap.add_argument("--trace-ssh", action="store_true")
    ap.add_argument("--list-cases", action="store_true", help="List available test cases and exit.")
    ap.add_argument("--case", action="append", dest="cases", default=[], help="Run only the named case.")
    ap.add_argument("--from-case", default="", help="Run starting from the named case.")
    ap.add_argument("--serial-port", default=os.environ.get("A2M_SERIAL_PORT", "/dev/ttyUSB0"))
    ap.add_argument("--flash-baud", default=os.environ.get("A2M_FLASH_BAUD", "460800"))
    ap.add_argument("--pi-wifi-iface", default=os.environ.get("PI_WIFI_IFACE", "wlan0"))
    args = ap.parse_args()
    if args.list_cases:
        for name in CASE_ORDER:
            print(name)
        return 0

    selected_cases = args.cases if args.cases else default_cases
    from_case = args.from_case.strip() if args.from_case else default_from_case
    for name in selected_cases:
        if name not in CASE_ORDER:
            raise PortalTestError(f"Unknown case: {name!r}")
    if from_case and from_case not in CASE_ORDER:
        raise PortalTestError(f"Unknown from-case: {from_case!r}")

    VERBOSE = bool(args.verbose or default_verbose)
    TRACE_HTTP = bool(args.trace_http or default_trace_http)
    TRACE_SSH = bool(args.trace_ssh or default_trace_ssh)

    mqtt_host = _require_env("MQTT_HOST")
    mqtt_port = int(os.environ.get("MQTT_PORT", "1883"))
    mqtt_user = _require_env("MQTT_USER")
    mqtt_pass = _read_mqtt_password()

    pi_host = _require_env("PI_HOSTNAME")
    pi_user = _require_env("PI_USER")
    pi_ssh_pwd = _require_env("PI_SSH_PWD")
    wifi_ssid = _require_env("WIFI_SSID")
    wifi_pwd = _require_env("WIFI_PWD")

    fw_path = _latest_real_firmware_path()
    ssh = PiSsh(pi_host, pi_user, pi_ssh_pwd)
    mqtt = MqttClient(mqtt_host, mqtt_port, mqtt_user, mqtt_pass)
    try:
        mqtt.connect()
        cases: list[tuple[str, Callable[[], None]]] = [
            (
                "wifi_only",
                lambda: _run_wifi_only_case(
                    ssh,
                    mqtt,
                    iface=args.pi_wifi_iface,
                    fw_path=fw_path,
                    serial_port=args.serial_port,
                    flash_baud=args.flash_baud,
                    wifi_ssid=wifi_ssid,
                    wifi_pwd=wifi_pwd,
                ),
            ),
            (
                "ap_bad_wifi_retains_saved_ssid",
                lambda: _run_ap_bad_wifi_retains_saved_ssid_case(
                    ssh,
                    mqtt,
                    iface=args.pi_wifi_iface,
                    fw_path=fw_path,
                    serial_port=args.serial_port,
                    flash_baud=args.flash_baud,
                    wifi_ssid=wifi_ssid,
                    wifi_pwd=wifi_pwd,
                ),
            ),
            (
                "wifi_plus_mqtt",
                lambda: _run_wifi_plus_mqtt_case(
                    ssh,
                    mqtt,
                    iface=args.pi_wifi_iface,
                    fw_path=fw_path,
                    serial_port=args.serial_port,
                    flash_baud=args.flash_baud,
                    wifi_ssid=wifi_ssid,
                    wifi_pwd=wifi_pwd,
                    mqtt_host=mqtt_host,
                    mqtt_port=mqtt_port,
                    mqtt_user=mqtt_user,
                    mqtt_pass=mqtt_pass,
                ),
            ),
            (
                "normal_to_wifi_portal_set_mqtt",
                lambda: _run_normal_to_wifi_portal_set_mqtt_case(
                    ssh,
                    mqtt,
                    iface=args.pi_wifi_iface,
                    fw_path=fw_path,
                    serial_port=args.serial_port,
                    flash_baud=args.flash_baud,
                    wifi_ssid=wifi_ssid,
                    wifi_pwd=wifi_pwd,
                    mqtt_host=mqtt_host,
                    mqtt_port=mqtt_port,
                    mqtt_user=mqtt_user,
                    mqtt_pass=mqtt_pass,
                ),
            ),
            (
                "normal_to_wifi_portal_bad_wifi_recovery_cycle",
                lambda: _run_normal_to_wifi_portal_bad_wifi_falls_back_to_ap_case(
                    ssh,
                    mqtt,
                    iface=args.pi_wifi_iface,
                    fw_path=fw_path,
                    serial_port=args.serial_port,
                    flash_baud=args.flash_baud,
                    wifi_ssid=wifi_ssid,
                    wifi_pwd=wifi_pwd,
                    mqtt_host=mqtt_host,
                    mqtt_port=mqtt_port,
                    mqtt_user=mqtt_user,
                    mqtt_pass=mqtt_pass,
                ),
            ),
        ]
        case_map = {name: fn for name, fn in cases}
        ordered_names = [name for name, _fn in cases]
        if from_case:
            ordered_names = ordered_names[ordered_names.index(from_case):]
        if selected_cases:
            selected_set = set(selected_cases)
            ordered_names = [name for name in ordered_names if name in selected_set]
        if not ordered_names:
            raise PortalTestError("No cases selected to run")

        for name in ordered_names:
            case_map[name]()

        _announce("OK")
        return 0
    finally:
        mqtt.close()
        ssh.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except PortalTestError as exc:
        _announce(f"FAIL: {exc}")
        raise SystemExit(2)
