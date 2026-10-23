"""An OpenCASCADE "host": it does what each CAD add-in does through its own API, so the core can be
exercised and measured without a CAD system.

- descriptor(face|edge)   -> the reference descriptor the exporter records (DESIGN.md section 4)
- candidates(shape, ref)  -> per-candidate type/params/size and sample-point distances for the matcher
- fingerprint(shape)      -> per-body mass-property fingerprint (DESIGN.md section 5)

Units: whatever the STEP file is in (mm for every file here).
"""
import math

from OCP.BRep import BRep_Tool
from OCP.BRepAdaptor import BRepAdaptor_Curve, BRepAdaptor_Surface
from OCP.BRepBndLib import BRepBndLib
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeVertex
from OCP.BRepClass import BRepClass_FaceClassifier
from OCP.BRepExtrema import BRepExtrema_DistShapeShape
from OCP.BRepGProp import BRepGProp
from OCP.BRepMesh import BRepMesh_IncrementalMesh
from OCP.TopLoc import TopLoc_Location
from OCP.BRepTools import BRepTools
from OCP.Bnd import Bnd_Box
from OCP.GeomAbs import (GeomAbs_BezierCurve, GeomAbs_BezierSurface, GeomAbs_BSplineCurve, GeomAbs_BSplineSurface,
                         GeomAbs_Circle, GeomAbs_Cone, GeomAbs_Cylinder, GeomAbs_Ellipse, GeomAbs_Line,
                         GeomAbs_OffsetSurface, GeomAbs_Plane, GeomAbs_Sphere, GeomAbs_SurfaceOfExtrusion,
                         GeomAbs_SurfaceOfRevolution, GeomAbs_Torus)
from OCP.GProp import GProp_GProps
from OCP.IFSelect import IFSelect_RetDone
from OCP.STEPControl import STEPControl_Reader
from OCP.TopAbs import TopAbs_EDGE, TopAbs_FACE, TopAbs_IN, TopAbs_SOLID, TopAbs_VERTEX
from OCP.TopExp import TopExp, TopExp_Explorer
from OCP.TopTools import TopTools_IndexedDataMapOfShapeListOfShape, TopTools_IndexedMapOfShape
from OCP.TopoDS import TopoDS
from OCP.gp import gp_Pnt, gp_Pnt2d

SURF = {GeomAbs_Plane: "plane", GeomAbs_Cylinder: "cylinder", GeomAbs_Cone: "cone", GeomAbs_Sphere: "sphere",
        GeomAbs_Torus: "torus", GeomAbs_BSplineSurface: "bspline", GeomAbs_BezierSurface: "bezier",
        GeomAbs_SurfaceOfRevolution: "surfaceOfRevolution", GeomAbs_SurfaceOfExtrusion: "surfaceOfExtrusion",
        GeomAbs_OffsetSurface: "offset"}
CURV = {GeomAbs_Line: "line", GeomAbs_Circle: "circle", GeomAbs_Ellipse: "ellipse",
        GeomAbs_BSplineCurve: "bspline", GeomAbs_BezierCurve: "bezier"}


def load_step(path):
    r = STEPControl_Reader()
    if r.ReadFile(path) != IFSelect_RetDone:
        raise IOError(f"cannot read {path}")
    r.TransferRoots()
    return r.OneShape()


def topology(shape, kind):
    """Faces or edges in a stable (map) order: list of TopoDS_Face/Edge."""
    m = TopTools_IndexedMapOfShape()
    TopExp.MapShapes_s(shape, TopAbs_FACE if kind == "face" else TopAbs_EDGE, m)
    cast = TopoDS.Face_s if kind == "face" else TopoDS.Edge_s
    return [cast(m.FindKey(i)) for i in range(1, m.Extent() + 1)]


def _xyz(p):
    return [p.X(), p.Y(), p.Z()]


def _dir(d):
    return [d.X(), d.Y(), d.Z()]


# --------------------------------------------------------------------------- geometry facts
def surface_info(face):
    s = BRepAdaptor_Surface(face, True)
    t = SURF.get(s.GetType(), "other")
    p = {}
    if t == "plane":
        pl = s.Plane()
        p = {"normal": _dir(pl.Axis().Direction()), "point": _xyz(pl.Location())}
    elif t == "cylinder":
        c = s.Cylinder()
        p = {"axis": _dir(c.Axis().Direction()), "origin": _xyz(c.Location()), "radius": c.Radius()}
    elif t == "cone":
        c = s.Cone()
        p = {"axis": _dir(c.Axis().Direction()), "apex": _xyz(c.Apex()), "semiAngle": abs(c.SemiAngle())}
    elif t == "sphere":
        sp = s.Sphere()
        p = {"center": _xyz(sp.Location()), "radius": sp.Radius()}
    elif t == "torus":
        to = s.Torus()
        p = {"axis": _dir(to.Axis().Direction()), "center": _xyz(to.Location()),
             "majorRadius": to.MajorRadius(), "minorRadius": to.MinorRadius()}
    return t, p


def curve_info(edge):
    c = BRepAdaptor_Curve(edge)
    t = CURV.get(c.GetType(), "other")
    p = {}
    if t == "line":
        p = {"direction": _dir(c.Line().Direction()), "point": _xyz(c.Line().Location())}
    elif t == "circle":
        ci = c.Circle()
        p = {"axis": _dir(ci.Axis().Direction()), "center": _xyz(ci.Location()), "radius": ci.Radius()}
    elif t == "ellipse":
        e = c.Ellipse()
        p = {"axis": _dir(e.Axis().Direction()), "center": _xyz(e.Location()),
             "majorRadius": e.MajorRadius(), "minorRadius": e.MinorRadius()}
    return t, p


def size(entity, kind):
    g = GProp_GProps()
    if kind == "face":
        BRepGProp.SurfaceProperties_s(entity, g)
    else:
        BRepGProp.LinearProperties_s(entity, g)
    return g.Mass()


def distance(point, entity):
    v = BRepBuilderAPI_MakeVertex(gp_Pnt(*point)).Vertex()
    d = BRepExtrema_DistShapeShape(v, entity)
    return d.Value() if d.IsDone() else float("inf")


# ------------------------------------------------------------------------------- samples
EDGE_FRACTIONS = (0.1, 0.3, 0.5, 0.7, 0.9)
WEAK_CLEARANCE = 0.01  # mm: 10x the default matching tolerance


def edge_samples(edge, fractions=EDGE_FRACTIONS):
    c = BRepAdaptor_Curve(edge)
    a, b = c.FirstParameter(), c.LastParameter()
    return [_xyz(c.Value(a + f * (b - a))) for f in fractions]


def _interior_points(face, limit=60):
    """Candidate interior points: centroids (in UV, evaluated on the exact surface) of the largest
    triangles of the face's mesh. Robust for thin rims and odd parametrisations, unlike a UV grid."""
    loc = TopLoc_Location()
    tri = BRep_Tool.Triangulation_s(face, loc)
    if tri is None:
        BRepMesh_IncrementalMesh(face, 0.05, False, 0.3, True)
        tri = BRep_Tool.Triangulation_s(face, loc)
    surf = BRepAdaptor_Surface(face, True)
    pts = []
    if tri is not None and tri.HasUVNodes():
        tris = []
        for i in range(1, tri.NbTriangles() + 1):
            n1, n2, n3 = tri.Triangle(i).Get()
            uv = [tri.UVNode(n) for n in (n1, n2, n3)]
            p3 = [tri.Node(n) for n in (n1, n2, n3)]
            a = p3[1].XYZ().Subtracted(p3[0].XYZ()).Crossed(p3[2].XYZ().Subtracted(p3[0].XYZ())).Modulus() / 2
            u = sum(q.X() for q in uv) / 3
            v = sum(q.Y() for q in uv) / 3
            tris.append((a, u, v))
        tris.sort(key=lambda t: -t[0])
        pts = [_xyz(surf.Value(u, v)) for _, u, v in tris[:limit]]
    if not pts:  # no mesh: UV grid, refined until something lands inside
        umin, umax, vmin, vmax = BRepTools.UVBounds_s(face)
        tol = BRep_Tool.Tolerance_s(face)
        for n_grid in (9, 17, 33, 65):
            for i in range(n_grid):
                for j in range(n_grid):
                    u = umin + (i + 0.5) / n_grid * (umax - umin)
                    v = vmin + (j + 0.5) / n_grid * (vmax - vmin)
                    if BRepClass_FaceClassifier(face, gp_Pnt2d(u, v), tol).State() == TopAbs_IN:
                        pts.append(_xyz(surf.Value(u, v)))
            if pts:
                break
    return pts


def face_samples(face, k=5, with_clearance=False):
    """Up to k interior points, as far from the face boundary as possible and spread out."""
    inside = _interior_points(face)
    if not inside:
        return ([], 0.0) if with_clearance else []
    edges = []
    ex = TopExp_Explorer(face, TopAbs_EDGE)
    while ex.More():
        edges.append(ex.Current())
        ex.Next()

    def clearance(p):
        return min(distance(p, e) for e in edges) if edges else 1.0

    scored = sorted(((clearance(p), p) for p in inside), key=lambda t: -t[0])
    chosen = [scored[0][1]]
    # then spread: greedily add the point farthest from those chosen, among the better-cleared half
    pool = [p for c, p in scored[: max(1, len(scored) // 2)]]
    while len(chosen) < k and len(chosen) < len(pool):
        best = max(pool, key=lambda p: min(math.dist(p, q) for q in chosen))
        if best in chosen:
            break
        chosen.append(best)
    return (chosen, scored[0][0]) if with_clearance else chosen


def own_samples(entity, kind):
    """Cheap points on a candidate entity, for the core's backward check (target lies on reference)."""
    if kind == "edge":
        return edge_samples(entity, (0.25, 0.5, 0.75))
    return _interior_points(entity, limit=3)


# ------------------------------------------------------------------------------ host API
def adjacency(shape, entity, kind):
    """Types (and plane normals) of the faces next to an edge, or of the faces sharing an edge with a face."""
    m = TopTools_IndexedDataMapOfShapeListOfShape()
    TopExp.MapShapesAndAncestors_s(shape, TopAbs_EDGE, TopAbs_FACE, m)
    out = []
    edges = [entity] if kind == "edge" else []
    if kind == "face":
        ex = TopExp_Explorer(entity, TopAbs_EDGE)
        while ex.More():
            edges.append(ex.Current())
            ex.Next()
    for e in edges:
        if not m.Contains(e):
            continue
        for f in m.FindFromKey(e):
            f = TopoDS.Face_s(f)
            if kind == "face" and f.IsSame(entity):
                continue
            t, p = surface_info(f)
            out.append({"type": t, **({"normal": p["normal"]} if "normal" in p else {})})
    return out


def is_seam(shape, edge):
    """True if the edge is the seam of a periodic face (not a real model edge; nothing references it)."""
    m = TopTools_IndexedDataMapOfShapeListOfShape()
    TopExp.MapShapesAndAncestors_s(shape, TopAbs_EDGE, TopAbs_FACE, m)
    if not m.Contains(edge):
        return False
    return any(BRep_Tool.IsClosed_s(edge, TopoDS.Face_s(f)) for f in m.FindFromKey(edge))


def descriptor(shape, entity, kind):
    t, p = surface_info(entity) if kind == "face" else curve_info(entity)
    g = {"type": t, "params": p, ("area" if kind == "face" else "length"): size(entity, kind),
         "adjacent": adjacency(shape, entity, kind)}
    bb = Bnd_Box()
    BRepBndLib.AddOptimal_s(entity, bb, False, False)
    g["bbox"] = list(bb.Get())
    if kind == "face":
        # freeform faces get more samples: the core cannot backward-check them against an analytic surface
        k = 5 if t in ("plane", "cylinder", "cone", "sphere", "torus") else 9
        g["samples"], g["clearance"] = face_samples(entity, k=k, with_clearance=True)
        # samples with no clearance from the boundary also touch the neighbouring faces
        if g["clearance"] < WEAK_CLEARANCE:
            g["weak"] = True
    else:
        g["samples"] = edge_samples(entity)
        if is_seam(shape, entity):
            g["seam"] = True
        if BRep_Tool.Degenerated_s(entity):  # e.g. the pole of a sphere: zero length, not a model edge
            g["degenerate"] = True
    return {"kind": kind, "by": "geom", "geom": g}


def neighbours(shape, kind):
    """{index: [indices of same-kind entities sharing an edge (faces) or a vertex (edges)]}."""
    ents = topology(shape, kind)
    index = TopTools_IndexedMapOfShape()
    TopExp.MapShapes_s(shape, TopAbs_FACE if kind == "face" else TopAbs_EDGE, index)
    m = TopTools_IndexedDataMapOfShapeListOfShape()
    if kind == "face":
        TopExp.MapShapesAndAncestors_s(shape, TopAbs_EDGE, TopAbs_FACE, m)
    else:
        TopExp.MapShapesAndAncestors_s(shape, TopAbs_VERTEX, TopAbs_EDGE, m)
    out = {i: set() for i in range(len(ents))}
    for k in range(1, m.Extent() + 1):
        idx = [index.FindIndex(s) - 1 for s in m.FindFromIndex(k)]
        for a in idx:
            out[a].update(b for b in idx if b != a)
    return {i: sorted(v) for i, v in out.items()}


class TargetCache:
    """Per-target cached entity data, as a host would keep while replaying one feature."""

    def __init__(self, shape):
        self.shape = shape
        self.ents = {k: topology(shape, k) for k in ("face", "edge")}
        self.nbrs = {k: neighbours(shape, k) for k in ("face", "edge")}
        self._info, self._own, self._box = {}, {}, {}

    def info(self, kind, i):
        if (kind, i) not in self._info:
            e = self.ents[kind][i]
            t, p = surface_info(e) if kind == "face" else curve_info(e)
            self._info[(kind, i)] = (t, p, size(e, kind))
        return self._info[(kind, i)]

    def own(self, kind, i):
        if (kind, i) not in self._own:
            self._own[(kind, i)] = own_samples(self.ents[kind][i], kind)
        return self._own[(kind, i)]

    def box(self, kind, i):
        if (kind, i) not in self._box:
            bb = Bnd_Box()
            BRepBndLib.Add_s(self.ents[kind][i], bb)
            self._box[(kind, i)] = bb
        return self._box[(kind, i)]


def candidates(target, ref, prefilter=None, backward=True):
    """Matcher candidates for `ref` among a target's entities of the same kind.

    target: a TopoDS_Shape or a TargetCache. prefilter (mm): skip entities whose bounding box is farther
    than this from every sample - what a real host does to avoid measuring every entity. With
    backward=True each candidate also carries its own sample points and neighbour ids, which lets the
    core check that it lies on the reference geometry and complete splits.
    """
    tc = target if isinstance(target, TargetCache) else TargetCache(target)
    kind = ref["kind"]
    samples = ref["geom"]["samples"]
    ref_box = None
    if prefilter is not None and ref["geom"].get("bbox"):
        # every piece of a split lies inside the reference entity's own box: filter on that, not on
        # the samples, or pieces no sample fell on are never offered for completion
        ref_box = Bnd_Box()
        ref_box.Update(*ref["geom"]["bbox"])
        ref_box.Enlarge(prefilter)
    out = []
    for i, e in enumerate(tc.ents[kind]):
        if prefilter is not None:
            bb = Bnd_Box()
            bb.Add(tc.box(kind, i))
            bb.Enlarge(prefilter)
            if ref_box is not None:
                if bb.IsOut(ref_box):
                    continue
            elif all(bb.IsOut(gp_Pnt(*s)) for s in samples):
                continue
        t, p, sz = tc.info(kind, i)
        c = {"id": f"{kind}:{i}", "type": t, "params": p, ("area" if kind == "face" else "length"): sz,
             "distances": [distance(s, e) for s in samples]}
        if backward:
            c["samples"] = tc.own(kind, i)
            c["neighbors"] = [f"{kind}:{j}" for j in tc.nbrs[kind][i]]
        out.append(c)
    if backward and prefilter is not None:
        # neighbours of kept candidates must be present too, or completion cannot reach them
        have = {c["id"] for c in out}
        need = {n for c in out for n in c["neighbors"]} - have
        for nid in sorted(need):
            j = int(nid.split(":")[1])
            e = tc.ents[kind][j]
            t, p, sz = tc.info(kind, j)
            out.append({"id": nid, "type": t, "params": p, ("area" if kind == "face" else "length"): sz,
                        "distances": [distance(s, e) for s in samples], "samples": tc.own(kind, j),
                        "neighbors": [f"{kind}:{x}" for x in tc.nbrs[kind][j]]})
    return out


def fingerprint(shape):
    bodies = []
    ex = TopExp_Explorer(shape, TopAbs_SOLID)
    while ex.More():
        s = ex.Current()
        g = GProp_GProps()
        BRepGProp.VolumeProperties_s(s, g)
        ga = GProp_GProps()
        BRepGProp.SurfaceProperties_s(s, ga)
        pp = g.PrincipalProperties()
        ix, iy, iz = pp.Moments()
        bb = Bnd_Box()
        BRepBndLib.AddOptimal_s(s, bb, False, False)
        xmin, ymin, zmin, xmax, ymax, zmax = bb.Get()
        bodies.append({"volume": g.Mass(), "area": ga.Mass(), "centroid": _xyz(g.CentreOfMass()),
                       "moments": sorted([ix, iy, iz]), "bbox": [xmin, ymin, zmin, xmax, ymax, zmax],
                       "faces": len(topology(s, "face")), "edges": len(topology(s, "edge"))})
        ex.Next()
    return {"bodies": bodies}


def face_compare_request(reference, target, prefilter=1.0):
    """The ncx_face_compare request for two shapes: every reference face described and measured against
    the target's faces (as a CAD host builds it after a replay)."""
    tc = TargetCache(target)
    faces = []
    for i, f in enumerate(topology(reference, "face")):
        ref = descriptor(reference, f, "face")
        faces.append({"id": f"ref:{i}", "ref": ref, "candidates": candidates(tc, ref, prefilter=prefilter)})
    targets = [{"id": f"face:{i}", "area": tc.info("face", i)[2]} for i in range(len(tc.ents["face"]))]
    return {"faces": faces, "targets": targets}
