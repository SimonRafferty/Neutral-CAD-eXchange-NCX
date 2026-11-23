// Origin planes by geometry (FORMAT-SPEC 5): `front` is the XY plane (normal Z), `top` XZ (normal Y), `right` YZ
// (normal X), in the model's own frame. SolidWorks' first three planes are the origin planes, but their order in the
// tree does not say where they lie: parts made from some templates have a "Top Plane" that is XY. Naming them by
// position in the tree put every sketch of such a part on the wrong plane at import (2026-11-23, AFPMG motor).
// Exporter and replayer both go through here, so each side names and finds the planes by where they lie.
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public static class OriginPlanes
    {
        /// <summary>The neutral names in the order the replayer keeps its planes (index 0 = front).</summary>
        public static readonly string[] Names = { "front", "top", "right" };

        /// <summary>Index of a neutral plane name in <see cref="Names"/>, or -1.</summary>
        public static int Index(string name) { return Array.IndexOf(Names, name); }

        /// <summary>The neutral name for a plane through the origin with this normal, or null if it is not one of the
        /// three axis planes. Normal: any length; origin: metres.</summary>
        public static string NameOf(double[] normal, double[] origin)
        {
            var n = V.Unit(normal);
            if (Math.Abs(V.Dot(origin, n)) > 1e-9) return null;   // not through the model origin (metres)
            for (int axis = 0; axis < 3; axis++)
                if (Math.Abs(n[axis]) > 1 - 1e-9) return axis == 2 ? "front" : axis == 1 ? "top" : "right";
            return null;
        }

        /// <summary>The neutral name of each of a document's origin planes (its first three RefPlane features, in tree
        /// order), keyed by feature name. Each is named by where it lies; if the three do not lie on the three distinct
        /// axis planes (never seen), the tree order names them, as before.</summary>
        public static Dictionary<string, string> Map(IEnumerable<IFeature> features)
        {
            var planes = features.Where(f => f.GetTypeName2() == "RefPlane").Take(3).ToList();
            var byGeometry = planes.Select(Geometric).ToList();
            bool usable = byGeometry.All(x => x != null) && byGeometry.Distinct().Count() == planes.Count;
            var map = new Dictionary<string, string>();
            for (int i = 0; i < planes.Count; i++) map[planes[i].Name] = usable ? byGeometry[i] : Names[i];
            return map;
        }

        /// <summary><see cref="Map"/> for a document's (or a component's) own feature tree.</summary>
        public static Dictionary<string, string> Map(IFeature first)
        {
            var list = new List<IFeature>();
            for (var f = first; f != null && list.Count < 3; f = (IFeature)f.GetNextFeature())
                if (f.GetTypeName2() == "RefPlane") list.Add(f);
            return Map(list);
        }

        /// <summary>The document's origin planes ordered front, top, right (missing ones are null).</summary>
        public static IFeature[] Ordered(IEnumerable<IFeature> features)
        {
            var planes = features.Where(f => f.GetTypeName2() == "RefPlane").Take(3).ToList();
            var map = Map(planes);
            var ordered = new IFeature[3];
            foreach (var f in planes) ordered[Index(map[f.Name])] = f;
            return ordered;
        }

        /// <summary>The origin plane a neutral name stands for in this feature tree, or null.</summary>
        public static IFeature Find(IFeature first, string name)
        {
            int i = Index(name);
            if (i < 0) return null;
            var list = new List<IFeature>();
            for (var f = first; f != null && list.Count < 3; f = (IFeature)f.GetNextFeature())
                if (f.GetTypeName2() == "RefPlane") list.Add(f);
            return Ordered(list)[i];
        }

        static string Geometric(IFeature f)
        {
            var rp = f.GetSpecificFeature2() as IRefPlane;
            var t = rp?.Transform as IMathTransform;
            var a = t?.ArrayData as double[];
            if (a == null || a.Length < 12) return null;
            // rows 0-2 of the 3x3 are the plane's x, y and normal; 9-11 its origin (metres)
            return NameOf(new[] { a[6], a[7], a[8] }, new[] { a[9], a[10], a[11] });
        }
    }
}
