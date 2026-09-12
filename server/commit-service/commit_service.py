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
import threading
import time
import urllib.request
from datetime import datetime

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

# Optional push channel (issue 021): set NTFY_URL (e.g. https://ntfy.sh/
# <unguessable-topic>) to get phone/desktop push on node-gone / gateway
# LWT alarms. Opt-in, no secrets: an ntfy topic IS the credential.
NTFY_URL = os.environ.get("NTFY_URL") or None

# ---- stage C: Influx projection (server/README) ----
#
# INFLUX_TOKEN set  -> tailer thread projects the JSONL into Influx.
# INFLUX_TOKEN unset-> behavior identical to pre-stage-C (topic absent).
#
# The JSONL stays the source of truth and the ack path. The tailer is a
# pure projection: its failures never touch watermarks, acks, alarms or
# the MQTT loop. Timestamps are derived from each record's own gw/node
# stamp, so a re-tail (rotation, crash between write and offset save)
# overwrites identical points — idempotent by construction.
INFLUX_URL = os.environ.get("INFLUX_URL")
INFLUX_TOKEN = os.environ.get("INFLUX_TOKEN")
INFLUX_ORG = os.environ.get("INFLUX_ORG", "bracino")
INFLUX_BUCKET = os.environ.get("INFLUX_BUCKET", "bracino")
INFLUX_TAIL_STATE = os.path.join(DATA_DIR, "influx_tail.json")
INFLUX_BATCH = 500          # max lines per POST
INFLUX_READ_CAP = 4_000_000  # max bytes pulled per cycle (backfill pacing)
INFLUX_HEALTH_S = 30


def notify_push(text):
    if not NTFY_URL:
        return
    try:
        urllib.request.urlopen(
            urllib.request.Request(NTFY_URL, data=text.encode()),
            timeout=10)
        log(f"push sent: {text[:80]}")
    except Exception as exc:  # push is best-effort; never block the loop
        log(f"!! push failed: {exc}")

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


class InfluxTailer(threading.Thread):
    """Projects the JSONL into InfluxDB (stage C). Own thread; the MQTT
    loop and ack path run untouched. Writes are idempotent: timestamps
    come from the records themselves, so any overlap re-writes identical
    points. On write failure the offset is NOT advanced — we retry with
    backoff and JSONL backpressure is absorbed by the file, not memory."""

    def __init__(self, svc):
        super().__init__(daemon=True, name="influx-tailer")
        self.svc = svc
        self.url = (f"{INFLUX_URL}/api/v2/write?org={INFLUX_ORG}"
                    f"&bucket={INFLUX_BUCKET}&precision=ns")
        self.ok = None            # None = never wrote / nothing yet
        self.last_ok_wall = None  # monotonic of last successful POST
        self.lines_written = 0
        self.last_health = 0.0

    # -- state --

    def _load_offset(self):
        try:
            with open(INFLUX_TAIL_STATE, encoding="utf-8") as f:
                return int(json.load(f).get("offset", 0))
        except (OSError, ValueError, AttributeError):
            return 0

    def _save_offset(self, off):
        tmp = INFLUX_TAIL_STATE + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"offset": off}, f)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, INFLUX_TAIL_STATE)

    # -- conversion --

    @staticmethod
    def _esc_tag(v):
        return (str(v).replace("\\", "\\\\").replace(" ", "\\ ")
                .replace(",", "\\,").replace("=", "\\="))

    @staticmethod
    def _esc_str(v):
        return '"' + str(v).replace("\\", "\\\\").replace('"', '\\"') + '"'

    @staticmethod
    def _iso_ns(s):
        if not s:
            return None
        try:
            dt = datetime.fromisoformat(str(s).replace("Z", "+00:00"))
            if dt.tzinfo is None:
                dt = dt.replace(tzinfo=datetime.timezone.utc)
            return int(dt.timestamp() * 1_000_000_000)
        except ValueError:
            return None

    def to_lp(self, o):
        """One JSONL record -> one line-protocol line (or None to skip)."""
        kind = o.get("kind")
        tags, fields = [], []
        if kind == "telemetry":
            for k in ("node_type", "node_id", "boot_session", "mode"):
                if o.get(k) is not None:
                    tags.append(f"{k}={self._esc_tag(o[k])}")
            for k in ("t_tpo", "t_tpu", "t_amb"):
                if isinstance(o.get(k), (int, float)):
                    fields.append(f"{k}={o[k]}")
            if o.get("ct_state") is not None:
                fields.append(f"ct_state={self._esc_str(o['ct_state'])}")
            for k in ("relay_state", "fault_flags", "capture_ms"):
                if isinstance(o.get(k), (int, float)):
                    fields.append(f"{k}={int(o[k])}i")
            ts = self._iso_ns(o.get("node_ts"))
            meas = "telemetry"
        elif kind == "event":
            for k in ("node_type", "node_id", "event"):
                if o.get(k) is not None:
                    tags.append(f"{k}={self._esc_tag(o[k])}")
            if o.get("fault_id") is not None:
                tags.append(f"fault_id={self._esc_tag(o['fault_id'])}")
                fields.append(f"fault_id={int(o['fault_id'])}i")
            else:
                fields.append("seen=1i")
            ts = self._iso_ns(o.get("gw_ts") or o.get("ts"))
            meas = "event"
        elif kind == "gw_ambient":
            tags.append("source=gw")
            if isinstance(o.get("t_amb"), (int, float)):
                fields.append(f"t_amb={o['t_amb']}")
            if o.get("fault") is not None:
                fields.append(f"fault={self._esc_str(o['fault'])}")
            ts = self._iso_ns(o.get("gw_ts") or o.get("ts"))
            meas = "gw_ambient"
        elif kind == "gw_status":
            tags.append("mode=" + self._esc_tag(o.get("mode") or "?"))
            for k in ("wifi", "broker", "time", "backend"):
                v = (o.get("legs") or {}).get(k)
                if isinstance(v, (int, float)):
                    fields.append(f"{k}={int(v)}i")
            for k in ("rssi_dbm", "channel", "uptime_s"):
                if isinstance(o.get(k), (int, float)):
                    fields.append(f"{k}={int(o[k])}i")
            ts = self._iso_ns(o.get("gw_ts") or o.get("ts"))
            meas = "gw_status"
        else:
            return None  # unknown future kind: advance past it, unchanged
        if not fields or ts is None:
            return None
        return f"{meas}{(',' + ','.join(tags)) if tags else ''} " \
               f"{','.join(fields)} {ts}"

    # -- io --

    def _collect(self, off):
        """Parse new complete lines from the JSONL. Returns (lines, newoff).
        Only advances past the last complete newline (a partially-written
        line stays for the next cycle)."""
        try:
            size = os.path.getsize(JSONL_PATH)
        except OSError:
            return [], off
        if size < off:  # rotated/truncated: start over (idempotent)
            log("influx: jsonl rotated/truncated — re-tailing from 0")
            off = 0
        with open(JSONL_PATH, encoding="utf-8", errors="replace") as f:
            f.seek(off)
            data = f.read(INFLUX_READ_CAP)
        nl = data.rfind("\n")
        if nl < 0:
            return [], off
        batch = []
        for ln in data[:nl].split("\n"):
            ln = ln.strip()
            if not ln:
                continue
            try:
                o = json.loads(ln)
            except ValueError:
                continue
            lp = self.to_lp(o)
            if lp:
                batch.append(lp)
        return batch, off + nl + 1

    def _post(self, body):
        req = urllib.request.Request(
            self.url, data=body.encode("utf-8"), method="POST",
            headers={"Authorization": f"Token {INFLUX_TOKEN}",
                     "Content-Type": "text/plain; charset=utf-8"})
        urllib.request.urlopen(req, timeout=15).read()

    def _publish_health(self):
        age = (round(time.monotonic() - self.last_ok_wall, 1)
               if self.last_ok_wall is not None else None)
        self.svc.client.publish(
            "bracino/backend/influx",
            json.dumps({"ok": bool(self.ok), "last_write_age_s": age,
                        "lines": self.lines_written}),
            qos=1, retain=True)

    def run(self):
        log(f"influx tailer: {INFLUX_URL} org={INFLUX_ORG} "
            f"bucket={INFLUX_BUCKET}")
        off = self._load_offset()
        backoff = 1.0
        while running:
            try:
                batch, newoff = self._collect(off)
            except OSError as exc:
                log(f"!! influx read failed: {exc}")
                time.sleep(5)
                continue
            if not batch:
                if time.monotonic() - self.last_health >= INFLUX_HEALTH_S:
                    self._publish_health()
                    self.last_health = time.monotonic()
                time.sleep(1)
                continue
            try:
                self._post("\n".join(batch))
            except Exception as exc:
                if self.ok is not False:
                    log(f"!! influx write failed ({len(batch)} lines): "
                        f"{exc} — retrying, ack path unaffected")
                self.ok = False
                time.sleep(backoff)
                backoff = min(backoff * 2, 60)
                continue
            if self.ok is not True:
                log(f"influx write ok: {len(batch)} lines")
            self.ok = True
            backoff = 1.0
            self.last_ok_wall = time.monotonic()
            self.lines_written += len(batch)
            off = newoff
            self._save_offset(off)
            if time.monotonic() - self.last_health >= INFLUX_HEALTH_S:
                self._publish_health()
                self.last_health = time.monotonic()


class Commit:
    def __init__(self):
        os.makedirs(DATA_DIR, exist_ok=True)
        self.state = self._load_state()
        self.last_commit_wall = None
        self.lines_written = 0
        self.gw_down = False        # gateway alarm state (issue 021/027)
        self.last_gw_marker = ""

    def _append_line(self, line):
        """Open-append-fsync-close per line. The fsync dominates cost, so
        the fresh handle is free — and it means an mv/deletion/rotation of
        the log file can never orphan writes: 2026-09-06 a `mv` of the
        jsonl left the held startup handle appending into a renamed inode
        (watermarks advanced, file vanished). Rotation in stage C relies
        on this being safe."""
        with open(JSONL_PATH, "a", encoding="utf-8") as f:
            f.write(json.dumps(line) + "\n")
            f.flush()
            os.fsync(f.fileno())
        self.lines_written += 1
        self.last_commit_wall = time.monotonic()

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

        self._append_line(line)
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
        self._append_line(line)
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
        self._append_line(line)

    # ---- alarms (issue 021) ----

    def handle_node_status(self, node_type, node_id, payload):
        """Retained node online/offline status (GW publishes on unreach
        raise/clear). offline -> retained alarm + push; online -> clear."""
        try:
            t = json.loads(payload)
        except ValueError:
            log(f"!! bad JSON on node status {node_type}/{node_id}")
            return
        online = bool(t.get("online"))
        if online:
            alarm = {"kind": "node_back", "node_type": node_type,
                     "node_id": node_id,
                     "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                         time.gmtime())}
            log(f"ALARM CLEARED: node {node_type}/{node_id} back online")
        else:
            alarm = {"kind": "node_gone", "node_type": node_type,
                     "node_id": node_id, **{k: t[k] for k in t
                                            if k != "online"},
                     "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                         time.gmtime())}
            log(f"!! ALARM: node {node_type}/{node_id} GONE "
                f"(silent {alarm.get('silent_s', '?')}s, "
                f"boot_session={alarm.get('boot_session', '?')})")
        body = json.dumps(alarm)
        self.client.publish("bracino/alarm", body, qos=1, retain=True)
        notify_push(body)

    def handle_gw_lwt(self):
        """GW down: empty LWT payload, or JSON online:false (gw-016+
        LWT message; issue 027 — the old matcher dropped the JSON LWT
        silently, so the Sep-10 11:52Z gw death raised no alarm)."""
        if not self.gw_down:
            self.gw_down = True
            alarm = {"kind": "gateway_gone",
                     "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                         time.gmtime())}
            log("!! ALARM: gateway DOWN (LWT or online:false)")
            body = json.dumps(alarm)
            self.client.publish("bracino/alarm", body, qos=1, retain=True)
            notify_push(body)

    def handle_gw_status(self, payload):
        """Retained gw status: informational heartbeats (mode, legs,
        rssi) from gw-016+, plus the online:false down signal."""
        try:
            t = json.loads(payload)
        except ValueError:
            log(f"!! bad JSON on gateway/status: {payload!r}")
            return
        if not t.get("online", False):
            self.handle_gw_lwt()
            return
        if self.gw_down:
            self.gw_down = False
            alarm = {"kind": "gateway_back",
                     "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                         time.gmtime())}
            log("ALARM CLEARED: gateway back online")
            body = json.dumps(alarm)
            self.client.publish("bracino/alarm", body, qos=1, retain=True)
            notify_push(body)
        # Persist every heartbeat to the JSONL (issue 027 follow-up):
        # rssi_dbm is the gw->AP leg time series 026 adjudication needs —
        # log-on-change-only threw the trace away. ~2.9k lines/day at 30 s.
        # The retained replay on each (re)connect writes one extra line;
        # acceptable for an informational record (no dedupe domain here).
        self._append_line({
            "kind": "gw_status",
            "gw_ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "online": True,
            "fw": t.get("fw"),
            "mode": t.get("mode"),
            "legs": t.get("legs"),
            "rssi_dbm": t.get("rssi_dbm"),
            "channel": t.get("channel"),
            "uptime_s": t.get("uptime_s"),
        })
        # log on mode/legs change only — heartbeats arrive every 30 s
        marker = f"{t.get('mode')} wifi={t.get('legs', {}).get('wifi')} " \
                 f"backend={t.get('legs', {}).get('backend')} " \
                 f"rssi={t.get('rssi_dbm')} ch={t.get('channel')}"
        if marker != self.last_gw_marker:
            self.last_gw_marker = marker
            log(f"gw status: {marker}")

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
            ("bracino/node/+/+/status", 1),
            ("bracino/gateway/status", 1),
            ("bracino/gateway/telemetry", 0),
        ])
        self.last_commit_wall = None  # gap: commit age restarts

    def on_message(self, client, _u, m):
        if m.topic == "bracino/gateway/status":
            if m.payload == b"":  # LWT: broker-side, no JSON
                self.handle_gw_lwt()
                return
            self.handle_gw_status(m.payload)
            return
        if m.topic == "bracino/gateway/telemetry":
            self.handle_gw_ambient(m.payload)
            return
        # bracino/node/<t>/<id>/<leaf>
        parts = m.topic.split("/")
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
        elif parts[4] == "status":
            self.handle_node_status(node_type, node_id, m.payload)

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

        if INFLUX_TOKEN and INFLUX_URL:
            InfluxTailer(self).start()
        else:
            log("influx tailer disabled (INFLUX_TOKEN/INFLUX_URL unset)")

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


if __name__ == "__main__":
    Commit().run()
