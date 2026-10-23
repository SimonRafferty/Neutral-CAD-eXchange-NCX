// Small vector/matrix helpers and unit conversion. The SolidWorks API is in metres and radians;
// the neutral model is in mm and degrees (FORMAT-SPEC section 4).
using System;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public static class U
    {
        public const double MM = 1000.0;              // metres -> mm
        public static double Mm(double m) { return m * MM; }
        public static double M(double mm) { return mm / MM; }
        public static double Deg(double rad) { return rad * 180.0 / Math.PI; }
        public static double Rad(double deg) { return deg * Math.PI / 180.0; }
        public static double[] Mm(double[] m) { var r = new double[m.Length]; for (int i = 0; i < m.Length; i++) r[i] = m[i] * MM; return r; }
        public static double R(double v, int digits = 9) { return Math.Round(v, digits); }
    }

    public static class V
    {
        public static double[] Sub(double[] a, double[] b) { return new[] { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
        public static double Dot(double[] a, double[] b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
        public static double Norm(double[] a) { return Math.Sqrt(Dot(a, a)); }
        public static double[] Cross(double[] a, double[] b) { return new[] { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; }
        public static double Dist(double[] a, double[] b) { return Norm(Sub(a, b)); }
        public static double[] Unit(double[] a) { double n = Norm(a); return n > 1e-300 ? new[] { a[0] / n, a[1] / n, a[2] / n } : a; }
        public static double[] P3(object arr)
        {
            var a = (double[])arr;
            return new[] { a[0], a[1], a[2] };
        }

        /// <summary>Eigenvalues of a symmetric 3x3 matrix (Jacobi), ascending.</summary>
        public static double[] SymEigen(double[,] m)
        {
            var a = (double[,])m.Clone();
            for (int sweep = 0; sweep < 50; sweep++)
            {
                double off = a[0, 1] * a[0, 1] + a[0, 2] * a[0, 2] + a[1, 2] * a[1, 2];
                if (off < 1e-30) break;
                for (int p = 0; p < 2; p++)
                    for (int q = p + 1; q < 3; q++)
                    {
                        if (Math.Abs(a[p, q]) < 1e-300) continue;
                        double theta = (a[q, q] - a[p, p]) / (2 * a[p, q]);
                        double t = Math.Sign(theta == 0 ? 1 : theta) / (Math.Abs(theta) + Math.Sqrt(theta * theta + 1));
                        double c = 1 / Math.Sqrt(t * t + 1), s = t * c;
                        for (int k = 0; k < 3; k++)
                        {
                            double akp = a[k, p], akq = a[k, q];
                            a[k, p] = c * akp - s * akq;
                            a[k, q] = s * akp + c * akq;
                        }
                        for (int k = 0; k < 3; k++)
                        {
                            double apk = a[p, k], aqk = a[q, k];
                            a[p, k] = c * apk - s * aqk;
                            a[q, k] = s * apk + c * aqk;
                        }
                    }
            }
            var ev = new[] { a[0, 0], a[1, 1], a[2, 2] };
            Array.Sort(ev);
            return ev;
        }
    }

    /// <summary>Model <-> sketch coordinate mapping for one sketch (all in metres).</summary>
    public class SketchFrame
    {
        public readonly double[] Origin, X, Y, Z;   // model coordinates of the sketch origin and axes

        public SketchFrame(ISldWorks sw, ISketch sk)
        {
            var mu = (IMathUtility)sw.GetMathUtility();
            var toSketch = (IMathTransform)sk.ModelToSketchTransform;
            var toModel = (IMathTransform)toSketch.Inverse();
            Origin = Map(mu, toModel, 0, 0, 0);
            X = V.Unit(V.Sub(Map(mu, toModel, 1, 0, 0), Origin));
            Y = V.Unit(V.Sub(Map(mu, toModel, 0, 1, 0), Origin));
            Z = V.Unit(V.Sub(Map(mu, toModel, 0, 0, 1), Origin));
        }

        public SketchFrame(double[] origin, double[] x, double[] z)
        {
            Origin = origin;
            X = V.Unit(x);
            Z = V.Unit(z);
            Y = new[] { Z[1] * X[2] - Z[2] * X[1], Z[2] * X[0] - Z[0] * X[2], Z[0] * X[1] - Z[1] * X[0] };
        }

        static double[] Map(IMathUtility mu, IMathTransform t, double x, double y, double z)
        {
            var p = (IMathPoint)mu.CreatePoint(new[] { x, y, z });
            p = (IMathPoint)p.MultiplyTransform(t);
            return V.P3(p.ArrayData);
        }

        public double[] ToModel(double sx, double sy)
        {
            return new[] { Origin[0] + sx * X[0] + sy * Y[0], Origin[1] + sx * X[1] + sy * Y[1], Origin[2] + sx * X[2] + sy * Y[2] };
        }

        public double[] ToSketch(double[] m)
        {
            var d = V.Sub(m, Origin);
            return new[] { V.Dot(d, X), V.Dot(d, Y), V.Dot(d, Z) };
        }
    }
}
