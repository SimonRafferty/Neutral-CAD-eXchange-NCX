"""Developer: drive one joint of the open import and list which occurrences followed and which stayed.

Usage: python fusion/tools/drive_check.py <joint name> [deg] [suppress JSON list]
Fusion's API does not solve a closed loop when a joint's value is set (FINDINGS 13), so a loop-closing joint may be
suppressed for the test (`suppress`); the probe restores it and the joint value afterwards.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fjob  # noqa: E402


def main(argv):
    name = argv[0]
    deg = float(argv[1]) if len(argv) > 1 else 10.0
    sup = json.loads(argv[2]) if len(argv) > 2 else []
    occ = fjob.run({"cmd": "probe", "what": "joints"}, timeout=120)
    paths = [o["path"] for o in occ.get("occurrences", []) if o.get("visible") and "NCX source parts" not in o["path"]]
    r = fjob.run({"cmd": "probe", "what": "jointmove", "name": name, "deg": deg, "suppress": sup}, timeout=120)
    if not r.get("ok"):
        print(r)
        return 1
    moved = {m["occurrence"] for m in r.get("moved", [])}
    print(f"joint {name} ({r.get('type')}), set {r.get('set')}, {deg} deg, suppressed {sup}")
    print(f"moved {len(moved)}:")
    for p in sorted(moved):
        print("  +", p)
    stayed = [p for p in paths if p not in moved]
    print(f"stayed {len(stayed)}:")
    for p in sorted(stayed):
        print("  -", p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
