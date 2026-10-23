// Exporter: sweeps and lofts. Profiles, paths and loft sections are references (sketches by id, faces
// and edges by geometry); the options that shape the result are carried, and anything that would change
// the shape but is not carried (thin walls, connectors, ...) is reported.
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        void ExportSweep(IFeature f, JObj nf, bool cut)
        {
            var d = (ISweepFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = cut ? "sweepCut" : "sweep";
                var def = new JObj();
                if (d.CircularProfile)
                {
                    def["circularProfile"] = new JObj { { "diameter", U.Mm(d.CircularProfileDiameter) } };
                }
                else
                {
                    var profile = EntityRef(d.Profile);
                    if (profile == null) { Opaque(nf, "sweep profile not recognised"); return; }
                    def["profile"] = profile;
                }
                var path = EntityRef(d.Path);
                if (path == null) { Opaque(nf, "sweep path not recognised"); return; }
                def["path"] = path;
                if (d.GetGuideCurvesCount() > 0)
                {
                    var guides = GuideRefs(d.GuideCurves, nf);
                    if (guides == null) return;
                    def["guides"] = guides;
                }
                def["twistControl"] = d.TwistControlType;          // swTwistControlType_e
                def["pathAlignment"] = d.PathAlignmentType;
                def["twistAngle"] = U.Deg(d.GetTwistAngle());
                def["tangentPropagation"] = d.TangentPropagation;
                def["maintainTangency"] = d.MaintainTangency;
                def["advancedSmoothing"] = d.AdvancedSmoothing;
                def["mergeSmoothFaces"] = d.MergeSmoothFaces;
                def["alignWithEndFaces"] = d.AlignWithEndFaces;
                def["startTangency"] = d.StartTangencyType;
                def["endTangency"] = d.EndTangencyType;
                def["direction"] = d.Direction;                    // swSweepDirection_e (both directions etc.)
                if (!cut) def["merge"] = d.Merge;
                if (d.IsThinFeature()) Approx(nf, "thin sweep replayed solid");
                nf["def"] = def;
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        /// <summary>A guide curve or loft centreline: a sketch (by id), a sketch segment, or a model edge.</summary>
        JObj GuideRef(object g)
        {
            var sk = g as ISketch;
            if (sk != null) return FeatureRef((IFeature)sk);
            return g is IFeature ? FeatureRef((IFeature)g) : EntityRef(g);
        }

        List<object> GuideRefs(object arr, JObj nf)
        {
            var refs = new List<object>();
            foreach (var g in (object[])arr ?? new object[0])
            {
                var r = GuideRef(g);
                if (r == null) { Opaque(nf, "guide curve not recognised"); return null; }
                refs.Add(r);
            }
            return refs;
        }

        void ExportLoft(IFeature f, JObj nf, bool cut)
        {
            var d = (ILoftFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = cut ? "loftCut" : "loft";
                var sections = FeatureRefs(d.Profiles, nf, "loft section");
                if (sections == null) return;
                List<object> guides = null;
                if (d.GetGuideCurvesCount() > 0 && (guides = GuideRefs(d.GuideCurves, nf)) == null) return;
                JObj centerline = null;
                if (d.Centerline != null && (centerline = GuideRef(d.Centerline)) == null) { Opaque(nf, "loft centreline not recognised"); return; }
                var def = new JObj
                {
                    { "sections", sections }, { "closed", d.Close }, { "maintainTangency", d.MaintainTangency },
                    { "startTangency", d.StartTangencyType }, { "endTangency", d.EndTangencyType },
                    { "startTangentLength", d.StartTangentLength }, { "endTangentLength", d.EndTangentLength },
                    { "reverseStartTangent", d.ReverseStartTangentDirection }, { "reverseEndTangent", d.ReverseEndTangentDirection }
                };
                if (guides != null) def["guides"] = guides;
                if (centerline != null) def["centerline"] = centerline;
                if (!cut) def["merge"] = d.Merge;
                if (d.IsThinFeature()) Approx(nf, "thin loft replayed solid");
                // the connectors (which vertices join across sections) are not carried: SolidWorks' default
                // connection is used, which is what an unedited loft has
                nf["def"] = def;
            }
            finally { d.ReleaseSelectionAccess(); }
        }
    }
}
