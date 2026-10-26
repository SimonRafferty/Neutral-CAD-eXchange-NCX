"""Show how an NCX file's assemblies hold their components (developer check).

Per assembly: its components (fixed / flexible / sub-assembly), and per component the mates that touch it, with
the partner on the other side. A component that is neither fixed nor in any active mate is flagged FREE (it comes
apart when dragged); one whose only mates are a single concentric (or a single plane mate) is flagged LOOSE (it
can slide or spin off its partner).

Usage: python fusion/tools/asm_check.py <file.ncx.step> [--brief]
"""
import json
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "python"))
from ncx import specread  # noqa: E402

MECH = {"gear", "screw", "rackPinion", "cam"}


def suppressed(m):
    s = m.get("suppressed")
    return isinstance(s, dict) and any(s.values())


def side_name(r, comps):
    if "component" not in r:
        return "(assembly)"
    n = comps.get(r["component"], {}).get("name", r["component"])
    if r.get("path"):
        n += "/" + "/".join(r["path"])
    return n


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    with open(argv[0], encoding="utf-8", errors="replace") as f:
        st, fields, raw = specread.read(f.read())
    model = json.loads(raw)
    docs = {d["id"]: d for d in model["documents"]}
    bad = 0
    for d in model["documents"]:
        if d.get("kind") != "assembly":
            continue
        comps = {c["id"]: c for c in d.get("components", [])}
        mates = d.get("mates", [])
        nfix = sum(1 for c in comps.values() if c.get("fixed"))
        print(f"\n== {d['name']}  ({'root' if d['id'] == model.get('root') else 'sub-assembly'}): "
              f"{len(comps)} components, {nfix} fixed, {len(mates)} mates "
              f"({sum(1 for m in mates if suppressed(m))} suppressed)")
        by = defaultdict(list)
        for m in mates:
            if suppressed(m):
                continue
            refs = m.get("refs", [])
            units = [r.get("component", "") for r in refs]
            for i, r in enumerate(refs):
                u = r.get("component", "")
                if not u:
                    continue
                others = sorted({side_name(refs[j], comps) for j in range(len(refs)) if units[j] != u})
                lim = ""
                p = m.get("params", {})
                if "min" in p or "max" in p:
                    lim = "[limit]"
                by[u].append(f"{m['type']}{lim}->{','.join(others) or 'self'}")
        for cid, c in comps.items():
            sub = docs.get(c["document"], {}).get("kind") == "assembly"
            tags = []
            if c.get("fixed"):
                tags.append("FIXED")
            if sub:
                tags.append("sub-assembly" + (" flexible" if c.get("flexible") else " rigid"))
            ms = by.get(cid, [])
            flag = ""
            if not c.get("fixed") and not ms:
                flag = "  <-- FREE"
            elif not c.get("fixed"):
                plain = [x for x in ms if x.split("->")[0].split("[")[0] not in MECH]
                partners = {x.split("->")[1] for x in plain}
                kinds = [x.split("->")[0] for x in plain]
                if len(plain) == 1 and kinds[0] in ("concentric", "coincident", "distance", "parallel"):
                    flag = "  <-- LOOSE"
                elif len(partners) == 1 and set(kinds) <= {"concentric"}:
                    flag = "  <-- LOOSE"
            if flag:
                bad += 1
            print(f"  {c.get('name', cid):40s} {' '.join(tags):26s} {len(ms):2d}: {'; '.join(ms)}{flag}")
    print(f"\n{bad} component(s) flagged")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
