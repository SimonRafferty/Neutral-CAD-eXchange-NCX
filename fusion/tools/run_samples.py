"""Import every sample (or the named ones) in Fusion through the add-in's job folder and tabulate the results.

  python fusion/tools/run_samples.py [name-substring ...] [--keep]

Writes fusion/results/<sample>.json (the import reply) and prints one line per sample. --keep leaves the
rebuilt designs open (default: closed unsaved).
"""
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import fjob  # noqa: E402

SAMPLES = os.path.join(ROOT, "samples")
OUT = os.path.join(ROOT, "fusion", "results")


def classify(rep):
    """exact / approximate / fallback / failed for one sample reply."""
    if not rep.get("ok"):
        return "failed"
    if rep.get("status") == "fallback":
        return "fallback"
    counts = rep.get("counts", {})
    ver = rep.get("verification", [])
    checks_ok = all(v.get("mass") in ("match", "count_only") and v.get("faces", "match") in ("match", "topology", "skipped")
                    and v.get("structure", "match") == "match" and v.get("flex", "match") in ("match", "skipped") for v in ver)
    if counts.get("failed"):
        return "failed"
    if counts.get("approximate") or not checks_ok:
        return "approximate"
    return "exact"


def main(argv):
    keep = "--keep" in argv
    names = [a for a in argv if not a.startswith("--")]
    os.makedirs(OUT, exist_ok=True)
    files = sorted(f for f in os.listdir(SAMPLES) if f.endswith(".ncx.step"))
    if names:
        files = [f for f in files if any(n in f for n in names)]
    rows = []
    for f in files:
        name = f[:-9]
        t0 = time.time()
        rep = fjob.run({"cmd": "import", "file": os.path.join(SAMPLES, f), "close": not keep}, timeout=1800)
        with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as fh:
            json.dump(rep, fh, indent=1)
        cls = classify(rep)
        ver = rep.get("verification", [])
        # worst result of each check over every verified document
        order = {"match": 0, "count_only": 0, "topology": 1, "skipped": 1, "-": -1, "mismatch": 3, "failed": 3}
        v = {}
        for k in ("mass", "faces", "structure", "flex"):
            vals = [x.get(k) for x in ver if x.get(k)]
            v[k] = max(vals, key=lambda s: order.get(s, 2)) if vals else "-"
        issues = [f"{e['item']}: {e['result']} - {e['reason']}" for e in rep.get("report", []) if e["result"] != "rebuilt"]
        line = f"{name:24s} {cls:12s} mass={v.get('mass','-'):10s} faces={v.get('faces','-'):9s} struct={v.get('structure','-'):8s} flex={v.get('flex','-'):8s} {time.time()-t0:6.1f}s"
        print(line, flush=True)
        for i in issues[:6]:
            print("      ", i[:220], flush=True)
        if not rep.get("ok"):
            print("       ERROR", rep.get("error"), flush=True)
        rows.append((name, cls))
    summary = {}
    for _, c in rows:
        summary[c] = summary.get(c, 0) + 1
    print("summary:", summary)


if __name__ == "__main__":
    main(sys.argv[1:])
