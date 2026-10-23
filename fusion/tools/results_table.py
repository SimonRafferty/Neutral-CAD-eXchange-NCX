"""Print the per-sample results table (Markdown) from fusion/results/*.json (run_samples.py) and
fusion/results/roundtrip_all.txt (roundtrip.py)."""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from run_samples import classify  # noqa: E402

RES = os.path.join(HERE, "..", "results")
# validator verdicts of the re-exports (validate_rt.json: ncx_validate.py --json over results/rt)
val = {}
p = os.path.join(RES, "validate_rt.json")
if os.path.exists(p):
    data = json.load(open(p, encoding="utf-8"))
    for rep in (data if isinstance(data, list) else data.get("files", [])):
        f = os.path.basename(rep.get("file", ""))
        if f.endswith(".ncx.step"):
            val[f[:-9]] = rep.get("verdict", "?")
rt = {}
p = os.path.join(RES, "roundtrip_all.txt")
if os.path.exists(p):
    for line in open(p, encoding="utf-8"):
        m = re.match(r"(\S+)\s+exported mapped=(\d+) opaque=(\d+).*-> reimport (\S+)", line)
        if m:
            rt[m.group(1)] = f"{m.group(4)} ({m.group(2)} mapped, {m.group(3)} as results)"
        m = re.match(r"(\S+)\s+skipped", line)
        if m:
            rt[m.group(1)] = "not tried (import fell back)"

order = {"match": 0, "count_only": 0, "skipped": 1, "topology": 1, "mismatch": 3, "failed": 3}
print("| sample | import | mass | faces | structure | flex | approximations / fallbacks (first ones) | round trip | validator (re-export) |")
print("|---|---|---|---|---|---|---|---|---|")
counts = {}
for f in sorted(os.listdir(RES)):
    if not f.endswith(".json") or f.startswith("validate"):
        continue
    name = f[:-5]
    rep = json.load(open(os.path.join(RES, f), encoding="utf-8"))
    cls = classify(rep)
    counts[cls] = counts.get(cls, 0) + 1
    ver = rep.get("verification", [])
    v = {}
    for k in ("mass", "faces", "structure", "flex"):
        vals = [x.get(k) for x in ver if x.get(k)]
        v[k] = max(vals, key=lambda s: order.get(s, 2)) if vals else "-"
    items = [e for e in rep.get("report", []) if e["result"] in ("approximate", "failed", "fallback")]
    why = "; ".join(f"{e['item']} ({e['result']})" for e in items[:4]) + (f"; +{len(items) - 4}" if len(items) > 4 else "")
    print(f"| {name} | {cls} | {v['mass']} | {v['faces']} | {v['structure']} | {v['flex']} | {why} | {rt.get(name, '-')} | {val.get(name, '-')} |")
print()
print("Import totals: " + ", ".join(f"{k} {n}" for k, n in sorted(counts.items())))
