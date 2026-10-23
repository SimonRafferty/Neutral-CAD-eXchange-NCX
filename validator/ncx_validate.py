"""NCX file validator: does an NCX file meet the format (FORMAT-SPEC.md), so that any NCX add-in can import it?

Usage:
    python validator/ncx_validate.py <file.ncx.step> [more files] [--json report.json] [--max-level N] [--quiet]

Four levels, each needing the one before it:
    1 container    the STEP file, the payload's placement, header, chunks and encoding (FORMAT-SPEC 1-2)
    2 schema       the payload JSON against schema/ncx-0.1.schema.json (FORMAT-SPEC 4)
    3 consistency  the model's own rules, without any CAD system: ids and references resolve, sketch seeds agree
                   with their relations and dimensions, frames and transforms are rigid, configurations, equations
                   and flex probes refer to things that exist, the file fingerprint is the sum of its documents
                   (FORMAT-SPEC 4-6)
    4 geometry     the file's own B-rep measured with OpenCASCADE: the notice body, the stale check against the
                   payload's fingerprint, feature results, and every reference that is measured in the finished
                   model (mates, face appearances) resolved by the shared matcher (FORMAT-SPEC 2.6, 4, 5, 6)

Verdict: PASS (every level ran, no errors), FAIL (at least one error) or INCOMPLETE (no errors, but a level could
not run: the core library is not built, or OpenCASCADE is not installed). Errors break a MUST of the spec or would
make an importer fall back or build something wrong; warnings are SHOULDs and likely mistakes.

Needs: Python 3.10+, jsonschema; for levels 3-4 the built core (core/build.ps1, or NCX_CORE_DLL); for level 4
cadquery-ocp (OpenCASCADE). It never changes the file.
"""
import argparse
import base64
import hashlib
import json
import math
import os
import re
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, "python"))

from ncx import specread  # noqa: E402  (the independent container reader, written from the spec)

VERSION = "0.1.0"
SCHEMA_PATH = os.path.join(REPO, "schema", "ncx-0.1.schema.json")
MAX_STRING = 8000          # FORMAT-SPEC 1.3
MAX_HEADER = 1000          # FORMAT-SPEC 2.2
LEN_TOL, REL_TOL = 1e-3, 1e-6   # sketch seed vs dimension: mm, relative
ANG_TOL = 1e-3             # deg
UNIT_TOL = 1e-6            # frames, transforms
ASSEMBLY_NON_GEOMETRY = {"sketch", "sketch3d", "plane", "axis", "point", "coordinateSystem", "componentPattern",
                         "mirrorComponents"}
DIRECTION_KEYS = {"normal", "axis", "direction", "x", "y", "z", "xDir", "yDir", "zDir", "dir", "majorAxis"}


# ------------------------------------------------------------------------------------------------ findings
class Report:
    def __init__(self, path):
        self.path = path
        self.findings = []
        self.levels = {}
        self.stats = {}

    def add(self, level, severity, code, spec, message, where=""):
        self.findings.append({"level": level, "severity": severity, "code": code, "spec": spec,
                              "where": where, "message": message})

    def error(self, level, code, spec, message, where=""):
        self.add(level, "error", code, spec, message, where)

    def warn(self, level, code, spec, message, where=""):
        self.add(level, "warning", code, spec, message, where)

    def count(self, key, n=1):
        self.stats[key] = self.stats.get(key, 0) + n

    def errors(self, level=None):
        return [f for f in self.findings if f["severity"] == "error" and (level is None or f["level"] == level)]

    def as_dict(self):
        errs = self.errors()
        skipped = [lv for lv, s in self.levels.items() if s.get("skipped")]
        verdict = "FAIL" if errs else ("INCOMPLETE" if skipped else "PASS")
        return {"file": self.path, "verdict": verdict, "validator": VERSION, "spec": "0.1",
                "summary": {"errors": len(errs), "warnings": len(self.findings) - len(errs)},
                "levels": [dict(level=lv, **s) for lv, s in sorted(self.levels.items())],
                "findings": self.findings, "checked": self.stats}


# ------------------------------------------------------------------------------------------------ helpers
def vsub(a, b):
    return [a[i] - b[i] for i in range(len(a))]


def vdot(a, b):
    return sum(a[i] * b[i] for i in range(len(a)))


def vnorm(a):
    return math.sqrt(vdot(a, a))


def vcross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def is_vec(v, n=3):
    return isinstance(v, list) and len(v) == n and all(isinstance(x, (int, float)) for x in v)


def close_len(a, b):
    return abs(a - b) <= max(LEN_TOL, REL_TOL * abs(b))


def mat_from_row_major(t):
    """4x4 row-major list -> (R 3x3 rows, translation)."""
    return [t[0:3], t[4:7], t[8:11]], [t[3], t[7], t[11]]


def mat_mul(a, b):
    """Compose two row-major 4x4 transforms (lists of 16): a * b."""
    return [sum(a[r * 4 + k] * b[k * 4 + c] for k in range(4)) for r in range(4) for c in range(4)]


def apply_point(t, p):
    return [t[r * 4] * p[0] + t[r * 4 + 1] * p[1] + t[r * 4 + 2] * p[2] + t[r * 4 + 3] for r in range(3)]


def apply_dir(t, d):
    return [t[r * 4] * d[0] + t[r * 4 + 1] * d[1] + t[r * 4 + 2] * d[2] for r in range(3)]


IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def transform_fingerprint(fp, t):
    """A body fingerprint moved by a rigid transform: centroid moved, box re-boxed; the rest is invariant."""
    out = dict(fp)
    out["centroid"] = apply_point(t, fp["centroid"])
    b = fp.get("bbox")
    if is_vec(b, 6):
        corners = [apply_point(t, [x, y, z]) for x in (b[0], b[3]) for y in (b[1], b[4]) for z in (b[2], b[5])]
        out["bbox"] = [min(c[i] for c in corners) for i in range(3)] + [max(c[i] for c in corners) for i in range(3)]
    return out


def header_form(s):
    """FORMAT-SPEC 2.6 'matching names': header form, path characters as _, spaces dropped."""
    s = "".join("_" if (ch in ";'\\" or ord(ch) < 32 or ord(ch) == 127) else ch for ch in s)
    s = re.sub(r'[/\\:*?"<>|]', "_", s)
    return s.replace(" ", "")


def names_match(body_name, notice):
    a, b = header_form(body_name), header_form(notice)
    return a == b or (len(a) >= 7 and b.startswith(a))


def load_core():
    try:
        from ncx.core import core
        c = core()
        return c, None
    except Exception as e:  # not built, or not loadable on this platform
        return None, str(e)


def load_occ():
    try:
        from ncx import occhost  # noqa: F401
        return occhost, None
    except Exception as e:
        return None, str(e)


# ------------------------------------------------------------------------------------------------ level 1
def string_literals(text):
    """(start offset, length) of every Part 21 string literal, '' escapes respected, comments skipped."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == "'":
            j = i + 1
            while j < n:
                if text[j] == "'":
                    if j + 1 < n and text[j + 1] == "'":
                        j += 2
                        continue
                    break
                j += 1
            out.append((i, j - i - 1))
            i = j + 1
        elif text.startswith("/*", i):
            k = text.find("*/", i + 2)
            i = n if k < 0 else k + 2
        else:
            i += 1
    return out


def level1(rep, text):
    L = 1
    if not text.lstrip().startswith("ISO-10303-21;"):
        rep.error(L, "C-PART21", "1.1", "not an ISO 10303-21 file (it must start with 'ISO-10303-21;')")
        return None
    for kw in ("HEADER;", "DATA;", "END-ISO-10303-21;"):
        if kw not in text:
            rep.error(L, "C-PART21", "1.1", f"Part 21 section marker {kw} missing")
    m = re.search(r"FILE_SCHEMA\s*\(\s*\(\s*'([^']*)'", text)
    schema = (m.group(1) if m else "").upper()
    recs, _, _ = specread._records(text)
    if recs is None:
        rep.error(L, "C-PART21", "1.1", "the DATA section could not be parsed")
        return None
    is_asm = any(t == "NEXT_ASSEMBLY_USAGE_OCCURRENCE" for _, t, _ in recs)
    if not schema:
        rep.error(L, "C-AP", "1.1", "no FILE_SCHEMA in the header")
    elif "AUTOMOTIVE_DESIGN" not in schema:
        if "AP242" in schema or "MANAGED_MODEL_BASED" in schema:
            rep.error(L, "C-AP", "1.1", "declared AP242" + (" for an assembly: SolidWorks 2018 never returns from "
                      "importing it" if is_asm else "") + "; NCX files MUST declare AP214 (AUTOMOTIVE_DESIGN)")
        else:
            rep.error(L, "C-AP", "1.1", f"FILE_SCHEMA is {schema}; NCX files MUST declare AP214 (AUTOMOTIVE_DESIGN)")

    long_strings = [(o, n) for o, n in string_literals(text) if n > MAX_STRING]
    for o, n in long_strings[:5]:
        line = text.count("\n", 0, o) + 1
        rep.error(L, "C-STRING-LEN", "1.3", f"string literal of {n} characters at line {line} (limit {MAX_STRING}; "
                  "OpenCASCADE/FreeCAD reject a whole file with any string over 16,380)")
    if len(long_strings) > 5:
        rep.error(L, "C-STRING-LEN", "1.3", f"... and {len(long_strings) - 5} more strings over {MAX_STRING} characters")

    by_id = {i: (t, b) for i, t, b in recs}
    pds = [i for i, t, b in recs if t == "PROPERTY_DEFINITION" and specread._args(b)[0] == "NCX"]
    on_pd = [p for p in pds if by_id.get(specread._args(by_id[p][1])[2], ("",))[0] == "PRODUCT_DEFINITION"]
    roots = specread._roots(recs)
    if len(on_pd) > 1:
        rep.error(L, "C-PROP-COUNT", "2.4", f"{len(on_pd)} NCX properties: a writer MUST remove an earlier payload "
                  "first (a file never carries two)")
    if on_pd and not any(specread._args(by_id[p][1])[2] in roots for p in on_pd):
        rep.error(L, "C-PROP-ROOT", "2.1", "the NCX property is not on the root PRODUCT_DEFINITION (the one never "
                  "used as a component)")
    if on_pd:
        pd = next((p for p in on_pd if specread._args(by_id[p][1])[2] in roots), on_pd[0])
        gps = [i for i, t, b in recs if t == "GENERAL_PROPERTY" and specread._args(b)[0] == "NCX"]
        assoc = [i for i, t, b in recs if t == "GENERAL_PROPERTY_ASSOCIATION"
                 and specread._args(b)[2] in gps and specread._args(b)[3] == pd]
        if not gps or not assoc:
            rep.warn(L, "C-GENPROP", "2.1", "no GENERAL_PROPERTY 'NCX' associated with the NCX PROPERTY_DEFINITION: "
                     "OpenCASCADE's metadata reader will not see the property")
        rep_id = next((specread._args(b)[1] for _, t, b in recs
                       if t == "PROPERTY_DEFINITION_REPRESENTATION" and specread._args(b)[0] == pd), None)
        if rep_id in by_id:
            items = specread._args(by_id[rep_id][1])[1]
            if specread._args(by_id[rep_id][1])[0] != "NCX":
                rep.warn(L, "C-REPR", "2.1", "the payload REPRESENTATION should be named 'NCX'")
            idx = []
            for it in items:
                t, b = by_id.get(it, ("", ""))
                if t != "DESCRIPTIVE_REPRESENTATION_ITEM":
                    rep.error(L, "C-REPR", "2.1", f"payload item #{it} is a {t}, not a DESCRIPTIVE_REPRESENTATION_ITEM")
                    continue
                name, value = specread._args(b)[:2]
                if name == "NCX":
                    if len(value) > MAX_HEADER:
                        rep.error(L, "C-HEADER-LEN", "2.2", f"header is {len(value)} characters (limit {MAX_HEADER}; "
                                  "SolidWorks 2018 silently drops longer property values)")
                    if items and items[0] != it:
                        rep.warn(L, "C-HEADER-FIRST", "2.1", "the header item should come first: SolidWorks shows the "
                                 "first item as the NCX custom property")
                elif re.fullmatch(r"NCX\.\d+", name):
                    if not re.fullmatch(r"NCX\.\d{4,}", name):
                        rep.error(L, "C-CHUNK-NAME", "2.1", f"chunk name {name} must be zero-padded to at least 4 digits")
                    idx.append(int(name[4:]))
                    if len(value) > MAX_STRING:
                        rep.error(L, "C-CHUNK-LEN", "2.3", f"chunk {name} is {len(value)} characters (limit {MAX_STRING})")
                else:
                    rep.warn(L, "C-REPR", "2.1", f"unexpected item '{name}' in the payload representation")
            if sorted(idx) != list(range(len(idx))):
                rep.error(L, "C-CHUNK-INDEX", "2.1", "chunk indices must count from 0 without gaps or repeats")

    status, fields, raw = specread.read(text)
    rep.levels[L] = {"name": "container", "ran": True, "read_status": status}
    if status != "OK":
        spec = {"NO_PAYLOAD": "2.1", "HEADER_UNREADABLE": "2.2", "CHUNK_MISSING": "2.1", "DIGEST_MISMATCH": "2.3",
                "SCHEMA_TOO_NEW": "4"}.get(status, "2.5")
        rep.error(L, "C-READ", spec, f"the payload does not read: {status}")
        return None
    for k in ("schema", "enc", "chunks", "b64len", "jsonlen", "sha256", "fp"):
        if k not in fields:
            rep.error(L, "C-HEADER-KEY", "2.2", f"header key '{k}' missing")
    if fields.get("enc") not in (None, "deflate-b64"):
        rep.error(L, "C-HEADER-KEY", "2.3", f"enc={fields.get('enc')}: 'deflate-b64' is the only encoding defined")
    fp = fields.get("fp", "")
    if fp and fp != "none" and not re.fullmatch(r"[0-9a-f]{16}", fp):
        rep.error(L, "C-HEADER-KEY", "2.2", f"fp={fp}: must be 16 lower-case hex digits or 'none'")
    if not fields.get("gen"):
        rep.warn(L, "C-HEADER-KEY", "2.2", "no 'gen' key naming the exporter")
    if "notice" not in fields:
        rep.warn(L, "C-NOTICE", "2.6", "no notice body declared (header key 'notice'): a reader without an NCX add-in "
                 "will not be told what the file is")

    core, why = load_core()
    if core is not None:
        st2, _, raw2 = core.payload_read(text.encode("latin-1", "replace"))
        if st2 != status or (raw2 or b"") != raw:
            rep.error(L, "C-READERS-DISAGREE", "3", f"the core reads this file as {st2} but the independent reader as "
                      f"{status}: the container is ambiguous")
    return fields, raw, recs


# ------------------------------------------------------------------------------------------------ level 2
def level2(rep, model):
    L = 2
    try:
        import jsonschema
    except ImportError:
        rep.levels[L] = {"name": "schema", "ran": False, "skipped": "jsonschema is not installed (pip install jsonschema)"}
        return
    schema = json.load(open(SCHEMA_PATH, encoding="utf-8"))
    cls = jsonschema.validators.validator_for(schema)
    errs = sorted(cls(schema).iter_errors(model), key=lambda e: list(e.absolute_path))
    for e in errs[:40]:
        where = "/" + "/".join(str(p) for p in e.absolute_path)
        rep.error(L, "S-SCHEMA", "4", e.message[:300], where)
    if len(errs) > 40:
        rep.error(L, "S-SCHEMA", "4", f"... and {len(errs) - 40} more schema errors")
    rep.levels[L] = {"name": "schema", "ran": True}


# ------------------------------------------------------------------------------------------------ level 3
class Sketch:
    """A sketch's entities in its own coordinates, for checking seeds against relations and dimensions."""

    def __init__(self, ft):
        d = ft.get("def") or {}
        self.is3d = ft["type"] == "sketch3d" or d.get("space") == "3d"
        self.n = 3 if self.is3d else 2
        self.ents = {e.get("id"): e for e in d.get("entities", []) if isinstance(e, dict)}
        fr = d.get("frame") or {}
        self.origin2 = None
        if not self.is3d and is_vec(fr.get("origin")) and is_vec(fr.get("x")) and is_vec(fr.get("z")):
            y = vcross(fr["z"], fr["x"])
            o = [-c for c in fr["origin"]]
            self.origin2 = [vdot(o, fr["x"]), vdot(o, y)]

    def point(self, r):
        """Position of a point-like reference (point id, the model origin), else None."""
        if isinstance(r, dict):
            if r.get("kind") == "origin":
                return [0.0, 0.0, 0.0] if self.is3d else self.origin2
            return None
        e = self.ents.get(r)
        if e and e.get("type") == "point" and is_vec(e.get("p"), self.n):
            return e["p"]
        return None

    def ent(self, r):
        return self.ents.get(r) if isinstance(r, str) else None


def point_line_distance(p, a, b):
    d = vsub(b, a)
    L = vnorm(d)
    if L < 1e-12:
        return vnorm(vsub(p, a))
    if len(p) == 2:
        return abs(d[0] * (p[1] - a[1]) - d[1] * (p[0] - a[0])) / L
    return vnorm(vcross(d, vsub(p, a))) / L


def check_dimension(sk, dm):
    """(measured value, None) or (None, reason it was not checked)."""
    kind = dm.get("kind")
    ents = dm.get("entities", [])
    pts = [sk.point(r) for r in ents]
    es = [sk.ent(r) for r in ents]
    if kind in ("diameter", "radius") and len(es) == 1 and es[0] and "radius" in es[0]:
        return (2 if kind == "diameter" else 1) * es[0]["radius"], None
    if kind in ("horizontal", "vertical") and not sk.is3d and len(pts) == 2 and all(p is not None for p in pts):
        i = 0 if kind == "horizontal" else 1
        return abs(pts[1][i] - pts[0][i]), None
    if kind == "distance":
        if len(ents) == 2 and all(p is not None for p in pts):
            return vnorm(vsub(pts[1], pts[0])), None
        if len(ents) == 1 and es[0] and es[0].get("type") == "line":
            return vnorm(vsub(es[0]["end"], es[0]["start"])), None
        if len(ents) == 2:
            for a, b in ((0, 1), (1, 0)):
                p, e = pts[a], es[b]
                if e and e.get("type") in ("circle", "arc") and p is None and es[a] and es[a].get("type") == "point":
                    continue
                if p is None and es[a] and es[a].get("type") in ("circle", "arc"):
                    p = es[a].get("center")
                if p is not None and e and e.get("type") == "line":
                    return point_line_distance(p, e["start"], e["end"]), None
                if p is not None and e and e.get("type") in ("circle", "arc"):
                    return vnorm(vsub(e["center"], p)), None
            if es[0] and es[1] and es[0].get("type") == "line" and es[1].get("type") == "line":
                return point_line_distance(es[0]["start"], es[1]["start"], es[1]["end"]), None
    if kind == "angle" and len(es) == 2 and all(e and e.get("type") == "line" for e in es):
        d1, d2 = vsub(es[0]["end"], es[0]["start"]), vsub(es[1]["end"], es[1]["start"])
        c = max(-1.0, min(1.0, vdot(d1, d2) / max(vnorm(d1) * vnorm(d2), 1e-12)))
        return math.degrees(math.acos(c)), None
    return None, f"{kind} between {[e if isinstance(e, str) else e.get('kind') for e in ents]}"


def line_dir(e):
    return vsub(e["end"], e["start"])


def check_constraint(sk, c):
    """True (holds), False (seeds break it) or None (not checked)."""
    t, ents = c.get("type"), c.get("entities", [])
    es = [sk.ent(r) for r in ents]
    pts = [sk.point(r) for r in ents]
    if t in ("horizontal", "vertical") and not sk.is3d:
        i = 1 if t == "horizontal" else 0
        if len(es) == 1 and es[0] and es[0].get("type") == "line":
            return abs(es[0]["end"][i] - es[0]["start"][i]) <= LEN_TOL
        if len(pts) == 2 and all(p is not None for p in pts):
            return abs(pts[1][i] - pts[0][i]) <= LEN_TOL
    if t in ("alongX", "alongY", "alongZ") and sk.is3d and len(es) == 1 and es[0] and es[0].get("type") == "line":
        d = line_dir(es[0])
        axis = {"alongX": 0, "alongY": 1, "alongZ": 2}[t]
        return all(abs(d[k]) <= LEN_TOL for k in range(3) if k != axis)
    if t == "coincident" and len(ents) == 2:
        if all(p is not None for p in pts):
            return vnorm(vsub(pts[0], pts[1])) <= LEN_TOL
        for a, b in ((0, 1), (1, 0)):
            if pts[a] is not None and es[b]:
                if es[b].get("type") == "line":
                    return point_line_distance(pts[a], es[b]["start"], es[b]["end"]) <= LEN_TOL
                if es[b].get("type") in ("circle", "arc"):
                    return abs(vnorm(vsub(pts[a], es[b]["center"])) - es[b]["radius"]) <= LEN_TOL
    if t in ("parallel", "perpendicular") and len(es) == 2 and all(e and e.get("type") == "line" for e in es):
        d1, d2 = line_dir(es[0]), line_dir(es[1])
        n = max(vnorm(d1) * vnorm(d2), 1e-12)
        if t == "parallel":
            return (vnorm(vcross(d1 + [0] * (3 - len(d1)), d2 + [0] * (3 - len(d2)))) / n) <= 1e-6
        return abs(vdot(d1, d2) / n) <= 1e-6
    if t == "equal" and len(es) == 2 and all(es):
        if all(e.get("type") == "line" for e in es):
            return close_len(vnorm(line_dir(es[0])), vnorm(line_dir(es[1])))
        if all("radius" in e for e in es):
            return close_len(es[0]["radius"], es[1]["radius"])
    return None


class Model:
    """Indexes of the payload for level 3 and 4 checks."""

    def __init__(self, model):
        self.m = model
        self.docs = {d.get("id"): d for d in model.get("documents", []) if isinstance(d, dict)}

    def features(self, doc):
        return {f.get("id"): (i, f) for i, f in enumerate(doc.get("features", [])) if isinstance(f, dict)}

    def body_fingerprints(self, doc_id, configuration=None):
        """Body fingerprints of a part in a configuration (by name or id), in the part's frame."""
        doc = self.docs.get(doc_id) or {}
        if configuration:
            for cfg in doc.get("configurations", []):
                if configuration in (cfg.get("name"), cfg.get("id")) and isinstance(cfg.get("fingerprint"), dict):
                    return cfg["fingerprint"].get("bodies", [])
        return [b["fingerprint"] for b in doc.get("bodies", []) if isinstance(b, dict) and "fingerprint" in b]

    def world_bodies(self, doc_id=None, t=IDENTITY, configuration=None, depth=0):
        """Every body of the root (or doc_id) placed in the file's frame: [(fingerprint, component path)]."""
        doc_id = doc_id or self.m.get("root")
        doc = self.docs.get(doc_id) or {}
        if depth > 20:
            return []
        if doc.get("kind") == "part":
            return [(transform_fingerprint(fp, t), ()) for fp in self.body_fingerprints(doc_id, configuration)]
        out = []
        for c in doc.get("components", []):
            if not isinstance(c, dict) or c.get("suppressed") is True or not is_vec(c.get("transform"), 16):
                continue
            for fp, path in self.world_bodies(c.get("document"), mat_mul(t, c["transform"]), c.get("configuration"),
                                               depth + 1):
                out.append((fp, (c.get("id"),) + path))
        return out


def walk_refs(o, path, out):
    """Every reference object {kind, by, ...} in a JSON tree, with its JSON path."""
    if isinstance(o, dict):
        if "kind" in o and "by" in o:
            out.append((path, o))
        for k, v in o.items():
            if k != "result":
                walk_refs(v, f"{path}/{k}", out)
    elif isinstance(o, list):
        for i, v in enumerate(o):
            walk_refs(v, f"{path}/{i}", out)


def check_descriptor(rep, ref, where):
    L = 3
    g = ref.get("geom")
    kind = ref.get("kind")
    if not isinstance(g, dict):
        rep.error(L, "M-DESC", "5", f"{kind} reference by geometry has no 'geom'", where)
        return
    rep.count("geometric descriptors checked")
    if kind == "body":
        if not (isinstance(g.get("volume"), (int, float)) and is_vec(g.get("centroid"))):
            rep.error(L, "M-DESC", "5", "body reference needs volume and centroid", where)
        return
    if g.get("seam") or g.get("degenerate"):
        rep.error(L, "M-DESC-SEAM", "5", f"a reference to a {'seam' if g.get('seam') else 'degenerate'} {kind}: "
                  "exporters MUST NOT emit these (never referenceable)", where)
    samples = g.get("samples")
    if not isinstance(samples, list) or not samples or not all(is_vec(s) for s in samples):
        rep.error(L, "M-DESC", "5", f"{kind} descriptor needs sample points [[x,y,z],...]", where)
        return
    if not g.get("type"):
        rep.error(L, "M-DESC", "5", f"{kind} descriptor needs a geometry type", where)
    bb = g.get("bbox")
    if not is_vec(bb, 6) or any(bb[i] > bb[i + 3] + 1e-9 for i in range(3)):
        rep.error(L, "M-DESC", "5", f"{kind} descriptor needs a bbox [xmin,ymin,zmin,xmax,ymax,zmax]", where)
    else:
        out = [s for s in samples if any(s[i] < bb[i] - 1e-3 or s[i] > bb[i + 3] + 1e-3 for i in range(3))]
        if out:
            rep.error(L, "M-DESC", "5", f"{len(out)} of {len(samples)} samples lie outside the descriptor's own bbox "
                      "(samples and box from different frames?)", where)
    p = g.get("params") or {}
    for k, v in p.items():
        if k in DIRECTION_KEYS and is_vec(v) and abs(vnorm(v) - 1) > 1e-6:
            rep.error(L, "M-DESC", "5", f"params.{k} is not a unit vector (length {vnorm(v):.6g})", where)
    if kind == "face" and g.get("type") == "plane" and is_vec(p.get("normal")) and is_vec(p.get("point")):
        off = [abs(vdot(vsub(s, p["point"]), p["normal"])) for s in samples]
        if max(off) > 1e-3:
            rep.error(L, "M-DESC", "5", f"plane samples lie up to {max(off):.4g} mm off the plane they describe", where)
    if kind == "face" and g.get("type") == "cylinder" and is_vec(p.get("axis")) and is_vec(p.get("origin")) \
            and isinstance(p.get("radius"), (int, float)):
        dev = [abs(vnorm(vcross(p["axis"], vsub(s, p["origin"]))) - p["radius"]) for s in samples]
        if max(dev) > 1e-3:
            rep.error(L, "M-DESC", "5", f"cylinder samples lie up to {max(dev):.4g} mm off the cylinder they describe",
                      where)


def check_fingerprint_sane(rep, fp, where, sheet=False):
    L = 3
    if not isinstance(fp, dict):
        rep.error(L, "M-FP", "6", "fingerprint is not an object", where)
        return
    for k in ("volume", "area"):
        if not isinstance(fp.get(k), (int, float)):
            rep.error(L, "M-FP", "6", f"fingerprint needs '{k}'", where)
            return
    if not is_vec(fp.get("centroid")) or not is_vec(fp.get("moments")):
        rep.error(L, "M-FP", "6", "fingerprint needs centroid[3] and moments[3]", where)
        return
    if fp["area"] <= 0 or (fp["volume"] <= 0 and not sheet):
        rep.error(L, "M-FP", "6", f"fingerprint of an empty body (volume {fp['volume']}, area {fp['area']})", where)
    if fp["moments"] != sorted(fp["moments"]):
        rep.error(L, "M-FP", "6", "principal moments must be sorted ascending", where)
    b = fp.get("bbox")
    if is_vec(b, 6):
        c = fp["centroid"]
        if any(c[i] < b[i] - 0.01 or c[i] > b[i + 3] + 0.01 for i in range(3)):
            rep.error(L, "M-FP", "6", "centroid outside the body's bbox (units or frame mixed up?)", where)


EQ_TOKEN = re.compile(r'\s*(?:(\d+\.?\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?)|("[^"]*")|([A-Za-z_][A-Za-z0-9_@.]*)'
                      r'|(\*\*|[-+*/^(),<>=!]=?|&&|\|\|))')
EQ_FUNCS = {"sin", "cos", "tan", "sqrt", "abs", "min", "max", "if"}


def parse_expression(s):
    """True when s is in the neutral equation grammar (FORMAT-SPEC 4 'Equations'): a recursive-descent check."""
    toks, i = [], 0
    s = s.strip()
    while i < len(s):
        m = EQ_TOKEN.match(s, i)
        if not m or m.end() == i:
            return False
        toks.append(m.group(0).strip())
        i = m.end()
    pos = [0]

    def peek():
        return toks[pos[0]] if pos[0] < len(toks) else None

    def eat(t=None):
        tok = peek()
        if tok is None or (t is not None and tok != t):
            raise ValueError
        pos[0] += 1
        return tok

    def primary():
        tok = peek()
        if tok in ("-", "+"):
            eat()
            return primary()
        if tok == "(":
            eat("(")
            expr()
            eat(")")
            return
        tok = eat()
        if re.fullmatch(r"[A-Za-z_]\w*", tok) and tok.lower() in EQ_FUNCS and peek() == "(":
            eat("(")
            expr()
            while peek() == ",":
                eat(",")
                expr()
            eat(")")
            return
        if not (tok[0].isdigit() or tok[0] == "." or tok[0] == '"' or re.fullmatch(r"[A-Za-z_][\w@.]*", tok)):
            raise ValueError

    def power():
        primary()
        while peek() in ("^", "**"):
            eat()
            primary()

    def term():
        power()
        while peek() in ("*", "/"):
            eat()
            power()

    def arith():
        term()
        while peek() in ("+", "-"):
            eat()
            term()

    def expr():
        arith()
        while peek() in ("<", ">", "<=", ">=", "==", "!=", "=", "&&", "||"):
            eat()
            arith()

    try:
        expr()
        return pos[0] == len(toks)
    except ValueError:
        return False


def level3(rep, model, core):
    L = 3
    rep.levels[L] = {"name": "consistency", "ran": True}
    if core is not None:
        st, report = core.validate(model)
        for e in (report or {}).get("errors", []):
            rep.error(L, "M-STRUCTURE", "4", e.get("message", str(e)) if isinstance(e, dict) else str(e),
                      e.get("path", "") if isinstance(e, dict) else "")
        for w in (report or {}).get("warnings", []):
            rep.warn(L, "M-STRUCTURE", "4", w.get("message", str(w)) if isinstance(w, dict) else str(w),
                     w.get("path", "") if isinstance(w, dict) else "")
    else:
        rep.levels[L]["partial"] = "core not built: the structural rules (ids, document links, cycles) were not checked"

    M = Model(model)
    if model.get("format") != "ncx":
        rep.error(L, "M-TOP", "4", "format must be 'ncx'", "/format")
    if str(model.get("schema", "")).split(".")[0] != "0":
        rep.error(L, "M-TOP", "4", f"schema {model.get('schema')}: this validator knows schema 0.x", "/schema")
    units = model.get("units") or {}
    if units.get("length") != "mm" or units.get("angle") != "deg":
        rep.error(L, "M-UNITS", "4", f"units must be mm and deg (found {units})", "/units")

    sketch_names = {}
    for di, doc in enumerate(model.get("documents", [])):
        dpath = f"/documents/{di}"
        did = doc.get("id")
        feats = M.features(doc)
        names = {}
        cfg_ids = {c.get("id") for c in doc.get("configurations", []) if isinstance(c, dict)}
        cfg_names = {c.get("name") for c in doc.get("configurations", []) if isinstance(c, dict)}
        if doc.get("kind") == "part" and doc.get("configurations"):
            if doc.get("activeConfiguration") not in cfg_ids | cfg_names:
                rep.error(L, "M-CONFIG", "4", f"activeConfiguration {doc.get('activeConfiguration')} is not one of the "
                          "document's configurations", dpath + "/activeConfiguration")
        dims_by_full = {}
        for fi, ft in enumerate(doc.get("features", [])):
            fpath = f"{dpath}/features/{fi}"
            name = ft.get("name")
            if name in names:
                rep.warn(L, "M-NAME", "6.2", f"feature name '{name}' is used twice in {did}: importers verify the "
                         "structure by name", fpath + "/name")
            names[name] = fi
            if ft.get("status") == "approximate" and not ft.get("note"):
                rep.warn(L, "M-STATUS", "4", f"approximate feature '{name}' has no note saying why", fpath)
            if ft.get("status") == "opaque" and "result" not in ft and doc.get("kind") == "part" \
                    and ft.get("type") not in ("sketch", "sketch3d", "plane", "axis", "point", "coordinateSystem"):
                rep.warn(L, "M-RESULT-MISSING", "4", f"opaque feature '{name}' carries no result: an importer must fall "
                         "back to plain geometry for the whole part if it changes geometry", fpath)
            for cfg in (ft.get("suppressed") or {}):
                if cfg not in cfg_ids | cfg_names:
                    rep.error(L, "M-CONFIG", "4", f"suppression state for unknown configuration {cfg}",
                              fpath + "/suppressed")
            if ft.get("type") in ("sketch", "sketch3d") and isinstance(ft.get("def"), dict):
                check_sketch(rep, ft, fpath, dims_by_full)
                sketch_names[(did, name)] = ft
            if isinstance(ft.get("result"), dict):
                check_result_shape(rep, ft, fpath)
        # references by id resolve, and point backwards in the history
        refs = []
        for fi, ft in enumerate(doc.get("features", [])):
            walk_refs(ft.get("def"), f"{dpath}/features/{fi}/def", refs)
            for ref_path, ref in refs:
                check_id_ref(rep, M, did, ref, ref_path, fi)
            refs.clear()
        for key in ("mates", "appearances", "cutLists"):
            walk_refs(doc.get(key), f"{dpath}/{key}", refs)
        for ref_path, ref in refs:
            check_id_ref(rep, M, did, ref, ref_path, None)
        refs.clear()
        walk_refs(doc, dpath, refs)
        for ref_path, ref in refs:
            if ref.get("by") == "geom":
                check_descriptor(rep, ref, ref_path)
        # bodies, configurations, cut lists
        for bi, b in enumerate(doc.get("bodies", []) or []):
            check_fingerprint_sane(rep, b.get("fingerprint"), f"{dpath}/bodies/{bi}", b.get("sheet", False))
        if doc.get("kind") == "part" and core is not None:
            active = doc.get("activeConfiguration")
            for cfg in doc.get("configurations", []):
                if cfg.get("id") == active or cfg.get("name") == active:
                    if isinstance(cfg.get("fingerprint"), dict) and doc.get("bodies"):
                        r = core.fingerprint_compare(cfg["fingerprint"], {"bodies": M.body_fingerprints(did)})
                        if r.get("result") == "mismatch":
                            rep.error(L, "M-CONFIG-FP", "6", f"the active configuration's fingerprint differs from the "
                                      f"document's bodies: {summarise_compare(r)}", dpath + "/configurations")
        own = M.body_fingerprints(did)
        for ci, cl in enumerate(doc.get("cutLists", []) or []):
            for bi, b in enumerate(cl.get("bodies", [])):
                g = (b or {}).get("geom") or {}
                if is_vec(g.get("centroid")) and isinstance(g.get("volume"), (int, float)):
                    hits = [fp for fp in own if abs(fp["volume"] - g["volume"]) <= 1e-5 * max(abs(g["volume"]), 1e-9)
                            and vnorm(vsub(fp["centroid"], g["centroid"])) <= 0.01]
                    if len(hits) != 1:
                        rep.error(L, "M-REF-BODY", "5", f"cut-list body reference matches {len(hits)} of the document's "
                                  "bodies (exactly one required)", f"{dpath}/cutLists/{ci}/bodies/{bi}")
        # assemblies
        for ci, c in enumerate(doc.get("components", []) or []):
            cpath = f"{dpath}/components/{ci}"
            t = c.get("transform")
            if is_vec(t, 16):
                R, _ = mat_from_row_major(t)
                if any(abs(x) > UNIT_TOL for x in t[12:15]) or abs(t[15] - 1) > UNIT_TOL:
                    rep.error(L, "M-TRANSFORM", "4", "component transform's last row must be 0 0 0 1 (row-major 4x4)",
                              cpath + "/transform")
                rtr = [[vdot(R[i], R[j]) for j in range(3)] for i in range(3)]
                if any(abs(rtr[i][j] - (1 if i == j else 0)) > UNIT_TOL for i in range(3) for j in range(3)):
                    rep.error(L, "M-TRANSFORM", "4", "component transform is not a rotation plus a translation "
                              "(scaled, sheared, or column-major?)", cpath + "/transform")
                elif vdot(vcross(R[0], R[1]), R[2]) < 0:
                    rep.error(L, "M-TRANSFORM", "4", "component transform is a reflection: mirrored copies must be their "
                              "own (mirrorPart) documents", cpath + "/transform")
            sub = M.docs.get(c.get("document")) or {}
            cfgs = {x.get("name") for x in sub.get("configurations", []) if isinstance(x, dict)}
            if c.get("configuration") and cfgs and c["configuration"] not in cfgs:
                rep.error(L, "M-CONFIG", "4", f"component uses configuration '{c['configuration']}', which "
                          f"{c.get('document')} does not have", cpath + "/configuration")
        # equations, parameters and flex probes
        params = {p.get("name") for p in doc.get("parameters", []) if isinstance(p, dict)}
        for pi, p in enumerate(doc.get("parameters", []) or []):
            if isinstance(p.get("expression"), str) and not parse_expression(p["expression"]):
                rep.warn(L, "M-EQUATION", "4", f"parameter {p.get('name')}: expression '{p['expression']}' is not in the "
                         "neutral grammar", f"{dpath}/parameters/{pi}")
        for ei, eq in enumerate(doc.get("equations", []) or []):
            epath = f"{dpath}/equations/{ei}"
            rep.count("equations checked")
            if eq.get("status", "mapped") == "mapped" and not parse_expression(str(eq.get("expression", ""))):
                rep.error(L, "M-EQUATION", "4", f"mapped equation '{eq.get('expression')}' is not in the neutral grammar "
                          "(keep it in 'native' with status approximate)", epath)
            tgt = eq.get("target", "")
            feature_dim = "@" in tgt and tgt.split("@", 1)[1] in names
            if tgt not in params and not feature_dim and not any(f == tgt or f.startswith(tgt + "@") for f in dims_by_full):
                rep.warn(L, "M-EQUATION", "4", f"equation target '{tgt}' is neither a parameter nor a dimension of "
                         f"{did}", epath + "/target")
        for xi, probe in enumerate(doc.get("flex", []) or []):
            xpath = f"{dpath}/flex/{xi}"
            rep.count("flex probes checked")
            prm, kind = probe.get("parameter", ""), probe.get("kind")
            if kind == "variable" and prm not in params:
                rep.error(L, "M-FLEX", "6.4", f"flex probe changes variable '{prm}', which {did} does not have", xpath)
            if kind in ("length", "angle") and not any(f == prm or f.startswith(prm + "@") for f in dims_by_full):
                rep.error(L, "M-FLEX", "6.4", f"flex probe changes dimension '{prm}', which {did} does not have", xpath)
            if probe.get("configuration") and cfg_ids | cfg_names and probe["configuration"] not in cfg_ids | cfg_names:
                rep.error(L, "M-FLEX", "6.4", f"flex probe's configuration '{probe['configuration']}' does not exist",
                          xpath)
            for k in (probe.get("fingerprints") or {}):
                if k not in M.docs:
                    rep.error(L, "M-FLEX", "6.4", f"flex probe records a fingerprint for unknown document {k}", xpath)

    # the file fingerprint is the documents' bodies placed by the assembly
    fp = model.get("fingerprint")
    if isinstance(fp, dict):
        for bi, b in enumerate(fp.get("bodies", [])):
            check_fingerprint_sane(rep, b, f"/fingerprint/bodies/{bi}", True)
        asm_geometry = [f.get("name") for d in M.docs.values() if d.get("kind") == "assembly"
                        for f in d.get("features", []) if f.get("type") not in ASSEMBLY_NON_GEOMETRY]
        if asm_geometry:
            rep.count("file fingerprint sum skipped (assembly-level geometry)")
        elif core is not None:
            expected = {"bodies": [f for f, _ in M.world_bodies()]}
            r = core.fingerprint_compare(fp, expected, {"profile": "exchange"})
            if r.get("result") == "mismatch":
                rep.error(L, "M-FP-TOTAL", "6", "the file fingerprint is not the root document's bodies placed by its "
                          f"components' transforms: {summarise_compare(r)}", "/fingerprint")
    return M


def summarise_compare(r):
    """The first few differences of a core fingerprint comparison, readably."""
    diffs = [d for d in r.get("differences", []) if d.get("severity") == "mismatch"] or r.get("differences", [])
    if not diffs:
        return json.dumps({k: v for k, v in r.items() if k not in ("result", "status")})[:400]
    parts = [f"body {d.get('body')} {d.get('quantity')} expected {d.get('expected')} got {d.get('actual')}"
             for d in diffs[:4]]
    return "; ".join(parts) + (f" (+{len(diffs) - 4} more)" if len(diffs) > 4 else "")


def check_sketch(rep, ft, fpath, dims_by_full):
    L = 3
    d = ft["def"]
    sk = Sketch(ft)
    rep.count("sketches checked")
    if not sk.is3d:
        fr = d.get("frame")
        if not (isinstance(fr, dict) and is_vec(fr.get("origin")) and is_vec(fr.get("x")) and is_vec(fr.get("z"))):
            rep.error(L, "M-FRAME", "4", f"sketch '{ft.get('name')}' needs a frame {{origin, x, z}}", fpath + "/def/frame")
        else:
            x, z = fr["x"], fr["z"]
            if abs(vnorm(x) - 1) > UNIT_TOL or abs(vnorm(z) - 1) > UNIT_TOL or abs(vdot(x, z)) > UNIT_TOL:
                rep.error(L, "M-FRAME", "4", f"sketch '{ft.get('name')}' frame axes are not perpendicular unit vectors",
                          fpath + "/def/frame")
    n = sk.n
    for ei, e in enumerate(d.get("entities", [])):
        epath = f"{fpath}/def/entities/{ei}"
        for k in ("p", "start", "end", "center"):
            if k in e and not is_vec(e[k], n):
                rep.error(L, "M-SKETCH", "4", f"entity {e.get('id')}.{k} must have {n} coordinates "
                          f"({'3D' if sk.is3d else '2D'} sketch)", epath)
        if e.get("type") == "line" and is_vec(e.get("start"), n) and is_vec(e.get("end"), n):
            if vnorm(vsub(e["end"], e["start"])) < 1e-9:
                rep.error(L, "M-SKETCH", "4", f"line {e.get('id')} has zero length", epath)
            for end, pid in (("start", e.get("startPoint")), ("end", e.get("endPoint"))):
                p = sk.point(pid) if pid else None
                if p is not None and vnorm(vsub(p, e[end])) > LEN_TOL:
                    rep.error(L, "M-SKETCH", "4", f"line {e.get('id')}'s {end} is not at its {end} point {pid}", epath)
        nb = e.get("nurbs") if e.get("type") == "spline" else None
        if isinstance(nb, dict):
            cps, kn, deg = nb.get("controlPoints") or [], nb.get("knots") or [], nb.get("degree")
            if not isinstance(deg, int) or len(kn) != len(cps) + deg + 1:
                rep.error(L, "M-SPLINE", "4", f"spline {e.get('id')}: {len(kn)} knots for {len(cps)} control points of "
                          f"degree {deg} (a full knot vector has control points + degree + 1)", epath)
            elif not nb.get("periodic") and e.get("points"):
                ends = [(cps[0], e["points"][0]), (cps[-1], e["points"][-1])]
                if any(vnorm(vsub(c[:len(q)], q)) > LEN_TOL for c, q in ends):
                    rep.error(L, "M-SPLINE", "4", f"spline {e.get('id')}: the exact curve does not start and end at its "
                              "fit points (control points in another frame or unit?)", epath)
            rep.count("spline curves checked")
        if e.get("type") in ("circle", "arc"):
            if not isinstance(e.get("radius"), (int, float)) or e["radius"] <= 0:
                rep.error(L, "M-SKETCH", "4", f"{e.get('type')} {e.get('id')} needs a positive radius", epath)
            elif e.get("type") == "arc" and is_vec(e.get("center"), n):
                for end in ("start", "end"):
                    if is_vec(e.get(end), n) and not close_len(vnorm(vsub(e[end], e["center"])), e["radius"]):
                        rep.error(L, "M-SKETCH", "4", f"arc {e.get('id')}'s {end} is not on its circle", epath)
    for ci, c in enumerate(d.get("constraints", [])):
        if c.get("supported") is False:
            continue
        ok = check_constraint(sk, c)
        if ok is None:
            rep.count("relations not checkable")
            continue
        rep.count("relations checked")
        if not ok:
            rep.error(L, "M-SEED-RELATION", "4", f"sketch '{ft.get('name')}': the seeded coordinates break its "
                      f"{c.get('type')} relation on {c.get('entities')} (seeds and relations from different states, "
                      "or a frame/unit mistake)", f"{fpath}/def/constraints/{ci}")
    for di, dm in enumerate(d.get("dimensions", [])):
        if dm.get("fullName"):
            dims_by_full[dm["fullName"]] = dm
        val = dm.get("value")
        measured, why = check_dimension(sk, dm)
        if measured is None or not isinstance(val, (int, float)):
            rep.count("dimensions not checkable")
            continue
        rep.count("dimensions checked")
        good = abs(measured - val) <= ANG_TOL if dm.get("kind") == "angle" else close_len(measured, val)
        if dm.get("kind") == "angle" and not good:
            good = abs((180 - measured) - val) <= ANG_TOL
        if not good:
            hint = ""
            if val and measured and any(abs(measured / val - k) < 1e-6 for k in (10, 0.1, 25.4, 1 / 25.4, 1000, 0.001)):
                hint = f" (out by a factor of {measured / val:.6g}: a unit mistake?)"
            rep.error(L, "M-SEED-DIMENSION", "4", f"sketch '{ft.get('name')}': dimension {dm.get('name')} is {val} but "
                      f"the seeded geometry measures {measured:.6g}{hint}", f"{fpath}/def/dimensions/{di}")


def check_result_shape(rep, ft, fpath):
    L = 3
    r = ft["result"]
    if r.get("format") != "step":
        rep.error(L, "M-RESULT", "4", f"feature result format must be 'step' (found {r.get('format')})", fpath + "/result")
    for i, fp in enumerate(r.get("replaces", []) or []):
        check_fingerprint_sane(rep, fp, f"{fpath}/result/replaces/{i}", True)
    for i, b in enumerate(r.get("bodies", []) or []):
        check_fingerprint_sane(rep, (b or {}).get("fingerprint"), f"{fpath}/result/bodies/{i}", (b or {}).get("sheet"))
    if r.get("bodies") and not str(r.get("data", "")).lstrip().startswith("ISO-10303-21;"):
        rep.error(L, "M-RESULT", "4", "feature result data must be the text of an ISO 10303-21 file", fpath + "/result/data")
    if not r.get("bodies") and r.get("data"):
        rep.warn(L, "M-RESULT", "4", "feature result has data but lists no bodies", fpath + "/result")
    if not r.get("bodies") and not r.get("replaces"):
        rep.warn(L, "M-RESULT", "4", "feature result neither replaces nor adds a body", fpath + "/result")


def check_id_ref(rep, M, did, ref, where, feature_index):
    """References by id (FORMAT-SPEC 5, rule 1) resolve; feature references point back in the history."""
    L = 3
    kind = ref.get("kind")
    rep.count("references by id checked") if ref.get("by") == "id" else None
    if kind == "feature" and ref.get("by") == "id":
        tdoc = M.docs.get(ref.get("document", did))
        if tdoc is None:
            rep.error(L, "M-REF-ID", "5", f"reference to unknown document {ref.get('document')}", where)
            return
        feats = M.features(tdoc)
        if ref.get("id") not in feats:
            rep.error(L, "M-REF-ID", "5", f"reference to feature {ref.get('id')}, which {tdoc.get('id')} does not have",
                      where)
        elif feature_index is not None and ref.get("document", did) == did and feats[ref["id"]][0] >= feature_index:
            rep.error(L, "M-REF-ORDER", "5", f"feature refers to {ref.get('id')}, which comes later in the history",
                      where)
    elif kind == "sketchEntity" and ref.get("by") == "id":
        tdoc = M.docs.get(ref.get("document", did))
        f = M.features(tdoc or {}).get(ref.get("feature"))
        if f is None:
            rep.error(L, "M-REF-ID", "5", f"sketch entity reference to unknown sketch {ref.get('feature')}", where)
        elif not any(e.get("id") == ref.get("id") for e in (f[1].get("def") or {}).get("entities", [])):
            rep.error(L, "M-REF-ID", "5", f"sketch {f[1].get('name')} has no entity {ref.get('id')}", where)
    elif kind == "transferred":
        tdoc = M.docs.get(ref.get("document"))
        if tdoc is None:
            rep.error(L, "M-REF-ID", "5", f"transferred item from unknown document {ref.get('document')}", where)
        elif ref.get("feature") and ref["feature"] not in M.features(tdoc):
            rep.error(L, "M-REF-ID", "5", f"transferred item names feature {ref['feature']}, which {tdoc.get('id')} "
                      "does not have", where)
    elif kind == "originPlane":
        if ref.get("plane") not in ("front", "top", "right"):
            rep.error(L, "M-REF-ID", "5", f"origin plane '{ref.get('plane')}' (front, top or right)", where)
    elif kind == "generated":
        tdoc = M.docs.get(did) or {}
        if not any(f.get("name") == ref.get("by") for f in tdoc.get("features", [])):
            rep.error(L, "M-REF-ID", "4", f"plane generated by '{ref.get('by')}', a feature {did} does not have", where)


# ------------------------------------------------------------------------------------------------ level 4
INTEGRATION_EPS = 1e-7    # adaptive integration: OpenCASCADE's default Gauss rule is ~1e-3 out on spline faces


def measure(shape):
    """Per-solid fingerprints (FORMAT-SPEC 6) with adaptive integration, each marked freeform when it has a face that
    is not a plane, cylinder, cone, sphere or torus; and whether any solid is."""
    from OCP.BRepAdaptor import BRepAdaptor_Surface
    from OCP.BRepBndLib import BRepBndLib
    from OCP.BRepGProp import BRepGProp
    from OCP.Bnd import Bnd_Box
    from OCP.GeomAbs import GeomAbs_Plane, GeomAbs_Cylinder, GeomAbs_Cone, GeomAbs_Sphere, GeomAbs_Torus
    from OCP.GProp import GProp_GProps
    from OCP.TopAbs import TopAbs_SOLID, TopAbs_FACE, TopAbs_EDGE
    from OCP.TopExp import TopExp_Explorer
    from OCP.TopoDS import TopoDS
    analytic = {GeomAbs_Plane, GeomAbs_Cylinder, GeomAbs_Cone, GeomAbs_Sphere, GeomAbs_Torus}
    out, freeform = [], False
    ex = TopExp_Explorer(shape, TopAbs_SOLID)
    while ex.More():
        s = ex.Current()
        g, ga = GProp_GProps(), GProp_GProps()
        BRepGProp.VolumeProperties_s(s, g, INTEGRATION_EPS)
        BRepGProp.SurfaceProperties_s(s, ga, INTEGRATION_EPS)
        ix, iy, iz = g.PrincipalProperties().Moments()
        bb = Bnd_Box()
        BRepBndLib.AddOptimal_s(s, bb, False, False)
        c = g.CentreOfMass()
        nf = ne = 0
        body_ff = False
        fx = TopExp_Explorer(s, TopAbs_FACE)
        while fx.More():
            nf += 1
            if BRepAdaptor_Surface(TopoDS.Face_s(fx.Current())).GetType() not in analytic:
                body_ff = True
            fx.Next()
        freeform = freeform or body_ff
        ee = TopExp_Explorer(s, TopAbs_EDGE)
        while ee.More():
            ne += 1
            ee.Next()
        out.append({"volume": g.Mass(), "area": ga.Mass(), "centroid": [c.X(), c.Y(), c.Z()],
                    "moments": sorted([ix, iy, iz]), "bbox": list(bb.Get()), "faces": nf, "edges": ne})
        if body_ff:
            out[-1]["freeform"] = True
        ex.Next()
    return out, freeform


def compare_exchange(core, expected, actual):
    """'match' or 'mismatch' under the exchange profile (freeform bodies get the freeform tolerances), with the
    core's report."""
    r = core.fingerprint_compare(expected, actual, {"profile": "exchange"})
    return ("match" if r.get("result") in ("match", "count_only") else "mismatch"), r


def level4(rep, path, text, fields, recs, M, core, occ):
    L = 4
    from OCP.BRepBuilderAPI import BRepBuilderAPI_Transform
    from OCP.BRep import BRep_Builder
    from OCP.IFSelect import IFSelect_RetDone
    from OCP.STEPControl import STEPControl_Reader
    from OCP.TopAbs import TopAbs_SOLID, TopAbs_SHELL, TopAbs_FACE
    from OCP.TopExp import TopExp_Explorer
    from OCP.TopoDS import TopoDS_Compound
    from OCP.gp import gp_Trsf
    rep.levels[L] = {"name": "geometry", "ran": True}

    reader = STEPControl_Reader()
    if reader.ReadFile(path) != IFSelect_RetDone:
        rep.error(L, "G-STEP", "1", "OpenCASCADE cannot read the STEP file")
        return
    reader.TransferRoots()
    whole = reader.OneShape()
    solids = []
    ex = TopExp_Explorer(whole, TopAbs_SOLID)
    while ex.More():
        solids.append(ex.Current())
        ex.Next()
    measured = [measure(s)[0][0] for s in solids]

    # the notice body (FORMAT-SPEC 2.6): found by name in the file, measured on its own, removed from the model
    notice = fields.get("notice")
    if notice and fields.get("noticebox"):
        # the box lets a reader whose STEP importer drops body names find the notice (FORMAT-SPEC 2.6)
        try:
            nb = [float(x) for x in fields["noticebox"].split(",")]
            assert len(nb) == 6
        except (ValueError, AssertionError):
            rep.error(L, "G-NOTICE-BOX", "2.2", f"noticebox={fields['noticebox']}: must be six numbers (mm)")
            nb = None
        if nb and not any(max(abs(a - b) for a, b in zip(m["bbox"], nb)) <= 0.01 for m in measured):
            rep.error(L, "G-NOTICE-BOX", "2.6", "no solid in the file has the header's noticebox (within 0.01 mm): a "
                      "reader that loses body names cannot find the notice")
    elif notice:
        rep.warn(L, "G-NOTICE-BOX", "2.6", "no noticebox in the header: a reader whose STEP importer drops body names "
                 "must find the notice some other way")
    if notice:
        named = [i for i, t, b in recs if t == "MANIFOLD_SOLID_BREP" and names_match(specread._args(b)[0], notice)]
        if not named:
            rep.error(L, "G-NOTICE", "2.6", "the header names a notice body, but no solid in the file has that name")
        model = reader.StepModel()
        for ent in named:
            r1 = STEPControl_Reader()
            r1.ReadFile(path)
            num = r1.StepModel().NextNumberForLabel(f"#{ent}", 0, True)
            if num <= 0 or not r1.TransferOne(num) or r1.NbShapes() == 0:
                rep.error(L, "G-NOTICE", "2.6", f"the notice body #{ent} does not transfer as a solid")
                continue
            nfp, _ = measure(r1.Shape(r1.NbShapes()))
            if len(nfp) != 1:
                rep.error(L, "G-NOTICE", "2.6", f"the notice body must be one closed solid (found {len(nfp)})")
                continue
            k = next((i for i, fp in enumerate(measured) if abs(fp["volume"] - nfp[0]["volume"]) <= 1e-6 * nfp[0]["volume"]
                      and vnorm(vsub(fp["centroid"], nfp[0]["centroid"])) <= 1e-3), None)
            if k is None:
                rep.error(L, "G-NOTICE", "2.6", "the notice body is not where the file's assembly places it (it must be "
                          "at the origin of the root, in the file's frame)")
                continue
            del measured[k]
            del solids[k]
            rep.count("notice bodies found")
        del model

    # stale check (FORMAT-SPEC 6): the file's B-rep against the payload's fingerprint, exchange tolerance
    expected = M.m.get("fingerprint") or {}
    exp_solids = {"bodies": [b for b in expected.get("bodies", []) if b.get("volume", 0) > 0]}
    verdict, r = compare_exchange(core, exp_solids, {"bodies": measured})
    rep.levels[L]["stale_check"] = verdict
    if verdict == "mismatch":
        rep.error(L, "G-STALE", "6", f"the payload's fingerprint does not describe the file's own geometry "
                  f"({len(exp_solids['bodies'])} solids expected, {len(measured)} in the file): every importer will "
                  f"import plain geometry. {summarise_compare(r)}")

    # feature results (FORMAT-SPEC 4 'Feature results'): the carried STEP is readable and is what it says
    with tempfile.TemporaryDirectory() as tmp:
        for doc in M.m.get("documents", []):
            for fi, ft in enumerate(doc.get("features", [])):
                res = ft.get("result")
                if not isinstance(res, dict) or not res.get("data"):
                    continue
                where = f"/documents/{M.m['documents'].index(doc)}/features/{fi}/result"
                rep.count("feature results checked")
                fn = os.path.join(tmp, "result.step")
                with open(fn, "w", encoding="latin-1", newline="") as f:
                    f.write(res["data"])
                rr = STEPControl_Reader()
                if rr.ReadFile(fn) != IFSelect_RetDone:
                    rep.error(L, "G-RESULT", "4", f"feature '{ft.get('name')}': the result's STEP data does not read",
                              where)
                    continue
                rr.TransferRoots()
                got, _ = measure(rr.OneShape())
                want = [b["fingerprint"] for b in res.get("bodies", []) if not b.get("sheet")]
                if not want and not got:
                    continue
                v, c = compare_exchange(core, {"bodies": want}, {"bodies": got})
                if v == "mismatch":
                    rep.error(L, "G-RESULT", "4", f"feature '{ft.get('name')}': the result's bodies are not the ones its "
                              f"fingerprints describe: {summarise_compare(c)}", where)

    # references measured in the finished model: mates (component frame) and face appearances (part frame)
    world = M.world_bodies()

    def component_shape(path_ids, root_doc=None):
        """The solids of one component instance, moved back into its part's own frame."""
        doc = M.docs.get(root_doc or M.m.get("root"))
        t = IDENTITY
        for cid in path_ids:
            comp = next((c for c in (doc or {}).get("components", []) if c.get("id") == cid), None)
            if comp is None or not is_vec(comp.get("transform"), 16):
                return None
            t = mat_mul(t, comp["transform"])
            doc = M.docs.get(comp.get("document"))
        want = [fp for fp, p in world if p[:len(path_ids)] == tuple(path_ids)]
        picked = []
        for fp in want:
            k = next((i for i, m in enumerate(measured) if abs(m["volume"] - fp["volume"]) <= 2e-4 * fp["volume"]
                      and vnorm(vsub(m["centroid"], fp["centroid"])) <= 0.01), None)
            if k is None:
                return None
            picked.append(solids[k])
        if not picked:
            return None
        comp_shape = TopoDS_Compound()
        bld = BRep_Builder()
        bld.MakeCompound(comp_shape)
        for s in picked:
            bld.Add(comp_shape, s)
        R, tr = mat_from_row_major(t)
        inv = gp_Trsf()   # inverse of a rigid transform: R^T, -R^T t
        rt = [[R[j][i] for j in range(3)] for i in range(3)]
        ti = [-sum(rt[i][k] * tr[k] for k in range(3)) for i in range(3)]
        inv.SetValues(rt[0][0], rt[0][1], rt[0][2], ti[0], rt[1][0], rt[1][1], rt[1][2], ti[1],
                      rt[2][0], rt[2][1], rt[2][2], ti[2])
        return BRepBuilderAPI_Transform(comp_shape, inv, True).Shape()

    caches, shapes = {}, {}

    def resolve(shape_key, shape, ref, where, what):
        if shape is None:
            rep.warn(L, "G-REF-SKIPPED", "5", f"{what}: the component's bodies could not be found in the file's B-rep, "
                     "so its reference was not checked", where)
            return
        if shape_key not in caches:
            caches[shape_key] = occ.TargetCache(shape)
        tc = caches[shape_key]
        rep.count("geometric references resolved")
        if ref["kind"] == "vertex":
            p = ((ref.get("geom") or {}).get("params") or {}).get("point")
            from OCP.TopExp import TopExp_Explorer as Ex
            from OCP.TopAbs import TopAbs_VERTEX
            from OCP.BRep import BRep_Tool
            from OCP.TopoDS import TopoDS
            hits, seen = 0, []
            e2 = Ex(shape, TopAbs_VERTEX)
            while e2.More():
                q = BRep_Tool.Pnt_s(TopoDS.Vertex_s(e2.Current()))
                q = [q.X(), q.Y(), q.Z()]
                if vnorm(vsub(q, p)) <= 0.01 and not any(vnorm(vsub(q, s)) < 1e-9 for s in seen):
                    hits += 1
                    seen.append(q)
                e2.Next()
            if hits != 1:
                rep.error(L, "G-REF", "5", f"{what}: vertex reference matches {hits} vertices of the model", where)
            return
        if ref["kind"] not in ("face", "edge"):
            return
        res = core.match({"ref": ref, "candidates": occ.candidates(tc, ref, prefilter=1.0)})
        if res.get("result") != "resolved":
            sev = rep.error if res.get("result") == "unresolved" else rep.warn
            sev(L, "G-REF", "5", f"{what}: the {ref['kind']} reference is {res.get('result')} in the file's own "
                f"geometry, so importers cannot use it ({'; '.join(res.get('notes', [])[:2])})", where)

    for di, doc in enumerate(M.m.get("documents", [])):
        if doc.get("kind") == "assembly":
            # only the root's own components can be located; nested assemblies are checked through the root
            if doc.get("id") != M.m.get("root"):
                continue
            for mi, mate in enumerate(doc.get("mates", []) or []):
                for ri, mr in enumerate(mate.get("refs", []) or []):
                    ref = (mr or {}).get("ref") or {}
                    if ref.get("by") != "geom" or not mr.get("component"):
                        continue
                    path_ids = [mr["component"]] + list(mr.get("path") or [])
                    key = tuple(path_ids)
                    if key not in shapes:
                        shapes[key] = component_shape(path_ids)
                    shape = shapes[key]
                    resolve(key, shape, ref, f"/documents/{di}/mates/{mi}/refs/{ri}",
                            f"mate '{mate.get('name')}'")
        else:
            faces = [(ai, a) for ai, a in enumerate(doc.get("appearances", []) or [])
                     if ((a or {}).get("target") or {}).get("by") == "geom"]
            if not faces:
                continue
            if doc.get("id") == M.m.get("root"):
                key, shape = ("root",), whole_without_notice(solids)
            else:
                inst = next((p for fp, p in world if p and M_doc_of(M, p) == doc.get("id")), None)
                key, shape = (inst, component_shape(list(inst))) if inst else (None, None)
            for ai, a in faces:
                resolve(key, shape, a["target"], f"/documents/{di}/appearances/{ai}", f"appearance on {doc.get('id')}")


def whole_without_notice(solids):
    from OCP.BRep import BRep_Builder
    from OCP.TopoDS import TopoDS_Compound
    c = TopoDS_Compound()
    b = BRep_Builder()
    b.MakeCompound(c)
    for s in solids:
        b.Add(c, s)
    return c


def M_doc_of(M, path_ids):
    """The document a component path ends in."""
    doc = M.docs.get(M.m.get("root"))
    for cid in path_ids:
        comp = next((c for c in (doc or {}).get("components", []) if c.get("id") == cid), None)
        if comp is None:
            return None
        doc = M.docs.get(comp.get("document"))
    return (doc or {}).get("id")


# ------------------------------------------------------------------------------------------------ driver
def validate(path, max_level=4):
    rep = Report(os.path.abspath(path))
    try:
        text = open(path, "rb").read().decode("latin-1")
    except OSError as e:
        rep.error(1, "C-FILE", "1", f"cannot read the file: {e}")
        return rep.as_dict()
    out = level1(rep, text)
    if out is None or rep.errors(1) and not out:
        for lv, name in ((2, "schema"), (3, "consistency"), (4, "geometry")):
            rep.levels[lv] = {"name": name, "ran": False, "skipped": "the payload could not be read"}
        return rep.as_dict()
    fields, raw, recs = out
    try:
        model = json.loads(raw.decode("utf-8"))
    except Exception as e:
        rep.error(1, "C-JSON", "4", f"the payload is not UTF-8 JSON: {e}")
        return rep.as_dict()
    if max_level >= 2:
        level2(rep, model)
    core, why_core = load_core()
    if core is not None and model.get("fingerprint") is not None and fields.get("fp") not in (None, "none"):
        h = core.fingerprint_hash(model["fingerprint"])
        if h and h != fields["fp"]:
            rep.error(1, "C-FP-HASH", "2.2", f"header fp={fields['fp']} but the payload's fingerprint hashes to {h}")
    if fields.get("schema") and str(model.get("schema")) != fields.get("schema"):
        rep.error(1, "C-HEADER-KEY", "2.2", f"header schema={fields.get('schema')} but the payload says {model.get('schema')}")
    M = None
    if max_level >= 3:
        M = level3(rep, model, core)
    if max_level >= 4:
        occ, why_occ = load_occ()
        if core is None or occ is None:
            rep.levels[4] = {"name": "geometry", "ran": False,
                             "skipped": why_core if core is None else f"OpenCASCADE not available ({why_occ}): "
                             "pip install cadquery-ocp"}
        else:
            try:
                level4(rep, path, text, fields, recs, M, core, occ)
            except Exception as e:  # a crash in the geometry layer is a validator fault, not a file fault
                rep.levels[4] = {"name": "geometry", "ran": False, "skipped": f"validator error: {e!r}"}
    return rep.as_dict()


def print_text(r, quiet=False):
    print(f"{r['verdict']}  {r['file']}  ({r['summary']['errors']} errors, {r['summary']['warnings']} warnings)")
    for lv in r["levels"]:
        extra = f" - skipped: {lv['skipped']}" if lv.get("skipped") else (f" - {lv['partial']}" if lv.get("partial") else "")
        print(f"  level {lv['level']} {lv['name']}: {'ran' if lv.get('ran') else 'not run'}{extra}")
    if quiet:
        return
    for f in r["findings"]:
        where = f" at {f['where']}" if f["where"] else ""
        print(f"  [{f['severity']}] {f['code']} (spec {f['spec']}){where}: {f['message']}")
    if r["checked"]:
        print("  checked: " + ", ".join(f"{k} {v}" for k, v in sorted(r["checked"].items())))


def main(argv=None):
    ap = argparse.ArgumentParser(description="Validate NCX files against FORMAT-SPEC.md")
    ap.add_argument("files", nargs="+")
    ap.add_argument("--json", help="write the reports (a list) to this file")
    ap.add_argument("--max-level", type=int, default=4, choices=[1, 2, 3, 4])
    ap.add_argument("--quiet", action="store_true", help="verdicts and level lines only")
    a = ap.parse_args(argv)
    reports = []
    for f in a.files:
        r = validate(f, a.max_level)
        reports.append(r)
        print_text(r, a.quiet)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(reports, fh, indent=1)
    return 0 if all(r["verdict"] == "PASS" for r in reports) else 1


if __name__ == "__main__":
    sys.exit(main())
