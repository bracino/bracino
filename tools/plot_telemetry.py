#!/usr/bin/env python3
"""Headless variant of plot_telemetry.py for agent visual analysis.

Renders NDJSON telemetry to PNG: TPO/TPU panel + separate AMB panel,
relay shading, event markers (FIFO-aligned), with gaps > 10x median
step broken so connect-the-dots lies don't mislead.

usage: plot_png.py FILE --out chart.png [--start ISO8601] [--end ISO8601]
                        [--timezone Z] [--align-events] [--show-gaps]
"""

import argparse
import json
import sys
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

import pandas as pd
import matplotlib
if "--show" not in sys.argv:
    matplotlib.use("Agg")   # headless render; --show leaves backend selection free
import matplotlib.pyplot as plt
import matplotlib.dates as mdates

STEP_MED_FALLBACK_S = 15.0


def read_log(filename):
    telemetry, events, gw_amb = [], [], []
    last_node_time, ev_off_ms = None, 0

    with open(filename, "r", encoding="utf-8") as f:
        for ln, line in enumerate(f, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                o = json.loads(line)
            except json.JSONDecodeError as e:
                print(f"warn: line {ln} bad JSON: {e}", file=sys.stderr)
                continue

            if o.get("kind") == "telemetry":
                ts = o.get("node_ts")
                if ts is None:
                    continue
                last_node_time = ts
                ev_off_ms = 0
                telemetry.append({
                    "time": ts,
                    "t_tpo": o.get("t_tpo"),
                    "t_tpu": o.get("t_tpu"),
                    "t_amb": o.get("t_amb"),
                    "relay": o.get("relay_state", 0),
                    "boot": o.get("boot_session"),
                    "capture_ms": o.get("capture_ms"),
                    "fault_flags": o.get("fault_flags"),
                })
            elif o.get("kind") == "event":
                gw_ts = o.get("gw_ts")
                if gw_ts is None:
                    continue
                t = (pd.Timestamp(last_node_time) + pd.Timedelta(milliseconds=ev_off_ms)
                     if last_node_time else pd.Timestamp(gw_ts))
                ev_off_ms += 1
                events.append({"time": t, "gw_time": gw_ts,
                               "event": o.get("event"), "fault_id": o.get("fault_id")})
            elif o.get("kind") == "gw_ambient":
                gw_amb.append({"time": o.get("gw_ts") or o.get("ts"),
                               "t_amb": o.get("t_amb"),
                               "fault": o.get("fault")})
    return telemetry, events, gw_amb


def break_gaps(df, cols):
    """Return df with rows inserted as NaN at big gaps (split lines).

    Also splits wherever the boot-session tag changes: interleaved
    streams (bench drills share node_id with the field node) must not
    be connected, or the plot paints vertical lies (2026-09-08).
    """
    times = pd.DatetimeIndex(df["time"])
    if len(times) < 3:
        return df
    steps = (times[1:] - times[:-1]).total_seconds()
    med = pd.Series(steps).median() or STEP_MED_FALLBACK_S
    thresh = max(10 * med, 600)
    bad = steps > thresh
    if "boot" in df.columns:
        boot = df["boot"].fillna(-1).astype(str)
        bad = bad | (boot.values[1:] != boot.values[:-1])
    if not bad.any():
        return df
    idx = [i for i in range(len(bad)) if bad[i]]
    new_rows = []
    for i in idx:
        mid = times[i] + (times[i + 1] - times[i]) / 2
        new_rows.append({c: None for c in df.columns})
        new_rows[-1]["time"] = mid
    out = pd.concat([df, pd.DataFrame(new_rows)], ignore_index=True)
    return out.sort_values("time").reset_index(drop=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--out", required=True)
    ap.add_argument("--start")
    ap.add_argument("--end")
    ap.add_argument("--timezone", default="Europe/Rome")
    ap.add_argument("--dpi", type=int, default=130)
    ap.add_argument("--figsize", default="15,8")
    ap.add_argument("--show", action="store_true",
                    help="open an interactive window (toolbar pan/zoom; "
                         "'w' writes a snapshot PNG next to --out)")
    args = ap.parse_args()

    try:
        tz = ZoneInfo(args.timezone)
    except ZoneInfoNotFoundError:
        raise SystemExit(f"unknown timezone {args.timezone}")

    tel, ev, gw = read_log(args.file)
    if not tel:
        raise SystemExit("no telemetry records")
    df = pd.DataFrame(tel)
    df["time"] = pd.to_datetime(df["time"], utc=True, errors="coerce").dt.tz_convert(tz)
    df = df.dropna(subset=["time"]).sort_values("time").reset_index(drop=True)

    if args.start:
        df = df[df["time"] >= pd.Timestamp(args.start, tz="UTC").tz_convert(tz)]
        df = df.reset_index(drop=True)
    if args.end:
        df = df[df["time"] <= pd.Timestamp(args.end, tz="UTC").tz_convert(tz)]
        df = df.reset_index(drop=True)
    if df.empty:
        raise SystemExit("empty after window filter")

    for c in ["t_tpo", "t_tpu", "t_amb"]:
        df[c] = pd.to_numeric(df[c], errors="coerce")
    df["faults"] = pd.to_numeric(df.get("fault_flags"), errors="coerce").fillna(0).astype(int)
    df["relay"] = pd.to_numeric(df["relay"], errors="coerce").fillna(0).astype(int)

    ev_df = pd.DataFrame(ev)
    if not ev_df.empty:
        ev_df["time"] = pd.to_datetime(ev_df["time"], utc=True, errors="coerce").dt.tz_convert(tz)
        ev_df = ev_df.dropna(subset=["time"])

    # gateway-local ambient (gw_ambient records): separate probe, own
    # timestamps. Junk-masked: firmware-faulted rows and the pre-
    # 2026-09-06T14:49Z misrange era (pinned ~-31 C) are dropped.
    gw_df = pd.DataFrame(gw)
    if not gw_df.empty:
        gw_df["time"] = pd.to_datetime(gw_df["time"], utc=True, errors="coerce").dt.tz_convert(tz)
        gw_df["t_amb"] = pd.to_numeric(gw_df["t_amb"], errors="coerce")
        gw_df = gw_df.dropna(subset=["time", "t_amb"])
        gw_df = gw_df[gw_df["fault"].isna() & (gw_df["t_amb"] > -10)]

    # ---- summary to stdout ----
    raw_steps = df["time"].diff().dt.total_seconds()
    print(f"file:        {args.file}")
    print(f"samples:     {len(df)}")
    print(f"range:       {df['time'].iloc[0]} -> {df['time'].iloc[-1]}")
    print(f"median step: {raw_steps.median():.0f}s  max step: {raw_steps.max():.0f}s")
    gap_thresh = max(10 * (raw_steps.median() or STEP_MED_FALLBACK_S), 600)
    big = raw_steps[raw_steps > gap_thresh]
    for i in big.index:
        print(f"  gap: {df['time'][i-1]} -> {df['time'][i]}  ({big[i]:.0f}s)")
    print(f"events:      {len(ev_df)}")

    # session starts: computed BEFORE break_gaps (which inserts NaN
    # separator rows at every stream alternation — labeling those gave
    # the 'boot NaT' smudge). One marker per distinct boot tag, first
    # appearance. Random 8-bit tags can collide across sessions; the
    # line-breaks stay per-row authoritative.
    session_starts = (df.dropna(subset=["boot"])
                        .drop_duplicates(subset=["boot"], keep="first")
                        .loc[:, ["time", "boot"]])

    df = break_gaps(df, ["t_tpo", "t_tpu", "t_amb"])

    fw, fh = (float(x) for x in args.figsize.split(","))
    fig, (ax1, ax2) = plt.subplots(
        2, 1, figsize=(fw, fh), sharex=True,
        gridspec_kw={"height_ratios": [2.2, 1.0], "hspace": 0.08})

    # relay shading on both panels
    on = (df["relay"] == 1)  # Series, keeps index
    t = df["time"]
    starts = on & ~on.shift(fill_value=False)
    for _, row in df.loc[starts].iterrows():
        nxt = df.loc[df["time"] > row["time"], "time"]
        end_t = nxt.iloc[0] if len(nxt) else t.iloc[-1]
        for ax in (ax1, ax2):
            ax.axvspan(row["time"], end_t, alpha=0.18, color="tab:orange")

    ax1.plot(df["time"], df["t_tpo"], lw=1.6, label="TPO", color="tab:red")
    ax1.plot(df["time"], df["t_tpu"], lw=1.6, label="TPU", color="tab:blue")
    fnz = df[df["faults"].notna() & (df["faults"] != 0)]
    if not fnz.empty:
        ax1.scatter(fnz["time"], fnz["t_tpo"], marker="x", s=80, color="darkred",
                    label=f"fault_flags={sorted(set(int(v) for v in fnz['faults']))}", zorder=5)
    ax1.set_ylabel("TPO/TPU (°C)")
    ax1.legend(loc="upper left", fontsize=9)
    ax1.grid(True, alpha=0.3)

    ax2.plot(df["time"], df["t_amb"], lw=1.3, label="AMB (node)", color="tab:green")
    if not gw_df.empty:
        ax2.plot(gw_df["time"], gw_df["t_amb"], lw=1.0, ls="--",
                 label="AMB (gw)", color="tab:purple")
    ax2.set_ylabel("AMB (°C)")
    ax2.set_xlabel(f"Time ({args.timezone})")
    ax2.grid(True, alpha=0.3)

    # event markers on top panel (window-filtered — stale markers
    # outside the visible range used to drag the axes/tight-bbox wide)
    if not ev_df.empty:
        ev_df = ev_df[(ev_df["time"] >= df["time"].min())
                      & (ev_df["time"] <= df["time"].max())]
    if not ev_df.empty:
        for _, e in ev_df.iterrows():
            et = e["event"]
            color, style = None, "-"
            if et == "FAULT_RAISED":
                color, style = "red", "--"
                lab = f"F{e['fault_id']}^"
            elif et == "FAULT_CLEARED":
                color, style = "green", ":"
                lab = f"F{e['fault_id']}v"
            else:
                lab = str(et)[:18]
            ax1.axvline(e["time"], color=color, ls=style, alpha=0.75, lw=1)
            ax1.text(e["time"], 0.02, lab, transform=ax1.get_xaxis_transform(),
                     rotation=90, ha="right", va="bottom", fontsize=7,
                     color=color or "black", backgroundcolor="white",
                     clip_on=True)

    # boot-session start markers
    for _, s in session_starts.iterrows():
        ax1.axvline(s["time"], color="slategray", ls=":", alpha=0.6, lw=1)
        ax1.text(s["time"], 0.98, f"boot {s['boot']}",
                 transform=ax1.get_xaxis_transform(), rotation=90,
                 ha="right", va="top", fontsize=6.5, color="slategray",
                 backgroundcolor="white", clip_on=True)

    loc = mdates.AutoDateLocator(tz=tz)
    for ax in (ax1, ax2):
        ax.xaxis.set_major_locator(loc)
        ax.xaxis.set_major_formatter(mdates.ConciseDateFormatter(loc, tz=tz))

    fig.suptitle(f"{args.file}" +
                 (f"  [{args.start} .. {args.end}]" if args.start or args.end else ""),
                 fontsize=11)
    fig.savefig(args.out, dpi=args.dpi, bbox_inches="tight")
    print(f"wrote: {args.out}")

    if args.show:
        import os

        def snap(event):
            if event.key == "w":
                out = os.path.join(
                    os.path.dirname(os.path.abspath(args.out)),
                    f"snap-{pd.Timestamp.now().strftime('%Y%m%d-%H%M%S')}.png")
                fig.savefig(out, dpi=args.dpi)  # no tight-bbox: zoomed
                print(f"snapshot: {out}")       # snapshots stay sane

        fig.canvas.mpl_connect("key_press_event", snap)
        print("interactive: toolbar pan/zoom/save; press 'w' for a silent snapshot")
        plt.show()
    else:
        plt.close(fig)


if __name__ == "__main__":
    main()
