#!/usr/bin/env python3
"""
Decode captured ESP8266 panic addresses against a selected firmware ELF.

Purpose:
- Keep the outer docker exec command shape fixed for repeated panic decoding.
- Resolve the xtensa addr2line tool inside arduino-cli-build.
- Decode a curated address list against the matching real/stub ELF build.

Key invariants:
- Edit this file to change the build timestamp, variant, or addresses.
- Run it only inside the arduino-cli-build container where the ESP8266 toolchain exists.
- Output is plain addr2line text so crash triage stays easy to diff.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys


BUILD_TS = "1773140478308"
BUILD_VARIANT = "stub"
ADDRESSES = [
    "0x40223a62",
    "0x40223aea",
    "0x402163dc",
    "0x40227119",
    "0x4021db84",
    "0x4020d77d",
    "0x4020f224",
    "0x4022a62d",
    "0x4020e7a7",
    "0x40205c68",
    "0x4020b04e",
    "0x4020f287",
    "0x40216a8e",
    "0x40215f0e",
    "0x40226ee4",
    "0x4020f347",
    "0x4021406c",
    "0x40223b6b",
]

FIRMWARE_DIR = pathlib.Path("/project/Alpha2MQTT/build/firmware")
TOOLCHAIN_ROOT = pathlib.Path("/root/.arduino15")


def find_addr2line() -> pathlib.Path:
    matches = sorted(TOOLCHAIN_ROOT.glob("packages/esp8266/tools/**/xtensa-lx106-elf-addr2line"))
    if not matches:
        raise FileNotFoundError("xtensa-lx106-elf-addr2line not found under /root/.arduino15")
    return matches[-1]


def main() -> int:
    elf_path = FIRMWARE_DIR / f"Alpha2MQTT_{BUILD_TS}_{BUILD_VARIANT}.elf"
    if not elf_path.is_file():
        print(f"panic decode: missing ELF: {elf_path}", file=sys.stderr)
        return 1
    if not ADDRESSES:
        print("panic decode: no addresses configured", file=sys.stderr)
        return 1

    addr2line = find_addr2line()
    cmd = [str(addr2line), "-e", str(elf_path), "-f", "-C", *ADDRESSES]

    print(f"panic decode: build_ts={BUILD_TS} variant={BUILD_VARIANT}")
    print(f"panic decode: elf={elf_path}")
    print(f"panic decode: tool={addr2line}")
    print("panic decode: addresses=" + " ".join(ADDRESSES))

    completed = subprocess.run(cmd, check=False, text=True, capture_output=True)
    if completed.stdout:
        sys.stdout.write(completed.stdout)
    if completed.stderr:
        sys.stderr.write(completed.stderr)
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
