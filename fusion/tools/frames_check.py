"""Joints by numbers: each joint the import made (the reply's `jointFrames`) against the `frame` of the mate it was
made from (FORMAT-SPEC 5, Mates: the mate's frame in the assembly's frame at the solved placement).

  python fusion/tools/frames_check.py <import reply .json> <payload .json>

For every joint: the angle between its axis and the mate frame's z (degrees, sense ignored: a joint's axis sense
does not change the motion), the origin's distance from the mate's axis line (mm: what matters for a turning joint),
and its offset along that axis (mm: where on the pin it sits, which does not change the motion). The mate is the
one the joint is named after (the first mate of the pair); when that is not an axial mate (a coincident plane), a
turning joint is compared with the axial (concentric) mate of its assembly whose axis line is nearest to the joint
(named in the table). A joint of a sub-assembly is compared in that sub-assembly's own frame, as both are recorded
there.
"""
import json
import math
import sys


def sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def dot(a, b):
    return sum(a[i] * b[i] for i in range(3))


def norm(a):
    return math.sqrt(dot(a, a))


def main(reply_path, payload_path):
    reply = json.load(open(reply_path, encoding="utf-8"))
    model = json.load(open(payload_path, encoding="utf-8"))
    mates = {}
    for d in model["documents"]:
        for m in d.get("mates", []):
            mates[(d["name"], m["name"])] = m
    worst_angle = worst_off = 0.0
    rows = []
    for j in reply.get("jointFrames", []):
        m = mates.get((j["assembly"], j["joint"]))
        if not m or "frame" not in m:
            rows.append((j["assembly"], j["joint"], j["motion"], "no mate frame", "", "", j.get("one", ""), j.get("two", "")))
            continue
        a = j["axis"]
        used = m["name"]
        if j["motion"] in ("revolute", "cylindrical") and m["type"] not in ("concentric", "hinge"):
            best = None
            for (asm, nm), mm in mates.items():
                if asm != j["assembly"] or mm["type"] not in ("concentric", "hinge") or "frame" not in mm:
                    continue
                zz, oo = mm["frame"]["z"], mm["frame"]["origin"]
                if abs(abs(dot(a, zz)) / (norm(a) * norm(zz)) - 1) > 1e-6:
                    continue
                dd = sub(j["origin"], oo)
                al = dot(dd, zz) / norm(zz)
                of = norm(sub(dd, [zz[i] * al / norm(zz) for i in range(3)]))
                if best is None or of < best[0]:
                    best = (of, mm)
            if best:
                m = best[1]
                used = m["name"] + " (nearest axial mate)"
        z = m["frame"]["z"]
        o = m["frame"]["origin"]
        c = abs(dot(a, z)) / (norm(a) * norm(z))
        ang = math.degrees(math.acos(min(1.0, c)))
        d = sub(j["origin"], o)
        along = dot(d, z) / norm(z)
        off = norm(sub(d, [z[i] * along / norm(z) for i in range(3)]))
        turning = j["motion"] in ("revolute", "cylindrical")
        if turning:
            worst_angle = max(worst_angle, ang)
            worst_off = max(worst_off, off)
        rows.append((j["assembly"], j["joint"] + ("" if used == j["joint"] else " vs " + used), j["motion"], f"{ang:.2e}", f"{off:.4f}", f"{along:.3f}", j.get("one", ""), j.get("two", "")))
    print("| assembly | joint | motion | axis vs mate z (deg) | origin off the mate's axis (mm) | along the axis (mm) | occurrence one | occurrence two |")
    print("|---|---|---|---|---|---|---|---|")
    for r in rows:
        print("| " + " | ".join(str(x) for x in r) + " |")
    print()
    print(f"turning joints: worst axis angle {worst_angle:.2e} deg, worst origin offset from the mate's axis {worst_off:.4f} mm")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
