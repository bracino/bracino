# commit service — DN005 skeleton (issue 015)

Bench phase of the backend pipeline: subscribes gateway telemetry/events,
writes JSONL durably, publishes commit watermarks the gateway's
BATCH_ACK is gated on. Grows into the real DN005 service (Influx write)
later — the JSONL schema is the Influx mapping.

## Files

- `commit_service.py` — subscriber + JSONL writer + watermark/time/health publisher
- `fake_publisher.py` — synthetic DN004 telemetry source (layer-1 test, zero firmware)
- `data/` — runtime output, **gitignored** (`telemetry.jsonl`, `commit_state.json`)

## Run (bench)

```bash
cd server/commit-service
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt

# terminal 1: broker (if not already running as a service)
#   sudo apt install mosquitto && systemctl start mosquitto

# terminal 2: the commit service
python3 commit_service.py

# terminal 3: watch the watermarks
mosquitto_sub -t 'bracino/gateway/commit' -t 'bracino/gateway/health' -v

# terminal 4: fake node
python3 fake_publisher.py --count 50 --interval 0.2
```

## Layer-1 acceptance (issue 015 drill list)

1. JSONL lines match published samples; watermarks monotonic, last
   watermark == max capture_ms written.
2. Kill the service mid-stream → watermarks/health stall; restart →
   `commit_state.json` resumes the dedupe cursor; no duplicate lines
   when the publisher re-sends overlapping samples.
3. Publisher re-send of already-committed capture_ms → zero new lines,
   zero watermark regression.
