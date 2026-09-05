#!/usr/bin/env python3
"""commit_service.py — Bracino commit service (DN005 skeleton, issue 015).

Bench contract (DN004-shaped, single-node pass 1):

  subscribes   bracino/node/+/+/telemetry   (QoS 0)
               bracino/node/+/+/event       (QoS 1)
  writes       data/telemetry.jsonl — one JSON object per line, fsync'd
  publishes    bracino/gateway/commit  {"node_type","node_id",
                                        "capture_ms_end","ok":true}
               bracino/gateway/health  (retained, 30 s)
               bracino/gateway/time    (time-set fallback for the GW)

The watermark is published ONLY after the fsync returns: the gateway
acks a BATCH to the node only when its watermark covers the batch's
end_ms, so "ack" means "durable in the JSONL file" end to end.

Dedupe: per-node (boot_session, capture_ms) high-water marks, persisted
in data/commit_state.json. A node that retransmits a batch after a lost
ack produces zero duplicate lines (or zero NEW watermark progress).

Idempotence note for the Influx future: lines carry (node_type, node_id,
boot_session, capture_ms) — the natural Influx tag/timestamp key.
"""

import json
import os
import signal
import sys
import time

import paho.mqtt.client as mqtt

BASE = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(BASE, "data")
JSONL_PATH = os.path.join(DATA_DIR, "telemetry.jsonl")
STATE_PATH = os.path.join(DATA_DIR, "commit_state.json")

MQTT_HOST = os.environ.get("MQTT_HOST", "localhost")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
# Single shared MQTT user (t520, allow_anonymous false). Unset =
# anonymous, which keeps VM bench drills working against an auth-less
# broker with zero config.
MQTT_USER = os.environ.get("MQTT_USER") or None
MQTT_PASS = os.environ.get("MQTT_PASS") or None

HEALTH_PERIOD_S = 30
TIME_PERIOD_S = 10

running = True


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def _stop(*_):
    global running
    running = False


signal.signal(signal.SIGINT, _stop)
signal.signal(signal.SIGTERM, _stop)


def wrap_le(a, b):
    """True if a <= b in 32-bit wraparound order (node_clock_ms domain)."""
    return ((b - a) & 0xFFFFFFFF) < 0x80000000


class Commit:
    def __init__(self):
        os.makedirs(DATA_DIR, exist_ok=True)
        self.state = self._load_state()
        self.out = open(JSONL_PATH, "a", encoding="utf-8")
        self.last_commit_wall = None
        self.lines_written = 0

    def _load_state(self):
        try:
            with open(STATE_PATH, encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    def _save_state(self):
        tmp = STATE_PATH + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.state, f)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, STATE_PATH)

    def node_state(self, key):
        return self.state.setdefault(
            key, {"boot_session": None, "capture_ms": None})

    # ---- telemetry ----

    def handle_telemetry(self, node_type, node_id, msg):
        try:
            t = json.loads(msg)
        except ValueError:
            log(f"!! bad JSON on telemetry {node_type}/{node_id}")
            return
        line = {
            "kind": "telemetry",
            "node_type": node_type,
            "node_id": node_id,
            "boot_session": t.get("boot_session"),
            "capture_ms": t.get("capture_ms"),
            "node_ts": t.get("node_ts"),
            "mode": t.get("mode"),
            "relay_state": t.get("relay_state"),
            "ct_state": t.get("ct_state"),
            "t_tpo": t.get("t_tpo"),
            "t_tpu": t.get("t_tpu"),
            "t_amb": t.get("t_amb"),
            "fault_flags": t.get("fault_flags"),
        }
        st = self.node_state(f"{node_type}/{node_id}")
        cap, boot = line["capture_ms"], line["boot_session"]
        if cap is None:
            log(f"!! telemetry without capture_ms {node_type}/{node_id}")
            return
        if st["boot_session"] == boot and st["capture_ms"] is not None \
                and wrap_le(cap, st["capture_ms"]):
            return  # duplicate: cap <= cursor (retransmitted batch / replay)

        self.out.write(json.dumps(line) + "\n")
        self.out.flush()
        os.fsync(self.out.fileno())
        self.lines_written += 1
        self.last_commit_wall = time.monotonic()
        st["capture_ms"] = cap
        st["boot_session"] = boot
        self._save_state()
        self.client.publish(
            "bracino/gateway/commit",
            json.dumps({"node_type": node_type, "node_id": node_id,
                        "capture_ms_end": cap, "ok": True}),
            qos=1, retain=False)

    # ---- events ----

    def handle_event(self, node_type, node_id, msg):
        try:
            t = json.loads(msg)
        except ValueError:
            log(f"!! bad JSON on event {node_type}/{node_id}")
            return
        line = {"kind": "event", "node_type": node_type,
                "node_id": node_id, "gw_ts": time.strftime(
                    "%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                **{k: v for k, v in t.items()
                   if k not in ("kind", "node_type", "node_id")}}
        self.out.write(json.dumps(line) + "\n")
        self.out.flush()
        os.fsync(self.out.fileno())
        self.lines_written += 1
        self.last_commit_wall = time.monotonic()
        log(f"event: {line}")

    # ---- gateway-local ambient (issue 015 DN004 addendum) ----

    def handle_gw_ambient(self, msg):
        """bracino/gateway/telemetry — gateway-local measurement.
        Not node-batch data: no watermark involvement. QoS 0, non-retained,
        no dedupe needed (clean-session sub; duplicates would only come
        from a same-session duplicate publish, which doesn't happen)."""
        try:
            t = json.loads(msg)
        except ValueError:
            log("!! bad JSON on gw telemetry")
            return
        line = {"kind": "gw_ambient", **t}
        self.out.write(json.dumps(line) + "\n")
        self.out.flush()
        os.fsync(self.out.fileno())
        self.lines_written += 1
        self.last_commit_wall = time.monotonic()

    # ---- periodic publications ----

    def publish_health(self):
        age = None
        if self.last_commit_wall is not None:
            age = round(time.monotonic() - self.last_commit_wall, 1)
        # 90 s staleness = backend down per the DN004 health gate the
        # gateway applies to this same topic — report honestly.
        ok = age is None or age < 90.0
        self.client.publish(
            "bracino/gateway/health",
            json.dumps({"ok": ok, "last_commit_age_s": age,
                        "lines": self.lines_written}),
            qos=1, retain=True)

    def publish_time(self):
        ms = int(time.time() * 1000)
        self.client.publish(
            "bracino/gateway/time",
            json.dumps({"epoch_ms": ms}),
            qos=1, retain=False)

    # ---- mqtt wiring ----

    def on_connect(self, client, _u, _f, rc, _p=None):
        log(f"broker connected (rc={rc})")
        client.subscribe([
            ("bracino/node/+/+/telemetry", 0),
            ("bracino/node/+/+/event", 1),
            ("bracino/gateway/telemetry", 0),
        ])
        self.last_commit_wall = None  # gap: commit age restarts

    def on_message(self, client, _u, m):
        parts = m.topic.split("/")
        if m.topic == "bracino/gateway/telemetry":
            self.handle_gw_ambient(m.payload)
            return
        # bracino/node/<t>/<id>/<leaf>
        if len(parts) != 5 or parts[1] != "node":
            return
        try:
            node_type, node_id = int(parts[2]), int(parts[3])
        except ValueError:
            return
        if parts[4] == "telemetry":
            self.handle_telemetry(node_type, node_id, m.payload)
        elif parts[4] == "event":
            self.handle_event(node_type, node_id, m.payload)

    def run(self):
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                        client_id="bracino-commit-svc")
        if MQTT_USER:
            c.username_pw_set(MQTT_USER, MQTT_PASS)
        c.on_connect = self.on_connect
        c.on_message = self.on_message
        self.client = c
        c.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
        c.loop_start()

        last_health = last_time = 0.0
        while running:
            now = time.monotonic()
            if now - last_health >= HEALTH_PERIOD_S:
                self.publish_health()
                last_health = now
            if now - last_time >= TIME_PERIOD_S:
                self.publish_time()
                last_time = now
            time.sleep(1)

        log("shutting down")
        c.loop_stop()
        c.disconnect()
        self.out.close()


if __name__ == "__main__":
    Commit().run()
