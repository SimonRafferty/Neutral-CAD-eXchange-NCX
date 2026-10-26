"""Developer: import an NCX file into Fusion, export the result again, close it (nothing saved).

Usage: python fusion/tools/imex.py <in.ncx.step> <out.ncx.step> <result.json> [import options JSON]
The import reply (report, joint frames) and the export reply go into result.json. The imported document is closed
unsaved afterwards (closeDocs) unless IMEX_KEEP is set.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fjob  # noqa: E402


def main(argv):
    src, out, res = argv[0], argv[1], argv[2]
    req = {"cmd": "import", "file": os.path.abspath(src), "quiet": True, "close": False}
    if len(argv) > 3:
        req.update(json.loads(argv[3]))
    imp = fjob.run(req, timeout=900)
    print("import:", imp.get("ok"), imp.get("status"), imp.get("seconds"), imp.get("error", ""))
    exp = None
    if imp.get("ok"):
        exp = fjob.run({"cmd": "export", "file": os.path.abspath(out), "quiet": True, "progress": True}, timeout=900)
        print("export:", exp.get("ok"), exp.get("validate"), exp.get("seconds"), exp.get("error", ""))
    if not os.environ.get("IMEX_KEEP"):
        print("close:", fjob.run({"cmd": "closeDocs"}, timeout=120).get("closed"))
    with open(res, "w", encoding="utf-8") as f:
        json.dump({"import": imp, "export": exp}, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
