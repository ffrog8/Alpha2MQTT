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
from typing import Any, Optional, Tuple


class PortalTestError(Exception):
    pass


VERBOSE = False
TRACE_HTTP = False
TRACE_SSH = False


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


def _http_request(method: str, url: str, *, body: Optional[bytes] = None, headers: Optional[dict[str, str]] = None, timeout_s: int = 20) -> Tuple[int, bytes]:
    req = urllib.request.Request(url, data=body, headers=headers or {}, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout_s) as resp:
            return int(resp.getcode()), resp.read()
    except urllib.error.HTTPError as e:  # type: ignore[name-defined]
        return int(e.code), e.read()


def _latest_real_firmware_path() -> Path:
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


def _container_firmware_path(path: Path) -> str:
    rel = path.relative_to(_repo_root())
    return f"/project/{rel.as_posix()}"


def _find_esptool_path() -> str:
    out = _run_checked(
        [
            "bash",
            "-lc",
            "tail -f /dev/null | docker exec -i arduino-cli-build bash -lc 'find /root/.arduino15 -maxdepth 8 -type f | grep esptool.py | head -n 1'",
        ],
        timeout_s=30,
    )
    path = out.strip().splitlines()[-1].strip() if out.strip() else ""
    if not path:
        raise PortalTestError("Could not find esptool.py inside arduino-cli-build")
    return path


def _erase_and_flash_real_firmware(fw_path: Path, *, serial_port: str, baud: str) -> None:
    esptool = _find_esptool_path()
    fw_in_container = _container_firmware_path(fw_path)
    erase_cmd = (
        f"tail -f /dev/null | docker exec -i arduino-cli-build bash -lc "
        f"\"python3 {shlex.quote(esptool)} --chip esp8266 --port {shlex.quote(serial_port)} "
        f"--baud {shlex.quote(baud)} --before default_reset --after hard_reset erase_flash\""
    )
    flash_cmd = (
        f"tail -f /dev/null | docker exec -i arduino-cli-build bash -lc "
        f"\"python3 {shlex.quote(esptool)} --chip esp8266 --port {shlex.quote(serial_port)} "
        f"--baud {shlex.quote(baud)} --before default_reset --after hard_reset "
        f"write_flash 0x0 {shlex.quote(fw_in_container)}\""
    )
    _announce(f"erase flash on {serial_port}")
    _run_checked(["bash", "-lc", erase_cmd], timeout_s=240)
    _announce(f"flash real firmware {fw_path.name}")
    _run_checked(["bash", "-lc", flash_cmd], timeout_s=240)


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
import json, subprocess, sys, time
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

last_scan = ""
last_error = ""
best = None
for _ in range(12):
    run(["nmcli", "radio", "wifi", "on"], check=False)
    run(["nmcli", "device", "wifi", "rescan", "ifname", iface], check=False)
    scan = run(["nmcli", "-t", "-f", "SSID,SIGNAL,SECURITY", "device", "wifi", "list", "ifname", iface], check=False)
    last_scan = scan
    best = None
    for raw in scan.splitlines():
        if not raw:
            continue
        parts = raw.split(":", 2)
        if len(parts) < 3:
            continue
        ssid, signal, security = parts
        if not ssid.startswith(ssid_prefix):
            continue
        try:
            score = int(signal)
        except ValueError:
            score = -1
        cand = {{"ssid": ssid, "signal": score, "security": security}}
        if best is None or cand["signal"] > best["signal"]:
            best = cand
    if best is not None:
        break
    time.sleep(5)

if best is None:
    raise SystemExit(json.dumps({{"error": "no matching AP", "scan": last_scan}}))

run(["sudo", "-S", "-p", "", "nmcli", "device", "disconnect", iface], input_text=sudo_pw + "\\n", check=False)
for _ in range(6):
    cp = subprocess.run(
        ["sudo", "-S", "-p", "", "nmcli", "device", "wifi", "connect", best["ssid"], "ifname", iface],
        input=sudo_pw + "\\n",
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if cp.returncode == 0:
        show = run(["nmcli", "-t", "-f", "GENERAL.STATE,IP4.ADDRESS,IP4.GATEWAY", "device", "show", iface], check=False)
        print(json.dumps({{"ssid": best["ssid"], "signal": best["signal"], "show": show}}))
        sys.exit(0)
    last_error = cp.stdout.strip()
    time.sleep(3)

raise SystemExit(json.dumps({{"error": last_error or "connect failed", "ssid": best["ssid"], "scan": last_scan}}))
"""
    out = ssh.run_python(script, timeout_s=90).strip()
    return json.loads(out.splitlines()[-1])


def _pi_http_request(ssh: PiSsh, *, method: str, url: str, form: Optional[dict[str, str]] = None, timeout_s: int = 20) -> dict[str, Any]:
    body_expr = "None"
    headers_expr = "{}"
    if form is not None:
        body_expr = f"urllib.parse.urlencode({json.dumps(form)}).encode('utf-8')"
        headers_expr = "{'Content-Type': 'application/x-www-form-urlencoded'}"
    script = f"""
import json, urllib.request, urllib.parse
url = {json.dumps(url)}
req = urllib.request.Request(url, data={body_expr}, headers={headers_expr}, method={json.dumps(method)})
try:
    with urllib.request.urlopen(req, timeout={int(timeout_s)}) as resp:
        body = resp.read().decode('utf-8', errors='replace')
        print(json.dumps({{"status": int(resp.getcode()), "body": body}}))
except urllib.error.HTTPError as e:
    body = e.read().decode('utf-8', errors='replace')
    print(json.dumps({{"status": int(e.code), "body": body}}))
"""
    out = ssh.run_python(script, timeout_s=timeout_s + 15).strip()
    return json.loads(out.splitlines()[-1])


def _assert_contains(text: str, needle: str, context: str) -> None:
    if needle not in text:
        raise PortalTestError(f"{context}: missing {needle!r}")


def _extract_runtime_ip_from_status(body: str) -> str:
    m = re.search(r"SSID:\s*[^<]+<br>IP:\s*([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)", body, flags=re.IGNORECASE)
    if not m:
        raise PortalTestError("Portal status page did not expose a runtime IP after WiFi connected")
    return m.group(1)


def _wait_for_portal_status_connected(ssh: PiSsh, *, timeout_s: int) -> Tuple[dict[str, Any], str]:
    deadline = time.time() + timeout_s
    last = ""
    while time.time() < deadline:
        resp = _pi_http_request(ssh, method="GET", url="http://192.168.4.1/status", timeout_s=10)
        body = str(resp.get("body", ""))
        last = body
        if "WiFi Status: Connected" in body or "STA status: Connected" in body:
            return resp, _extract_runtime_ip_from_status(body)
        time.sleep(2.0)
    raise PortalTestError(f"Timed out waiting for portal connected status. Last body snippet: {last[:300]!r}")


def _wait_for_runtime_ready(mqtt: MqttClient, *, device_root: str, expected_build_ts: int, timeout_s: int) -> Tuple[dict[str, Any], dict[str, Any]]:
    _wait_for_boot_fw_build_ts_ms(mqtt, f"{device_root}/boot", expected_build_ts, timeout_s=timeout_s)
    status = _fetch_latest_json(mqtt, f"{device_root}/status", "status", timeout_s=30)
    net = _fetch_latest_json(mqtt, f"{device_root}/status/net", "status/net", timeout_s=30)
    return status, net


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


def _wait_for_sta_portal(ssh: PiSsh, base_url: str, *, timeout_s: int) -> str:
    deadline = time.time() + timeout_s
    last = ""
    while time.time() < deadline:
        try:
            resp = _pi_http_request(ssh, method="GET", url=base_url + "/config/polling", timeout_s=10)
        except Exception as exc:
            last = f"exc={exc}"
            time.sleep(2.0)
            continue
        status = int(resp.get("status", 0))
        last = str(resp.get("body", ""))
        if status == 200 and "Polling" in last:
            return last
        time.sleep(2.0)
    raise PortalTestError(f"Timed out waiting for STA portal polling page. Last={last[:300]!r}")


def _extract_form_action(body: str) -> str:
    m = re.search(r"<form[^>]*action=['\"]([^'\"]+)['\"]", body, flags=re.IGNORECASE)
    if not m:
        raise PortalTestError("Could not find form action in HTML")
    return m.group(1)


def main() -> int:
    global VERBOSE, TRACE_HTTP, TRACE_SSH

    _load_json_file_defaults(_default_json_file())
    _load_env_file_defaults(_default_env_file())
    _load_secrets_defaults(_default_secrets_file())

    ap = argparse.ArgumentParser(description="Real-device captive portal verification")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--trace-http", action="store_true")
    ap.add_argument("--trace-ssh", action="store_true")
    ap.add_argument("--serial-port", default=os.environ.get("A2M_SERIAL_PORT", "/dev/ttyUSB0"))
    ap.add_argument("--flash-baud", default=os.environ.get("A2M_FLASH_BAUD", "460800"))
    ap.add_argument("--pi-wifi-iface", default=os.environ.get("PI_WIFI_IFACE", "wlan0"))
    args = ap.parse_args()
    VERBOSE = args.verbose
    TRACE_HTTP = args.trace_http
    TRACE_SSH = args.trace_ssh

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
    build_ts = _firmware_build_ts_ms_from_filename(fw_path)

    _erase_and_flash_real_firmware(fw_path, serial_port=args.serial_port, baud=args.flash_baud)
    time.sleep(8)

    ssh = PiSsh(pi_host, pi_user, pi_ssh_pwd)
    mqtt = MqttClient(mqtt_host, mqtt_port, mqtt_user, mqtt_pass)
    try:
        mqtt.connect()

        join = _pi_join_esp_ap(ssh, iface=args.pi_wifi_iface, ssid_prefix="Alpha2MQTT-")
        ap_ssid = str(join.get("ssid", ""))
        if not ap_ssid.startswith("Alpha2MQTT-"):
            raise PortalTestError(f"Unexpected AP SSID: {ap_ssid!r}")
        device_root = ap_ssid
        _announce(f"joined portal AP {ap_ssid}")

        root_resp = _pi_http_request(ssh, method="GET", url="http://192.168.4.1/")
        if int(root_resp.get("status", 0)) != 200:
            raise PortalTestError(f"Portal GET / returned {root_resp.get('status')}")
        root_body = str(root_resp.get("body", ""))
        _assert_contains(root_body, "/0wifi", "portal root")

        wifi_resp = _pi_http_request(ssh, method="GET", url="http://192.168.4.1/0wifi")
        if int(wifi_resp.get("status", 0)) != 200:
            raise PortalTestError(f"Portal GET /0wifi returned {wifi_resp.get('status')}")
        wifi_body = str(wifi_resp.get("body", ""))
        wifi_action = _extract_form_action(wifi_body)
        _assert_contains(wifi_body, "name='s'", "wifi form")
        _assert_contains(wifi_body, "name='p'", "wifi form")

        save_wifi = _pi_http_request(
            ssh,
            method="POST",
            url=urllib.parse.urljoin("http://192.168.4.1/0wifi", wifi_action),
            form={"s": wifi_ssid, "p": wifi_pwd},
            timeout_s=30,
        )
        if int(save_wifi.get("status", 0)) != 200:
            raise PortalTestError(f"Portal POST /wifisave returned {save_wifi.get('status')}")
        _assert_contains(str(save_wifi.get("body", "")), "Trying to connect", "wifisave response")

        _connected_status, portal_runtime_ip = _wait_for_portal_status_connected(ssh, timeout_s=60)
        portal_runtime_base = f"http://{portal_runtime_ip}"
        _announce(f"portal connected via {portal_runtime_base}")

        param_status, param_body_bytes = _http_request("GET", portal_runtime_base + "/param", timeout_s=20)
        if param_status != 200:
            raise PortalTestError(f"Portal GET /param returned {param_status}")
        param_body = param_body_bytes.decode("utf-8", errors="replace")
        param_action = _extract_form_action(param_body)
        _assert_contains(param_body, "server", "mqtt param form")
        _assert_contains(param_body, "port", "mqtt param form")
        _assert_contains(param_body, "user", "mqtt param form")
        _assert_contains(param_body, "mpass", "mqtt param form")

        save_param_status, _save_param_body = _http_request(
            "POST",
            urllib.parse.urljoin(portal_runtime_base + "/param", param_action),
            body=urllib.parse.urlencode(
                {
                "server": mqtt_host,
                "port": str(mqtt_port),
                "user": mqtt_user,
                "mpass": mqtt_pass,
                "inverter_label": "",
                }
            ).encode("utf-8"),
            headers={"Content-Type": "application/x-www-form-urlencoded"},
            timeout_s=30,
        )
        if save_param_status not in (200, 302):
            raise PortalTestError(f"Portal POST /paramsave returned {save_param_status}")

        # The portal usually schedules a reboot after MQTT params are saved; force it if still up.
        time.sleep(3)
        try:
            _http_request("POST", portal_runtime_base + "/reboot/normal", timeout_s=10)
        except Exception:
            pass

        status, net = _wait_for_runtime_ready(mqtt, device_root=device_root, expected_build_ts=build_ts, timeout_s=180)
        runtime_ip = str(net.get("ip", "")).strip()
        runtime_ssid = str(net.get("ssid", "")).strip()
        if not runtime_ip:
            raise PortalTestError("status/net did not provide device IP after onboarding")
        if runtime_ssid != wifi_ssid:
            raise PortalTestError(f"Device joined unexpected SSID after onboarding: {runtime_ssid!r}")

        base_url = f"http://{runtime_ip}"
        root_page = _wait_for_runtime_root(ssh, base_url, timeout_s=60)
        _assert_contains(root_page, "Boot mode: normal", "runtime root")

        reboot_wifi_resp = _pi_http_request(ssh, method="POST", url=base_url + "/reboot/wifi", timeout_s=15)
        reboot_wifi_status = int(reboot_wifi_resp.get("status", 0))
        if reboot_wifi_status != 200:
            raise PortalTestError(f"/reboot/wifi returned unexpected status={reboot_wifi_status}")

        _wait_for_sta_portal(ssh, base_url, timeout_s=60)

        bucket_map = "State_of_Charge=ten_sec;"
        save_resp = _pi_http_request(
            ssh,
            method="POST",
            url=base_url + "/config/polling/save",
            form={
                "family": "battery",
                "page": "1",
                "poll_interval_s": "13",
                "bucket_map_full": bucket_map,
            },
            timeout_s=20,
        )
        save_status = int(save_resp.get("status", 0))
        if save_status != 200:
            raise PortalTestError(f"/config/polling/save returned unexpected status={save_status}")
        save_text = str(save_resp.get("body", ""))
        if "saved=1" not in save_text and "Rebooting to normal mode" not in save_text and "Runtime will now restart" not in save_text:
            _http_log(f"polling save body: {save_text[:500]}")

        reboot_normal_resp = _pi_http_request(ssh, method="POST", url=base_url + "/reboot/normal", timeout_s=15)
        reboot_normal_status = int(reboot_normal_resp.get("status", 0))
        if reboot_normal_status != 200:
            raise PortalTestError(f"/reboot/normal returned unexpected status={reboot_normal_status}")

        _wait_for_runtime_root(ssh, base_url, timeout_s=60)
        config = _fetch_latest_json(mqtt, f"{device_root}/config", "config", timeout_s=30)
        intervals = config.get("entity_intervals", {})
        if str(config.get("entity_intervals_encoding", "")) == "bucket_map_chunks":
            # Pull chunked config if needed.
            chunk_count = int(config.get("entity_intervals_chunks", 0))
            merged: dict[str, str] = {}
            for idx in range(chunk_count):
                chunk = _fetch_latest_json(mqtt, f"{device_root}/config/entity_intervals/{idx}", f"config-chunk-{idx}", timeout_s=15)
                raw_map = str(chunk.get("active_bucket_map", ""))
                for token in raw_map.split(";"):
                    token = token.strip()
                    if not token or "=" not in token:
                        continue
                    key, value = token.split("=", 1)
                    if key.strip() and value.strip():
                        merged[key.strip()] = value.strip()
            intervals = merged
        if not isinstance(intervals, dict) or str(intervals.get("State_of_Charge", "")) != "ten_sec":
            raise PortalTestError(f"Polling config did not persist State_of_Charge=ten_sec: {intervals!r}")

        if str(status.get("presence", "")).lower() != "online":
            raise PortalTestError(f"Unexpected runtime presence after onboarding: {status!r}")

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
