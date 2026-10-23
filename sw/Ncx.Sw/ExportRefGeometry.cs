// Exporter: reference axes, points and coordinate systems (FORMAT-SPEC 4.8). Each carries what it was built from
// (references resolved geometrically like any other) and the geometry it produced, which the importer checks.
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        List<object> Refs(object[] entities, JObj nf, string what)
        {
            var refs = new List<object>();
            foreach (var e in entities ?? new object[0])
            {
                var r = EntityRef(e);
                if (r == null) { Opaque(nf, what + " reference not recognised"); return null; }
                refs.Add(r);
            }
            return refs;
        }

        void ExportRefAxis(IFeature f, JObj nf)
        {
            nf["type"] = "axis";
            // For an axis built on an edge, out of process, the definition and even the axis's own line can fault
            // (RPC_E_SERVERFAULT). Read what can be read: the references, else find them from the line.
            IRefAxisFeatureData d = null;
            double[] p = null;
            try { d = (IRefAxisFeatureData)f.GetDefinition(); } catch (System.Runtime.InteropServices.COMException) { }
            try { p = (double[])((IRefAxis)f.GetSpecificFeature2()).GetRefAxisParams(); }   // start, end (m)
            catch (System.Runtime.InteropServices.COMException) { }
            int axisType = 0;
            List<object> refs = null;
            if (d != null)
            {
                bool accessed = false;
                try
                {
                    axisType = d.Type;
                    accessed = d.AccessSelections(doc, null);
                    object types;
                    refs = Refs((object[])d.GetSelections(out types), nf, "axis");
                }
                catch (System.Runtime.InteropServices.COMException) { refs = null; }
                finally { if (accessed) d.ReleaseSelectionAccess(); }
            }
            if (refs == null && nf.Str("status") == "opaque") return;   // a reference that is not recognised
            if (refs == null)
            {
                if (p == null) { Opaque(nf, "axis definition and line both unreadable"); return; }
                // what it was built on, from the line: an edge along it, or a cylindrical/conical face around it,
                // in the model as the axis saw it (an earlier feature's export may have left the model rolled back)
                Rollback((IFeatureManager)doc.FeatureManager, f);
                var hit = EntityOnLine(U.Mm(new[] { p[0], p[1], p[2] }), U.Mm(new[] { p[3], p[4], p[5] }));
                if (hit == null) { Opaque(nf, "axis references unreadable and no edge or face lies on the axis"); return; }
                refs = new List<object> { hit.Item1 };
                axisType = hit.Item2;
            }
            var def = new JObj { { "axisType", axisType }, { "references", refs } };
            if (p != null)
                def["line"] = new JObj { { "start", U.Mm(new[] { p[0], p[1], p[2] }) }, { "end", U.Mm(new[] { p[3], p[4], p[5] }) } };
            nf["def"] = def;
        }

        /// <summary>A model edge lying on the line a-b (axis type 0, one line), else a cylindrical or conical face
        /// whose axis is that line (type 3) - as a reference, with its axis type.</summary>
        Tuple<JObj, int> EntityOnLine(double[] a, double[] b)
        {
            var u = V.Unit(V.Sub(b, a));
            Func<double[], double> off = q => V.Norm(V.Cross(V.Sub(q, a), u));
            foreach (IBody2 body in (object[])part.GetBodies2(0, false) ?? new object[0])
            {
                foreach (IEdge e in (object[])body.GetEdges() ?? new object[0])
                {
                    JObj cp;
                    if (Host.CurveInfo(e, out cp) != "line") continue;
                    var pts = Host.EdgePoints(e, new[] { 0.0, 1.0 });
                    if (pts.All(q => off(q) < 1e-4)) { var r = EntityRef(e); if (r != null) return Tuple.Create(r, 0); }
                }
                foreach (IFace2 fc in (object[])body.GetFaces() ?? new object[0])
                {
                    JObj sp;
                    string t = Host.SurfaceInfo(fc, out sp);
                    if (t != "cylinder" && t != "cone") continue;
                    var ax = sp.Vec("axis");
                    var o = sp.Vec("origin") ?? sp.Vec("apex");
                    if (ax == null || o == null || Math.Abs(Math.Abs(V.Dot(V.Unit(ax), u)) - 1) > 1e-9 || off(o) > 1e-4) continue;
                    var r = EntityRef(fc);
                    if (r != null) return Tuple.Create(r, 3);
                }
            }
            return null;
        }

        void ExportRefPoint(IFeature f, JObj nf)
        {
            var d = (IRefPointFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "point";
                var refs = Refs((object[])d.Selections, nf, "point");
                if (refs == null) return;
                var pt = (double[])((IRefPoint)f.GetSpecificFeature2()).GetRefPoint().ArrayData;
                int along = d.AlongCurveOption;   // swRefPointAlongCurveType_e: 0 distance, 1 percentage, 2 evenly
                nf["def"] = new JObj
                {
                    { "pointType", d.Type }, { "references", refs }, { "alongCurve", along },
                    { "distance", along == 0 ? U.Mm(d.Distance) : d.Distance }, { "point", U.Mm(new[] { pt[0], pt[1], pt[2] }) }
                };
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportCoordinateSystem(IFeature f, JObj nf)
        {
            var d = (ICoordinateSystemFeatureData)f.GetDefinition();
            d.AccessSelections((ModelDoc2)doc, null);   // this one wants the coclass
            try
            {
                nf["type"] = "coordinateSystem";
                var def = new JObj { { "xFlipped", d.XFlipped }, { "yFlipped", d.YFlipped }, { "zFlipped", d.ZFlipped } };
                if (d.OriginEntity != null)
                {
                    var o = EntityRef(d.OriginEntity);
                    if (o == null) { Opaque(nf, "coordinate system origin not recognised"); return; }
                    def["origin"] = o;
                }
                foreach (var ax in new[] { "x", "y", "z" })
                {
                    object ents = ax == "x" ? d.XAxisEntities : ax == "y" ? d.YAxisEntities : d.ZAxisEntities;
                    var refs = Refs(ents as object[], nf, ax + " axis");
                    if (refs == null) return;
                    if (refs.Count > 0) def[ax + "Axis"] = refs;
                }
                // the frame it produced: origin (mm) and unit axes
                var t = (double[])d.Transform.ArrayData;   // 3x3 rotation (rows = axes), translation, scale
                def["frame"] = new JObj
                {
                    { "origin", U.Mm(new[] { t[9], t[10], t[11] }) },
                    { "x", new[] { t[0], t[1], t[2] } }, { "y", new[] { t[3], t[4], t[5] } }, { "z", new[] { t[6], t[7], t[8] } }
                };
                if (d.OriginEntity == null && !def.Has("xAxis") && !def.Has("yAxis") && !def.Has("zAxis"))
                {
                    var o = def.Obj("frame").Vec("origin");
                    if (o.Any(v => System.Math.Abs(v) > 1e-6)) Approx(nf, "coordinate system defined by values, not entities: placed at the origin");
                }
                nf["def"] = def;
            }
            finally { d.ReleaseSelectionAccess(); }
        }
    }
}
