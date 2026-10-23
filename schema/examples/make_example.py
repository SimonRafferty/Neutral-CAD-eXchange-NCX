"""Build the schema example in the user's modelling style and write it as a real NCX container.

  doc:layout  Frame-Layout  layout part: parameters W/H with an equation, SK_Outline + SK_HolePos
  doc:plate   Plate         Insert Part of the layout, extrude SK_Outline, cut SK_HolePos, fillet the
                            four vertical edges (geometric descriptors, recorded before the fillet)
  doc:pin     Pin           revolve of a sketch on the Right plane
  doc:asm     Frame         origin-to-origin assembly: Plate fixed, Pin placed in the hole, one mate

Geometry, descriptors and fingerprints come from OpenCASCADE, so the example is consistent.
Outputs: frame.ncx.json (the payload) and frame.step (AP214 B-rep with the payload embedded).

Usage: .venv\\Scripts\\python.exe schema\\examples\\make_example.py
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "python"))

import cadquery as cq  # noqa: E402
from OCP.IFSelect import IFSelect_RetDone  # noqa: E402
from OCP.Interface import Interface_Static  # noqa: E402
from OCP.STEPCAFControl import STEPCAFControl_Writer  # noqa: E402
from OCP.TCollection import TCollection_ExtendedString  # noqa: E402
from OCP.TDataStd import TDataStd_Name  # noqa: E402
from OCP.TDocStd import TDocStd_Document  # noqa: E402
from OCP.TopLoc import TopLoc_Location  # noqa: E402
from OCP.XCAFDoc import XCAFDoc_DocumentTool  # noqa: E402
from OCP.gp import gp_Trsf, gp_Vec  # noqa: E402

from ncx import occhost  # noqa: E402
from ncx.core import core  # noqa: E402

W, H_RATIO, T, R_FILLET, HOLE_X, HOLE_Y, HOLE_D = 200.0, 0.6, 8.0, 10.0, 30.0, 30.0, 12.0
H = W * H_RATIO
IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
FRONT = {"origin": [0, 0, 0], "x": [1, 0, 0], "z": [0, 0, 1]}
RIGHT = {"origin": [0, 0, 0], "x": [0, 1, 0], "z": [1, 0, 0]}


def r6(v):
    return [round(x, 6) for x in v] if isinstance(v, list) else round(v, 6)


def clean_desc(d):
    """Round a descriptor for a readable example (still far inside the matching tolerance)."""
    g = d["geom"]
    g["samples"] = [r6(p) for p in g["samples"]]
    g["params"] = {k: r6(v) for k, v in g["params"].items()}
    for k in ("area", "length", "clearance"):
        if k in g:
            g[k] = r6(g[k])
    g["bbox"] = r6(g["bbox"])
    g.pop("adjacent", None)  # optional; omitted to keep the example short
    return d


# ------------------------------------------------------------------ geometry (what the source has)
plate_before_fillet = (cq.Workplane("XY").rect(W, H, centered=False).extrude(T)
                       .faces(">Z").workplane(origin=(0, 0, T)).center(HOLE_X, HOLE_Y).hole(HOLE_D))
pb = plate_before_fillet.val().wrapped
# the four outer vertical edges (not the hole's seam line, which is also a vertical line)
vertical = [e for e in occhost.topology(pb, "edge")
            if occhost.curve_info(e)[0] == "line" and abs(abs(occhost.curve_info(e)[1]["direction"][2]) - 1) < 1e-9
            and not occhost.is_seam(pb, e)]
assert len(vertical) == 4, len(vertical)
plate = plate_before_fillet.edges("(|Z and >X) or (|Z and <X)").fillet(R_FILLET)
hole_face = next(f for f in occhost.topology(plate.val().wrapped, "face") if occhost.surface_info(f)[0] == "cylinder"
                 and abs(occhost.surface_info(f)[1]["radius"] - HOLE_D / 2) < 1e-9)
pin = (cq.Workplane("YZ").polyline([(0, -T - 4), (9, -T - 4), (9, -T), (5.9, -T), (5.9, T + 2), (0, T + 2)])
       .close().revolve(360, (0, 0, 0), (0, 1, 0)))
pin_cyl = next(f for f in occhost.topology(pin.val().wrapped, "face") if occhost.surface_info(f)[0] == "cylinder"
               and abs(occhost.surface_info(f)[1]["radius"] - 5.9) < 1e-9)

# ------------------------------------------------------------------------------------- the model
layout = {
    "id": "doc:layout", "kind": "part", "name": "Frame-Layout", "sourceFile": "Frame-Layout.SLDPRT",
    "configurations": [{"id": "cfg:default", "name": "Default"}], "activeConfiguration": "cfg:default",
    "parameters": [{"name": "W", "value": W, "unit": "mm"}, {"name": "H", "value": H, "unit": "mm"}],
    "equations": [{"target": "H", "expression": "W * 0.6", "native": "\"H\" = \"W\" * 0.6", "status": "mapped"}],
    "features": [
        {"id": "f:sk_outline", "type": "sketch", "name": "SK_Outline", "status": "mapped",
         "native": {"app": "SOLIDWORKS", "type": "ProfileFeature"},
         "def": {"plane": {"kind": "originPlane", "by": "id", "plane": "front"}, "frame": FRONT,
                 "entities": [
                     {"id": "p1", "type": "point", "p": [0, 0]}, {"id": "p2", "type": "point", "p": [W, 0]},
                     {"id": "p3", "type": "point", "p": [W, H]}, {"id": "p4", "type": "point", "p": [0, H]},
                     {"id": "l1", "type": "line", "start": [0, 0], "end": [W, 0], "startPoint": "p1", "endPoint": "p2"},
                     {"id": "l2", "type": "line", "start": [W, 0], "end": [W, H], "startPoint": "p2", "endPoint": "p3"},
                     {"id": "l3", "type": "line", "start": [W, H], "end": [0, H], "startPoint": "p3", "endPoint": "p4"},
                     {"id": "l4", "type": "line", "start": [0, H], "end": [0, 0], "startPoint": "p4", "endPoint": "p1"}],
                 "constraints": [
                     {"type": "coincident", "entities": ["p1", {"kind": "origin", "by": "id"}]},
                     {"type": "horizontal", "entities": ["l1"]}, {"type": "horizontal", "entities": ["l3"]},
                     {"type": "vertical", "entities": ["l2"]}, {"type": "vertical", "entities": ["l4"]}],
                 "dimensions": [
                     {"id": "d1", "name": "D1@SK_Outline", "kind": "horizontal", "entities": ["l1"], "value": W,
                      "driving": True, "param": "W"},
                     {"id": "d2", "name": "D2@SK_Outline", "kind": "vertical", "entities": ["l2"], "value": H,
                      "driving": True, "param": "H"}]}},
        {"id": "f:sk_hole", "type": "sketch", "name": "SK_HolePos", "status": "mapped",
         "native": {"app": "SOLIDWORKS", "type": "ProfileFeature"},
         "def": {"plane": {"kind": "originPlane", "by": "id", "plane": "front"}, "frame": FRONT,
                 "entities": [{"id": "c1", "type": "circle", "center": [HOLE_X, HOLE_Y], "radius": HOLE_D / 2}],
                 "dimensions": [
                     {"id": "d1", "name": "D1@SK_HolePos", "kind": "horizontal",
                      "entities": ["c1", {"kind": "origin", "by": "id"}], "value": HOLE_X, "driving": True},
                     {"id": "d2", "name": "D2@SK_HolePos", "kind": "vertical",
                      "entities": ["c1", {"kind": "origin", "by": "id"}], "value": HOLE_Y, "driving": True},
                     {"id": "d3", "name": "D3@SK_HolePos", "kind": "diameter", "entities": ["c1"], "value": HOLE_D,
                      "driving": True}]}}],
    "bodies": [],
}

plate_doc = {
    "id": "doc:plate", "kind": "part", "name": "Plate", "sourceFile": "Plate.SLDPRT",
    "features": [
        {"id": "f:ins", "type": "insertPart", "name": "Frame-Layout", "status": "mapped",
         "native": {"app": "SOLIDWORKS", "type": "Stock"},
         "def": {"source": "doc:layout", "sourceConfiguration": "cfg:default", "transfer": ["sketches", "planes"],
                 "transform": IDENTITY,
                 "items": [{"source": "f:sk_outline", "name": "SK_Outline-Frame-Layout", "id": "f:ins.sk_outline"},
                           {"source": "f:sk_hole", "name": "SK_HolePos-Frame-Layout", "id": "f:ins.sk_hole"}]}},
        {"id": "f:boss", "type": "extrude", "name": "Boss-Extrude1", "status": "mapped",
         "native": {"app": "SOLIDWORKS", "type": "Extrusion"},
         "def": {"profile": {"kind": "transferred", "by": "id", "document": "doc:layout", "feature": "f:sk_outline"},
                 "direction": [0, 0, 1], "end": {"type": "blind", "depth": T, "param": "D1@Boss-Extrude1"}}},
        {"id": "f:hole", "type": "cut", "name": "Cut-Extrude1", "status": "mapped",
         "native": {"app": "SOLIDWORKS", "type": "ICE"},
         "def": {"profile": {"kind": "transferred", "by": "id", "document": "doc:layout", "feature": "f:sk_hole"},
                 "direction": [0, 0, 1], "end": {"type": "throughAll"}}},
        {"id": "f:fillet", "type": "fillet", "name": "Fillet1", "status": "mapped",
         "native": {"app": "SOLIDWORKS", "type": "Fillet"},
         "def": {"radius": R_FILLET, "param": "D1@Fillet1",
                 "edges": [clean_desc(occhost.descriptor(pb, e, "edge")) for e in vertical]}}],
    "bodies": [{"id": "body:1", "name": "Fillet1",
                "fingerprint": occhost.fingerprint(plate.val().wrapped)["bodies"][0]}],
}

pin_doc = {
    "id": "doc:pin", "kind": "part", "name": "Pin", "sourceFile": "Pin.SLDPRT",
    "features": [
        {"id": "f:sk", "type": "sketch", "name": "SK_PinProfile", "status": "mapped",
         "def": {"plane": {"kind": "originPlane", "by": "id", "plane": "right"}, "frame": RIGHT,
                 "entities": [{"id": "ax", "type": "line", "construction": True, "start": [0, -20], "end": [0, 20]},
                              {"id": "pl", "type": "spline", "points": [[0, -T - 4], [9, -T - 4], [9, -T], [5.9, -T],
                                                                        [5.9, T + 2], [0, T + 2]],
                               "name": "profile polyline (6 lines in the source; abbreviated)"}]}},
        {"id": "f:rev", "type": "revolve", "name": "Revolve1", "status": "mapped",
         "def": {"profile": {"kind": "feature", "by": "id", "id": "f:sk"},
                 "axis": {"kind": "sketchEntity", "by": "id", "feature": "f:sk", "id": "ax"}, "angle": 360}}],
    "bodies": [{"id": "body:1", "name": "Revolve1", "fingerprint": occhost.fingerprint(pin.val().wrapped)["bodies"][0]}],
}

pin_at = [1, 0, 0, HOLE_X, 0, 1, 0, HOLE_Y, 0, 0, 1, 0, 0, 0, 0, 1]
asm = {
    "id": "doc:asm", "kind": "assembly", "name": "Frame", "sourceFile": "Frame.SLDASM",
    "components": [
        {"id": "c:plate", "document": "doc:plate", "name": "Plate-1", "transform": IDENTITY, "fixed": True},
        {"id": "c:pin", "document": "doc:pin", "name": "Pin-1", "transform": pin_at, "fixed": False}],
    "mates": [{"id": "m:1", "name": "Concentric1", "type": "concentric", "alignment": "aligned", "status": "mapped",
               "refs": [{"component": "c:plate", "ref": clean_desc(occhost.descriptor(plate.val().wrapped, hole_face, "face"))},
                        {"component": "c:pin", "ref": clean_desc(occhost.descriptor(pin.val().wrapped, pin_cyl, "face"))}]}],
}

# file-level fingerprint: what the STEP B-rep must reproduce (the assembly's bodies in place)
tr = gp_Trsf()
tr.SetTranslation(gp_Vec(HOLE_X, HOLE_Y, 0))
pin_placed = pin.val().wrapped.Moved(TopLoc_Location(tr))
fp = {"bodies": occhost.fingerprint(plate.val().wrapped)["bodies"] + occhost.fingerprint(pin_placed)["bodies"]}
model = {"format": "ncx", "schema": "0.1",
         "generator": {"app": "example", "version": "stage2", "exporter": "make_example.py"},
         "units": {"length": "mm", "angle": "deg", "mass": "kg"},
         "root": "doc:asm", "documents": [layout, plate_doc, pin_doc, asm], "fingerprint": fp}
raw = json.dumps(model, indent=1).encode()
open(os.path.join(HERE, "frame.ncx.json"), "wb").write(raw)

# ------------------------------------------------------------------- the container (AP214 B-rep)
doc = TDocStd_Document(TCollection_ExtendedString("XmlOcaf"))
st = XCAFDoc_DocumentTool.ShapeTool_s(doc.Main())
root = st.NewShape()
TDataStd_Name.Set_s(root, TCollection_ExtendedString("Frame"))
for name, shp, trsf in (("Plate", plate.val().wrapped, gp_Trsf()), ("Pin", pin.val().wrapped, tr)):
    lab = st.AddShape(shp, False)
    TDataStd_Name.Set_s(lab, TCollection_ExtendedString(name))
    st.AddComponent(root, lab, TopLoc_Location(trsf))
st.UpdateAssemblies()
Interface_Static.SetCVal_s("write.step.schema", "AP214IS")
w = STEPCAFControl_Writer()
w.Transfer(doc)
tmp = os.path.join(HERE, "_frame_brep.step")
assert w.Write(tmp) == IFSelect_RetDone
status, report, out = core().payload_write(open(tmp, "rb").read(), raw, generator="make_example.py")
os.remove(tmp)
assert status == "OK", report
open(os.path.join(HERE, "frame.step"), "wb").write(out)
print("frame.ncx.json", len(raw), "bytes;  frame.step", len(out), "bytes;", report["chunks"], "chunks;",
      "header:", report["header"][:80], "...")
