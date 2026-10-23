// Replayer: appearances (FORMAT-SPEC 4.7), applied last (faces are found in the finished model). Plain colours go
// through SolidWorks' colour calls; a textured appearance is looked up in this SolidWorks' own library by its
// library path, else by file name - and if this system does not have it, it is skipped (the user's rule) with a
// note, never a failure.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        Dictionary<string, string> appearanceLibrary;   // file name (lower case) -> full path

        void ReplayAppearances(IModelDoc2 d, JObj nd, Dictionary<string, IComponent2> comps = null)
        {
            var apps = nd.Arr("appearances").Cast<JObj>().ToList();
            if (apps.Count == 0) return;
            int applied = 0;
            var notes = new List<string>();
            var problems = new List<string>();
            foreach (var a in apps)
            {
                string why;
                var targets = AppearanceTargets(d, a.Obj("target"), comps, out why);
                if (targets == null) { problems.Add(why); continue; }
                if (a.Str("kind") == "appearance")
                {
                    string path = FindAppearance(a.Str("file"));
                    if (path == null) { notes.Add("texture " + a.Str("file") + " is not in this system's appearance library: skipped"); continue; }
                    var rm = (IRenderMaterial)d.Extension.CreateRenderMaterial(path);
                    if (rm == null) { problems.Add("appearance " + a.Str("file") + " could not be created"); continue; }
                    var m = a.Obj("mapping");
                    if (m != null)
                    {
                        rm.MappingType = (int)m.Num("type");
                        rm.FixedAspectRatio = m.Bool("fixedAspect");
                        rm.Width = U.M(m.Num("width"));
                        rm.Height = U.M(m.Num("height"));
                        rm.RotationAngle = m.Num("rotation");
                        rm.XPosition = U.M(m.Num("x"));
                        rm.YPosition = U.M(m.Num("y"));
                    }
                    // the source's colour: another version's library copy of the same appearance may differ
                    var col = a.Vec("color");
                    if (col != null && col.Length >= 3)
                        rm.PrimaryColor = (int)Math.Round(col[0] * 255) | (int)Math.Round(col[1] * 255) << 8 | (int)Math.Round(col[2] * 255) << 16;
                    foreach (var t in targets) rm.AddEntity(t);
                    int id;
                    if (!d.Extension.AddRenderMaterial((RenderMaterial)rm, out id)) { problems.Add("appearance " + a.Str("file") + " was not applied"); continue; }
                }
                else
                {
                    var c = a.Vec("color") ?? new[] { 0.8, 0.8, 0.8 };
                    var o = a.Obj("optical") ?? new JObj();
                    var v = new[] { c[0], c[1], c[2], o.Num("ambient", 1), o.Num("diffuse", 1), o.Num("specular", 0.5),
                                    o.Num("shininess", 0.4), o.Num("transparency", 0), o.Num("emission", 0) };
                    foreach (var t in targets) SetColour(d, t, v);
                }
                applied++;
            }
            string summary = applied + " of " + apps.Count + " appearances applied";
            if (problems.Count > 0) Report.Add(docId, "appearances", "approximate", summary + "; " + string.Join("; ", problems));
            else Report.Add(docId, "appearances", "rebuilt", summary);
            foreach (var n in notes) Report.Add(docId, "appearances", "note", n);
        }

        static void SetColour(IModelDoc2 d, object t, double[] v)
        {
            if (t is IFace2) ((IFace2)t).MaterialPropertyValues = v;
            else if (t is IBody2) ((IBody2)t).MaterialPropertyValues2 = v;
            else if (t is IComponent2) ((IComponent2)t).SetMaterialPropertyValues2(v, 2, null);   // swAllConfiguration
            else if (t is IFeature) ((IFeature)t).SetMaterialPropertyValues2(v, 2, null);
            else d.MaterialPropertyValues = v;
        }

        /// <summary>What an appearance goes on, in the rebuilt model (a face may have become several).</summary>
        List<object> AppearanceTargets(IModelDoc2 d, JObj r, Dictionary<string, IComponent2> comps, out string why)
        {
            why = null;
            switch (r?.Str("kind"))
            {
                case "document": return new List<object> { d };
                case "feature":
                {
                    IFeature f;
                    if (features.TryGetValue(r.Str("id"), out f)) return new List<object> { f };
                    why = "appearance target feature " + r.Str("id") + " was not rebuilt";
                    return null;
                }
                case "body":
                {
                    var b = FindBody(r);
                    if (b != null) return new List<object> { b };
                    why = "appearance target body not found";
                    return null;
                }
                case "face":
                {
                    List<object> ents;
                    var res = Host.Resolve((IPartDoc)d, r, out ents);
                    if (res.Str("result") == "resolved") return ents;
                    why = "appearance target face " + res.Str("result");
                    return null;
                }
                case "component":
                {
                    IComponent2 c;
                    if (comps != null && comps.TryGetValue(r.Str("id"), out c)) return new List<object> { c };
                    why = "appearance target component " + r.Str("id") + " not found";
                    return null;
                }
            }
            why = "appearance target " + r?.Str("kind") + " not supported";
            return null;
        }

        /// <summary>The appearance in this SolidWorks' library: by its library path, else by file name.</summary>
        string FindAppearance(string libraryPath)
        {
            if (string.IsNullOrEmpty(libraryPath)) return null;
            string exe = sw.GetExecutablePath();
            string root = Path.Combine(Directory.Exists(exe) ? exe : Path.GetDirectoryName(exe), "data", "graphics", "Materials");
            string direct = Path.Combine(root, libraryPath.Replace('/', '\\'));
            if (File.Exists(direct)) return direct;
            if (appearanceLibrary == null)
            {
                appearanceLibrary = new Dictionary<string, string>();
                if (Directory.Exists(root))
                    foreach (var f in Directory.EnumerateFiles(root, "*.p2m", SearchOption.AllDirectories))
                    {
                        string k = Path.GetFileName(f).ToLowerInvariant();
                        if (!appearanceLibrary.ContainsKey(k)) appearanceLibrary[k] = f;
                    }
            }
            string hit;
            return appearanceLibrary.TryGetValue(Path.GetFileName(libraryPath).ToLowerInvariant(), out hit) ? hit : null;
        }
    }
}
