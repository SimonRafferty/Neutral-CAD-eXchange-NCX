// The SolidWorks "host" of the reference protocol (FORMAT-SPEC section 5) and the fingerprint
// (section 6): what python/ncx/occhost.py does for OpenCASCADE, done through the SolidWorks API.
// Everything leaving this class is in mm.
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public static class Host
    {
        public const double WeakClearanceMm = 0.01;
        static readonly double[] EdgeFractions = { 0.1, 0.3, 0.5, 0.7, 0.9 };
        static readonly HashSet<string> Analytic = new HashSet<string> { "plane", "cylinder", "cone", "sphere", "torus" };

        /// <summary>A copy of `body` (from any document) added to the part `target` as an imported-body feature;
        /// returns the feature or null.</summary>
        public static IFeature AddBody(IModelDoc2 target, IBody2 body)
        {
            object copy = body.Copy();
            try { return ((IPartDoc)target).CreateFeatureFromBody3(copy, false, 0) as IFeature; }
            catch (System.Runtime.InteropServices.COMException) { }
            // out of process the typed call can refuse the body (DISP_E_BADINDEX): pass it through IDispatch
            return ((object)target).GetType().InvokeMember("CreateFeatureFromBody3", System.Reflection.BindingFlags.InvokeMethod, null, target,
                                                 new object[] { new System.Runtime.InteropServices.DispatchWrapper(copy), false, 0 }) as IFeature;
        }

        // ------------------------------------------------------------------- geometry facts
        public static string SurfaceInfo(IFace2 f, out JObj p)
        {
            var s = (ISurface)f.GetSurface();
            p = new JObj();
            if (s.IsPlane())
            {
                var a = (double[])s.PlaneParams;
                p["normal"] = new[] { a[0], a[1], a[2] };
                p["point"] = U.Mm(new[] { a[3], a[4], a[5] });
                return "plane";
            }
            if (s.IsCylinder())
            {
                var a = (double[])s.CylinderParams;
                p["axis"] = new[] { a[3], a[4], a[5] };
                p["origin"] = U.Mm(new[] { a[0], a[1], a[2] });
                p["radius"] = U.Mm(a[6]);
                return "cylinder";
            }
            if (s.IsCone())
            {
                // ConeParams: point on axis, axis, radius there, half angle. The apex lies on the axis at
                // radius / tan(angle) from that point - on whichever side the face actually is.
                var a = (double[])s.ConeParams;
                double[] o = { a[0], a[1], a[2] }, u = V.Unit(new[] { a[3], a[4], a[5] });
                double r = a[6], ang = a[7];
                double k = r / Math.Tan(ang);
                double[] ap1 = { o[0] - u[0] * k, o[1] - u[1] * k, o[2] - u[2] * k };
                double[] ap2 = { o[0] + u[0] * k, o[1] + u[1] * k, o[2] + u[2] * k };
                // decide on points inside the face: both candidate cones pass through the reference circle
                // itself, so a point there (the old test) chose either apex at random
                var qs = InteriorPoints(f, 3).Select(x => x.Select(U.M).ToArray()).ToList();
                if (qs.Count == 0) qs.Add(V.P3(f.GetClosestPointOn(o[0] + r, o[1], o[2])));
                p["axis"] = u;
                p["apex"] = U.Mm(qs.Sum(q => ConeErr(q, ap1, u, ang)) <= qs.Sum(q => ConeErr(q, ap2, u, ang)) ? ap1 : ap2);
                p["semiAngle"] = ang;
                return "cone";
            }
            if (s.IsSphere())
            {
                var a = (double[])s.SphereParams;
                p["center"] = U.Mm(new[] { a[0], a[1], a[2] });
                p["radius"] = U.Mm(a[3]);
                return "sphere";
            }
            if (s.IsTorus())
            {
                var a = (double[])s.TorusParams;
                p["center"] = U.Mm(new[] { a[0], a[1], a[2] });
                p["axis"] = new[] { a[3], a[4], a[5] };
                p["majorRadius"] = U.Mm(a[6]);
                p["minorRadius"] = U.Mm(a[7]);
                return "torus";
            }
            return "bspline";
        }

        static double ConeErr(double[] x, double[] apex, double[] u, double ang)
        {
            var v = V.Sub(x, apex);
            double h = V.Dot(v, u), rho = Math.Sqrt(Math.Max(0, V.Dot(v, v) - h * h));
            return Math.Min(Math.Abs(rho * Math.Cos(ang) - h * Math.Sin(ang)), Math.Abs(rho * Math.Cos(ang) + h * Math.Sin(ang)));
        }

        public static string CurveInfo(IEdge e, out JObj p)
        {
            var c = (ICurve)e.GetCurve();
            p = new JObj();
            if (c.IsLine())
            {
                var a = (double[])c.LineParams;
                p["direction"] = V.Unit(new[] { a[3], a[4], a[5] });
                p["point"] = U.Mm(new[] { a[0], a[1], a[2] });
                return "line";
            }
            if (c.IsCircle())
            {
                var a = (double[])c.CircleParams;
                p["center"] = U.Mm(new[] { a[0], a[1], a[2] });
                p["axis"] = new[] { a[3], a[4], a[5] };
                p["radius"] = U.Mm(a[6]);
                return "circle";
            }
            if (c.IsEllipse()) return "ellipse";
            return "bspline";
        }

        public static double[] EdgePoint(IEdge e, double fraction) { return EdgePoints(e, new[] { fraction })[0]; }

        static double[] Cross(double[] a, double[] b)
        {
            return new[] { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
        }

        static double[] Rotate(double[] v, double[] k, double ang)   // Rodrigues, k unit
        {
            double c = Math.Cos(ang), s = Math.Sin(ang), d = V.Dot(k, v);
            var kv = Cross(k, v);
            return new[] { v[0] * c + kv[0] * s + k[0] * d * (1 - c), v[1] * c + kv[1] * s + k[1] * d * (1 - c), v[2] * c + kv[2] * s + k[2] * d * (1 - c) };
        }

        static bool OnEdge(IEdge e, double[] pM)
        {
            var q = e.GetClosestPointOn(pM[0], pM[1], pM[2]);
            return q != null && V.Dist(V.P3(q), pM) < 1e-7;
        }

        /// <summary>Points on an edge at the given fractions of its extent (mm). The edge's parameter range
        /// (GetCurveParams3) does not match its curve's Evaluate2 parametrisation (measured: a 0..80 mm line
        /// evaluated at 80..160), so lines and circles are sampled from their end points and geometry, and
        /// other curves through ReverseEvaluate of the end points; samples are checked to lie on the edge.</summary>
        public static List<double[]> EdgePoints(IEdge e, IList<double> fractions)
        {
            var c = (ICurve)e.GetCurve();
            var cp = (ICurveParamData)e.GetCurveParams3();
            double[] s = V.P3((object)cp.StartPoint);
            double[] t = V.P3((object)cp.EndPoint);
            var res = new List<double[]>();
            if (c.IsLine())
            {
                foreach (var f in fractions) res.Add(U.Mm(new[] { s[0] + f * (t[0] - s[0]), s[1] + f * (t[1] - s[1]), s[2] + f * (t[2] - s[2]) }));
                return res;
            }
            if (c.IsCircle())
            {
                var a = (double[])c.CircleParams;
                double[] ctr = { a[0], a[1], a[2] }, k = V.Unit(new[] { a[3], a[4], a[5] });
                var va = V.Sub(s, ctr);
                var vb = V.Sub(t, ctr);
                double ang = Math.Atan2(V.Dot(k, Cross(va, vb)), V.Dot(va, vb));
                if (V.Dist(s, t) < 1e-9) ang = 2 * Math.PI;
                if (ang < 0) ang += 2 * Math.PI;
                // the edge runs either way round: keep the sweep whose middle lies on the edge
                double sweep = ang;
                var mid = V.Sub(Rotate(va, k, sweep / 2), new[] { -ctr[0], -ctr[1], -ctr[2] });
                if (!OnEdge(e, mid)) sweep = ang - 2 * Math.PI;
                foreach (var f in fractions)
                {
                    var p = Rotate(va, k, f * sweep);
                    res.Add(U.Mm(new[] { p[0] + ctr[0], p[1] + ctr[1], p[2] + ctr[2] }));
                }
                return res;
            }
            double u0 = c.ReverseEvaluate(s[0], s[1], s[2]), u1 = c.ReverseEvaluate(t[0], t[1], t[2]);
            if (Math.Abs(u1 - u0) < 1e-12) { u0 = cp.UMinValue; u1 = cp.UMaxValue; }   // closed curve
            Func<double, double[]> at = u => V.P3(c.Evaluate2(u, 0));
            if (!OnEdge(e, at((u0 + u1) / 2)))
            {
                u0 = cp.UMinValue;
                u1 = cp.UMaxValue;
            }
            foreach (var f in fractions) res.Add(U.Mm(at(u0 + f * (u1 - u0))));
            return res;
        }

        public static double Length(IEdge e)
        {
            var c = (ICurve)e.GetCurve();
            var cp = (ICurveParamData)e.GetCurveParams3();
            return U.Mm(c.GetLength3(cp.UMinValue, cp.UMaxValue));
        }

        public static double Area(IFace2 f) { return f.GetArea() * U.MM * U.MM; }

        public static double DistanceMm(object entity, double[] pMm)
        {
            double x = U.M(pMm[0]), y = U.M(pMm[1]), z = U.M(pMm[2]);
            object q = entity is IFace2 ? ((IFace2)entity).GetClosestPointOn(x, y, z) : ((IEdge)entity).GetClosestPointOn(x, y, z);
            if (q == null) return double.PositiveInfinity;
            return U.Mm(V.Dist(V.P3(q), new[] { x, y, z }));
        }

        public static double[] Box(object entity)
        {
            if (entity is IFace2)
                return U.Mm((double[])((IFace2)entity).GetBox());
            var e = (IEdge)entity;
            double[] lo = { 1e300, 1e300, 1e300 }, hi = { -1e300, -1e300, -1e300 };
            foreach (var p in EdgePoints(e, Enumerable.Range(0, 33).Select(i => i / 32.0).ToList()))
            {
                for (int k = 0; k < 3; k++) { lo[k] = Math.Min(lo[k], p[k]); hi[k] = Math.Max(hi[k], p[k]); }
            }
            return new[] { lo[0], lo[1], lo[2], hi[0], hi[1], hi[2] };
        }

        public static bool IsSeam(IEdge e)
        {
            var faces = (object[])e.GetTwoAdjacentFaces2();
            return faces != null && faces.Length == 2 && faces[0] != null && ReferenceEquals(faces[0], faces[1]);
        }

        // ------------------------------------------------------------------------ samples
        static List<double[]> InteriorPoints(IFace2 f, int limit)
        {
            var tris = (float[])f.GetTessTriangles(true);
            var cand = new List<KeyValuePair<double, double[]>>();
            if (tris != null)
                for (int i = 0; i + 8 < tris.Length; i += 9)
                {
                    double[] a = { tris[i], tris[i + 1], tris[i + 2] }, b = { tris[i + 3], tris[i + 4], tris[i + 5] },
                             c = { tris[i + 6], tris[i + 7], tris[i + 8] };
                    var ab = V.Sub(b, a);
                    var ac = V.Sub(c, a);
                    double area = 0.5 * V.Norm(new[] { ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0] });
                    cand.Add(new KeyValuePair<double, double[]>(area, new[] { (a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3 }));
                }
            var pts = new List<double[]>();
            foreach (var kv in cand.OrderByDescending(t => t.Key).Take(limit))
            {
                // facet centroid -> exact point on the face
                var q = f.GetClosestPointOn(kv.Value[0], kv.Value[1], kv.Value[2]);
                if (q != null) pts.Add(U.Mm(V.P3(q)));
            }
            return pts;
        }

        public static List<double[]> FaceSamples(IFace2 f, int k, out double clearance)
        {
            var inside = InteriorPoints(f, 60);
            clearance = 0;
            if (inside.Count == 0) return inside;
            var edges = ((object[])f.GetEdges() ?? new object[0]).Cast<IEdge>().ToList();
            Func<double[], double> clear = p => edges.Count == 0 ? 1.0 : edges.Min(e => DistanceMm(e, p));
            var scored = inside.Select(p => new KeyValuePair<double, double[]>(clear(p), p)).OrderByDescending(t => t.Key).ToList();
            clearance = scored[0].Key;
            var chosen = new List<double[]> { scored[0].Value };
            var pool = scored.Take(Math.Max(1, scored.Count / 2)).Select(t => t.Value).ToList();
            while (chosen.Count < k && chosen.Count < pool.Count)
            {
                var best = pool.OrderByDescending(p => chosen.Min(q => V.Dist(p, q))).First();
                if (chosen.Contains(best)) break;
                chosen.Add(best);
            }
            return chosen;
        }

        public static List<double[]> OwnSamples(object entity)
        {
            if (entity is IEdge)
                return EdgePoints((IEdge)entity, new[] { 0.25, 0.5, 0.75 });
            return InteriorPoints((IFace2)entity, 3);
        }

        // ---------------------------------------------------------------------- descriptor
        public static JObj Descriptor(object entity)
        {
            var g = new JObj();
            JObj p;
            string kind;
            if (entity is IFace2)
            {
                var f = (IFace2)entity;
                kind = "face";
                string t = SurfaceInfo(f, out p);
                g["type"] = t;
                g["params"] = p;
                double clearance;
                g["samples"] = FaceSamples(f, Analytic.Contains(t) ? 5 : 9, out clearance);
                g["clearance"] = clearance;
                if (clearance < WeakClearanceMm) g["weak"] = true;
                g["area"] = Area(f);
            }
            else
            {
                var e = (IEdge)entity;
                kind = "edge";
                g["type"] = CurveInfo(e, out p);
                g["params"] = p;
                g["samples"] = EdgePoints(e, EdgeFractions);
                double len = Length(e);
                g["length"] = len;
                if (IsSeam(e)) g["seam"] = true;
                if (len < 1e-9) g["degenerate"] = true;
            }
            g["bbox"] = Box(entity);
            return new JObj { { "kind", kind }, { "by", "geom" }, { "geom", g } };
        }

        // ---------------------------------------------------------------------- resolution
        /// <summary>The bodies' faces or edges with stable ids for one resolution (solids and surfaces: a sketch or
        /// feature can refer to a surface body).</summary>
        public class Target
        {
            public readonly List<object> Ents = new List<object>();
            public readonly List<string> Ids = new List<string>();
            public readonly IList<IBody2> Bodies;

            public Target(IPartDoc part, string kind) : this(((object[])part.GetBodies2(-1, false) ?? new object[0]).Cast<IBody2>().ToList(), kind) { }

            public Target(IList<IBody2> bodyList, string kind)
            {
                Bodies = bodyList;
                if (kind == "vertex") return;   // vertices are resolved by point, without candidates
                var bodies = bodyList.Cast<object>().ToArray();
                for (int b = 0; b < bodies.Length; b++)
                {
                    var body = (IBody2)bodies[b];
                    var list = (object[])(kind == "face" ? body.GetFaces() : body.GetEdges()) ?? new object[0];
                    for (int i = 0; i < list.Length; i++)
                    {
                        Ents.Add(list[i]);
                        Ids.Add(kind + ":" + b + ":" + i);
                    }
                }
            }

            public int IndexOf(object ent)
            {
                for (int i = 0; i < Ents.Count; i++)
                    if (ReferenceEquals(Ents[i], ent)) return i;
                return -1;
            }

            public List<string> Neighbours(int i)
            {
                var res = new List<string>();
                object e = Ents[i];
                IEnumerable<object> others;
                if (e is IFace2)
                    others = ((object[])((IFace2)e).GetEdges() ?? new object[0])
                        .SelectMany(x => (object[])((IEdge)x).GetTwoAdjacentFaces2() ?? new object[0]);
                else
                {
                    var ed = (IEdge)e;
                    var vs = new[] { ed.GetStartVertex(), ed.GetEndVertex() }.Where(v => v != null);
                    others = vs.SelectMany(v => (object[])((IVertex)v).GetEdges() ?? new object[0]);
                }
                foreach (var o in others)
                {
                    int j = IndexOf(o);
                    if (j >= 0 && j != i && !res.Contains(Ids[j])) res.Add(Ids[j]);
                }
                return res;
            }
        }

        /// <summary>Resolve one geometric reference in the current state of a part. Returns the core's
        /// result JSON; the matched entities (only when resolved) are returned in `matched`.</summary>
        public static JObj Resolve(IPartDoc part, JObj reference, out List<object> matched, double tolerance = 0.001)
        {
            return Resolve(new Target(part, reference.Str("kind")), reference, out matched, tolerance);
        }

        /// <summary>Resolve against given bodies (e.g. an assembly component's, in the part frame).</summary>
        public static JObj Resolve(IList<IBody2> bodies, JObj reference, out List<object> matched, double tolerance = 0.001)
        {
            return Resolve(new Target(bodies, reference.Str("kind")), reference, out matched, tolerance);
        }

        /// <summary>A B-rep vertex: its point (FORMAT-SPEC 5: resolved as the one vertex within tolerance).</summary>
        public static JObj VertexDescriptor(IVertex v)
        {
            var p = U.Mm((double[])v.GetPoint());
            return new JObj
            {
                { "kind", "vertex" }, { "by", "geom" },
                { "geom", new JObj { { "type", "point" }, { "params", new JObj { { "point", p } } },
                                     { "samples", new List<object> { p } }, { "bbox", new[] { p[0], p[1], p[2], p[0], p[1], p[2] } } } }
            };
        }

        /// <summary>A vertex reference: the only vertex of the bodies within tolerance of the recorded point
        /// (retried once at 10x, as faces and edges are).</summary>
        static JObj ResolveVertex(IEnumerable<IBody2> bodies, JObj reference, List<object> matched, double tolerance)
        {
            var want = reference.Obj("geom").Obj("params").Vec("point");
            var all = bodies.SelectMany(b => ((object[])b.GetVertices() ?? new object[0]).Cast<IVertex>()).ToList();
            foreach (double tol in new[] { tolerance, tolerance * 10 })
            {
                var hits = all.Where(v => V.Norm(V.Sub(U.Mm((double[])v.GetPoint()), want)) <= tol).ToList();
                if (hits.Count == 1) { matched.Add(hits[0]); return new JObj { { "result", "resolved" }, { "tolerance", tol } }; }
                if (hits.Count > 1) return new JObj { { "result", "ambiguous" }, { "notes", new List<object> { hits.Count + " vertices coincide" } } };
            }
            return new JObj { { "result", "unresolved" }, { "notes", new List<object> { "no vertex at the point" } } };
        }

        static JObj Resolve(Target tgt, JObj reference, out List<object> matched, double tolerance)
        {
            matched = new List<object>();
            string kind = reference.Str("kind");
            if (kind == "vertex") return ResolveVertex(tgt.Bodies, reference, matched, tolerance);
            var geom = reference.Obj("geom");
            var samples = geom.Arr("samples").Select(s => ((List<object>)s).Select(Convert.ToDouble).ToArray()).ToList();
            var rbox = geom.Vec("bbox");
            var cands = new List<object>();
            var keep = new HashSet<int>();
            for (int i = 0; i < tgt.Ents.Count; i++)
            {
                var b = Box(tgt.Ents[i]);
                if (rbox != null && (b[3] < rbox[0] - 1 || b[0] > rbox[3] + 1 || b[4] < rbox[1] - 1 || b[1] > rbox[4] + 1 ||
                                     b[5] < rbox[2] - 1 || b[2] > rbox[5] + 1))
                    continue;
                keep.Add(i);
            }
            foreach (int i in keep.ToList())          // neighbours of kept candidates, for split completion
                foreach (var n in tgt.Neighbours(i)) keep.Add(tgt.Ids.IndexOf(n));
            foreach (int i in keep)
            {
                object e = tgt.Ents[i];
                JObj p;
                string t = e is IFace2 ? SurfaceInfo((IFace2)e, out p) : CurveInfo((IEdge)e, out p);
                var c = new JObj
                {
                    { "id", tgt.Ids[i] }, { "type", t }, { "params", p },
                    { "distances", samples.Select(s => DistanceMm(e, s)).ToList() },
                    { "samples", OwnSamples(e) }, { "neighbors", tgt.Neighbours(i) }
                };
                c[kind == "face" ? "area" : "length"] = e is IFace2 ? Area((IFace2)e) : Length((IEdge)e);
                cands.Add(c);
            }
            var req = new JObj { { "ref", reference }, { "tolerance", tolerance }, { "candidates", cands } };
            var res = (JObj)Json.Parse(NcxCore.MatchReference(Json.Write(req, true)).ReportJson);
            if (res.Str("result") == "resolved")
                foreach (var id in res.Arr("ids")) matched.Add(tgt.Ents[tgt.Ids.IndexOf((string)id)]);
            return res;
        }

        /// <summary>A sketch's definition state (swConstrainedStatus_e) as the payload names it.</summary>
        public static string ConstrainedStatus(ISketch sk)
        {
            switch (sk.GetConstrainedStatus())
            {
                case 2: return "under";
                case 3: return "full";
                case 4: return "over";
                case 5: case 6: return "invalid";
                default: return "unknown";   // 1 unknown, 7 autosolve off
            }
        }

        // ---------------------------------------------------------------------- face-by-face verification
        /// <summary>Face-by-face comparison (FORMAT-SPEC 6.3): every reference face described and measured
        /// against the rebuilt bodies' faces, judged by the core. Both sets must be in the same frame.</summary>
        public static JObj FaceCompare(IList<IBody2> reference, IList<IBody2> rebuilt, double tolerance = 0.001)
        {
            var tgt = new Target(rebuilt, "face");
            // measured once per rebuilt face, not once per reference face
            var boxes = tgt.Ents.Select(Box).ToList();
            var info = new List<JObj>();
            for (int i = 0; i < tgt.Ents.Count; i++)
            {
                var f = (IFace2)tgt.Ents[i];
                JObj p;
                string t = SurfaceInfo(f, out p);
                info.Add(new JObj { { "id", tgt.Ids[i] }, { "type", t }, { "params", p }, { "area", Area(f) },
                                    { "samples", OwnSamples(f) }, { "neighbors", tgt.Neighbours(i) } });
            }
            // pair each reference body with its rebuilt body (volume, centroid): faces are only compared within a
            // pair, so coincident faces of two touching bodies (a mirrored body) cannot both claim one face
            var pairOf = PairBodies(reference, rebuilt);
            var faces = new List<object>();
            int n = 0;
            for (int bi = 0; bi < reference.Count; bi++)
                foreach (IFace2 f in (object[])reference[bi].GetFaces() ?? new object[0])
                {
                    string scope = pairOf[bi] < 0 ? null : "face:" + pairOf[bi] + ":";
                    var desc = Descriptor(f);
                    var geom = desc.Obj("geom");
                    var samples = (List<double[]>)geom["samples"];   // built here by Descriptor, not parsed
                    var rb = geom.Vec("bbox");
                    var keep = new HashSet<int>();
                    for (int i = 0; i < boxes.Count; i++)
                    {
                        if (scope != null && !tgt.Ids[i].StartsWith(scope)) continue;
                        var b = boxes[i];
                        if (b[3] < rb[0] - 1 || b[0] > rb[3] + 1 || b[4] < rb[1] - 1 || b[1] > rb[4] + 1 || b[5] < rb[2] - 1 || b[2] > rb[5] + 1)
                            continue;
                        keep.Add(i);
                    }
                    foreach (int i in keep.ToList())   // neighbours too, so a split can be completed
                        foreach (var id in (List<string>)info[i]["neighbors"])
                            if (scope == null || id.StartsWith(scope)) keep.Add(tgt.Ids.IndexOf(id));
                    var cands = keep.Select(i =>
                    {
                        var c = new JObj();
                        foreach (var kv in info[i]) c[kv.Key] = kv.Value;
                        c["distances"] = samples.Select(s => DistanceMm(tgt.Ents[i], s)).ToList();
                        return (object)c;
                    }).ToList();
                    faces.Add(new JObj { { "id", "ref:" + (n++) }, { "ref", desc }, { "candidates", cands } });
                }
            var req = new JObj
            {
                { "tolerance", tolerance }, { "faces", faces },
                { "targets", info.Select(x => (object)new JObj { { "id", x.Str("id") }, { "area", x["area"] } }).ToList() }
            };
            string dump = System.Environment.GetEnvironmentVariable("NCX_DUMP_FACES");   // developer: keep the request
            if (!string.IsNullOrEmpty(dump)) System.IO.File.WriteAllText(dump, Json.Write(req, true));
            return (JObj)Json.Parse(NcxCore.FaceCompare(Json.Write(req, true)).ReportJson);
        }

        /// <summary>For each reference body, the index of the rebuilt body with the closest volume and centroid
        /// (each used once); -1 when there are fewer rebuilt bodies.</summary>
        static int[] PairBodies(IList<IBody2> reference, IList<IBody2> rebuilt)
        {
            Func<IBody2, double[]> props = b => (double[])b.GetMassProperties(1.0);   // centroid xyz, volume ...
            var rp = reference.Select(props).ToList();
            var tp = rebuilt.Select(props).ToList();
            var used = new HashSet<int>();
            var res = new int[reference.Count];
            for (int i = 0; i < rp.Count; i++)
            {
                int best = -1;
                double bestCost = double.MaxValue;
                for (int j = 0; j < tp.Count; j++)
                {
                    if (used.Contains(j)) continue;
                    double cost = Math.Abs(rp[i][3] - tp[j][3]) / Math.Max(rp[i][3], 1e-12) +
                                  V.Dist(new[] { rp[i][0], rp[i][1], rp[i][2] }, new[] { tp[j][0], tp[j][1], tp[j][2] }) * 1000;
                    if (cost < bestCost) { bestCost = cost; best = j; }
                }
                res[i] = best;
                if (best >= 0) used.Add(best);
            }
            return res;
        }

        // ---------------------------------------------------------------------- fingerprint
        public static JObj BodyFingerprint(IBody2 b)
        {
            var m = (double[])b.GetMassProperties(1.0);   // CoM xyz, volume, area, mass, Ixx Iyy Izz Ixy Izx Iyz (SI)
            const double k5 = 1e15;                        // kg m^2 at density 1 -> mm^5
            var I = new double[3, 3]
            {
                { m[6], -m[9], -m[10] },
                { -m[9], m[7], -m[11] },
                { -m[10], -m[11], m[8] }
            };
            var ev = V.SymEigen(I).Select(x => x * k5).ToArray();
            var box = U.Mm((double[])b.GetBodyBox());
            var fp = new JObj
            {
                { "volume", m[3] * 1e9 }, { "area", m[4] * 1e6 }, { "centroid", U.Mm(new[] { m[0], m[1], m[2] }) },
                { "moments", ev }, { "bbox", box },
                { "faces", b.GetFaceCount() }, { "edges", b.GetEdgeCount() }
            };
            if (IsFreeform(b)) fp["freeform"] = true;
            return fp;
        }

        /// <summary>True when any face is not a plane, cylinder, cone, sphere or torus (FORMAT-SPEC 6: such a body
        /// is compared with the freeform tolerances after going through STEP).</summary>
        public static bool IsFreeform(IBody2 b)
        {
            foreach (IFace2 f in (object[])b.GetFaces() ?? new object[0])
            {
                var s = (ISurface)f.GetSurface();
                if (!(s.IsPlane() || s.IsCylinder() || s.IsCone() || s.IsSphere() || s.IsTorus())) return true;
            }
            return false;
        }

        public static JObj PartFingerprint(IPartDoc part)
        {
            var bodies = (object[])part.GetBodies2(0, false) ?? new object[0];
            return new JObj { { "bodies", bodies.Select(b => (object)BodyFingerprint((IBody2)b)).ToList() } };
        }

        /// <summary>Fingerprint of a part, or of every body of an assembly's (resolved, unsuppressed) leaf
        /// components placed in the assembly frame: what an importer measures on the STEP B-rep.</summary>
        public static JObj DocFingerprint(IModelDoc2 d)
        {
            if (d.GetType() != 2) return PartFingerprint((IPartDoc)d);
            var res = new List<object>();
            foreach (IComponent2 c in ((object[])((IAssemblyDoc)d).GetComponents(false) ?? new object[0]))
            {
                if (c.IsSuppressed()) continue;
                var bodies = c.GetBodies2(0) as object[];
                if (bodies == null) continue;
                var m = Exporter.ToMatrix((double[])((IMathTransform)c.Transform2).ArrayData);
                foreach (IBody2 b in bodies) res.Add(Placed(BodyFingerprint(b), m));
            }
            return new JObj { { "bodies", res } };
        }

        /// <summary>A body fingerprint moved by a rigid transform (row-major 4x4, mm): centroid and box move,
        /// volume, area and principal moments do not.</summary>
        static JObj Placed(JObj fp, double[] m)
        {
            fp["centroid"] = Apply(m, fp.Vec("centroid") ?? (double[])fp["centroid"]);
            var bb = (double[])fp["bbox"];
            fp["bbox"] = BoxOf(Corners(bb).Select(p => Apply(m, p)));
            return fp;
        }

        public static double[] Apply(double[] m, double[] p)
        {
            return new[] { m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3], m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7], m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11] };
        }

        static double[] Rotate(double[] m, double[] v)
        {
            return new[] { m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[4] * v[0] + m[5] * v[1] + m[6] * v[2], m[8] * v[0] + m[9] * v[1] + m[10] * v[2] };
        }

        /// <summary>Inverse of a rigid row-major 4x4 transform.</summary>
        public static double[] Inverse(double[] m)
        {
            // R^T, -R^T t
            double[] t = { m[3], m[7], m[11] };
            var r = new[] { m[0], m[4], m[8], 0, m[1], m[5], m[9], 0, m[2], m[6], m[10], 0, 0, 0, 0, 1.0 };
            var nt = Rotate(r, t);
            r[3] = -nt[0]; r[7] = -nt[1]; r[11] = -nt[2];
            return r;
        }

        static IEnumerable<double[]> Corners(double[] b)
        {
            for (int i = 0; i < 8; i++)
                yield return new[] { (i & 1) == 0 ? b[0] : b[3], (i & 2) == 0 ? b[1] : b[4], (i & 4) == 0 ? b[2] : b[5] };
        }

        static double[] BoxOf(IEnumerable<double[]> pts)
        {
            double[] lo = { 1e300, 1e300, 1e300 }, hi = { -1e300, -1e300, -1e300 };
            foreach (var p in pts)
                for (int k = 0; k < 3; k++) { lo[k] = Math.Min(lo[k], p[k]); hi[k] = Math.Max(hi[k], p[k]); }
            return new[] { lo[0], lo[1], lo[2], hi[0], hi[1], hi[2] };
        }

        /// <summary>Move a descriptor by a rigid transform (points, directions, box).</summary>
        public static JObj TransformDescriptor(JObj desc, double[] m)
        {
            var g = desc.Obj("geom");
            g["samples"] = g.Arr("samples").Select(s => (object)Apply(m, ((List<object>)s).Select(Convert.ToDouble).ToArray())).ToList();
            var p = g.Obj("params");
            if (p != null)
                foreach (var k in p.Select(kv => kv.Key).ToList())
                {
                    var v = p.Vec(k) ?? p[k] as double[];
                    if (v == null || v.Length != 3) continue;
                    p[k] = k == "normal" || k == "axis" || k == "direction" ? Rotate(m, v) : Apply(m, v);
                }
            var bb = g.Vec("bbox") ?? g["bbox"] as double[];
            if (bb != null) g["bbox"] = BoxOf(Corners(bb).Select(c => Apply(m, c)));
            return desc;
        }

        /// <summary>Descriptor of a mate entity in its component's part frame. The entity may report its
        /// geometry in the part frame or the assembly frame: whichever lies on the component's own
        /// (part-frame) bodies is the part frame.</summary>
        public static JObj ComponentLocalDescriptor(ISldWorks sw, IComponent2 comp, object entity)
        {
            var bodiesV = ((object[])comp.GetBodies2(0) ?? new object[0]).Cast<IBody2>().ToList();
            if (entity is IVertex)
            {
                // a vertex: its point in the part frame (whichever frame puts it on the component's own vertices)
                var q = U.Mm((double[])((IVertex)entity).GetPoint());
                bool onOwn = bodiesV.Any(b => ((object[])b.GetVertices() ?? new object[0]).Cast<IVertex>()
                                                .Any(v => V.Dist(U.Mm((double[])v.GetPoint()), q) < 1e-4));
                if (!onOwn) q = Apply(Inverse(Exporter.ToMatrix((double[])((IMathTransform)comp.Transform2).ArrayData)), q);
                return new JObj
                {
                    { "kind", "vertex" }, { "by", "geom" },
                    { "geom", new JObj { { "type", "point" }, { "params", new JObj { { "point", q } } },
                                         { "samples", new List<object> { q } }, { "bbox", new[] { q[0], q[1], q[2], q[0], q[1], q[2] } } } }
                };
            }
            var desc = (JObj)Json.Parse(Json.Write(Descriptor(entity), true));   // plain JSON values
            var s0 = ((List<object>)desc.Obj("geom").Arr("samples")[0]).Select(Convert.ToDouble).ToArray();
            var bodies = ((object[])comp.GetBodies2(0) ?? new object[0]).Cast<IBody2>().ToList();
            bool local = bodies.Any(b => ((object[])b.GetFaces() ?? new object[0]).Any(f => DistanceMm(f, s0) < 1e-4));
            if (local) return desc;
            var m = Exporter.ToMatrix((double[])((IMathTransform)comp.Transform2).ArrayData);
            return TransformDescriptor(desc, Inverse(m));
        }
    }
}
