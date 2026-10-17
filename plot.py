# Plot throughput vs. packet-loss rate from results.csv
# Usage: python3 plot.py [results.csv]    (needs: pip3 install matplotlib)
import csv, sys, statistics as st
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

path = sys.argv[1] if len(sys.argv) > 1 else "results.csv"
rows = defaultdict(list)
for r in csv.DictReader(open(path)):
    if r["ok"] == "1" and r["throughput_mbps"]:
        rows[float(r["loss"])].append(r)

losses = sorted(rows)
mean = [st.mean(float(r["throughput_mbps"]) for r in rows[l]) for l in losses]
lo   = [min(float(r["throughput_mbps"]) for r in rows[l]) for l in losses]
hi   = [max(float(r["throughput_mbps"]) for r in rows[l]) for l in losses]

print(f"{'loss%':>6} {'runs':>4} {'mean Mbit/s':>12} {'min':>9} {'max':>9} {'resent recs':>12} {'timeouts':>9}")
for l, m, a, b in zip(losses, mean, lo, hi):
    rs = st.mean(float(r["records_resent"]) for r in rows[l])
    to = st.mean(float(r["timeouts"]) for r in rows[l])
    print(f"{l*100:6.1f} {len(rows[l]):4d} {m:12.2f} {a:9.2f} {b:9.2f} {rs:12.0f} {to:9.1f}")

x = [l * 100 for l in losses]
plt.figure(figsize=(7, 4.5))
plt.errorbar(x, mean, yerr=[[m - a for m, a in zip(mean, lo)], [b - m for m, b in zip(mean, hi)]],
             marker="o", capsize=4)
plt.yscale("log")
plt.xlabel("Packet loss rate (%)")
plt.ylabel("Throughput (Mbit/s, log scale)")
plt.title("BLAST throughput vs. packet loss rate")
plt.grid(True, which="both", alpha=0.3)
plt.tight_layout()
plt.savefig("throughput_vs_loss.png", dpi=150)
print("saved throughput_vs_loss.png")
