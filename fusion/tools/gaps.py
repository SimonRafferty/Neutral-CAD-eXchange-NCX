"""Import one file through the job folder and print the progress timeline: phase seconds and the longest gaps
between two progress updates (the import reply's "progress").

  python fusion/tools/gaps.py <file.ncx.step> <out.json> [extra JSON for the job]
  python fusion/tools/gaps.py --show <result.json>
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fjob  # noqa: E402


def show(rep, top=10):
    p = rep.get("progress", {})
    print(f"status={rep.get('status')} seconds={rep.get('seconds', 0):.1f} steps={p.get('steps')} counts={rep.get('counts')}")
    for k, v in sorted(p.get("phases", {}).items(), key=lambda kv: -kv[1]):
        print(f"  phase {k:20s} {v:8.1f} s")
    for g in p.get("longestGaps", [])[:top]:
        print(f"  gap {g['seconds']:7.2f} s  [{g.get('phase','')}]  {g['before'][:70]!r} -> {g['after'][:70]!r}")


def main(argv):
    if argv[0] == "--show":
        with open(argv[1], encoding="utf-8") as f:
            show(json.load(f), 15)
        return
    req = {"cmd": "import", "file": os.path.abspath(argv[0]), "close": True}
    if len(argv) > 2:
        req.update(json.loads(argv[2]))
    rep = fjob.run(req, timeout=3600)
    with open(argv[1], "w", encoding="utf-8") as f:
        json.dump(rep, f, indent=1)
    show(rep)


if __name__ == "__main__":
    main(sys.argv[1:])
