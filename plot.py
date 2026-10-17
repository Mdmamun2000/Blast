# Plot throughput vs. packet-loss rate from results.csv.
# Produces throughput_vs_loss.png (one line per record size / M setting) and
# throughput_max.png (best setting at each loss rate = maximum achievable throughput).
# Lines show the median of the runs; error bars show min..max. The median is used because
# throughput is bimodal under loss: a run either has no timeout or loses >= 300 ms to one.
# Usage: python3 plot.py [results.csv]    (needs: pip3 install matplotlib)
import csv, sys, statistics as st
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

path = sys.argv[1] if len(sys.argv) > 1 else "results.csv"
rows = defaultdict(lambda: defaultdict(list))        # (record_size, M) -> loss -> [rows]
for r in csv.DictReader(open(path)):
    if r["ok"] == "1" and r["throughput_mbps"]:
        rows[(int(r["record_size"]), int(r["M"]))][float(r["loss"])].append(r)

def tput(rs): return [float(r["throughput_mbps"]) for r in rs]

print(f"{'rec':>5} {'M':>6} {'loss%':>6} {'runs':>4} {'median Mbit/s':>14} {'min':>9} {'max':>9} "
      f"{'resent recs':>12} {'timeouts':>9}")
plt.figure(figsize=(8, 5))
for (rs, M), by_loss in sorted(rows.items()):
    losses = sorted(by_loss)
    med = [st.median(tput(by_loss[l])) for l in losses]
    lo = [min(tput(by_loss[l])) for l in losses]
    hi = [max(tput(by_loss[l])) for l in losses]
    for l, m, a, b in zip(losses, med, lo, hi):
        resent = st.mean(float(r["records_resent"]) for r in by_loss[l])
        to = st.mean(float(r["timeouts"]) for r in by_loss[l])
        print(f"{rs:5d} {M:6d} {l*100:6.1f} {len(by_loss[l]):4d} {m:14.2f} {a:9.2f} {b:9.2f} {resent:12.0f} {to:9.1f}")
    plt.errorbar([l * 100 for l in losses], med,
                 yerr=[[m - a for m, a in zip(med, lo)], [b - m for m, b in zip(med, hi)]],
                 marker="o", capsize=3, label=f"record {rs} B, M={M}")
plt.yscale("log")
plt.xlabel("Packet loss rate (%)")
plt.ylabel("Throughput (Mbit/s, log scale)")
plt.title("BLAST throughput vs. packet loss rate (median, min-max bars)")
plt.grid(True, which="both", alpha=0.3)
plt.legend(fontsize=8)
plt.tight_layout()
plt.savefig("throughput_vs_loss.png", dpi=150)

# best setting at each loss rate = maximum achievable throughput
best = {}
for key, by_loss in rows.items():
    for l, rs in by_loss.items():
        m = st.median(tput(rs))
        if l not in best or m > best[l][0]: best[l] = (m, key)
print("\nmaximum throughput per loss rate (best median):")
for l in sorted(best):
    m, (rs, M) = best[l]
    print(f"  loss {l*100:5.1f}%: {m:10.2f} Mbit/s  (record {rs} B, M={M})")

losses = sorted(best)
plt.figure(figsize=(7, 4))
plt.plot([l * 100 for l in losses], [best[l][0] for l in losses], marker="o")
plt.yscale("log")
plt.xlabel("Packet loss rate (%)")
plt.ylabel("Throughput (Mbit/s, log scale)")
plt.title("Maximum BLAST throughput vs. packet loss rate")
plt.grid(True, which="both", alpha=0.3)
plt.tight_layout()
plt.savefig("throughput_max.png", dpi=150)
print("saved throughput_vs_loss.png, throughput_max.png")
