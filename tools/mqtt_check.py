#!/usr/bin/env python3
"""
Fixed-surface MQTT check for local Alpha2MQTT development.

Purpose:
- Keep one stable CLI command for repeated MQTT checks.
- Reuse the repo's existing E2E MQTT client and local config.
- Prove broker connectivity by reading one device status topic.

Key invariants:
- Change behavior by editing this file, not the command line.
- Do not print secrets.
- Exit non-zero on config, connect, discovery, or topic-read failure.
"""

from __future__ import annotations

import importlib.util
import os
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
E2E_SCRIPT = REPO_ROOT / "tools" / "e2e" / "test_rs485_stub.py"
LOCAL_CONFIG = REPO_ROOT / "tools" / "e2e" / "e2e.local.json"
STATUS_POLL_SUFFIX = "/status/poll"


def _load_e2e_module():
    spec = importlib.util.spec_from_file_location("alpha2mqtt_e2e", E2E_SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"unable to load E2E helpers from {E2E_SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    try:
        e2e = _load_e2e_module()
        e2e._load_json_file_defaults(LOCAL_CONFIG)

        host = e2e._require_env("MQTT_HOST")
        port = int(os.environ.get("MQTT_PORT", "1883"))
        user = e2e._require_env("MQTT_USER")
        password = e2e._read_pass()

        mqtt = e2e.MqttClient(host=host, port=port, user=user, password=password, client_id="mqtt-check")
        mqtt.connect()
        try:
            device_root = os.environ.get("DEVICE_TOPIC")
            if not device_root:
                device_root = e2e._discover_device_topic(mqtt, STATUS_POLL_SUFFIX)
            topic = f"{device_root}{STATUS_POLL_SUFFIX}"
            poll = e2e._fetch_poll(mqtt, topic)
        finally:
            mqtt.close()

        print(f"mqtt_check: config={LOCAL_CONFIG}")
        print(f"mqtt_check: broker=tcp://{host}:{port}")
        print(f"mqtt_check: topic={topic}")
        print(
            "mqtt_check: "
            f"backend={poll.get('rs485_backend')} "
            f"mode={poll.get('rs485_stub_mode')} "
            f"snapshot_ok={poll.get('ess_snapshot_last_ok')} "
            f"attempts={poll.get('ess_snapshot_attempts')}"
        )
        return 0
    except Exception as exc:
        print(f"mqtt_check: ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
