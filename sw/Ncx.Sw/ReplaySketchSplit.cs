// Large sketches split into smaller ones (developer option, off by default: NCX_SPLIT_SKETCHES=1).
//
// Written when SolidWorks 2018 crashed importing the Rear AC Half Shell at its large face sketch (Sketch15, 139
// entities). The crash turned out not to be the sketch: it was the add-in's menu callback (AddIn.cs OnEnableExport),
// and 2018 builds the whole sketch once that is fixed (2026-11-25: whole and split runs give the same result). Kept
// for a sketch that really is too big for an older version. Such a sketch, used
// whole by extrudes or cuts and by nothing else, is replayed as several smaller sketches, each holding some of its
// separate outlines, and each of those features is repeated once per sketch. The result is the same solid; the
// report says what was split. Relations and dimensions between outlines that end up in different sketches are left
// out (the geometry keeps its solved place); construction geometry several outlines refer to is copied into each.
using System;
using System.Collections.Generic;
using System.Linq;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        /// <summary>A sketch is split above this many curves (lines, arcs, circles, splines...).</summary>
        const int SplitAboveCurves = 40;
        /// <summary>Each part of a split sketch holds outlines up to about this many curves.</summary>
        const int SplitChunkCurves = 30;

        /// <summary>Only on request (NCX_SPLIT_SKETCHES=1): no SolidWorks version is known to need it.</summary>
        bool SplitLargeSketchesHere()
        {
            return Environment.GetEnvironmentVariable("NCX_SPLIT_SKETCHES") == "1";
        }

        static List<string> PointRefs(JObj e)
        {
            var r = new List<string>();
            foreach (var k in new[] { "startPoint", "endPoint", "centerPoint", "point" })
            {
                var s = e.Str(k);
                if (s != null) r.Add(s);
            }
            foreach (var p in e.Arr("pointIds")) if (p is string) r.Add((string)p);
            return r;
        }

        /// <summary>The string entity ids a constraint or dimension refers to (references to the origin, edges and the
        /// like are objects, not ids, and travel with every part).</summary>
        static IEnumerable<string> EntityIds(JObj c)
        {
            return c.Arr("entities").OfType<string>();
        }

        static JObj Clone(JObj o) { return (JObj)Json.Parse(Json.Write(o, true)); }

        /// <summary>The part's features with each large sketch that SolidWorks 2018 may not build split (see top).</summary>
        List<object> SplitLargeSketches(JObj d, List<object> feats)
        {
            if (!SplitLargeSketchesHere()) return feats;
            string all = Json.Write(new JObj { { "f", feats }, { "e", d.Arr("equations") } }, true);
            var outList = new List<object>();
            foreach (JObj f in feats)
            {
                if (f.Str("type") != "sketch" || f.Str("status") == "opaque") { outList.Add(f); continue; }
                var def = f.Obj("def");
                string id = f.Str("id");
                var ents = def?.Arr("entities").Cast<JObj>().ToList() ?? new List<JObj>();
                var curves = ents.Where(e => e.Str("type") != "point").ToList();
                if (curves.Count <= SplitAboveCurves) { outList.Add(f); continue; }
                // used whole by extrudes and cuts only: the profile reference is the sketch's only mention
                var users = feats.Cast<JObj>().Where(u => u.Obj("def")?.Obj("profile")?.Str("id") == id).ToList();
                if (users.Count == 0 || users.Any(u => (u.Str("type") != "extrude" && u.Str("type") != "cut") || u.Str("status") == "opaque"
                                                     || u.Obj("def").Has("regions") || u.Obj("def").Has("contours") || u.Obj("def").Has("thin"))) { outList.Add(f); continue; }
                int mentions = CountOf(all, "\"" + id + "\"");
                int expected = 1 + users.Count + d.Arr("equations").Cast<JObj>().Count(e => e.Obj("targetRef")?.Str("feature") == id);
                bool usersFree = users.All(u => CountOf(all, "\"" + u.Str("id") + "\"") == 1);
                if (mentions != expected || !usersFree) { outList.Add(f); continue; }

                var parts = SplitSketch(def, curves, ents);
                if (parts == null || parts.Count < 2) { outList.Add(f); continue; }
                string baseName = f.Str("name");
                int dropped = 0;
                for (int i = 0; i < parts.Count; i++)
                {
                    var sk = Clone(f);
                    var sdef = sk.Obj("def");
                    var keep = parts[i];
                    sdef["entities"] = ents.Where(e => keep.Contains(e.Str("id"))).Cast<object>().ToList();
                    var cons = def.Arr("constraints").Cast<JObj>().ToList();
                    var dims = def.Arr("dimensions").Cast<JObj>().ToList();
                    sdef["constraints"] = cons.Where(c => EntityIds(c).All(keep.Contains) && EntityIds(c).Any()).Cast<object>().ToList();
                    sdef["dimensions"] = dims.Where(c => EntityIds(c).All(keep.Contains) && EntityIds(c).Any()).Cast<object>().ToList();
                    if (i == 0)
                        dropped = cons.Count(c => !parts.Any(p => EntityIds(c).All(p.Contains)))
                                + dims.Count(c => !parts.Any(p => EntityIds(c).All(p.Contains)));
                    if (i > 0)
                    {
                        sk["id"] = id + "#split" + (i + 1);
                        sk["name"] = baseName + " (" + (i + 1) + ")";
                    }
                    outList.Add(sk);
                }
                Report.Add(docId, baseName, "approximate",
                    "split into " + parts.Count + " sketches by its separate outlines (on request); the features using it are repeated once per sketch"
                    + (dropped > 0 ? "; " + dropped + " relations or dimensions between outlines in different sketches left out" : ""));
                // the users are repeated after the last part of the sketch: each part's copy, the carried result on the last
                splitUsers[id] = parts.Count;
            }
            if (splitUsers.Count == 0) return outList;
            var result = new List<object>();
            foreach (JObj f in outList)
            {
                string pid = f.Obj("def")?.Obj("profile")?.Str("id");
                int n;
                if (pid == null || !splitUsers.TryGetValue(pid, out n) || (f.Str("type") != "extrude" && f.Str("type") != "cut")) { result.Add(f); continue; }
                for (int i = 0; i < n; i++)
                {
                    var c = Clone(f);
                    if (i > 0)
                    {
                        c["id"] = f.Str("id") + "#split" + (i + 1);
                        c["name"] = f.Str("name") + " (" + (i + 1) + ")";
                        c.Obj("def").Obj("profile")["id"] = pid + "#split" + (i + 1);
                    }
                    if (i < n - 1) c.Remove("result");
                    result.Add(c);
                }
            }
            splitUsers.Clear();
            return result;
        }

        readonly Dictionary<string, int> splitUsers = new Dictionary<string, int>();

        static int CountOf(string s, string what)
        {
            int n = 0;
            for (int i = s.IndexOf(what, StringComparison.Ordinal); i >= 0; i = s.IndexOf(what, i + what.Length, StringComparison.Ordinal)) n++;
            return n;
        }

        /// <summary>The sketch's entity ids, grouped into parts of whole outlines; null when it has only one outline.</summary>
        static List<HashSet<string>> SplitSketch(JObj def, List<JObj> curves, List<JObj> ents)
        {
            // curves joined through shared points are one outline (union-find over curve indices)
            var parent = Enumerable.Range(0, curves.Count).ToArray();
            Func<int, int> find = null;
            find = i => parent[i] == i ? i : (parent[i] = find(parent[i]));
            var byPoint = new Dictionary<string, int>();
            for (int i = 0; i < curves.Count; i++)
                foreach (var p in PointRefs(curves[i]))
                {
                    int j;
                    if (byPoint.TryGetValue(p, out j)) parent[find(i)] = find(j);
                    else byPoint[p] = i;
                }
            // coincident points of different curves join them too
            var curveOfPoint = new Dictionary<string, int>(byPoint);
            foreach (JObj c in def.Arr("constraints"))
            {
                if (c.Str("type") != "coincident" && c.Str("type") != "tangent" && c.Str("type") != "concentric") continue;
                var ids = EntityIds(c).ToList();
                var idx = ids.Select(x => curveOfPoint.ContainsKey(x) ? curveOfPoint[x] : curves.FindIndex(cv => cv.Str("id") == x)).Where(i => i >= 0).ToList();
                for (int k = 1; k < idx.Count; k++) parent[find(idx[k])] = find(idx[0]);
            }
            var groups = Enumerable.Range(0, curves.Count).GroupBy(find).Select(g => g.ToList()).ToList();
            // outlines (with a real curve) and construction-only groups (shared references, copied where used)
            var outlines = groups.Where(g => g.Any(i => !curves[i].Bool("construction"))).ToList();
            var shared = groups.Where(g => g.All(i => curves[i].Bool("construction"))).ToList();
            if (outlines.Count < 2) return null;
            Func<List<int>, HashSet<string>> idsOf = g =>
            {
                var s = new HashSet<string>();
                foreach (int i in g) { s.Add(curves[i].Str("id")); foreach (var p in PointRefs(curves[i])) s.Add(p); }
                return s;
            };
            var parts = new List<HashSet<string>>();
            var cur = new HashSet<string>();
            int n = 0;
            foreach (var g in outlines)
            {
                if (n > 0 && n + g.Count > SplitChunkCurves) { parts.Add(cur); cur = new HashSet<string>(); n = 0; }
                cur.UnionWith(idsOf(g));
                n += g.Count;
            }
            if (n > 0) parts.Add(cur);
            if (parts.Count < 2) return null;
            // stand-alone points go with the part whose relations name them; shared construction groups are copied
            // into every part that a relation or dimension ties them to
            var relations = def.Arr("constraints").Cast<JObj>().Concat(def.Arr("dimensions").Cast<JObj>()).Select(c => EntityIds(c).ToList()).ToList();
            var loose = ents.Where(e => e.Str("type") == "point" && !parts.Any(p => p.Contains(e.Str("id")))).Select(e => e.Str("id")).ToList();
            var sharedIds = shared.Select(idsOf).ToList();
            foreach (var part in parts)
            {
                bool grew = true;
                while (grew)
                {
                    grew = false;
                    foreach (var r in relations)
                    {
                        if (!r.Any(part.Contains)) continue;
                        foreach (var s in sharedIds)
                            if (r.Any(s.Contains) && !s.IsSubsetOf(part)) { part.UnionWith(s); grew = true; }
                        foreach (var p in loose)
                            if (r.Contains(p) && !part.Contains(p)) { part.Add(p); grew = true; }
                    }
                }
            }
            return parts;
        }
    }
}
