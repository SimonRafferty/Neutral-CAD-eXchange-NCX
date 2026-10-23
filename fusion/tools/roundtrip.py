"""Round trip in Fusion: import a sample, export the rebuilt design as NCX, import that file again.
  python fusion/tools/roundtrip.py [name-substring ...]
Writes fusion/results/rt/<name>.ncx.step (+ .json payload) and prints one line per sample."""
import json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import fjob
from run_samples import classify
SAMPLES = os.path.join(ROOT, "samples")
OUT = os.path.join(ROOT, "fusion", "results", "rt")
os.makedirs(OUT, exist_ok=True)
names = sys.argv[1:]
files = sorted(f for f in os.listdir(SAMPLES) if f.endswith(".ncx.step"))
if names:
    files = [f for f in files if any(n in f for n in names)]
summary = {}
for f in files:
    name = f[:-9]
    a = fjob.run({"cmd": "import", "file": os.path.join(SAMPLES, f)})
    if a.get("status") not in ("rebuilt", "partial"):
        fjob.run({"cmd": "closeDocs"})
        print(f"{name:24s} skipped (first import {a.get('status')})")
        continue
    out = os.path.join(OUT, name + ".ncx.step")
    e = fjob.run({"cmd": "export", "file": out, "dumpModel": True})
    fjob.run({"cmd": "closeDocs"})
    if not e.get("ok"):
        print(f"{name:24s} EXPORT FAILED {e.get('error')}")
        summary["export failed"] = summary.get("export failed", 0) + 1
        continue
    b = fjob.run({"cmd": "import", "file": out, "close": True})
    cls = classify(b)
    summary[cls] = summary.get(cls, 0) + 1
    issues = [f"{x['item']}: {x['result']} - {x['reason']}" for x in b.get("report", []) if x["result"] not in ("rebuilt", "note")]
    print(f"{name:24s} exported mapped={e.get('mapped')} opaque={e.get('opaque')} validate={e.get('validate')} -> reimport {cls} ({b.get('status')}, stale={b.get('staleCheck')})")
    for nn in e.get("notes", [])[:4]:
        print("       export note:", nn[:200])
    for i in issues[:5]:
        print("      ", i[:200])
print("summary:", summary)
