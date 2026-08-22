# 006 — Firmware skeletons for node-bbu and gateway

- **Status:** open
- **Type:** task
- **Opened:** 2026-07-26
- **Refs:** `firmware/node-bbu/`, `firmware/gateway/`, `~/projects/shared` ESP-IDF

## Context

Empty placeholders only. Need ESP-IDF project skeletons that build, flash, and separate **control loop** tasks from **link/telemetry** tasks.

## Expected

Two buildable projects; README notes for IDF path; control task runnable without gateway; gateway stubs ESP-NOW + WiFi/MQTT hooks.

## Proposal

Start minimal; wire schemas from 004/005 as they land. Do not block first PWM/GPIO blink-style bring-up on schema perfection.

`node-bbu` builds (ESP-IDF 5.2, `esp32c3`) as a serial bring-up app. Flashed polarity + A0–A3 look sane. GPIO8 heartbeat confirmed on the v0.08 protoboard (USB and external 5 V). Do not add a webserver on this node. Control-loop work is **009**. Gateway still a placeholder.

## Fix

## Verify
