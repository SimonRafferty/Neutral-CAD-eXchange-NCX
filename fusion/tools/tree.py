"""Print the active Fusion design's component tree: bodies (volume mm^3, box mm) and occurrence offsets.
  python fusion/tools/tree.py"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fjob


def show(n, d=0):
    t = n.get("transform")
    off = f" at ({t[3]*10:.3f}, {t[7]*10:.3f}, {t[11]*10:.3f})" if t else ""
    print(" " * d + n["component"] + (" [" + n["occurrence"] + "]" if n.get("occurrence") else "") + off)
    for b in n["bodies"]:
        print(" " * d + "  body", b["name"], "solid" if b["solid"] else "surface", round(b["volume_mm3"], 3),
              [round(x, 3) for x in b.get("bbox", [])])
    for o in n["occurrences"]:
        show(o, d + 2)


r = fjob.run({"cmd": "probe", "what": "tree"}, 60)
if not r.get("ok"):
    print(r)
else:
    show(r["tree"])
