// Exporter: features layoutcad builds beyond the core set - weldment trim/extend and end caps, sheet-metal lofted
// bends, extruded and revolved surfaces (FORMAT-SPEC 4.9).
using System;
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        void ExportWeldmentTrim(IFeature f, JObj nf)
        {
            var d = (IWeldmentTrimExtendFeatureData)f.GetDefinition();
            d.AccessSelections((ModelDoc2)doc, null);   // this one wants the coclass
            try
            {
                nf["type"] = "weldmentTrim";
                var trimmed = BodyRefs(d.BodiesToBeTrimmed, nf, "trimmed body");
                if (trimmed == null) return;
                int boundaryType;
                var boundaryObjs = (object[])d.GetTrimmingBoundary(out boundaryType) ?? new object[0];
                // the boundary is bodies (member against member) or faces
                var boundary = new List<object>();
                foreach (var o in boundaryObjs)
                {
                    var r = o is IBody2 ? BodyRef(o) : EntityRef(o);
                    if (r == null) { Opaque(nf, "trim boundary not recognised"); return; }
                    boundary.Add(r);
                }
                nf["def"] = new JObj { { "cornerType", d.CornerType }, { "trimmed", trimmed }, { "boundary", boundary } };
                // the extension and weld-gap options are not readable through the API: replayed as trim, no gap
                Approx(nf, "trim options (allow extension, weld gap) not readable; replayed as trim only");
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportEndCap(IFeature f, JObj nf)
        {
            var d = (IEndCapFeatureData)f.GetDefinition();
            d.AccessSelections((ModelDoc2)doc, null);   // this one wants the coclass
            try
            {
                nf["type"] = "endCap";
                var face = EntityRef(d.Face);
                if (face == null) { Opaque(nf, "end cap face not recognised"); return; }
                nf["def"] = new JObj
                {
                    { "face", face }, { "depth", U.Mm(d.Thickness) },   // the cap's thickness (DepthDistance is another setting)
                    { "offsetByRatio", d.UseThicknessRatioForOffset }, { "offset", U.Mm(d.OffsetDistance) },
                    { "ratio", d.ThicknessRatioForOffset },
                    { "chamfer", d.UseChamferCorners }, { "chamferDistance", U.Mm(d.ChamferDistance) },
                    { "cornerTreatment", d.UseCornerTreatment }, { "reverse", d.UseReverse }
                };
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportLoftedBend(IFeature f, JObj nf)
        {
            var d = (ILoftedBendsFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "loftedBend";
                var profiles = FeatureRefs(d.Profiles, nf, "lofted bend profile");
                if (profiles == null) return;
                nf["def"] = new JObj
                {
                    { "profiles", profiles }, { "thickness", U.Mm(d.Thickness) }, { "reverseThickness", d.Direction },
                    { "formed", d.FormedMethod }, { "referToEndPoint", d.ReferToEndPoint },
                    { "faceting", d.FacetingOption }, { "facetValue", d.FacetValue }, { "bendLines", d.NumberOfBendLines },
                    { "maxDeviation", U.Mm(d.MaximumDeviation) }
                };
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportExtrudedSurface(IFeature f, JObj nf)
        {
            var d = (ISurfExtrudeFeatureData)f.GetDefinition();   // not IExtrudeFeatureData2
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "extrudedSurface";
                var def = new JObj { { "profile", ProfileRef(f, nf) } };
                if (nf.Str("status") == "opaque") return;
                def["reverse"] = d.ReverseDirection;
                def["end"] = SurfaceEnd(d, true, nf);
                if (d.BothDirections) def["end2"] = SurfaceEnd(d, false, nf);
                nf["def"] = def;
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        JObj SurfaceEnd(ISurfExtrudeFeatureData d, bool forward, JObj nf)
        {
            int t = d.GetEndCondition(forward);
            string name;
            if (!EndTypes.TryGetValue(t, out name) || (name != "blind" && name != "midPlane"))
            {
                Approx(nf, "surface end condition " + t + " replaced by blind");
                name = "blind";
            }
            return new JObj { { "type", name }, { "depth", U.Mm(d.GetDepth(forward)) } };
        }

        void ExportRib(IFeature f, JObj nf)
        {
            var d = (IRibFeatureData2)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "rib";
                var def = new JObj { { "profile", ProfileRef(f, nf) } };
                if (nf.Str("status") == "opaque") return;
                def["twoSided"] = d.IsTwoSided;
                def["thickness"] = U.Mm(d.Thickness);
                def["reverseThickness"] = d.ReverseThicknessDir;   // the side of a one-sided rib
                def["flipMaterial"] = d.FlipSide;                  // which way from the sketch the rib fills
                def["extrusion"] = d.ExtrusionDirection;           // swRibExtrusionDirection_e (parallel / normal to the sketch)
                def["ribType"] = d.Type;
                if (d.EnableDraft)
                    def["draft"] = new JObj { { "angle", U.Deg(d.DraftAngle) }, { "outward", d.DraftOutward }, { "fromWall", d.DraftFromWall } };
                nf["def"] = def;
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportMoveBody(IFeature f, JObj nf)
        {
            var d = (IMoveCopyBodyFeatureData)f.GetDefinition();
            d.AccessSelections((ModelDoc2)doc, null);   // this one wants the coclass
            try
            {
                nf["type"] = "moveBody";
                var bodies = BodyRefs(d.Bodies, nf, "moved body");
                if (bodies == null) return;
                var def = new JObj { { "bodies", bodies }, { "copy", d.Copy }, { "copies", d.NumberOfCopies } };
                // TransformX/Y/Z: a translation (m), or rotation angles (rad) about the model's x, y and z axes through
                // the rotation origin
                switch (d.TransformType)
                {
                    case 1:
                        def["translate"] = U.Mm(new[] { d.TransformX, d.TransformY, d.TransformZ });
                        break;
                    case 2:
                        var angles = new[] { U.Deg(d.TransformX), U.Deg(d.TransformY), U.Deg(d.TransformZ) };
                        def["rotate"] = new JObj { { "origin", U.Mm(new[] { d.RotationOriginX, d.RotationOriginY, d.RotationOriginZ }) }, { "angles", angles } };
                        if (angles.Count(a => Math.Abs(a) > 1e-9) > 1)
                            Approx(nf, "rotation about several axes: the order they apply in is not known (checked by geometry at import)");
                        break;
                    default:
                        Opaque(nf, "move/copy by mates (transform type " + d.TransformType + ") is not mapped yet");
                        return;
                }
                nf["def"] = def;
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        /// <summary>A cosmetic thread: the circular edge it starts from, its diameter, depth and callout.</summary>
        void ExportCosmeticThread(IFeature f, JObj nf)
        {
            // the sub-feature handle's data reports no edge: look the thread up by name
            var tf = (IFeature)part.FeatureByName(f.Name) ?? f;
            var d = (ICosmeticThreadFeatureData)tf.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "cosmeticThread";
                // the typed Edge reads back as nothing out of process; through IDispatch it is there
                var e = Late(d, "Edge");
                var edge = EntityRef(e);
                if (edge == null) { Opaque(nf, "thread edge not recognised (" + KindOf(e) + ")"); return; }
                nf["def"] = new JObj
                {
                    { "edge", edge }, { "diameter", U.Mm(d.Diameter) }, { "diameterType", d.DiameterType },
                    { "endCondition", d.EndCondition }, { "depth", U.Mm(d.BlindDepth) },
                    { "callout", d.ThreadCallout }, { "standard", d.Standard }, { "standardType", d.StandardType },
                    { "size", d.Size }, { "applyThread", d.ApplyThread }
                };
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        /// <summary>A weldment gusset: its two supporting faces and its profile, thickness and position.</summary>
        void ExportGusset(IFeature f, JObj nf)
        {
            var d = (IGussetFeatureData)f.GetDefinition();
            d.AccessSelections((ModelDoc2)doc, null);   // this one wants the coclass
            try
            {
                nf["type"] = "gusset";
                Face2 f1, f2;
                d.GetSupportingFaces(out f1, out f2);
                var r1 = EntityRef(f1);
                var r2 = EntityRef(f2);
                if (r1 == null || r2 == null) { Opaque(nf, "gusset supporting face not recognised"); return; }
                nf["def"] = new JObj
                {
                    { "faces", new List<object> { r1, r2 } }, { "profileType", d.ProfileType },   // swGussetProfileType_e
                    { "d1", U.Mm(d.ProfileDistance1) }, { "d2", U.Mm(d.ProfileDistance2) }, { "d3", U.Mm(d.ProfileDistance3) },
                    { "d4", U.Mm(d.ProfileDistance4) }, { "angle", U.Deg(d.ProfileAngle) }, { "useAngle", d.UseAngle },
                    { "thicknessType", d.ThicknessType }, { "thickness", U.Mm(d.Thickness) },
                    { "location", d.ProfileLocation }, { "offset", d.OffsetUsed }, { "offsetDistance", U.Mm(d.ProfileOffsetDistance) },
                    { "flipOffset", d.FlipOffsetDirection }, { "flipDistances", d.FlipProfileDistanceParameters }
                };
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        void ExportRevolvedSurface(IFeature f, JObj nf)
        {
            var d = (ISurfRevolveFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "revolvedSurface";
                var def = new JObj { { "profile", ProfileRef(f, nf) } };   // the axis is the sketch's centreline
                if (nf.Str("status") == "opaque") return;
                int t = d.Type;   // swRevolveType_e
                def["mode"] = t % 3 == 0 ? "oneDirection" : t % 3 == 1 ? "midPlane" : "twoDirection";
                def["revolveType"] = t;
                def["angle"] = U.Deg(d.GetRevolutionAngle(true));
                if (t % 3 == 2) def["angle2"] = U.Deg(d.GetRevolutionAngle(false));
                def["reverse"] = d.ReverseDirection;
                nf["def"] = def;
            }
            finally { d.ReleaseSelectionAccess(); }
        }
    }
}
