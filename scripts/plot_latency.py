#!/usr/bin/env python3
"""Plot latency histograms from the [lat-full] sections of latency.log."""
import argparse
import re
import sys
from pathlib import Path

import matplotlib.pyplot as plt

HEADER_RE = re.compile(r"\[lat-full\]\s+(\S+)\s+n=(\d+)")
BIN_RE = re.compile(r"\s*\[\s*(\d+)\]\s+(\S+\.\.\S+)\s+n=(\d+)\s+cum=\s*([\d.]+)%")


def parse(path):
    sections = {}
    current = None
    for line in path.read_text().splitlines():
        m = HEADER_RE.match(line)
        if m:
            current = m.group(1)
            sections[current] = {"total": int(m.group(2)), "bins": []}
            continue
        m = BIN_RE.match(line)
        if m and current is not None:
            sections[current]["bins"].append(
                {
                    "idx": int(m.group(1)),
                    "range": m.group(2),
                    "count": int(m.group(3)),
                    "cum": float(m.group(4)),
                }
            )
    return sections


def plot(sections, out=None):
    n = len(sections)
    fig, axes = plt.subplots(n, 1, figsize=(12, 4 * n), squeeze=False)
    for ax, (name, data) in zip(axes[:, 0], sections.items()):
        bins = data["bins"]
        labels = [b["range"] for b in bins]
        counts = [b["count"] for b in bins]
        x = list(range(len(bins)))

        ax.bar(x, counts, color="steelblue", edgecolor="black", width=0.9)
        ax.set_yscale("log")
        ax.set_xticks(x)
        ax.set_xticklabels(labels, rotation=45, ha="right", fontsize=8)
        ax.set_ylabel("count (log)")
        ax.set_xlabel("latency bucket (log2-spaced)")
        ax.set_title(f"{name}  (n={data['total']:,})")
        ax.grid(True, axis="y", which="both", alpha=0.3)

        ax2 = ax.twinx()
        ax2.plot(x, [b["cum"] for b in bins], color="crimson", marker="o", linewidth=1.2)
        ax2.set_ylabel("cumulative %", color="crimson")
        ax2.set_ylim(0, 105)
        ax2.tick_params(axis="y", colors="crimson")

    fig.tight_layout()
    if out:
        fig.savefig(out, dpi=150)
        print(f"saved {out}")
    else:
        plt.show()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", nargs="?", default="build/latency.log", type=Path,
                    help="path to latency.log (default: build/latency.log)")
    ap.add_argument("-o", "--out", type=Path,
                    help="output image path; if omitted, show interactively")
    args = ap.parse_args()

    if not args.log.exists():
        print(f"error: {args.log} not found", file=sys.stderr)
        sys.exit(1)

    sections = parse(args.log)
    if not sections:
        print("no [lat-full] sections found in log", file=sys.stderr)
        sys.exit(1)
    plot(sections, args.out)


if __name__ == "__main__":
    main()
