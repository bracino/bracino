# server

Self-hosted supervision stack — **t520 backend** (HP t520, 192.168.1.215, issue 015 stage A).

Recovery path: `git clone && docker compose up` (after the one-time steps below).

## Design (2026-09-05, settled)

- **Capture chain:** mosquitto → commit service → JSONL. BATCH_ACK is gated on **fsync'd JSONL** only. InfluxDB (stage C) is a *projection*: written by a tailer, never gates acks, always rebuildable from JSONL.
- **Storage, three failure domains:**

| Store | Location | Role |
|---|---|---|
| live JSONL | t520 local `/var/lib/bracino/commit` (30-day rotation, stage C) | source of truth; ack path depends on nothing external |
| JSONL mirror | `/mnt/nas-data2/bracino/commit` (daily rsync) | recovery copy, second failure domain |
| InfluxDB | `/mnt/nas-data1/bracino/influx` | queryable store; UI on :8086 for desktop/Grafana |

- **Auth:** single shared MQTT user `bracino` (hashed `passwd` file, gitignored; plaintext only in `server/.env` (gitignored) and the gateway's NVS via `/prov`). Plaintext 1883 on the LAN is proportionate; per-user ACLs are the upgrade path.
- **Health:** `bracino/gateway/health` = **commit path only** (broker seen, JSONL writable, heartbeating). Influx gets its own topic in stage C — an Influx failure must never gate ESP-NOW.

## Files

| Path | What |
|------|------|
| `docker-compose.yml` | mosquitto + commit (always); influxdb behind `--profile influx` (stage C) |
| `mosquitto/mosquitto.conf` | broker config: auth required, persistence on (DN004) |
| `mosquitto/passwd` | hashed credentials — **gitignored, generated on host** |
| `commit-service/` | DN005 service + Dockerfile + fake_publisher (bench) |

## t520 bring-up (stage B)

### 0. Prereqs

```bash
git clone git@github.com:bracino/bracino.git ~/bracino && cd ~/bracino/server
findmnt | grep -E 'nas-data[12]'      # both drives durably mounted
ip addr | grep 192.168.1.215          # DHCP reservation live
docker run --rm hello-world
```

### 1. Directory skeleton

```bash
sudo mkdir -p /mnt/nas-data1/bracino/{mosquitto/{data,log},influx/{data,config}}
sudo mkdir -p /mnt/nas-data2/bracino/commit
sudo chown -R 1883:1883 /mnt/nas-data1/bracino/mosquitto
sudo chown -R 1000:1000 /mnt/nas-data1/bracino/influx /mnt/nas-data2/bracino/commit
sudo mkdir -p /var/lib/bracino/commit && sudo chown 1000:1000 /var/lib/bracino/commit
```

### 2. Broker auth

```bash
docker run --rm -v "$PWD/mosquitto:/mosquitto/config" eclipse-mosquitto:2 \
  mosquitto_passwd -c /mosquitto/config/passwd bracino
# ↑ don't drop the command tail — bare `docker run ... eclipse-mosquitto:2`
# starts the BROKER, which then errors on the missing passwd file.
sudo chown 1883:1883 mosquitto/passwd && chmod 640 mosquitto/passwd
# generated as root:0600 otherwise; broker (uid 1883) couldn't read it
cat mosquitto/passwd    # expect: bracino:$7$… (hash, not plaintext)
```

### 3. Secrets

```bash
cat > .env << 'EOF'
MQTT_USER=bracino
MQTT_PASS=<your password>
EOF
chmod 600 .env
```

### 4. Up + smoke test

```bash
docker compose up -d mosquitto commit
docker compose logs --tail 20 commit     # connected, subscribed
mosquitto_sub -h localhost -t 'bracino/#' -u bracino -P "$MQTT_PASS" -v &
mosquitto_pub -h localhost -t 'bracino/test' -m ping -u bracino -P "$MQTT_PASS"
```

### 5. Daily JSONL mirror (install once)

```bash
( crontab -l 2>/dev/null; \
  echo '17 3 * * * rsync -a --delete /var/lib/bracino/commit/ /mnt/nas-data2/bracino/commit/' ) | crontab -
```

## Gateway provisioning (after stage B)

Flash from the dev VM (`idf.py -p /dev/ttyACM1 flash monitor`), long-press GPIO27 → join `bracino-gateway01`/`bracinoAdmin` → `http://192.168.5.1/prov`: house WiFi, broker `192.168.1.215:1883`, **MQTT user `bracino` + password**. Serial fallback: `u <user> [pass]` (or `u -` to clear). Status page shows the user, never the password.

## Stage C (influx), not yet deployed

```bash
docker compose --profile influx up -d      # + INFLUX_TOKEN/INFLUX_PASSWORD in .env
```

Commit service gains `INFLUX_URL`/`INFLUX_TOKEN`-gated line-protocol tailing (JSONL remains the ack path). Grafana stays on the desktop, pointed at `http://192.168.1.215:8086`.

## commit-service bench (unchanged)

The VM bench workflow (venv, fake_publisher, layer-1 drills 1–3 results) lives in [`commit-service/README.md`](commit-service/README.md). The service falls back to anonymous MQTT when `MQTT_USER`/`MQTT_PASS` are unset, so drills keep working against an auth-less bench broker.
