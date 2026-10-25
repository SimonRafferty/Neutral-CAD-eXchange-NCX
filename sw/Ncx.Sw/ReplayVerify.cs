// Replayer: verification beyond mass properties (FORMAT-SPEC 6.2-6.4), run on each part that was rebuilt
// (not on one replaced by its reference geometry):
//  - structure: every feature of the payload is in the rebuilt tree, under its name and native type;
//  - parameter flex: the payload's probes applied to the rebuild give the source's flexed geometry;
//  - faces: the rebuild matches the file's own B-rep face by face.
// Each adds one entry to Verification; none of them changes the rebuilt model.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        HashSet<string> equationTargets = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        /// <summary>Is this dimension (full name "D1@Sketch1@Part") the target of one of the document's equations?</summary>
        bool EquationTarget(string fullName)
        {
            if (string.IsNullOrEmpty(fullName)) return false;
            return equationTargets.Any(t => fullName.Equals(t, StringComparison.OrdinalIgnoreCase) ||
                                            fullName.StartsWith(t + "@", StringComparison.OrdinalIgnoreCase));
        }

        void VerifyRebuilt(JObj d)
        {
            Step(d.Str("name") + ": verifying");
            VerifyStructure(d);
            VerifyFaces(d);   // flex is checked once the whole model is rebuilt (VerifyFlex)
        }

        void VerifyStructure(JObj d)
        {
            var missing = new List<object>();
            var wrongType = new List<object>();
            var notDefined = new List<object>();
            int n = 0;
            foreach (JObj nf in d.Arr("features"))
            {
                if (nf.Str("status") == "opaque") continue;   // reported as a fallback already
                n++;
                var f = part.FeatureByName(nf.Str("name")) as IFeature;
                string want = nf.Obj("native")?.Str("type");
                if (f == null) missing.Add(nf.Str("name"));
                else if (want != null && f.GetTypeName2() != want)
                    wrongType.Add(new JObj { { "feature", nf.Str("name") }, { "expected", want }, { "actual", f.GetTypeName2() } });
                // a fully defined sketch must arrive fully defined: a lost relation or dimension, or a reference
                // dimension where a driving one was, leaves it under-defined although its shape is right
                else if (nf.Str("type") == "sketch" && nf.Obj("def")?.Str("constrained") == "full")
                {
                    var sk = f.GetSpecificFeature2() as ISketch;
                    string now = sk == null ? "unknown" : Host.ConstrainedStatus(sk);
                    if (now != "full") notDefined.Add(new JObj { { "sketch", nf.Str("name") }, { "now", now } });
                }
            }
            var props = PropertyDifferences(doc, d);
            Verification.Add(new JObj
            {
                { "document", docId + " (structure)" }, { "check", "structure" },
                { "result", missing.Count + wrongType.Count + notDefined.Count + props.Count == 0 ? "match" : "mismatch" },
                { "features", n }, { "missing", missing }, { "wrongType", wrongType }, { "notFullyDefined", notDefined },
                { "properties", props }
            });
        }

        /// <summary>Apply every document's flex probes to the rebuilt model (all documents rebuilt by now, so a
        /// layout's probe reaches the derived parts) and compare each part it moved with the source's.
        /// The model is left exactly as built; documents the probes dirtied are saved again.</summary>
        void VerifyFlex(JObj model, Dictionary<string, IModelDoc2> rebuilt, HashSet<string> fellBack)
        {
            if (!model.Arr("documents").Cast<JObj>().Any(d => d.Arr("flex").Count > 0)) return;
            Step("checking parameter behaviour");
            var parts = model.Arr("documents").Cast<JObj>()
                .Where(d => d.Str("kind") == "part" && rebuilt.ContainsKey(d.Str("id")) && rebuilt[d.Str("id")] is IPartDoc)
                .Select(d => new KeyValuePair<string, IModelDoc2>(d.Str("id"), rebuilt[d.Str("id")])).ToList();
            var baseline = Flex.Measure(parts);
            var names = parts.ToDictionary(p => p.Key, p => Flex.TreeNames(p.Value));
            foreach (JObj d in model.Arr("documents"))
                foreach (JObj p in d.Arr("flex"))
                {
                    var entry = new JObj { { "document", d.Str("id") + " (flex " + p.Str("parameter") + ")" }, { "check", "flex" },
                                           { "parameter", p.Str("parameter") }, { "value", p.Num("value") } };
                    Verification.Add(entry);
                    var expected = p.Obj("fingerprints") ?? new JObj();
                    IModelDoc2 md;
                    if (!rebuilt.TryGetValue(d.Str("id"), out md) || fellBack.Contains(d.Str("id")) ||
                        expected.Select(kv => kv.Key).Any(k => fellBack.Contains(k) || !baseline.ContainsKey(k)))
                    {
                        entry["result"] = "skipped";
                        entry["reason"] = "a part it moves was not rebuilt parametrically";
                        continue;
                    }
                    if (expected.Select(kv => kv.Key).Concat(new[] { d.Str("id") }).Any(k => fixedGeometry.Contains(k)))
                    {
                        entry["result"] = "skipped";
                        entry["reason"] = "a part it moves holds fixed geometry (a carried feature result: FORMAT-SPEC 4, Feature results) that does not follow parameters";
                        continue;
                    }
                    var prm = p.Str("kind") == "variable" ? Flex.Variable(sw, md, p.Str("parameter"))
                                                          : Flex.Dimension(md, p.Str("parameter"), p.Str("kind"));
                    if (prm == null)
                    {
                        entry["result"] = "failed";
                        entry["reason"] = "parameter not found in the rebuild";
                        continue;
                    }
                    string active = ((IConfiguration)md.GetActiveConfiguration()).Name;
                    string cfg = p.Str("configuration");
                    if (cfg != null && cfg != active) md.ShowConfiguration2(cfg);
                    double orig = prm.Get();
                    prm.Set(Flex.ToSystem(p.Str("kind"), p.Num("value")));
                    Flex.Rebuild(parts);
                    var actual = Flex.Measure(parts);
                    prm.Set(orig);
                    Flex.Rebuild(parts);
                    if (cfg != null && cfg != active) md.ShowConfiguration2(active);
                    // every part the source probe moved must move the same way; any other part must not move
                    var diffs = new List<object>();
                    foreach (var kv in actual)
                    {
                        var want = expected.Obj(kv.Key) ?? baseline[kv.Key];
                        if (Flex.Compare(want, kv.Value) != "match") diffs.Add(kv.Key);
                    }
                    entry["result"] = diffs.Count == 0 ? "match" : "mismatch";
                    entry["documents"] = expected.Select(kv => (object)kv.Key).ToList();
                    if (diffs.Count > 0)
                    {
                        entry["differs"] = diffs;
                        entry["actual"] = diffs.Cast<string>().Select(k => (object)new JObj { { "document", k }, { "fingerprint", actual[k] } }).ToList();
                    }
                }
            foreach (var p in parts) Flex.RestoreNames(p.Value, names[p.Key]);
            // the rebuild must be left exactly as it was built
            var after = Flex.Measure(parts);
            foreach (var p in parts)
                if (Flex.Compare(baseline[p.Key], after[p.Key]) != "match")
                    Verification.Add(new JObj { { "document", p.Key + " (flex restore)" }, { "check", "flex" }, { "result", "mismatch" },
                                                { "reason", "the part did not return exactly to its values after the flex probes" } });
            foreach (var p in rebuilt.Values)
                if (p.GetSaveFlag())
                {
                    int err = 0, warn = 0;
                    p.Save3(1, ref err, ref warn);   // silent
                }
        }

        void VerifyFaces(JObj d)
        {
            // the file's B-rep holds a part as the assembly shows it: a part an assembly cut changes differs there
            var cutBy = Model?.Arr("documents").Cast<JObj>().Where(a => a.Str("kind") == "assembly")
                .SelectMany(a => a.Arr("features").Cast<JObj>().Where(f => f.Str("type") == "cut")
                    .SelectMany(f => (f.Obj("def")?.Arr("components") ?? new List<object>()).Cast<string>()
                        .Select(cid => a.Arr("components").Cast<JObj>().FirstOrDefault(c => c.Str("id") == cid)?.Str("document"))))
                .Where(x => x != null).ToList() ?? new List<string>();
            if (cutBy.Contains(d.Str("id")))
            {
                Verification.Add(new JObj { { "document", docId + " (faces)" }, { "check", "faces" }, { "result", "skipped" },
                                            { "reason", "an assembly cut changes this part in the file's geometry" } });
                return;
            }
            // an assembly's B-rep holds each part under its name; a part file's B-rep is its root part
            // (saved one part per product when the file was read; opened only for this comparison)
            string refPath;
            if (!refParts.TryGetValue(d.Str("name"), out refPath) && d.Str("id") == Model.Str("root")) refPath = rootRefPath;
            if (refPath == null || !File.Exists(refPath)) return;   // no B-rep in the file for this part (nothing to compare with)
            var open = (IModelDoc2)sw.GetOpenDocumentByName(refPath);
            int err = 0, warn = 0;
            var reference = open ?? (IModelDoc2)sw.OpenDoc6(refPath, 1, 1 | 2, "", ref err, ref warn);   // silent, read-only
            var refPart = reference as IPartDoc;
            if (refPart == null) return;
            JObj rep;
            try
            {
                var refBodies = ((object[])refPart.GetBodies2(0, false) ?? new object[0]).Cast<IBody2>().ToList();
                var bodies = ((object[])part.GetBodies2(0, false) ?? new object[0]).Cast<IBody2>().ToList();
                rep = Host.FaceCompare(refBodies, bodies);
            }
            finally
            {
                if (open == null) sw.CloseDoc(reference.GetTitle());
            }
            rep.Remove("status");
            rep["document"] = docId + " (faces)";
            rep["check"] = "faces";
            Verification.Add(rep);
        }
    }
}
