// Exporter: weldments. The Weldment feature is a marker. A structural member carries its profile as a
// library identity (standard / type / size), because each SolidWorks version keeps its own profile library
// in its own layout: 2019+ has one file per type with the sizes as configurations
// (<root>\iso\square tube.sldlfp, configuration "20 x 20 x 2"); 2018 has a file per size
// (<root>\iso\square tube\20 x 20 x 2.sldlfp). Path segments are sketch-entity references.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        /// <summary>{standard, type, size} of a weldment profile from its library path (+ configuration).</summary>
        static JObj ProfileIdentity(string path, string configuration)
        {
            if (string.IsNullOrEmpty(path)) return null;
            string file = Path.GetFileNameWithoutExtension(path);
            string dir = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(configuration))   // one file per type, sizes as configurations
                return new JObj { { "standard", Path.GetFileName(dir) }, { "type", file }, { "size", configuration } };
            return new JObj { { "standard", Path.GetFileName(Path.GetDirectoryName(dir)) }, { "type", Path.GetFileName(dir) }, { "size", file } };
        }

        /// <summary>Corner treatment names (FORMAT-SPEC 4, structuralMember) by swSolidworksWeldmentEndCondOptions_e.</summary>
        static readonly Dictionary<int, string> CornerTreatments = new Dictionary<int, string>
        {
            { 0, "none" }, { 1, "miter" }, { 2, "butt1" }, { 3, "butt2" }
        };

        /// <summary>True when the part has per-corner treatments (Corner Management, SolidWorks 2021+). No API
        /// call creates them, so members are then carried with their result.</summary>
        bool HasCornerManagement()
        {
            try
            {
                object fm = doc.FeatureManager;
                var folders = fm.GetType().InvokeMember("GetCornerManagementFolders",
                    System.Reflection.BindingFlags.InvokeMethod, null, fm, null) as object[];
                return folders != null && folders.Length > 0;
            }
            catch { return false; }   // 2018-2020: no corner management
        }

        void ExportStructuralMember(IFeature f, JObj nf)
        {
            var d = (IStructuralMemberFeatureData)f.GetDefinition();
            d.AccessSelections((ModelDoc2)doc, null);
            try
            {
                nf["type"] = "structuralMember";
                string path = d.WeldmentProfilePath, cfg = null;
                try { cfg = d.ConfigurationName; } catch { }   // not on every version
                var profile = ProfileIdentity(path, cfg);
                if (profile == null) { Opaque(nf, "structural member profile not found"); return; }
                profile["native"] = new JObj { { "path", path }, { "configuration", cfg ?? "" } };
                var groups = new List<object>();
                foreach (IStructuralMemberGroup g in (object[])d.Groups ?? new object[0])
                {
                    var segs = FeatureRefs(g.Segments, nf, "member path segment");
                    if (segs == null) return;
                    if (g.MirrorProfile) Approx(nf, "mirrored profile not carried");
                    if (g.LocateProfilePoint != null) Approx(nf, "located profile point not carried (profile centred)");
                    string corner;
                    if (!g.ApplyCornerTreatment) corner = "none";
                    else if (!CornerTreatments.TryGetValue(g.CornerTreatmentType, out corner))
                    {
                        corner = "none";
                        Approx(nf, "corner treatment " + g.CornerTreatmentType + " not carried");
                    }
                    groups.Add(new JObj
                    {
                        { "segments", segs }, { "angle", U.Deg(g.Angle) },
                        { "cornerTreatment", corner },
                        { "gapWithinGroup", U.Mm(g.GapWithinGroup) }, { "gapForOtherGroups", U.Mm(g.GapForOtherGroups) },
                        { "mergeArcSegmentBodies", g.MergeArcSegmentBodies }, { "miterMergeCondition", g.MiterMergeCondition }
                    });
                }
                nf["def"] = new JObj
                {
                    { "profile", profile }, { "groups", groups },
                    { "connectedSegments", d.ConnectedSegmentsOption == 2 ? "copedCut" : "simpleCut" },
                    { "allowProtrusion", d.AllowProtrusion }
                };
                if (HasCornerManagement())
                {
                    Approx(nf, "per-corner treatments (Corner Management) are not carried: no API call creates them; the member's result is");
                    needsResult.Add(nf);
                }
            }
            finally { d.ReleaseSelectionAccess(); }
        }
    }
}
