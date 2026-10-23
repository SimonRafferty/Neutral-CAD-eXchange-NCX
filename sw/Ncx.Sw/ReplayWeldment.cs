// Replayer: weldments. The structural member's profile is looked up by identity in THIS SolidWorks' profile
// library (Tools > Options > File Locations > Weldment Profiles), in either library layout.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        string ReplayWeldment(JObj nf)
        {
            int n0 = FeatureCount();
            var feat = FM.InsertWeldmentFeature();
            return Created(n0, feat as IFeature, nf, "weldment");
        }

        /// <summary>(file, configuration) of a profile in this SolidWorks' weldment profile folders, or null.</summary>
        string[] FindProfile(JObj p)
        {
            string std = p.Str("standard"), type = p.Str("type"), size = p.Str("size");
            foreach (var root in (sw.GetUserPreferenceStringValue(29) ?? "").Split(';'))   // swFileLocationsWeldmentProfiles
            {
                if (string.IsNullOrWhiteSpace(root)) continue;
                string oneFile = Path.Combine(root, std, type + ".sldlfp");
                if (File.Exists(oneFile)) return new[] { oneFile, size };
                string perSize = Path.Combine(root, std, type, size + ".sldlfp");
                if (File.Exists(perSize)) return new[] { perSize, "" };
            }
            return null;
        }

        string ReplayStructuralMember(JObj nf, JObj def)
        {
            var p = def.Obj("profile");
            var lib = FindProfile(p);
            if (lib == null) return "profile " + p.Str("standard") + "/" + p.Str("type") + "/" + p.Str("size") + " is not in this SolidWorks' weldment profile library";
            // arrays must be typed DispatchWrapper[] (SAFEARRAY of VT_DISPATCH): an object[] of wrappers arrives as
            // VARIANTs and the group silently keeps no segments (measured)
            var groups = new List<DispatchWrapper>();
            foreach (JObj g in def.Arr("groups"))
            {
                var segs = new List<DispatchWrapper>();
                foreach (JObj r in g.Arr("segments"))
                {
                    SketchMap m;
                    ISketchSegment seg;
                    if (r.Str("kind") != "sketchEntity" || !sketchMaps.TryGetValue(r.Str("feature"), out m) || !m.Segs.TryGetValue(r.Str("id"), out seg))
                        return "member path segment " + r.Str("id") + " not found";
                    segs.Add(new DispatchWrapper(seg));
                }
                var grp = (IStructuralMemberGroup)FM.CreateStructuralMemberGroup();
                grp.Segments = segs.ToArray();
                grp.Angle = U.Rad(g.Num("angle"));
                if (g["cornerTreatment"] != null)
                {
                    grp.ApplyCornerTreatment = true;
                    grp.CornerTreatmentType = (int)g.Num("cornerTreatment");
                }
                grp.GapWithinGroup = U.M(g.Num("gapWithinGroup"));
                grp.GapForOtherGroups = U.M(g.Num("gapForOtherGroups"));
                grp.MergeArcSegmentBodies = g.Bool("mergeArcSegmentBodies");
                grp.MiterMergeCondition = g.Bool("miterMergeCondition");
                groups.Add(new DispatchWrapper(grp));
            }
            doc.ClearSelection2(true);
            Func<List<IFeature>> members = () =>
            {
                var list = new List<IFeature>();
                for (var f = (IFeature)doc.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
                    if (f.GetTypeName2() == "WeldMemberFeat") list.Add(f);
                return list;
            };
            int m0 = members().Count;
            int n0 = FeatureCount();
            var feat = FM.InsertStructuralWeldment5(lib[0], (int)def.Num("connectedSegments", 1), def.Bool("allowProtrusion", true),
                                                    groups.ToArray(), lib[1]);
            // the call returns nothing usable and the tree's last item is not the member (cut-list folders follow
            // it); members are listed in creation order, so the new one is the next in that list. (Earlier members'
            // objects may have been replaced, with default names, so "the unregistered one" is not reliable.)
            var after = members();
            if (after.Count <= m0) return "structural member was not created (profile " + lib[0] + ")";
            IFeature member = after[m0];
            string err = Created(n0, member, nf, "structural member");
            if (err == null && member != null && member.Name != nf.Str("name"))
                Approx(nf, "member could not be renamed (kept '" + member.Name + "')");
            if (err != null)
                return err + " (profile " + lib[0] + (lib[1] == "" ? "" : " / " + lib[1]) + "; segments per group " +
                       string.Join(",", groups.Select(g => ((IStructuralMemberGroup)g.WrappedObject).GetSegmentsCount())) + ")";
            if (err == null && Path.GetFileNameWithoutExtension(lib[0]) != Path.GetFileNameWithoutExtension(p.Obj("native")?.Str("path") ?? lib[0]))
                Approx(nf, "profile taken from this version's library: " + lib[0] + (lib[1] == "" ? "" : " (" + lib[1] + ")"));
            return err;
        }
    }
}
