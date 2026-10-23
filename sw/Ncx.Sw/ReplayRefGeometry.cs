// Replayer: reference axes, points and coordinate systems (FORMAT-SPEC 4.8): the references are selected and
// SolidWorks infers the kind from them, then the result is checked against the source's geometry.
using System;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        const double RefTolMm = 0.01;

        string SelectAll(JObj def, string key, int mark, bool append = false)
        {
            foreach (JObj r in def.Arr(key))
            {
                string why = SelectRef(r, append, mark);
                if (why != null) return why;
                append = true;
            }
            return null;
        }

        string ReplayAxis(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectAll(def, "references", 0);
            if (why != null) return "axis reference: " + why;
            int n0 = FeatureCount();
            doc.InsertAxis2(true);
            string err = Created(n0, null, nf, "axis");
            if (err != null) return err;
            // the same infinite line: parallel, and the source's start point on it
            var p = (double[])((IRefAxis)features[nf.Str("id")].GetSpecificFeature2()).GetRefAxisParams();
            var line = def.Obj("line");
            if (line == null) return null;
            double[] a = U.Mm(new[] { p[0], p[1], p[2] }), b = U.Mm(new[] { p[3], p[4], p[5] });
            double[] s = line.Vec("start"), e = line.Vec("end");
            var u = V.Unit(V.Sub(b, a));
            var w = V.Unit(V.Sub(e, s));
            double off = V.Norm(V.Cross(V.Sub(s, a), u));
            if (Math.Abs(Math.Abs(V.Dot(u, w)) - 1) > 1e-6 || off > RefTolMm)
                return "axis is not on the source's line (offset " + off.ToString("0.###") + " mm)";
            return null;
        }

        string ReplayPoint(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            string why = SelectAll(def, "references", 0);
            if (why != null) return "point reference: " + why;
            int along = Neutral.Code("alongCurve", def["alongCurve"]);
            double dist = along == 0 ? U.M(def.Num("distance")) : def.Num("distance");
            int n0 = FeatureCount();
            FM.InsertReferencePoint(Neutral.Code("pointType", def["pointType"]), along, dist, 1);
            string err = Created(n0, null, nf, "point");
            if (err != null) return err;
            var pt = (double[])((IRefPoint)features[nf.Str("id")].GetSpecificFeature2()).GetRefPoint().ArrayData;
            var want = def.Vec("point");
            if (want != null)
            {
                double off = V.Norm(V.Sub(U.Mm(new[] { pt[0], pt[1], pt[2] }), want));
                if (off > RefTolMm) return "point is " + off.ToString("0.###") + " mm from the source's";
            }
            return null;
        }

        string ReplayCoordinateSystem(JObj nf, JObj def)
        {
            doc.ClearSelection2(true);
            bool append = false;
            if (def.Obj("origin") != null)
            {
                string why = SelectRef(def.Obj("origin"), false, 1);   // origin: mark 1
                if (why != null) return "origin: " + why;
                append = true;
            }
            foreach (var ax in new[] { new { Key = "xAxis", Mark = 2 }, new { Key = "yAxis", Mark = 4 }, new { Key = "zAxis", Mark = 8 } })
            {
                string why = SelectAll(def, ax.Key, ax.Mark, append);
                if (why != null) return ax.Key + ": " + why;
                if (def.Arr(ax.Key).Count > 0) append = true;
            }
            int n0 = FeatureCount();
            FM.InsertCoordinateSystem(def.Bool("xFlipped"), def.Bool("yFlipped"), def.Bool("zFlipped"));
            string err = Created(n0, null, nf, "coordinate system");
            if (err != null || nf.Str("status") == "approximate") return err;
            var frame = def.Obj("frame");
            if (frame == null) return null;
            var t = (double[])((ICoordinateSystemFeatureData)features[nf.Str("id")].GetDefinition()).Transform.ArrayData;
            double off = V.Norm(V.Sub(U.Mm(new[] { t[9], t[10], t[11] }), frame.Vec("origin")));
            double turn = new[] { "x", "y", "z" }.Select((k, i) => 1 - V.Dot(new[] { t[3 * i], t[3 * i + 1], t[3 * i + 2] }, frame.Vec(k))).Max();
            if (off > RefTolMm || turn > 1e-6) return "coordinate system differs from the source's (origin " + off.ToString("0.###") + " mm away)";
            return null;
        }
    }
}
