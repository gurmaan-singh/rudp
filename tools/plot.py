"""Turn the benchmark CSVs into the figures in the README.

    build/Release/rudp_bench.exe > out/bench.csv
    build/Release/rudp_sim.exe --bytes 8000000 --bandwidth 1000000 --queue 60000 --trace out/cwnd.csv
    python tools/plot.py
"""

import csv
import pathlib
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "docs" / "img"
OUT.mkdir(parents=True, exist_ok=True)

BG, FG, DIM, RED = "#070707", "#e8e8e8", "#7a7a7a", "#ff2d2d"
plt.rcParams.update({
    "figure.facecolor": BG, "axes.facecolor": BG, "savefig.facecolor": BG,
    "axes.edgecolor": "#333", "axes.labelcolor": FG, "text.color": FG,
    "xtick.color": DIM, "ytick.color": DIM, "grid.color": "#1c1c1c", "axes.grid": True,
    "font.size": 10, "axes.spines.top": False, "axes.spines.right": False, "legend.frameon": False,
})


def goodput_vs_loss():
    rows = defaultdict(lambda: defaultdict(list))  # rtt -> variant -> [(loss, goodput)]
    mathis = defaultdict(list)
    with open(ROOT / "out" / "bench.csv") as f:
        for r in csv.DictReader(f):
            rtt, loss = float(r["rtt_ms"]), float(r["loss"])
            rows[rtt][r["variant"]].append((loss, float(r["goodput_MBps"])))
            if loss > 0 and r["variant"] == "sack+cc":
                mathis[rtt].append((loss, float(r["mathis_MBps"])))
    fig, axes = plt.subplots(1, 3, figsize=(13, 3.9), sharey=True)
    for ax, rtt in zip(axes, sorted(rows)):
        for variant, colour, style in (("sack+cc", RED, "-"), ("dupack+cc", FG, "-")):
            pts = [(l, g) for l, g in rows[rtt][variant] if l > 0]
            ax.plot(*zip(*pts), color=colour, lw=1.8, marker="o", ms=3.5, ls=style, label=variant.replace("+cc", ", congestion control"))
        ax.plot(*zip(*mathis[rtt]), color=DIM, lw=1.2, ls="--", label="Mathis estimate")
        ax.set(xscale="log", yscale="log", xlabel="packet loss (each direction)", title=f"{rtt:.0f} ms round trip")
    axes[0].set_ylabel("goodput (MB/s)")
    axes[0].legend(loc="lower left")
    fig.tight_layout()
    fig.savefig(OUT / "goodput_vs_loss.png", dpi=130, bbox_inches="tight")
    plt.close(fig)
    print("wrote goodput_vs_loss.png")


def sawtooth():
    t, cwnd, flight, rto = [], [], [], []
    with open(ROOT / "out" / "cwnd.csv") as f:
        for r in csv.DictReader(f):
            t.append(float(r["time_s"]))
            cwnd.append(int(r["cwnd"]) / 1000)
            flight.append(int(r["in_flight"]) / 1000)
    fig, ax = plt.subplots(figsize=(9.5, 3.8))
    ax.plot(t, cwnd, color=RED, lw=1.5, label="congestion window")
    ax.plot(t, flight, color=DIM, lw=0.8, label="bytes in flight")
    ax.axhline(40 + 60, color="#555", ls=":")
    ax.text(2.4, 40 + 60 + 4, "link pipe (40 KB) + queue (60 KB)", color=DIM)
    ax.set(xlabel="time (s)", ylabel="kilobytes", title="One flow through a 1 MB/s bottleneck with a 60 KB queue", xlim=(0, 9))
    ax.legend(loc="upper right")
    fig.tight_layout()
    fig.savefig(OUT / "sawtooth.png", dpi=130, bbox_inches="tight")
    plt.close(fig)
    print("wrote sawtooth.png")


if __name__ == "__main__":
    goodput_vs_loss()
    sawtooth()
