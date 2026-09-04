#!/usr/bin/env python3
"""fake_publisher.py — layer-1 bench test for the commit service (015).

Publishes synthetic DN004 telemetry to mosquitto so the commit service's
watermark leg can be exercised with ZERO firmware:

    python3 fake_publisher.py --count 50 --interval 0.2 --gap-after 10

Watermark behavior to verify while it runs (mosquitto_sub):
    mosquitto_sub -t 'bracino/gateway/commit' -t 'bracino/gateway/health' -v

Watermarks must be monotonic per node, cover exactly the capture_ms of
the lines written (tail data/telemetry.jsonl), and stall if you Ctrl-C
the publisher mid-run (health keeps publishing, commit age grows).
"""

import argparse
import json
import math
import time

import paho.mqtt.client as mqtt

ap = argparse.ArgumentParser()
ap.add_argument("--host", default="localhost")
ap.add_argument("--port", type=int, default=1883)
ap.add_argument("--node-type", type=int, default=1)
ap.add_argument("--node-id", type=int, default=2)
ap.add_argument("--count", type=int, default=30, help="0 = forever")
ap.add_argument("--interval", type=float, default=1.0, help="s between samples")
ap.add_argument("--boot-session", type=int, default=1)
ap.add_argument("--capture-ms", type=int, default=0, help="start node_clock_ms")
ap.add_argument("--t0", type=float, default=None, help="start epoch (s, default now)")
args = ap.parse_args()

MODES = ["AUTO", "AUTO", "AUTO", "MANUAL"]


def sample(i):
    t = args.t0 if args.t0 is not None else time.time()
    node_ts = time.strftime("%Y-%m-%dT%H:%M:%SZ",
                            time.gmtime(t + i * args.interval))
    return {
        "mode": MODES[i % len(MODES)],
        "relay_state": 1,
        "ct_state": "NOT_FITTED",
        "t_tpo": round(58 + 4 * math.sin(i / 12) + i * 0.01, 1),
        "t_tpu": round(58 + 4 * math.sin(i / 12) + i * 0.01 - 0.3, 1),
        "t_amb": 21.0,
        "fault_flags": 0,
        "boot_session": args.boot_session,
        "capture_ms": args.capture_ms + i * int(args.interval * 1000),
        "node_ts": node_ts,
    }


c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="fake-pub")
c.connect(args.host, args.port, keepalive=30)
c.loop_start()
topic = f"bracino/node/{args.node_type}/{args.node_id}/telemetry"
i = 0
try:
    while args.count == 0 or i < args.count:
        s = sample(i)
        c.publish(topic, json.dumps(s), qos=0, retain=False)
        if i % 10 == 0:
            print(f"published {i}: capture_ms={s['capture_ms']} "
                  f"t_tpo={s['t_tpo']}", flush=True)
        i += 1
        time.sleep(args.interval)
finally:
    c.loop_stop()
    c.disconnect()
    print(f"done — published {i} samples, last capture_ms="
          f"{args.capture_ms + (i - 1) * int(args.interval * 1000)}")
