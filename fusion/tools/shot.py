"""Import a sample (kept open), screenshot it, list its timeline, close it.
  python fusion/tools/shot.py <sample.ncx.step> <out.png> [view] [extra import JSON]"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fjob
f, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
view = sys.argv[3] if len(sys.argv) > 3 else "iso"
req = {"cmd": "import", "file": f, "keepPartial": True}
if len(sys.argv) > 4:
    req.update(json.loads(sys.argv[4]))
rep = fjob.run(req)
print("status", rep.get("status"))
tl = fjob.run({"cmd": "probe", "what": "timeline"})
for t in tl.get("timeline", []):
    print(" ", t["name"], t["type"].split("::")[-1], t["health"], t["message"][:150])
print(fjob.run({"cmd": "screenshot", "file": out, "view": view}))
fjob.run({"cmd": "closeDocs"})
