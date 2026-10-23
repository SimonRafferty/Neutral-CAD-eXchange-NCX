// Exporter: shape operations beyond the core set - dome, split body, planar surface, knit, wrap - and the
// sheet-metal unfold / fold pair (FORMAT-SPEC 4, "Feature definitions").
using System.Collections.Generic;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Exporter
    {
        void ExportDome(IFeature f, JObj nf)
        {
            var d = (IDomeFeatureData2)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "dome";
                var faces = EntityRefs(d.Faces, nf, "dome face");
                if (faces == null) return;
                nf["def"] = new JObj { { "faces", faces }, { "height", U.Mm(d.Height) }, { "reverse", d.ReverseDir }, { "elliptical", d.Elliptical } };
                if (d.ConstraintPointOrSketch != null || d.Direction != null)
                    Approx(nf, "the dome's constraint point or direction is not carried (a plain dome is made)");
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        /// <summary>A split body: the tools that cut and whether they are consumed. Every resulting body is kept (saving
        /// bodies to their own files is not carried).</summary>
        void ExportSplit(IFeature f, JObj nf)
        {
            var d = (ISplitBodyFeatureData)f.GetDefinition();
            d.AccessSelections((ModelDoc2)doc, null);   // this one wants the coclass
            try
            {
                nf["type"] = "split";
                var tools = EntityRefs(d.TrimTools, nf, "split tool");
                if (tools == null) return;
                nf["def"] = new JObj { { "tools", tools }, { "consume", d.Consume } };
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        /// <summary>A planar surface bounded by a closed sketch.</summary>
        void ExportPlanarSurface(IFeature f, JObj nf)
        {
            nf["type"] = "planarSurface";
            var profile = ProfileRef(f, nf);
            if (profile == null) return;
            nf["def"] = new JObj { { "profile", profile } };
        }

        void ExportKnit(IFeature f, JObj nf)
        {
            var d = (ISurfaceKnitFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = "knit";
                var ents = new List<object>();
                foreach (var o in (object[])d.Entities ?? new object[0])
                {
                    var r = o is IBody2 ? BodyRef(o) : EntityRef(o);
                    if (r == null) { Opaque(nf, "knitted surface not recognised (" + KindOf(o) + ")"); return; }
                    ents.Add(r);
                }
                if (ents.Count == 0) { Opaque(nf, "no knitted surfaces found"); return; }
                nf["def"] = new JObj
                {
                    { "surfaces", ents }, { "formSolid", d.UseTryToFormSolid }, { "mergeEntities", d.UseMergeEntities },
                    { "gapControl", d.UseGapFilters }, { "tolerance", U.Mm(d.KnitTolerance) }, { "maxGap", U.Mm(d.MaxValueForGapRange) }
                };
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        /// <summary>A sketch wrapped onto a face: embossed, debossed or scribed.</summary>
        void ExportWrap(IFeature f, JObj nf)
        {
            var d = (IWrapSketchFeatureData)f.GetDefinition();
            d.AccessSelections((ModelDoc2)doc, null);   // this one wants the coclass
            try
            {
                nf["type"] = "wrap";
                var sk = d.SourceSketch as IFeature;
                var sketch = sk == null ? null : FeatureRef(sk);
                if (sketch == null) { Opaque(nf, "wrapped sketch not recognised"); return; }
                var face = EntityRef(d.Face);
                if (face == null) { Opaque(nf, "wrap face not recognised"); return; }
                int t = d.Type;   // swWrapSketchType_e: 1 emboss, 2 deboss, 3 scribe (as the creation call takes it)
                nf["def"] = new JObj { { "sketch", sketch }, { "face", face }, { "wrapType", t == 2 ? "deboss" : t == 3 ? "scribe" : "emboss" },
                                       { "thickness", U.Mm(d.Thickness) }, { "reverse", d.ReverseDirection } };
                if (d.PullDirection != null) Approx(nf, "the wrap's pull direction is not carried (normal to the sketch)");
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        /// <summary>Sheet-metal unfold or fold: the face that stays put and the bends, each as (its feature, its
        /// position among that feature's sub-features).</summary>
        void ExportFolds(IFeature f, JObj nf, bool fold)
        {
            var d = (IFoldsFeatureData)f.GetDefinition();
            d.AccessSelections(doc, null);
            try
            {
                nf["type"] = fold ? "fold" : "unfold";
                var face = EntityRef(d.FixedFace);
                if (face == null) { Opaque(nf, "fixed face not recognised"); return; }
                var bends = new List<object>();
                foreach (IFeature b in (object[])d.Bends ?? new object[0])
                {
                    var r = BendRef(b);
                    if (r == null) { Opaque(nf, "bend " + b.Name + " not recognised"); return; }
                    // the bend's inner (smaller) cylindrical face, in the state the feature saw: how another system,
                    // which has no bend sub-features, finds the bend. Flat (before a fold), the bend is a flat strip:
                    // its larger face is carried instead.
                    var bendFaces = ((object[])b.GetFaces() ?? new object[0]).Cast<IFace2>().ToList();
                    var inner = bendFaces.Where(x => ((ISurface)x.GetSurface()).IsCylinder())
                        .OrderBy(x => ((double[])((ISurface)x.GetSurface()).CylinderParams)[6]).FirstOrDefault()
                        ?? bendFaces.OrderByDescending(x => x.GetArea()).FirstOrDefault();
                    var bendFace = inner == null ? null : EntityRef(inner);
                    if (bendFace != null) r["face"] = bendFace;
                    bends.Add(r);
                }
                if (bends.Count == 0) { Opaque(nf, "no bends found"); return; }
                nf["def"] = new JObj { { "fixedFace", face }, { "bends", bends } };
            }
            finally { d.ReleaseSelectionAccess(); }
        }

        /// <summary>A bend: {feature, sub} - the exported feature it belongs to and its index among that feature's
        /// sub-features.</summary>
        JObj BendRef(IFeature bend)
        {
            foreach (var kv in exportedFeatures)
            {
                int i = 0;
                for (var sub = (IFeature)kv.Key.GetFirstSubFeature(); sub != null; sub = (IFeature)sub.GetNextSubFeature(), i++)
                    if (sub.Name == bend.Name)
                        return new JObj { { "feature", kv.Value.Str("id") }, { "sub", i }, { "name", bend.Name } };
            }
            return null;
        }
    }
}
