"""Run a job inside Fusion through the NCX add-in's developer job folder.

Fusion has no out-of-process API. The NCX add-in watches %LOCALAPPDATA%\\NcxFusion\\jobs (only when that folder
exists): a "<name>.job.json" dropped there runs on Fusion's main thread and is answered with
"<name>.result.json".

Usage:
  python fusion/tools/fjob.py ping
  python fusion/tools/fjob.py reload                      # load a rebuilt NcxFusionImpl.dll
  python fusion/tools/fjob.py docs                        # open documents (name, modified, project)
  python fusion/tools/fjob.py '{"cmd": "probe", ...}'     # any JSON request
  python fusion/tools/fjob.py import <file.ncx.step> [extra JSON]

As a module: run(request_dict, timeout) -> reply dict.
"""
import json
import os
import sys
import time
import uuid

JOBS = os.path.join(os.environ["LOCALAPPDATA"], "NcxFusion", "jobs")


def run(request, timeout=600):
    os.makedirs(JOBS, exist_ok=True)
    name = time.strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:6]
    tmp = os.path.join(JOBS, name + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(request, f)
    os.replace(tmp, os.path.join(JOBS, name + ".job.json"))
    res = os.path.join(JOBS, name + ".result.json")
    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.exists(res):
            time.sleep(0.05)
            with open(res, encoding="utf-8") as f:
                data = json.load(f)
            os.remove(res)
            return data
        time.sleep(0.2)
    # leave the job: Fusion may still pick it up; the caller decides
    return {"ok": False, "error": f"timeout after {timeout}s (is Fusion running with the NCX add-in?)", "job": name}


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    a = argv[0]
    if a in ("ping", "reload", "docs", "version", "closeDocs"):
        req = {"cmd": a}
    elif a == "probe":   # probe <what> [file] [extra JSON]
        req = {"cmd": "probe", "what": argv[1]}
        if len(argv) > 2:
            req["file"] = os.path.abspath(argv[2])
        if len(argv) > 3:
            req.update(json.loads(argv[3]))
    elif a == "import":
        req = {"cmd": "import", "file": os.path.abspath(argv[1])}
        if len(argv) > 2:
            req.update(json.loads(argv[2]))
    else:
        req = json.loads(a)
    timeout = float(os.environ.get("FJOB_TIMEOUT", "900"))
    print(json.dumps(run(req, timeout), indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
