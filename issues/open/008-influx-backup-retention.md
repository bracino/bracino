# 008 — InfluxDB backup and retention policy

- **Status:** open
- **Type:** design
- **Opened:** 2026-07-26
- **Refs:** `server/influx-init/`, `docs/project_slug.md` (year-on-year from ~2027)

## Context

Influx data is accumulated state, not compose config. Needs NAS + cloud backup cadence and retention that supports multi-year comparison without filling disks blindly.

## Expected

Written policy in `docs/`: retention buckets, snapshot schedule, restore drill once.

## Proposal

Policy can land before heavy data; implement automation with compose cross-over.

## Fix

## Verify
