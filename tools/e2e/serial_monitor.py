#!/usr/bin/env python3
"""
Purpose: Provide a long-running serial sidecar for the E2E runner.
Responsibilities: Keep one raw serial log for the whole run, reconnect across
device resets, and emit compact parsed milestone events to stdout as JSON lines.
Invariants: Raw serial stays in the sidecar file; stdout carries only structured
control/event records for the parent runner to consume.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import stat
import sys
import time
from pathlib import Path
from typing import Any, Optional

import serial


HEAP_RE = re.compile(
    r"(?:(?P<uptime_ms>\d+)\s+)?Heap (?P<label>[^:]+): "
    r"free=(?P<free>\d+)(?: max=(?P<max>\d+))?(?: frag=(?P<frag>\d+))?"
)
HEAP_TAGGED_RE = re.compile(
    r"free=(?P<free>\d+)(?: max=(?P<max>\d+))?(?: frag=(?P<frag>\d+))?"
    r"\s+tag=(?P<tag>[a-z0-9_]+)\s+at_ms=(?P<at_ms>\d+)"
)
MQTT_START_RE = re.compile(r"mqttReconnect attempt (?P<attempt>\d+) start @ (?P<start_ms>\d+) ms")
MQTT_SUCCESS_RE = re.compile(r"mqttReconnect attempt (?P<attempt>\d+) succeeded after (?P<elapsed_ms>\d+) ms")
PORT_OPEN_RETRY_S = 0.5
EXPLICIT_BOOT_LABEL_PHASES = {
    "very-early": 0,
    "boot": 1,
}
INITIAL_BOOT_CANDIDATE_LABELS = set(EXPLICIT_BOOT_LABEL_PHASES.keys()) | {
    "boot",
    "after-pref-read",
    "pre-wifi",
    "after WiFi",
    "after MQTT payload",
    "after MQTT connect",
    "before RS485 init",
    "after RS485 init",
    "sta-portal-entry",
    "ap-portal-entry",
}
HEAP_TAG_LABELS = {
    "very_early": "very-early",
    "boot": "boot",
    "after_pref_read": "after-pref-read",
    "pre_wifi": "pre-wifi",
    "after_wifi": "after WiFi",
    "after_mqtt_payload": "after MQTT payload",
    "before_wifi_guard": "before WiFi guard",
    "after_wifi_guard": "after WiFi guard",
    "after_mqtt_connect": "after MQTT connect",
    "before_rs485_init": "before RS485 init",
    "after_rs485_init": "after RS485 init",
    "sta_portal_entry": "sta-portal-entry",
    "ap_portal_entry": "ap-portal-entry",
}
ESP8266_SERIAL_BAUD = 115200


def _emit(payload: dict[str, Any]) -> None:
    sys.stdout.write(json.dumps(payload, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def _to_int(raw: Optional[str]) -> Optional[int]:
    if raw is None or raw == "":
        return None
    try:
        return int(raw)
    except ValueError:
        return None


def _parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Long-running serial monitor for the E2E runner")
    ap.add_argument("--port", required=True)
    ap.add_argument("--raw-log")
    ap.add_argument("--baud", type=int, default=ESP8266_SERIAL_BAUD)
    ap.add_argument("--startup-timeout", type=float, default=10.0)
    ap.add_argument("--check-only", action="store_true", help="Verify no other process already owns the serial port and exit.")
    args = ap.parse_args()
    if not args.check_only and not args.raw_log:
        ap.error("--raw-log is required unless --check-only is set")
    return args


def _proc_cmdline(pid: int) -> str:
    cmdline_path = Path("/proc") / str(pid) / "cmdline"
    try:
        raw = cmdline_path.read_bytes()
    except OSError:
        raw = b""
    text = raw.replace(b"\x00", b" ").decode("utf-8", errors="replace").strip()
    if text:
        return text[:200]
    comm_path = Path("/proc") / str(pid) / "comm"
    try:
        return comm_path.read_text(encoding="utf-8", errors="replace").strip()[:200]
    except OSError:
        return "unknown"


def _find_port_owners(port: str, *, skip_pid: Optional[int] = None) -> list[dict[str, Any]]:
    port_stat = os.stat(port)
    owners: list[dict[str, Any]] = []
    proc_root = Path("/proc")
    for proc_entry in proc_root.iterdir():
        if not proc_entry.name.isdigit():
            continue
        pid = int(proc_entry.name)
        if skip_pid is not None and pid == skip_pid:
            continue
        fd_dir = proc_entry / "fd"
        try:
            fd_entries = list(fd_dir.iterdir())
        except OSError:
            continue
        for fd_entry in fd_entries:
            try:
                fd_stat = fd_entry.stat()
            except OSError:
                continue
            if not stat.S_ISCHR(fd_stat.st_mode):
                continue
            if fd_stat.st_rdev != port_stat.st_rdev:
                continue
            owners.append({"pid": pid, "cmd": _proc_cmdline(pid)})
            break
    owners.sort(key=lambda item: int(item.get("pid", 0)))
    return owners


def _format_owner_detail(port: str, owners: list[dict[str, Any]]) -> str:
    rendered = []
    for owner in owners[:5]:
        rendered.append(f"pid={owner['pid']} cmd={owner['cmd']}")
    if len(owners) > 5:
        rendered.append(f"+{len(owners) - 5} more")
    return f"serial port {port} already open by " + "; ".join(rendered)


def _open_serial(*, port: str, baud: int) -> serial.Serial:
    # Keep tty ownership inside the long-lived sidecar so the runner does not
    # churn the adapter state between reboots.
    ser = serial.Serial(
        port=port,
        baudrate=baud,
        timeout=0.25,
        rtscts=False,
        dsrdtr=False,
        xonxoff=False,
    )
    try:
        ser.dtr = False
        ser.rts = False
    except Exception:
        pass
    return ser


def main() -> int:
    args = _parse_args()
    try:
        owners = _find_port_owners(args.port, skip_pid=os.getpid())
    except Exception as exc:
        detail = str(exc).strip() or f"could not inspect serial port {args.port}"
        status = {
            "type": "port_check",
            "status": "error",
            "port": args.port,
            "detail": detail,
        }
        _emit(status)
        return 4 if args.check_only else 2
    if owners:
        payload = {
            "type": "port_check",
            "status": "busy",
            "port": args.port,
            "owners": owners,
            "detail": _format_owner_detail(args.port, owners),
        }
        _emit(payload)
        return 3 if args.check_only else 2
    if args.check_only:
        _emit({"type": "port_check", "status": "idle", "port": args.port})
        return 0

    raw_log_path = Path(args.raw_log)
    raw_log_path.parent.mkdir(parents=True, exist_ok=True)

    startup_deadline = time.monotonic() + max(0.1, float(args.startup_timeout))
    current_boot_count = 0
    mqtt_attempt_starts: dict[int, int] = {}
    open_count = 0
    last_open_error = ""
    current_boot_explicit_phase: Optional[int] = None

    with raw_log_path.open("a", encoding="utf-8", errors="replace") as raw_log:
        while True:
            try:
                ser = _open_serial(port=args.port, baud=args.baud)
            except Exception as exc:
                last_open_error = str(exc).strip() or "serial open failed"
                if open_count == 0 and time.monotonic() >= startup_deadline:
                    _emit(
                        {
                            "type": "startup",
                            "status": "error",
                            "port": args.port,
                            "baud": args.baud,
                            "detail": last_open_error,
                        }
                    )
                    return 2
                time.sleep(PORT_OPEN_RETRY_S)
                continue

            open_count += 1
            if open_count == 1:
                _emit(
                    {
                        "type": "startup",
                        "status": "ready",
                        "port": args.port,
                        "baud": args.baud,
                        "raw_log": str(raw_log_path),
                    }
                )
            else:
                _emit(
                    {
                        "type": "port_reopen",
                        "port": args.port,
                        "baud": args.baud,
                        "open_count": open_count,
                    }
                )

            buffered = ""
            try:
                while True:
                    try:
                        chunk = ser.read(512)
                    except serial.SerialException as exc:
                        detail = str(exc)
                        if "device reports readiness to read but returned no data" in detail:
                            time.sleep(0.05)
                            continue
                        raise
                    if not chunk:
                        continue

                    text = chunk.decode("utf-8", errors="replace")
                    raw_log.write(text)
                    raw_log.flush()
                    buffered += text

                    while "\n" in buffered:
                        line, buffered = buffered.split("\n", 1)
                        line = line.rstrip("\r")
                        if not line:
                            continue

                        heap_match = HEAP_RE.search(line)
                        heap_tag_match = HEAP_TAGGED_RE.search(line)
                        if heap_match or heap_tag_match:
                            label = ""
                            uptime_ms: Optional[int] = None
                            free = None
                            max_block = None
                            frag = None
                            if heap_match:
                                label = str(heap_match.group("label")).strip()
                                uptime_ms = _to_int(heap_match.group("uptime_ms"))
                                free = _to_int(heap_match.group("free"))
                                max_block = _to_int(heap_match.group("max"))
                                frag = _to_int(heap_match.group("frag"))
                            if heap_tag_match:
                                tag = str(heap_tag_match.group("tag")).strip()
                                label = HEAP_TAG_LABELS.get(tag, tag)
                                uptime_ms = _to_int(heap_tag_match.group("at_ms"))
                                free = _to_int(heap_tag_match.group("free"))
                                max_block = _to_int(heap_tag_match.group("max"))
                                frag = _to_int(heap_tag_match.group("frag"))
                            is_new_boot = False
                            explicit_phase = EXPLICIT_BOOT_LABEL_PHASES.get(label)
                            # Only explicit boot markers advance boot_count after startup.
                            # Some later boot phases are replayed with earlier uptime_ms
                            # values, so non-boot markers cannot safely imply a reboot.
                            if current_boot_count == 0:
                                if label in INITIAL_BOOT_CANDIDATE_LABELS:
                                    is_new_boot = True
                            elif explicit_phase is not None:
                                if current_boot_explicit_phase is None or explicit_phase <= current_boot_explicit_phase:
                                    is_new_boot = True
                            if is_new_boot:
                                current_boot_count += 1
                                current_boot_explicit_phase = None
                            if explicit_phase is not None:
                                current_boot_explicit_phase = explicit_phase
                            heap_payload = {
                                "type": "heap",
                                "label": label,
                                "boot_count": current_boot_count,
                                "uptime_ms": uptime_ms,
                                "free": free,
                                "max": max_block,
                                "frag": frag,
                            }
                            _emit(
                                heap_payload
                            )
                            if label == "after MQTT connect":
                                _emit(
                                    {
                                        "type": "mqtt_connect",
                                        "boot_count": current_boot_count,
                                        "uptime_ms": uptime_ms,
                                    }
                                )
                            continue

                        mqtt_start = MQTT_START_RE.search(line)
                        if mqtt_start:
                            attempt = int(mqtt_start.group("attempt"))
                            mqtt_attempt_starts[attempt] = int(mqtt_start.group("start_ms"))
                            _emit(
                                {
                                    "type": "mqtt_attempt_start",
                                    "boot_count": current_boot_count,
                                    "attempt": attempt,
                                    "start_ms": mqtt_attempt_starts[attempt],
                                }
                            )
                            continue

                        mqtt_success = MQTT_SUCCESS_RE.search(line)
                        if mqtt_success:
                            attempt = int(mqtt_success.group("attempt"))
                            elapsed_ms = int(mqtt_success.group("elapsed_ms"))
                            start_ms = mqtt_attempt_starts.get(attempt)
                            uptime_ms = (start_ms + elapsed_ms) if start_ms is not None else None
                            _emit(
                                {
                                    "type": "mqtt_connect",
                                    "boot_count": current_boot_count,
                                    "attempt": attempt,
                                    "start_ms": start_ms,
                                    "elapsed_ms": elapsed_ms,
                                    "uptime_ms": uptime_ms,
                                }
                            )
                            continue
            except KeyboardInterrupt:
                return 130
            except Exception as exc:
                _emit(
                    {
                        "type": "port_lost",
                        "port": args.port,
                        "baud": args.baud,
                        "detail": str(exc),
                    }
                )
            finally:
                try:
                    ser.close()
                except Exception:
                    pass

            time.sleep(PORT_OPEN_RETRY_S)


if __name__ == "__main__":
    raise SystemExit(main())
