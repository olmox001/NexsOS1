#!/usr/bin/env python3
"""
tools/analyze_drift.py — drift analysis for the NEXS performance campaign (perf brief §4).

Ingests a serial-log capture from a /bin/stress run (the run emits CSV lines
prefixed "MEMSTAT,"), then for each counter:
  - cumulative counters (alloc/free calls, ctx switches, cycles) -> per-interval
    RATES and the headline throughput (cycles/s);
  - level counters that must be FLAT at steady state (free pages, largest contig
    run, free-run count, kmalloc in-use/live bytes, zombies, live kobjects) ->
    a Mann-Kendall monotonic-trend test + slope, with a FLAT/DRIFT verdict;
  - derived per-interval mean PMM bitmap-search ns (the fragmentation-sensitive
    latency) -> FLAT/DRIFT.

Pure stdlib (no numpy/scipy) so it runs anywhere python3 does.

Usage:
  python3 tools/analyze_drift.py <serial-log> [--warmup N] [--alpha 0.05] [--eps 0.02]
  # --warmup N : drop the first N samples (boot/warm-up transient)
  # acceptance: every FLAT-expected counter reports FLAT for the run to pass.
"""
import sys, math, re

COLS = ["t_s","free","lcr","runs","alloc_calls","free_calls","search_ns_total",
        "search_ns_max","km_inuse","km_hi","km_live","ctxsw","runnable","zomb",
        "objF","objP","objR","objW","objC","cycles"]
# counters expected FLAT at steady state (drift => leak/fragmentation)
FLAT = ["free","lcr","runs","km_inuse","km_live","zomb","objF","objP","objR","objW","objC"]
# cumulative counters -> reported as rates
RATE = ["alloc_calls","free_calls","ctxsw","cycles"]

def parse(path):
    rows = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            i = line.find("MEMSTAT,")
            if i < 0:
                continue
            parts = line[i:].strip().split(",")
            if len(parts) < len(COLS) or parts[1] == "t_s":
                continue  # header or short line
            try:
                rows.append([int(x) for x in parts[1:1+len(COLS)]])
            except ValueError:
                continue
    return rows

def col(rows, name):
    j = COLS.index(name)
    return [r[j] for r in rows]

def mann_kendall(x):
    """Return (S, z). |z|>1.96 ~ significant monotonic trend at 95%."""
    n = len(x)
    if n < 4:
        return 0, 0.0
    S = 0
    for i in range(n-1):
        for k in range(i+1, n):
            S += (x[k] > x[i]) - (x[k] < x[i])
    var = n*(n-1)*(2*n+5)/18.0
    if var <= 0:
        return S, 0.0
    if S > 0:   z = (S-1)/math.sqrt(var)
    elif S < 0: z = (S+1)/math.sqrt(var)
    else:       z = 0.0
    return S, z

def slope(t, x):
    """Least-squares slope per second (Theil-sen-ish via simple LS)."""
    n = len(t)
    mt = sum(t)/n; mx = sum(x)/n
    den = sum((ti-mt)**2 for ti in t)
    if den == 0: return 0.0
    return sum((t[i]-mt)*(x[i]-mx) for i in range(n))/den

def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__); sys.exit(2)
    path = args[0]
    warmup = 0; alpha_z = 1.96; eps = 0.02
    i = 1
    while i < len(args):
        if args[i] == "--warmup": warmup = int(args[i+1]); i += 2
        elif args[i] == "--alpha":
            # crude: map common alphas to z
            a = float(args[i+1]); alpha_z = 2.576 if a <= 0.01 else 1.96; i += 2
        elif args[i] == "--eps": eps = float(args[i+1]); i += 2
        else: i += 1

    rows = parse(path)
    if warmup: rows = rows[warmup:]
    if len(rows) < 4:
        print(f"!! only {len(rows)} MEMSTAT samples after warmup — need >=4. "
              f"Is the serial log captured and the run long enough?")
        sys.exit(2)

    t = col(rows, "t_s")
    span = t[-1]-t[0] if t[-1] > t[0] else 1
    print(f"== analyze_drift: {len(rows)} samples over {span}s "
          f"({t[0]}..{t[-1]}s), warmup={warmup} ==\n")

    # --- throughput / rates ---
    print("-- throughput (cumulative deltas over the run) --")
    for name in RATE:
        x = col(rows, name)
        d = x[-1]-x[0]
        rate = d/span
        print(f"  {name:12s} +{d:<12d} = {rate:12.1f}/s")
    # derived mean PMM search latency per interval
    ac = col(rows,"alloc_calls"); sns = col(rows,"search_ns_total")
    means = []
    for k in range(1,len(rows)):
        da = ac[k]-ac[k-1]; ds = sns[k]-sns[k-1]
        means.append(ds/da if da>0 else 0.0)
    if means:
        S,z = mann_kendall([int(m) for m in means])
        verdict = "DRIFT" if abs(z) > alpha_z and means[-1] > means[0]*(1+eps) else "FLAT "
        print(f"  mean_search_ns/interval first={means[0]:.0f} last={means[-1]:.0f} "
              f"max={max(means):.0f}  z={z:+.2f}  [{verdict}]")
    print()

    # --- flat-expected level counters: Mann-Kendall + magnitude ---
    print("-- level counters (must be FLAT; DRIFT = leak/fragmentation) --")
    drift_found = []
    for name in FLAT:
        x = col(rows, name)
        S, z = mann_kendall(x)
        sl = slope(t, x)
        base = max(abs(x[0]), 1)
        rel = (x[-1]-x[0]) / base
        # DRIFT iff statistically monotonic AND magnitude beyond noise band.
        is_drift = abs(z) > alpha_z and abs(rel) > eps
        verdict = "DRIFT" if is_drift else "FLAT "
        if is_drift: drift_found.append(name)
        print(f"  {name:9s} first={x[0]:<11d} last={x[-1]:<11d} "
              f"min={min(x):<11d} max={max(x):<11d} d/s={sl:+10.3f} z={z:+6.2f} [{verdict}]")

    print()
    if drift_found:
        print(f"RESULT: DRIFT detected in {len(drift_found)} counter(s): {', '.join(drift_found)}")
        sys.exit(1)
    print("RESULT: all level counters FLAT (no monotonic drift beyond noise band) — PASS")
    sys.exit(0)

if __name__ == "__main__":
    main()
