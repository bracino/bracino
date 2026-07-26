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

## Fix

## Verify
