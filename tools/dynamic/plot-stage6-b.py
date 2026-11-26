"""Plot the Stage 6 B-side write sweep from ``summary.json``.

Two panels, because the two costs have different causes and must not be read as
one curve: acknowledgement is a durability cost (one fsync per write), recovery
is an open cost dominated by segment metadata rather than by history length.
"""

import argparse
import json
from pathlib import Path


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    summary = json.loads(args.summary.read_text(encoding="utf-8"))
    runs = summary["runs"]
    if not runs:
        raise SystemExit("summary has no runs to plot")
    writes = [run["writes"] for run in runs]

    def milliseconds(values):
        return [value / 1e6 for value in values]

    figure, (left, right) = plt.subplots(1, 2, figsize=(11, 4.2))

    left.plot(writes, milliseconds([run["write_acknowledge_ns"]["p50"] for run in runs]),
              marker="o", color="#1f77b4", label="acknowledge p50")
    left.plot(writes, milliseconds([run["write_acknowledge_ns"]["p95"] for run in runs]),
              marker="s", color="#ff7f0e", label="acknowledge p95")
    left.plot(writes, milliseconds([run["write_acknowledge_ns"]["p99"] for run in runs]),
              marker="^", color="#d62728", label="acknowledge p99")
    left.plot(writes, milliseconds([run["visible_read_ns"]["p50"] for run in runs]),
              marker="D", color="#2ca02c", label="read-back p50")
    left.set_xscale("log")
    left.set_yscale("log")
    left.set_xticks(writes)
    left.set_xticklabels([str(value) for value in writes])
    left.xaxis.set_minor_formatter(matplotlib.ticker.NullFormatter())
    left.set_xlabel("writes in the run")
    left.set_ylabel("latency (ms, log scale)")
    left.set_title("Acknowledged write vs immediate read-back")
    left.grid(True, which="both", alpha=0.3)
    left.legend(fontsize=8, loc="upper center", bbox_to_anchor=(0.5, -0.22), ncol=2,
                frameon=False)

    right.plot(writes, milliseconds([run["reopen_ns"] for run in runs]),
               marker="o", color="#9467bd", label="reopen (same process)")
    right.plot(writes, milliseconds([run["second_process_open_ns"] for run in runs]),
               marker="s", color="#8c564b", label="open (new process)")
    right.set_xscale("log")
    right.set_xticks(writes)
    right.set_xticklabels([str(value) for value in writes])
    right.xaxis.set_minor_formatter(matplotlib.ticker.NullFormatter())
    right.set_xlabel("writes replayed from the WAL")
    right.set_ylabel("open latency (ms)")
    right.set_title("Recovery cost against history length")
    right.grid(True, alpha=0.3)
    right.legend(fontsize=8, loc="upper center", bbox_to_anchor=(0.5, -0.22), ncol=2,
                 frameon=False)

    figure.tight_layout()
    figure.savefig(args.output, dpi=140, bbox_inches="tight")
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
