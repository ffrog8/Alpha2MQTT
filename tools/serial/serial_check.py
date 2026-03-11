#!/usr/bin/env python3
"""
Serial monitor helper for the fixed arduino-cli-build command shape.

Purpose:
- Open the ESP serial device inside arduino-cli-build.
- Hunt for readable text across likely baud rates.
- Keep streaming after baud detection so crash/reset paths are captured.

Key invariants:
- The outer docker exec command shape stays constant.
- All tuning happens by editing this file only.
- Exit non-zero if no readable serial text is found.
"""

from __future__ import annotations

import pathlib
import sys
import time

import serial


SERIAL_DEVICE = "/dev/ttyUSB0"
SERIAL_TIMEOUT_S = 0.25
TOTAL_WINDOW_S = 60.0
PER_BAUD_WINDOW_S = 12.0
IDLE_LIMIT_S = 4.0
FOLLOW_WINDOW_S = 180.0
FOLLOW_IDLE_LIMIT_S = 20.0
BAUD_CANDIDATES = (9600, 74880, 115200)
GOOD_TOKENS = (
    "boot_",
    "RS485",
    "Heap",
    "Exception",
    "Config",
    "MQTT",
    "Alpha2MQTT",
    "connected",
    "online",
)
PANIC_TOKENS = (
    "Exception",
    "epc1=",
    "stack",
    "Fatal exception",
    "Soft WDT reset",
    "Reset reason",
)
LOG_DIR = pathlib.Path("/project/tools/serial/logs")


def score_text(text: str) -> int:
    if not text:
        return 0
    printable = sum(1 for ch in text if ch.isprintable() or ch in "\r\n\t")
    score = printable
    score -= text.count("\x00") * 4
    lowered = text.lower()
    for token in GOOD_TOKENS:
        if token.lower() in lowered:
            score += 200
    return score


def capture_at_baud(baud: int) -> tuple[int, str]:
    try:
        ser = serial.Serial(SERIAL_DEVICE, baud, timeout=SERIAL_TIMEOUT_S)
    except Exception as exc:
        return (-10_000, f"serial open failed at {baud}: {exc}\n")

    chunks: list[bytes] = []
    start = time.monotonic()
    last_data = start

    try:
        while True:
            now = time.monotonic()
            if (now - start) >= PER_BAUD_WINDOW_S:
                break
            if chunks and (now - last_data) >= IDLE_LIMIT_S:
                break

            chunk = ser.read(512)
            if not chunk:
                continue
            chunks.append(chunk)
            last_data = time.monotonic()
    finally:
        ser.close()

    text = b"".join(chunks).decode("utf-8", errors="replace")
    return (score_text(text), text)


def follow_selected_baud(baud: int) -> tuple[bool, str]:
    ser = serial.Serial(SERIAL_DEVICE, baud, timeout=SERIAL_TIMEOUT_S)
    start = time.monotonic()
    last_data = start
    panic_seen = False
    chunks: list[bytes] = []

    try:
        while True:
            now = time.monotonic()
            if (now - start) >= FOLLOW_WINDOW_S:
                break
            if chunks and (now - last_data) >= FOLLOW_IDLE_LIMIT_S:
                break

            chunk = ser.read(512)
            if not chunk:
                continue

            chunks.append(chunk)
            last_data = time.monotonic()
            text = chunk.decode("utf-8", errors="replace")
            sys.stdout.write(text)
            sys.stdout.flush()
            lowered = text.lower()
            if any(token.lower() in lowered for token in PANIC_TOKENS):
                panic_seen = True
    finally:
        ser.close()

    return panic_seen, b"".join(chunks).decode("utf-8", errors="replace")


def write_log(baud: int, text: str) -> pathlib.Path:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    ts = time.strftime("%Y%m%d_%H%M%S", time.gmtime())
    path = LOG_DIR / f"serial_{ts}_{baud}.log"
    path.write_text(text, encoding="utf-8", errors="replace")
    return path


def main() -> int:
    print(f"serial monitor: device={SERIAL_DEVICE} total_window_s={TOTAL_WINDOW_S} baud_candidates={BAUD_CANDIDATES}")
    best_score = -10_000
    best_baud = 0
    best_text = ""

    deadline = time.monotonic() + TOTAL_WINDOW_S
    while time.monotonic() < deadline:
        for baud in BAUD_CANDIDATES:
            print(f"\n--- trying baud {baud} ---")
            score, text = capture_at_baud(baud)
            preview = text[:2000]
            if preview:
                sys.stdout.write(preview)
                if not preview.endswith("\n"):
                    sys.stdout.write("\n")
            else:
                print("(no data)")
            print(f"--- baud {baud} score={score} ---")

            if score > best_score:
                best_score = score
                best_baud = baud
                best_text = text

            if score >= 200:
                print(f"\nserial monitor: selected baud={baud} score={score}")
                panic_seen, follow_text = follow_selected_baud(baud)
                full_text = text + follow_text
                log_path = write_log(baud, full_text)
                print(f"\nserial monitor: wrote {log_path}")
                if panic_seen:
                    print("serial monitor: panic/reset markers observed")
                return 0

        time.sleep(1.0)

    print(
        f"\nserial monitor: no readable output found; best_baud={best_baud} best_score={best_score}",
        file=sys.stderr,
    )
    if best_text:
        print(best_text[:4000], file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
