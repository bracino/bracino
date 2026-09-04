# server

Self-hosted supervision stack (Mosquitto, Node-RED, InfluxDB, Grafana).

Intended recovery path: `git clone && docker compose up`.

## Current contents

| Path | What |
|------|------|
| `docker-compose.yml` | compose-lite: mosquitto (docker not yet on the dev VM — bench runs apt mosquitto natively) |
| `mosquitto/mosquitto.conf` | broker config in git (persistence on, per DN004; anonymous is bench-only) |
| `commit-service/` | DN005 skeleton (issue 015): JSONL acker + watermark publisher + fake telemetry source |

