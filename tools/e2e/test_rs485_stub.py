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

MQTT execution:
  MOSQUITTO_SUB        Path to mosquitto_sub (optional; auto-detected)
  MOSQUITTO_PUB        Path to mosquitto_pub (optional; auto-detected)
  MQTT_DOCKER_CONTAINER If mosquitto_sub/pub are not available locally, use docker exec in this container (default: mqtt)

Device discovery:
  DEVICE_TOPIC         Optional fixed device topic root (e.g. Alpha2MQTT-XXXXXX). If unset, script auto-discovers
                       by subscribing to '+/status/poll' and selecting a device that exposes 'rs485_backend' and
                       'ess_snapshot_attempts' in its JSON.

Optional OTA upload (one-time per run):
  DEVICE_HTTP_BASE     Base URL for the device, e.g. http://<device-ip> (required only if --ota is used)
  FIRMWARE_BIN         Firmware path for OTA upload (required only if --ota is used)
  DEVICE_REBOOT_WIFI_PATH  HTTP path to trigger "reboot into Wi-Fi portal" (optional; can be discovered from repo code)
  DEVICE_OTA_UPLOAD_PATH   HTTP path that accepts firmware upload in the portal (required for --ota unless discoverable
                           from source; many portals implement this inside the WiFiManager library, not this repo)

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
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Optional, Tuple


class E2EError(Exception):
    pass


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


@dataclass
class MqttTools:
    sub: str
    pub: str
    use_docker_container: Optional[str]


def _resolve_mqtt_tools() -> MqttTools:
    sub = os.environ.get("MOSQUITTO_SUB") or shutil.which("mosquitto_sub") or ""
    pub = os.environ.get("MOSQUITTO_PUB") or shutil.which("mosquitto_pub") or ""
    if sub and pub:
        return MqttTools(sub=sub, pub=pub, use_docker_container=None)

    container = os.environ.get("MQTT_DOCKER_CONTAINER", "mqtt")
    if not shutil.which("docker"):
        raise E2EError("No mosquitto_sub/pub found and docker not available; cannot run MQTT E2E")
    return MqttTools(sub="mosquitto_sub", pub="mosquitto_pub", use_docker_container=container)


def _run(cmd: list[str], timeout_s: int, env: Optional[dict[str, str]] = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout_s,
        env=env,
    )


class MqttClientShim:
    def __init__(self, host: str, port: int, user: str, password: str, tools: MqttTools):
        self.host = host
        self.port = port
        self.user = user
        self.password = password
        self.tools = tools

    def _base_args(self) -> list[str]:
        return ["-h", self.host, "-p", str(self.port), "-u", self.user, "-P", self.password]

    def _wrap(self, args: list[str]) -> list[str]:
        if not self.tools.use_docker_container:
            return args
        # Avoid printing password: it's passed via env into container.
        # Use robust shell quoting because payloads may contain spaces/quotes.
        return [
            "docker",
            "exec",
            "-i",
            "-e",
            "MQTT_PW",
            self.tools.use_docker_container,
            "sh",
            "-lc",
            " ".join(shlex.quote(a) for a in args),
        ]

    def publish(self, topic: str, payload: str, retain: bool = False, timeout_s: int = 5) -> None:
        args = [self.tools.pub] + self._base_args() + ["-t", topic, "-m", payload]
        if retain:
            args.append("-r")
        env = None
        if self.tools.use_docker_container:
            env = {"MQTT_PW": self.password}
            # Run the mosquitto tools *inside* the container, but still connect to MQTT_HOST.
            # (Don't force 127.0.0.1; the broker may be external to the container.)
            args = [
                self.tools.pub,
                "-h",
                self.host,
                "-p",
                str(self.port),
                "-u",
                self.user,
                "-P",
                "$MQTT_PW",
                "-t",
                topic,
                "-m",
                payload,
            ]
            if retain:
                args.append("-r")
            args = self._wrap(args)
        res = _run(args, timeout_s=timeout_s, env=env)
        if res.returncode != 0:
            raise E2EError(f"MQTT publish failed (rc={res.returncode}): {res.stdout.strip()}")

    def subscribe_once(self, topic: str, timeout_s: int = 5) -> list[Tuple[str, str]]:
        # -v includes topic in output.
        args = [self.tools.sub] + self._base_args() + ["-t", topic, "-v", "-C", "1", "-W", str(timeout_s)]
        env = None
        if self.tools.use_docker_container:
            env = {"MQTT_PW": self.password}
            args = [
                self.tools.sub,
                "-h",
                self.host,
                "-p",
                str(self.port),
                "-u",
                self.user,
                "-P",
                "$MQTT_PW",
                "-t",
                topic,
                "-v",
                "-C",
                "1",
                "-W",
                str(timeout_s),
            ]
            args = self._wrap(args)
        res = _run(args, timeout_s=timeout_s + 2, env=env)
        out = res.stdout.strip()
        if res.returncode != 0 or not out:
            return []
        lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
        pairs: list[Tuple[str, str]] = []
        for ln in lines:
            if " " not in ln:
                continue
            t, p = ln.split(" ", 1)
            pairs.append((t, p))
        return pairs


def _parse_json(payload: str) -> dict[str, Any]:
    return json.loads(payload)


def _discover_device_topic(mqtt: MqttClientShim, status_poll_suffix: str) -> str:
    configured = os.environ.get("DEVICE_TOPIC")
    if configured:
        return configured.strip()

    pairs = mqtt.subscribe_once(f"+{status_poll_suffix}", timeout_s=5)
    for topic, payload in pairs:
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


def _fetch_poll(mqtt: MqttClientShim, topic: str) -> dict[str, Any]:
    pairs = mqtt.subscribe_once(topic, timeout_s=5)
    if not pairs:
        raise E2EError(f"No MQTT message received for {topic}")
    _, payload = pairs[-1]
    return _parse_json(payload)


def _ota_upload() -> None:
    base = _require_env("DEVICE_HTTP_BASE").rstrip("/")
    fw = _require_env("FIRMWARE_BIN")
    if not Path(fw).exists():
        raise E2EError(f"Firmware not found: {fw}")

    reboot_path = os.environ.get("DEVICE_REBOOT_WIFI_PATH") or _discover_reboot_wifi_path_from_code()
    upload_path = os.environ.get("DEVICE_OTA_UPLOAD_PATH")
    if not reboot_path:
        raise E2EError(
            "OTA requested but reboot-to-wifi-config path is unknown. "
            "Set DEVICE_REBOOT_WIFI_PATH explicitly."
        )
    if not upload_path:
        raise E2EError(
            "OTA requested but firmware upload path is unknown. "
            "Set DEVICE_OTA_UPLOAD_PATH explicitly to match the portal's upload handler."
        )

    # Use curl to avoid implementing multipart in this script.
    reboot = ["curl", "-fsS", "-X", "POST", f"{base}{reboot_path}"]
    res = _run(reboot, timeout_s=10)
    if res.returncode != 0:
        raise E2EError(f"Failed to reboot into Wi-Fi portal: {res.stdout.strip()}")

    time.sleep(6)
    upload = ["curl", "-fsS", "-F", f"update=@{fw}", f"{base}{upload_path}"]
    res = _run(upload, timeout_s=120)
    if res.returncode != 0:
        raise E2EError(f"OTA upload failed: {res.stdout.strip()}")
    time.sleep(10)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ota", action="store_true", help="Perform one OTA upload before running tests (requires DEVICE_HTTP_BASE and FIRMWARE_BIN).")
    args = ap.parse_args()

    if args.ota:
        _ota_upload()

    host = _require_env("MQTT_HOST")
    port = int(os.environ.get("MQTT_PORT", "1883"))
    user = _require_env("MQTT_USER")
    password = _read_pass()
    tools = _resolve_mqtt_tools()

    mqtt = MqttClientShim(host=host, port=port, user=user, password=password, tools=tools)

    control_suffix = _discover_control_suffix_from_code()
    status_poll_suffix = _discover_status_poll_suffix_from_code()
    device_root = _discover_device_topic(mqtt, status_poll_suffix)

    poll_topic = f"{device_root}{status_poll_suffix}"
    control_topic = f"{device_root}{control_suffix}"

    print(f"[e2e] device_root={device_root}")
    print(f"[e2e] poll_topic={poll_topic}")
    print(f"[e2e] control_topic={control_topic}")

    # Verify stub backend is actually active.
    first = _fetch_poll(mqtt, poll_topic)
    if first.get("rs485_backend") != "stub":
        raise E2EError(f"Expected rs485_backend=stub but got: {first.get('rs485_backend')}")

    def set_mode(payload: str) -> None:
        mqtt.publish(control_topic, payload, retain=False, timeout_s=5)

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
