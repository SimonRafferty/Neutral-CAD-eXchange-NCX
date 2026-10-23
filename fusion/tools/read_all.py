"""Step 2.1 check: run ncx_payload_read (the core, via its ctypes binding) on every conformance vector and
every sample; write each sample's payload JSON to fusion/out/<name>.json for inspection.
Usage: python fusion/tools/read_all.py   (needs core/build/Release/ncx_core.dll)"""
import json, os, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "python"))
from ncx.core import Core
core = Core()
vec_dir = os.path.join(ROOT, "conformance", "vectors")
bad = 0
for v in json.load(open(os.path.join(vec_dir, "vectors.json"))):
    data = open(os.path.join(vec_dir, v["file"]), "rb").read()
    r = core.payload_read(data)
    st = r[0]
    ok = st == v["expect"]
    if ok and v.get("model"):
        exp = open(os.path.join(vec_dir, v["model"]), "rb").read()
        ok = r[2] == exp
    bad += not ok
    print(("PASS" if ok else "FAIL"), v["name"], st, "expected", v["expect"])
print("conformance failures:", bad)
out = os.path.join(ROOT, "fusion", "out"); os.makedirs(out, exist_ok=True)
sd = os.path.join(ROOT, "samples")
for f in sorted(os.listdir(sd)):
    if not f.endswith(".ncx.step"): continue
    r = core.payload_read(open(os.path.join(sd, f), "rb").read())
    name = f[:-9]
    if r[0] == "OK":
        open(os.path.join(out, name + ".json"), "wb").write(r[2])
    print(name, r[0], len(r[2] or b""))
