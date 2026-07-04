#!/usr/bin/env python3
"""Parse mem_stress UART logs and plot an A/B allocator comparison.

The MEM_STRESS example (examples/mem_stress.c) streams one CSV row per heap
operation, embedded in ordinary log lines:

    ... @MS,<seq>,<A|F|O>,<size>,<cycles>,<probes>,<live>,<bytes_in_use>

framed by @MSTART,... and @MSEND,... . This script strips the log/UART framing,
extracts those rows from one or more run logs, and renders TWO figures:

  <prefix>_probes.png  malloc search cost in free-list PROBES vs. op sequence,
                       plus its histogram/CDF. Deterministic and platform-
                       independent — the clean O(n)-vs-O(1) signal.
  <prefix>_cycles.png  malloc cost in cycles (real DWT on HW; QEMU virtual-clock
                       time via SYS_ELAPSED, host-jittery without -icount).

and prints a summary table (mean / median / p99 / max) for both metrics.

Usage:
    plot_mem_stress.py SEGLIST=seglist.log TLSF=tlsf.log [-o out_prefix]
    plot_mem_stress.py run.log                      # single run, label from @MSTART
"""
import argparse
import re
import sys

import matplotlib
matplotlib.use("Agg")  # headless
import matplotlib.pyplot as plt
import numpy as np

ANSI = re.compile(r"\x1b\[[0-9;]*m")
ROW = re.compile(r"@MS,(\d+),([AFO]),(\d+),(\d+),(\d+),(\d+),(\d+)")
START = re.compile(r"@MSTART,([^\r\n]*)")


def parse_log(path):
    """Return (label, rows) parsed from one run log."""
    label = None
    cols = {k: [] for k in ("seq", "kind", "size", "cyc", "probes", "live",
                            "inuse")}
    with open(path, errors="replace") as f:
        for line in f:
            line = ANSI.sub("", line)
            if label is None:
                m = START.search(line)
                if m:
                    kv = dict(p.split("=", 1) for p in m.group(1).split(",")
                              if "=" in p)
                    label = kv.get("backend")
            m = ROW.search(line)
            if not m:
                continue
            cols["seq"].append(int(m.group(1)))
            cols["kind"].append(m.group(2))
            cols["size"].append(int(m.group(3)))
            cols["cyc"].append(int(m.group(4)))
            cols["probes"].append(int(m.group(5)))
            cols["live"].append(int(m.group(6)))
            cols["inuse"].append(int(m.group(7)))
    return label, {k: np.array(v) for k, v in cols.items()}


def summary(label, rows):
    def stats(metric, mask, name):
        c = rows[metric][mask]
        if len(c) == 0:
            return f"    {name:6s} (none)"
        return (f"    {name:6s} n={len(c):5d}  mean={c.mean():9.1f}  "
                f"med={np.median(c):7.1f}  p99={np.percentile(c,99):9.1f}  "
                f"max={c.max():9d}")
    a = rows["kind"] == "A"
    f = rows["kind"] == "F"
    o = rows["kind"] == "O"
    print(f"[{label}]  ops={len(rows['seq'])}  oom={int(o.sum())}")
    print("  probes (deterministic search cost):")
    print(stats("probes", a, "alloc"))
    print("  cycles (DWT / QEMU SYS_ELAPSED):")
    print(stats("cyc", a, "alloc"))
    print(stats("cyc", f, "free"))


def _panel(runs, metric, unit, title, out_png, colors):
    """A 3-up figure for one metric: timeline scatter, histogram, CDF."""
    fig, (axt, axh, axc) = plt.subplots(1, 3, figsize=(15, 4.2))
    for label, rows in runs:
        a = rows["kind"] == "A"
        vals = rows[metric][a]
        if len(vals) == 0:
            continue
        col = colors.get(label, None)
        axt.scatter(rows["seq"][a], vals, s=6, alpha=0.35, color=col,
                    label=label)
        axh.hist(vals, bins=50, alpha=0.5, color=col, label=label)
        vs = np.sort(vals)
        axc.plot(vs, np.linspace(0, 1, len(vs)), color=col, lw=2, label=label)
    axt.set(xlabel="operation #", ylabel=f"{metric} / malloc ({unit})",
            title="over the run")
    axh.set(xlabel=f"{metric} / malloc ({unit})", ylabel="count",
            title="distribution")
    axc.set(xlabel=f"{metric} / malloc ({unit})", ylabel="cumulative fraction",
            title="CDF (right tail = worst case)")
    for ax in (axt, axh, axc):
        ax.grid(True, alpha=0.3)
        ax.legend(markerscale=2)
    fig.suptitle(title, fontweight="bold")
    fig.tight_layout()
    fig.savefig(out_png, dpi=130)
    return out_png


def plot(runs, out_prefix):
    colors = {"SEGLIST": "#d64550", "TLSF": "#2a7de1"}
    p1 = _panel(runs, "probes", "free blocks inspected",
                "malloc search cost — PROBES (deterministic O(n) vs O(1))",
                f"{out_prefix}_probes.png", colors)
    p2 = _panel(runs, "cyc", "cycles / ns",
                "malloc cost — CYCLES (DWT on HW; QEMU SYS_ELAPSED virtual time)",
                f"{out_prefix}_cycles.png", colors)
    print(f"\nwrote {p1} and {p2}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inputs", nargs="+",
                    help="LOG or LABEL=LOG (label overrides @MSTART backend)")
    ap.add_argument("-o", "--out", default="mem_stress",
                    help="output PNG prefix (default: mem_stress)")
    args = ap.parse_args()

    runs = []
    for item in args.inputs:
        if "=" in item and not item.split("=", 1)[1].count("=") and \
           not item.startswith("/"):
            label, path = item.split("=", 1)
        else:
            label, path = None, item
        parsed_label, rows = parse_log(path)
        label = label or parsed_label or path
        if len(rows["seq"]) == 0:
            print(f"WARNING: no @MS rows found in {path}", file=sys.stderr)
        runs.append((label, rows))

    print("=" * 68)
    for label, rows in runs:
        summary(label, rows)
    print("=" * 68)
    plot(runs, args.out)


if __name__ == "__main__":
    main()
