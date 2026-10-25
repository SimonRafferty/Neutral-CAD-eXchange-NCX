// Replayer: Insert Part (derived parts) and assemblies.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public partial class Replayer
    {
        static readonly Dictionary<string, int> TransferOptions = new Dictionary<string, int>
        {
            { "solidBodies", 1 }, { "surfaceBodies", 2 }, { "axes", 4 }, { "planes", 8 }, { "sketches", 32 },
            { "unabsorbedSketches", 64 }, { "customProperties", 128 }, { "coordinateSystems", 256 }
        };

        static readonly Dictionary<string, int> MateTypes = new Dictionary<string, int>
        {
            { "coincident", 0 }, { "concentric", 1 }, { "perpendicular", 2 }, { "parallel", 3 }, { "tangent", 4 },
            { "distance", 5 }, { "angle", 6 }, { "lock", 16 }, { "gear", 10 }, { "screw", 17 }, { "cam", 9 }, { "rackPinion", 13 }
        };

        // mates that only couple motion: without them every component is still where the file places it
        static readonly HashSet<string> CouplingMates = new HashSet<string> { "gear", "screw", "rackPinion", "cam" };

        string ReplayInsertPart(JObj nf, JObj def)
        {
            string src;
            if (!savedByDoc.TryGetValue(def.Str("source"), out src)) return "source document " + def.Str("source") + " was not rebuilt";
            int options = def.Arr("transfer").Select(t => { int v; return TransferOptions.TryGetValue((string)t, out v) ? v : 0; }).Sum();
            int n0 = FeatureCount();
            part.InsertPart2(src, options);
            if (FeatureCount() == n0) return "insert part was not created";
            IFeature stock = null;   // the new Insert Part: the last one
            for (var f = (IFeature)doc.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
                if (f.GetTypeName2() == "Stock") stock = f;
            if (stock != null) { Name(stock, nf.Str("name")); features[nf.Str("id")] = stock; }
            // the transferred items carry the source title as suffix: here the rebuilt file's title, which need not be
            // the title the source document had (a file renamed after the insert keeps the old suffix). Found by name
            // first, else as the Insert Part's sub-feature "<source item>-<anything>", and given the source's name.
            var srcNames = new Dictionary<string, string>();   // source feature id -> name
            var srcDoc = Model.Arr("documents").Cast<JObj>().FirstOrDefault(d => d.Str("id") == def.Str("source"));
            foreach (JObj sf in srcDoc?.Arr("features") ?? new List<object>()) srcNames[sf.Str("id")] = sf.Str("name");
            var subs = new List<IFeature>();
            for (var s = (IFeature)stock?.GetFirstSubFeature(); s != null; s = (IFeature)s.GetNextSubFeature()) subs.Add(s);
            foreach (JObj it in def.Arr("items"))
            {
                var f = (IFeature)part.FeatureByName(it.Str("name"));
                string srcName;
                if (f == null && srcNames.TryGetValue(it.Str("source") ?? "", out srcName))
                {
                    f = subs.FirstOrDefault(s => s.Name.StartsWith(srcName + "-"));
                    if (f != null) Name(f, it.Str("name"));
                }
                if (f != null) transferredItems[def.Str("source") + "|" + it.Str("source")] = f;
                if (f != null && it.Bool("hidden") && f.GetTypeName2() == "ProfileFeature") Hide(f);
            }
            int missing = def.Arr("items").Count - transferredItems.Count(kv => kv.Key.StartsWith(def.Str("source") + "|"));
            return missing > 0 ? missing + " transferred item(s) not found by name" : null;
        }

        /// <summary>A new part mirrored from a rebuilt source about one of its origin planes (Mirror Part), or null.</summary>
        IModelDoc2 MirroredDocument(JObj def)
        {
            string path;
            if (!savedByDoc.TryGetValue(def.Str("source"), out path)) return null;
            var src = (IModelDoc2)sw.GetOpenDocumentByName(path);
            int err = 0, warn = 0;
            if (src == null) src = (IModelDoc2)sw.OpenDoc6(path, 1, 1, "", ref err, ref warn);
            if (src == null) return null;
            // (no ActivateDoc: activating documents during an import lets SolidWorks 2018's FeatureWorks prompt to
            // recognise features in the imported reference, a modal box that stalls the import)
            int index = Array.IndexOf(new[] { "front", "top", "right" }, def.Obj("plane")?.Str("plane"));
            int planes = 0;
            IFeature plane = null;
            for (var f = (IFeature)src.FirstFeature(); f != null && plane == null; f = (IFeature)f.GetNextFeature())
                if (f.GetTypeName2() == "RefPlane" && planes++ == index) plane = f;
            if (plane == null) return null;
            src.ClearSelection2(true);
            plane.Select2(false, 0);
            int options = def.Arr("transfer").Select(x => { int v; return TransferOptions.TryGetValue((string)x, out v) ? v : 0; }).Sum();
            ModelDoc2 result;
            ((IPartDoc)src).MirrorPart2(false, options == 0 ? 1 : options, out result);
            var made = result as IModelDoc2;
            if (made != null) { created.Add(made); quiet?.Add(made); }
            return made;
        }

        string ReplayMirrorPart(JObj nf)
        {
            var f = FindFeature("MirrorStock", x => !Registered(x));
            if (f == null) return "the mirrored part could not be made from its source";
            Name(f, nf.Str("name"));
            features[nf.Str("id")] = f;
            return null;
        }

        // ------------------------------------------------------------------------------ assembly
        /// <summary>Neutral row-major 4x4 (mm) -> SolidWorks MathTransform array (inverse of Exporter.ToMatrix).</summary>
        static double[] ToArray(double[] m)
        {
            return new[] { m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10], U.M(m[3]), U.M(m[7]), U.M(m[11]), 1, 0, 0, 0 };
        }

        IModelDoc2 ReplayAssembly(JObj d, JObj fingerprint)
        {
            docId = d.Str("id");
            string template = sw.GetUserPreferenceStringValue(9);   // swDefaultTemplateAssembly
            var adoc = (IModelDoc2)sw.NewDocument(template, 0, 0, 0);
            if (adoc == null) throw new InvalidOperationException("cannot create an assembly from " + template);
            created.Add(adoc);
            quiet?.Add(adoc);
            var asm = (IAssemblyDoc)adoc;
            var mu = (IMathUtility)sw.GetMathUtility();
            var comps = new Dictionary<string, IComponent2>();
            assemblyFeatures.Clear();
            assemblySketchMaps.Clear();
            // SolidWorks numbers instances of a document in the order they are added ("Block-1", "Block-2"): add them
            // in the source's instance order so each comes back under its own name
            Func<JObj, int> instance = c =>
            {
                var mm = System.Text.RegularExpressions.Regex.Match(c.Str("name") ?? "", @"-(\d+)$");
                return mm.Success ? int.Parse(mm.Groups[1].Value) : 0;
            };
            // components a pattern makes are left to the pattern (if it rebuilds; else they are added after all)
            var patterns = d.Arr("features").Cast<JObj>().Where(f => (f.Str("type") == "componentPattern" || f.Str("type") == "mirrorComponents") &&
                                                                     f.Str("status") != "opaque").ToList();
            var patternIds = new HashSet<string>(patterns.Select(f => f.Str("id")));
            Func<JObj, bool> byPattern = c => patternIds.Contains(c.Str("pattern") ?? "");
            // plain components and patterns in the order SolidWorks made them, which their instance numbers show: a
            // component added after a pattern of its part is numbered after the pattern's instances ("Tab-7" after
            // "Tab-2".."Tab-6"), so it must be added after that pattern to get its number back
            var steps = new List<Tuple<int, int, JObj, bool>>();   // (instance number, source order, item, is pattern)
            int order = 0;
            foreach (JObj c in d.Arr("components").Cast<JObj>().Where(c => !byPattern(c)))
                steps.Add(Tuple.Create(instance(c), order++, c, false));
            foreach (var pf in patterns)
            {
                var made = d.Arr("components").Cast<JObj>().Where(c => c.Str("pattern") == pf.Str("id")).Select(instance).DefaultIfEmpty(0);
                steps.Add(Tuple.Create(made.Min(), order++, pf, true));
            }
            bool rebuilt = false;
            foreach (var step in steps.OrderBy(s => s.Item1).ThenBy(s => s.Item4).ThenBy(s => s.Item2))
            {
                if (!step.Item4) { AddPlainComponent(adoc, asm, mu, step.Item3, comps); rebuilt = false; continue; }
                if (!rebuilt) { adoc.EditRebuild3(); rebuilt = true; }
                var pf = step.Item3;
                var instances = d.Arr("components").Cast<JObj>().Where(c => c.Str("pattern") == pf.Str("id")).ToList();
                string why = pf.Str("type") == "mirrorComponents" ? ReplayMirrorComponents(adoc, comps, pf, instances)
                                                                    : ReplayComponentPattern(adoc, comps, pf, instances);
                Report.Add(docId, pf.Str("name"), why != null ? "fallback" : pf.Str("status") == "approximate" ? "approximate" : "rebuilt",
                           why ?? pf.Str("note"));
                if (why == null) continue;
                // the pattern did not rebuild: its instances are placed as plain components
                foreach (var c in instances.OrderBy(instance)) AddPlainComponent(adoc, asm, mu, c, comps);
            }
            adoc.EditRebuild3();
            foreach (JObj pf in d.Arr("features").Cast<JObj>().Where(f => f.Str("type") == "plane" && f.Str("status") != "opaque"))
            {
                string why = ReplayAssemblyPlane(adoc, comps, pf);
                Report.Add(docId, pf.Str("name"), why == null ? "rebuilt" : "failed", why);
            }
            foreach (JObj f in d.Arr("features").Cast<JObj>().Where(f => f.Str("status") == "opaque"))
                Report.Add(docId, f.Str("name"), f.Bool("noGeometry") ? "note" : "fallback", f.Str("reason"));

            var placed = d.Arr("components").Cast<JObj>().Where(c => c.Vec("transform") != null && c.Str("id") != null)
                          .GroupBy(c => c.Str("id")).ToDictionary(g => g.Key, g => g.First());
            foreach (JObj m in d.Arr("mates"))
            {
                if (m.Str("status") == "opaque") { Report.Add(docId, m.Str("name"), "fallback", m.Str("reason")); continue; }
                string note = null;
                if (m.Obj("suppressed")?.Bool("cfg:active") == true)
                {
                    string sw2 = SuppressedMate(adoc, asm, comps, placed, m, ref note);
                    Report.Add(docId, m.Str("name"), sw2 != null ? "approximate" : note != null ? "approximate" : "rebuilt",
                               sw2 != null ? "suppressed mate not made (" + sw2 + ")" : note);
                    continue;
                }
                string why = PlacedMate(adoc, asm, comps, placed, m, ref note);
                if (why != null && CouplingMates.Contains(m.Str("type")))
                {
                    // something simpler (FORMAT-SPEC 5, "Mates with no counterpart"): the parts stay where they are
                    Report.Add(docId, m.Str("name"), "approximate", m.Str("type") + " mate not made (" + why + "): components placed, not coupled");
                    continue;
                }
                Report.Add(docId, m.Str("name"), why != null ? "failed" : note != null ? "approximate" : "rebuilt", why ?? note);
            }
            // assembly sketches and cuts, in tree order, on the components as placed
            foreach (JObj af in d.Arr("features").Cast<JObj>().Where(f => (f.Str("type") == "sketch" || f.Str("type") == "cut") && f.Str("status") != "opaque"))
            {
                string note = null;
                string why;
                try { why = ReplayAssemblyLevel(adoc, comps, af, ref note); }
                catch (Exception e) { why = e.Message; }
                Report.Add(docId, af.Str("name"), why != null ? "failed" : note != null || af.Str("status") == "approximate" ? "approximate" : "rebuilt",
                           why ?? note ?? af.Str("note"));
            }
            adoc.EditRebuild3();
            ReplayPropertiesAndMaterials(adoc, d);
            ReplayAppearances(adoc, d, comps);
            VerifyAssembly(d, adoc, comps, fingerprint);
            return adoc;
        }

        /// <summary>A mate that keeps every component where the source has it. Which side a face normal or a distance is
        /// taken from can be read differently by the target, so a mate made with the source's alignment and flip flags
        /// may turn a part over. The source's components sit where its mates hold them, so a mate made right moves
        /// nothing: one that moves a component is made again with its flags turned, and the variant that leaves every
        /// component in place is kept (geometry wins).</summary>
        string PlacedMate(IModelDoc2 adoc, IAssemblyDoc asm, Dictionary<string, IComponent2> comps, Dictionary<string, JObj> placed,
                          JObj m, ref string note)
        {
            var before = OffPlacement(adoc, comps, placed);   // already elsewhere (an earlier mate): not this one's doing
            string why = ReplayMate(adoc, asm, comps, m, ref note);
            if (why != null) return why;
            Func<string> movedNow = () => OffPlacement(adoc, comps, placed).FirstOrDefault(n => !before.Contains(n));
            string moved = movedNow();
            if (moved == null) return null;
            var variants = new List<JObj>();
            Func<bool, bool, JObj> variant = (flip, turnAlign) =>
            {
                var v = (JObj)Json.Parse(Json.Write(m, false));
                if (flip) v["flipped"] = !m.Bool("flipped");
                if (turnAlign) v["alignment"] = m.Str("alignment") == "antiAligned" ? "aligned" : "antiAligned";
                return v;
            };
            bool canAlign = m.Str("alignment") == "aligned" || m.Str("alignment") == "antiAligned";
            variants.Add(variant(true, false));
            if (canAlign) { variants.Add(variant(false, true)); variants.Add(variant(true, true)); }
            foreach (var v in variants)
            {
                if (!DeleteMate(adoc, m.Str("name"))) return null;   // cannot try others: keep it as made
                string n2 = null;
                if (ReplayMate(adoc, asm, comps, v, ref n2) != null) continue;
                if (movedNow() == null)
                {
                    note = n2 ?? note;
                    return null;
                }
            }
            // none keeps the placement: back to the source's flags
            DeleteMate(adoc, m.Str("name"));
            string n3 = null;
            why = ReplayMate(adoc, asm, comps, m, ref n3);
            if (why == null) note = (n3 == null ? "" : n3 + "; ") + "moves " + moved + " from where the source has it";
            return why;
        }

        /// <summary>A mate suppressed in the source: made, then suppressed. While it existed it may have moved
        /// components; they go back to where the source has them (the mate holds nothing once suppressed).</summary>
        string SuppressedMate(IModelDoc2 adoc, IAssemblyDoc asm, Dictionary<string, IComponent2> comps, Dictionary<string, JObj> placed,
                              JObj m, ref string note)
        {
            string why = ReplayMate(adoc, asm, comps, m, ref note);
            if (why != null) return why;
            var mate = LastMate(adoc);
            if (mate == null || mate.Name != m.Str("name")) return "made, but not found to suppress";
            mate.SetSuppression2(0, 1, null);   // swSuppressFeature, swThisConfiguration
            var mu = (IMathUtility)sw.GetMathUtility();
            var byName = comps.Values.ToDictionary(c => c.Name2, c => c);
            var idOf = comps.ToDictionary(kv => kv.Value.Name2, kv => kv.Key);
            foreach (var name in OffPlacement(adoc, comps, placed))
                byName[name].Transform2 = (MathTransform)mu.CreateTransform(ToArray(placed[idOf[name]].Vec("transform")));
            adoc.EditRebuild3();
            return null;
        }

        /// <summary>The components not at their recorded placement (0.01 mm, 1e-5 in rotation), after a rebuild.</summary>
        static HashSet<string> OffPlacement(IModelDoc2 adoc, Dictionary<string, IComponent2> comps, Dictionary<string, JObj> placed)
        {
            var off = new HashSet<string>();
            adoc.EditRebuild3();
            foreach (var kv in comps)
            {
                JObj c;
                if (!placed.TryGetValue(kv.Key, out c)) continue;
                double[] want = ToArray(c.Vec("transform"));
                double[] have;
                try { have = (double[])kv.Value.Transform2?.ArrayData; }
                catch (System.Runtime.InteropServices.COMException) { continue; }
                if (have == null) continue;
                bool moved = false;
                for (int i = 0; i < 9 && !moved; i++) moved = Math.Abs(have[i] - want[i]) > 1e-5;
                for (int i = 9; i < 12 && !moved; i++) moved = Math.Abs(have[i] - want[i]) > 1e-5;   // metres
                if (moved) off.Add(kv.Value.Name2);
            }
            return off;
        }

        /// <summary>Delete the mate of this name (the one just made).</summary>
        static bool DeleteMate(IModelDoc2 adoc, string name)
        {
            var mate = LastMate(adoc);
            if (mate == null || mate.Name != name) return false;
            adoc.ClearSelection2(true);
            if (!mate.Select2(false, 0)) return false;
            bool ok = adoc.Extension.DeleteSelection2(0);
            adoc.ClearSelection2(true);
            return ok && (LastMate(adoc)?.Name != name);
        }

        string ReplayMate(IModelDoc2 adoc, IAssemblyDoc asm, Dictionary<string, IComponent2> comps, JObj m, ref string note)
        {
            if (m.Str("type") == "width") return ReplayWidthMate(adoc, asm, comps, m);
            if (m.Str("type") == "hinge") return ReplayHingeMate(adoc, asm, comps, m, ref note);
            if (m.Str("type") == "symmetric") return ReplaySymmetricMate(adoc, asm, comps, m);
            if (m.Str("type") == "slot") return ReplaySlotMate(adoc, asm, comps, m);
            if (m.Str("type") == "rackPinion")
            {
                // AddMate5 refuses rack-and-pinion in SolidWorks 2018 (every entity order, distance and mark tried)
                string viaData = ReplayRackPinionMate(adoc, asm, comps, m);
                return viaData == "no mate data" ? "this SolidWorks' API cannot make a rack-and-pinion mate (no mate data before 2019)" : viaData;
            }
            // AddMate5 crashed SolidWorks 2026 on a cam-follower mate, and cam mate data is refused: not attempted
            if (m.Str("type") == "cam") return "cam-follower mates cannot be made through the SolidWorks API";
            if (m.Str("type") == "screw" && Math.Abs((m.Obj("params")?.Num("pitch", 1) ?? 1) - 1) > 1e-9)
                note = "screw pitch " + m.Obj("params").Num("pitch") + " mm per turn not settable through the SolidWorks API: made with the default 1 mm";
            int type;
            if (!MateTypes.TryGetValue(m.Str("type"), out type)) return "mate type " + m.Str("type") + " not supported";
            adoc.ClearSelection2(true);
            bool append = false;
            foreach (JObj r in m.Arr("refs"))
            {
                string why = SelectAssemblyRef(adoc, comps, r, append, 0);
                if (why != null) return why;
                append = true;
            }
            return AddMate(adoc, asm, m, type);
        }

        /// <summary>Select one assembly reference {component, path?, ref} - or {ref}, the assembly's own plane or
        /// origin - with a selection mark (mates use 0, patterns their own marks).</summary>
        string SelectAssemblyRef(IModelDoc2 adoc, Dictionary<string, IComponent2> comps, JObj r, bool append, int mark)
        {
            var sd = (SelectData)((ISelectionMgr)adoc.SelectionManager).CreateSelectData();
            sd.Mark = mark;
            if (!r.Has("component"))
                return SelectAssemblyDatum(adoc, r.Obj("ref"), append, sd);
            IComponent2 comp;
            if (!comps.TryGetValue(r.Str("component"), out comp)) return "component " + r.Str("component") + " missing";
            // a nested component: follow the path down through the sub-assemblies, each step by the component
            // rebuilt for that source instance (its name here may differ from the source's)
            string level = ComponentDocument(docId, r.Str("component"));
            foreach (string cid in r.Arr("path").Cast<string>())
            {
                string name = cid.StartsWith("c:") ? cid.Substring(2) : cid;
                Dictionary<string, string> names;
                string built;
                if (level != null && builtNames.TryGetValue(level, out names) && names.TryGetValue(cid, out built)) name = built;
                level = level == null ? null : ComponentDocument(level, cid);
                var children = ((object[])comp.GetChildren() ?? new object[0]).Cast<IComponent2>().ToList();
                var child = children.FirstOrDefault(k => k.Name2 == name || k.Name2.EndsWith("/" + name));
                if (child == null)
                {
                    // the rebuilt sub-assembly numbers its instances from 1 ("Cheek-1"), the source may not ("Cheek-2"
                    // when instance 1 was deleted): the same part's instances, in their numbering order
                    var m = System.Text.RegularExpressions.Regex.Match(name, @"^(.*)-(\d+)$");
                    if (m.Success)
                    {
                        Func<IComponent2, string> leaf = k => k.Name2.Split('/').Last();
                        var same = children.Where(k => System.Text.RegularExpressions.Regex.Replace(leaf(k), @"-\d+$", "") == m.Groups[1].Value).ToList();
                        if (same.Count == 1) child = same[0];   // the only instance of that part: unambiguous
                    }
                }
                if (child == null) return "nested component " + name + " missing under " + comp.Name2;
                comp = child;
            }
            var rf = r.Obj("ref");
            string kind = rf.Str("kind");
            if (kind == "origin")
            {
                // the origin's point (a mate takes the point, not the origin feature)
                IFeature origin = null;
                for (var of = (IFeature)comp.FirstFeature(); of != null && origin == null; of = (IFeature)of.GetNextFeature())
                    if (of.GetTypeName2() == "OriginProfileFeature") origin = of;
                var opts = (object[])((origin?.GetSpecificFeature2() as ISketch)?.GetSketchPoints2()) ?? new object[0];
                if (opts.Length == 0 || !((ISketchPoint)opts[0]).Select4(append, sd)) return "origin of " + comp.Name2 + " not selectable";
                return null;
            }
            if (kind == "sketchEntity")
            {
                // the rebuilt part's entity, then its instance in this component
                Dictionary<string, SketchMap> maps;
                SketchMap m2;
                object partEnt = null;
                if (sketchMapsByDoc.TryGetValue(rf.Str("document"), out maps) && maps.TryGetValue(rf.Str("feature"), out m2))
                    partEnt = Resolve(m2, rf.Str("id"));
                var inComp = partEnt == null ? null : comp.GetCorresponding(partEnt);
                bool ok = inComp is ISketchSegment ? ((ISketchSegment)inComp).Select4(append, sd)
                        : inComp is ISketchPoint && ((ISketchPoint)inComp).Select4(append, sd);
                return ok ? null : "sketch entity " + rf.Str("id") + " not found on " + comp.Name2;
            }
            if (kind == "originPlane" || kind == "feature")
            {
                var feat = ComponentFeature(comp, rf);
                if (feat == null) return kind + " not found on " + comp.Name2;
                return feat.Select2(append, mark) ? null : "could not select " + feat.Name + " on " + comp.Name2;
            }
            // component bodies are in the part frame, like the exported descriptor
            var bodies = ((object[])comp.GetBodies2(0) ?? new object[0]).Cast<IBody2>().ToList();
            List<object> ents;
            var res = Host.Resolve(bodies, rf, out ents);
            if (res.Str("result") != "resolved") return "reference on " + comp.Name2 + " " + res.Str("result");
            foreach (var e in ents)
            {
                if (!((IEntity)e).Select4(append, sd)) return "could not select an entity on " + comp.Name2;
                append = true;
            }
            return null;
        }

        string AddMate(IModelDoc2 adoc, IAssemblyDoc asm, JObj m, int type)
        {
            int align = m.Str("alignment") == "antiAligned" ? 1 : m.Str("alignment") == "closest" ? 2 : 0;
            var p = m.Obj("params") ?? new JObj();
            int err = 0;
            double dist = U.M(p.Num("distance"));
            // a limit mate carries the range; otherwise both limits are the value itself
            bool limits = p.Has("max") && p.Has("min");
            bool angular = m.Str("type") == "angle";
            double hiD = limits && !angular ? U.M(p.Num("max")) : dist, loD = limits && !angular ? U.M(p.Num("min")) : dist;
            double ang = U.Rad(p.Num("angle"));
            double hiA = limits && angular ? U.Rad(p.Num("max")) : 0, loA = limits && angular ? U.Rad(p.Num("min")) : 0;
            // a gear's ratio: AddMate5 takes it the other way up from how the mate data reports it
            var ratio = p.Arr("ratio");
            double num = ratio.Count == 2 ? Convert.ToDouble(ratio[1]) : 0, den = ratio.Count == 2 ? Convert.ToDouble(ratio[0]) : 0;
            bool flip = m.Str("type") == "gear" || m.Str("type") == "rackPinion" ? p.Bool("reverse") : m.Bool("flipped");   // these turn by their own flag
            if (m.Str("type") == "rackPinion") dist = hiD = loD = U.M(p.Num("pitchDiameter"));   // AddMate5 takes the pitch diameter as its distance
            var mate = asm.AddMate5(type, align, flip, dist, hiD, loD, num, den,
                                    ang, hiA, loA, false, false, 0, out err);
            adoc.ClearSelection2(true);
            if (mate == null) return "mate rejected (error " + err + ")";
            Name(LastMate(adoc), m.Str("name"));
            return null;
        }

        /// <summary>The entities of the refs with this role, resolved by selecting each and reading the selection back.</summary>
        string RoleEntities(IModelDoc2 adoc, Dictionary<string, IComponent2> comps, JObj m, string role, out DispatchWrapper[] ents)
        {
            var list = new List<DispatchWrapper>();
            ents = null;
            foreach (JObj r in m.Arr("refs").Cast<JObj>().Where(x => x.Str("role") == role))
            {
                adoc.ClearSelection2(true);
                string why = SelectAssemblyRef(adoc, comps, r, false, 0);
                if (why != null) return role + ": " + why;
                list.Add(new DispatchWrapper(((ISelectionMgr)adoc.SelectionManager).GetSelectedObject6(1, -1)));
            }
            adoc.ClearSelection2(true);
            ents = list.ToArray();
            return null;
        }

        /// <summary>A width mate, through its mate data (AddMate5 refuses it; the data is in the 2018 API).</summary>
        string ReplayWidthMate(IModelDoc2 adoc, IAssemblyDoc asm, Dictionary<string, IComponent2> comps, JObj m)
        {
            DispatchWrapper[] width, tab;
            string why = RoleEntities(adoc, comps, m, "width", out width);
            if (why == null) why = RoleEntities(adoc, comps, m, "tab", out tab); else tab = null;
            if (why != null) return why;
            var p = m.Obj("params") ?? new JObj();
            var md = (IWidthMateFeatureData)asm.CreateMateData(11);   // swMateWIDTH
            md.WidthSelection = width;
            md.TabSelection = tab;
            int wc = Neutral.Code("widthConstraint", p["constraint"]);
            md.ConstraintType = wc;
            if (wc == 2) md.DistanceFromEnd = U.M(p.Num("distance"));
            if (wc == 3) md.PercentDistanceFromEnd = p.Num("percent");
            md.FlipDimension = p.Bool("flip");
            if (asm.CreateMate(md) == null) return "width mate rejected";
            Name(LastMate(adoc), m.Str("name"));
            return null;
        }

        /// <summary>A symmetric mate, through its mate data (in the 2018 API too).</summary>
        string ReplaySymmetricMate(IModelDoc2 adoc, IAssemblyDoc asm, Dictionary<string, IComponent2> comps, JObj m)
        {
            DispatchWrapper[] ents, plane;
            string why = RoleEntities(adoc, comps, m, "entity", out ents);
            if (why == null) why = RoleEntities(adoc, comps, m, "plane", out plane); else plane = null;
            if (why != null) return why;
            if (plane.Length != 1) return "symmetric mate needs one plane";
            var md = (ISymmetricMateFeatureData)asm.CreateMateData(8);   // swMateSYMMETRIC
            md.SymmetryPlane = plane[0].WrappedObject;
            md.EntitiesToMate = ents;
            md.MateAlignment = (int)(m.Obj("params")?.Num("alignment") ?? 0);
            if (asm.CreateMate(md) == null) return "symmetric mate rejected";
            Name(LastMate(adoc), m.Str("name"));
            return null;
        }

        /// <summary>A slot mate: through its mate data where this SolidWorks has it (2019 on), else AddMate5.</summary>
        string ReplaySlotMate(IModelDoc2 adoc, IAssemblyDoc asm, Dictionary<string, IComponent2> comps, JObj m)
        {
            var refs = m.Arr("refs").Cast<JObj>().ToList();
            var ents = new List<DispatchWrapper>();
            foreach (var r in refs)
            {
                adoc.ClearSelection2(true);
                string why = SelectAssemblyRef(adoc, comps, r, false, 0);
                if (why != null) return why;
                ents.Add(new DispatchWrapper(((ISelectionMgr)adoc.SelectionManager).GetSelectedObject6(1, -1)));
            }
            adoc.ClearSelection2(true);
            var p = m.Obj("params") ?? new JObj();
            try
            {
                object md = asm.CreateMateData(21);   // swMateSLOT
                if (md != null)
                {
                    var bind = System.Reflection.BindingFlags.SetProperty;
                    Action<string, object> set = (name, v) => md.GetType().InvokeMember(name, bind, null, md, new[] { v });
                    set("EntitiesToMate", ents.ToArray());
                    int sc = Neutral.Code("slotConstraint", p["constraint"]);
                    set("Constraint", sc);
                    if (sc == 2) set("Distance", U.M(p.Num("distance")));
                    if (sc == 3) set("Percent", p.Num("percent"));
                    set("FlipDirection", p.Bool("flip"));
                    if (asm.CreateMate(md) != null)
                    {
                        Name(LastMate(adoc), m.Str("name"));
                        return null;
                    }
                }
            }
            catch (Exception e) when (e is System.Reflection.TargetInvocationException || e is MissingMethodException ||
                                      e is COMException) { }
            // no slot mate data (SolidWorks 2018): AddMate5, which makes a free slot mate
            for (int i = 0; i < ents.Count; i++) ((IEntity)ents[i].WrappedObject).Select4(i > 0, null);
            int err;
            var mate = asm.AddMate5(21, 2, false, 0, 0, 0, 0, 0, 0, 0, 0, false, false, Neutral.Code("slotConstraint", p["constraint"]), out err);
            adoc.ClearSelection2(true);
            if (mate == null) return "slot mate rejected (error " + err + ")";
            Name(LastMate(adoc), m.Str("name"));
            return null;
        }

        /// <summary>A hinge through its mate data where this SolidWorks has it (2019 on); otherwise its functional
        /// equivalent, a concentric and a coincident mate.</summary>
        string ReplayHingeMate(IModelDoc2 adoc, IAssemblyDoc asm, Dictionary<string, IComponent2> comps, JObj m, ref string note)
        {
            DispatchWrapper[] conc, coin;
            string why = RoleEntities(adoc, comps, m, "concentric", out conc);
            if (why == null) why = RoleEntities(adoc, comps, m, "coincident", out coin); else coin = null;
            if (why != null) return why;
            var p = m.Obj("params");
            try
            {
                object md = asm.CreateMateData(22);   // swMateHINGE
                if (md != null)
                {
                    var bind = System.Reflection.BindingFlags.SetProperty;
                    md.GetType().InvokeMember("EntitiesToMate", bind, null, md, new object[] { 0, conc });
                    md.GetType().InvokeMember("EntitiesToMate", bind, null, md, new object[] { 1, coin });
                    if (p != null && p.Has("angle"))
                    {
                        md.GetType().InvokeMember("AngleSelection", bind, null, md, new object[] { true });
                        md.GetType().InvokeMember("Angle", bind, null, md, new object[] { U.Rad(p.Num("angle")) });
                        md.GetType().InvokeMember("MaxVal", bind, null, md, new object[] { U.Rad(p.Num("max")) });
                        md.GetType().InvokeMember("MinVal", bind, null, md, new object[] { U.Rad(p.Num("min")) });
                    }
                    if (asm.CreateMate(md) != null)
                    {
                        Name(LastMate(adoc), m.Str("name"));
                        return null;
                    }
                }
            }
            catch (Exception e) when (e is System.Reflection.TargetInvocationException || e is MissingMethodException ||
                                      e is System.Runtime.InteropServices.COMException) { }
            // no hinge in this SolidWorks: the same freedom from a concentric and a coincident mate
            int err;
            foreach (var pair in new[] { new { ents = conc, type = 1, suffix = "" }, new { ents = coin, type = 0, suffix = "-Coincident" } })
            {
                adoc.ClearSelection2(true);
                for (int i = 0; i < pair.ents.Length; i++)
                    ((IEntity)pair.ents[i].WrappedObject).Select4(i > 0, null);
                if (asm.AddMate5(pair.type, 2, false, 0, 0, 0, 0, 0, 0, 0, 0, false, false, 0, out err) == null)
                    return "hinge equivalent: " + (pair.type == 1 ? "concentric" : "coincident") + " mate rejected (error " + err + ")";
                Name(LastMate(adoc), m.Str("name") + pair.suffix);
            }
            adoc.ClearSelection2(true);
            note = "no hinge mate in this SolidWorks: rebuilt as a concentric and a coincident mate" +
                   (p != null && p.Has("angle") ? " (its angle limits are not carried)" : "");
            return null;
        }

        /// <summary>A rack-and-pinion mate through its mate data (2019 on): the rack's entity, the pinion's, and the
        /// pinion's pitch diameter or the rack travel per turn. Returns "no mate data" where this SolidWorks has none.</summary>
        string ReplayRackPinionMate(IModelDoc2 adoc, IAssemblyDoc asm, Dictionary<string, IComponent2> comps, JObj m)
        {
            DispatchWrapper[] rack, pinion;
            string why = RoleEntities(adoc, comps, m, "rack", out rack);
            if (why == null) why = RoleEntities(adoc, comps, m, "pinion", out pinion); else pinion = null;
            if (why != null) return why;
            if (rack.Length != 1 || pinion.Length != 1) return "rack-and-pinion needs one rack and one pinion reference";
            var p = m.Obj("params") ?? new JObj();
            try
            {
                object md = asm.CreateMateData(13);   // swMateRACKPINION
                if (md == null) return "no mate data";
                var bind = System.Reflection.BindingFlags.SetProperty;
                // one entity per side (VT_DISPATCH), not an array as for a hinge
                md.GetType().InvokeMember("EntitiesToMate", bind, null, md, new object[] { 0, rack[0] });
                md.GetType().InvokeMember("EntitiesToMate", bind, null, md, new object[] { 1, pinion[0] });
                bool byTravel = p.Bool("byTravel");
                md.GetType().InvokeMember("DiameterType", bind, null, md, new object[] { byTravel ? 1 : 0 });
                md.GetType().InvokeMember("DiameterVal", bind, null, md, new object[] { U.M(byTravel ? p.Num("travelPerTurn") : p.Num("pitchDiameter")) });
                // the flag set at creation reads back inverted (made unset, a mate reports Reverse true)
                md.GetType().InvokeMember("Reverse", bind, null, md, new object[] { !p.Bool("reverse") });
                if (asm.CreateMate(md) == null) return "rack-and-pinion mate rejected";
                Name(LastMate(adoc), m.Str("name"));
                return null;
            }
            catch (Exception e) when (e is System.Reflection.TargetInvocationException || e is MissingMethodException ||
                                      e is COMException)
            {
                return "no mate data";
            }
        }

        // assembly-level features rebuilt so far (planes), by payload id
        readonly Dictionary<string, IFeature> assemblyFeatures = new Dictionary<string, IFeature>();
        // assembly document id -> source component id -> the name its rebuilt component has (SolidWorks may refuse a
        // rename, and numbers instances from 1: "IGUS-2" of the source can be "IGUS-1" here)
        readonly Dictionary<string, Dictionary<string, string>> builtNames = new Dictionary<string, Dictionary<string, string>>();

        /// <summary>The document a component of an assembly document places, from the payload.</summary>
        string ComponentDocument(string asmDocId, string componentId)
        {
            var ad = Model?.Arr("documents").Cast<JObj>().FirstOrDefault(x => x.Str("id") == asmDocId);
            return ad?.Arr("components").Cast<JObj>().FirstOrDefault(c => c.Str("id") == componentId)?.Str("document");
        }

        /// <summary>A reference plane of the assembly, from assembly references, checked against the recorded plane.</summary>
        string ReplayAssemblyPlane(IModelDoc2 adoc, Dictionary<string, IComponent2> comps, JObj nf)
        {
            var def = nf.Obj("def");
            var refs = def.Arr("references").Cast<JObj>().ToList();
            if (refs.Count == 0 || refs.Count > 3) return "plane needs 1-3 references";
            adoc.ClearSelection2(true);
            var c = new int[3];
            var v = new double[3];
            for (int i = 0; i < refs.Count; i++)
            {
                string why = SelectAssemblyRef(adoc, comps, refs[i].Obj("ref"), i > 0, i);   // marks 0, 1, 2
                if (why != null) return "plane reference " + i + ": " + why;
                c[i] = Neutral.PlaneConstraintCode(refs[i]);
                v[i] = (c[i] & 16) != 0 ? U.Rad(refs[i].Num("value")) : U.M(refs[i].Num("value"));
            }
            var feat = (IFeature)((IFeatureManager)adoc.FeatureManager).InsertRefPlane(c[0], v[0], c[1], v[1], c[2], v[2]);
            adoc.ClearSelection2(true);
            if (feat == null) return "plane was not created";
            Name(feat, nf.Str("name"));
            assemblyFeatures[nf.Str("id")] = feat;
            var a = (double[])((IMathTransform)((IRefPlane)feat.GetSpecificFeature2()).Transform).ArrayData;
            var want = def.Vec("origin");
            var wantN = def.Vec("normal");
            if (want != null && wantN != null)
            {
                double off = Math.Abs(V.Dot(V.Sub(U.Mm(new[] { a[9], a[10], a[11] }), want), V.Unit(wantN)));
                double ang = 1 - Math.Abs(V.Dot(V.Unit(new[] { a[6], a[7], a[8] }), V.Unit(wantN)));
                if (off > 1e-3 || ang > 1e-9) return "plane differs from the source (offset " + off.ToString("G4") + " mm)";
            }
            return null;
        }

        /// <summary>Select an origin plane (by position in the tree), a rebuilt assembly plane, or the origin point of
        /// the assembly itself.</summary>
        string SelectAssemblyDatum(IModelDoc2 adoc, JObj rf, bool append, SelectData sd)
        {
            string kind = rf.Str("kind");
            if (kind == "feature")
            {
                IFeature af;
                if (!assemblyFeatures.TryGetValue(rf.Str("id"), out af)) return "assembly feature " + rf.Str("id") + " was not rebuilt";
                return af.Select2(append, sd.Mark) ? null : "could not select " + af.Name;
            }
            if (kind == "originPlane")
            {
                int index = Array.IndexOf(new[] { "front", "top", "right" }, rf.Str("plane"));
                int planes = 0;
                for (var f = (IFeature)adoc.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
                    if (f.GetTypeName2() == "RefPlane" && planes++ == index)
                        return f.Select2(append, sd.Mark) ? null : "could not select the assembly's " + rf.Str("plane") + " plane";
                return "the assembly has no " + rf.Str("plane") + " plane";
            }
            if (kind == "origin")
            {
                for (var f = (IFeature)adoc.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
                {
                    if (f.GetTypeName2() != "OriginProfileFeature") continue;
                    var pts = (object[])((f.GetSpecificFeature2() as ISketch)?.GetSketchPoints2()) ?? new object[0];
                    return pts.Length > 0 && ((ISketchPoint)pts[0]).Select4(append, sd) ? null : "the assembly's origin is not selectable";
                }
                return "the assembly has no origin";
            }
            return "assembly-level reference " + kind + " not supported";
        }

        /// <summary>Add one component where the payload places it, fixed or not as in the source.</summary>
        void AddPlainComponent(IModelDoc2 adoc, IAssemblyDoc asm, IMathUtility mu, JObj c, Dictionary<string, IComponent2> comps)
        {
            string path;
            if (!savedByDoc.TryGetValue(c.Str("document"), out path))
            {
                Report.Add(docId, c.Str("name"), "failed", "component document was not rebuilt");
                return;
            }
            var comp = (IComponent2)asm.AddComponent5(path, 0, "", false, "", 0, 0, 0);
            if (comp == null) { Report.Add(docId, c.Str("name"), "failed", "component could not be added"); return; }
            // SolidWorks numbers a document's instances itself and ignores a rename: a source numbered with a gap
            // ("IGUS-2" with no "-1", the first deleted) gets its number back the way it arose - stand-ins take the
            // missing numbers and are deleted again
            Func<string, int> number = n =>
            {
                var mm = System.Text.RegularExpressions.Regex.Match(n ?? "", @"-(\d+)$");
                return mm.Success ? int.Parse(mm.Groups[1].Value) : 0;
            };
            var standIns = new List<IComponent2>();
            while (comp != null && number(comp.Name2) > 0 && number(c.Str("name")) > number(comp.Name2) && standIns.Count < 50)
            {
                standIns.Add(comp);
                comp = (IComponent2)asm.AddComponent5(path, 0, "", false, "", 0, 0, 0);
            }
            if (standIns.Count > 0)
            {
                adoc.ClearSelection2(true);
                bool sel = false;
                foreach (var s in standIns) sel |= s.Select4(sel, null, false);
                if (sel) adoc.Extension.DeleteSelection2(0);
                adoc.ClearSelection2(true);
            }
            if (comp == null) { Report.Add(docId, c.Str("name"), "failed", "component could not be added"); return; }
            if (comp.Name2 != c.Str("name") && c.Str("name") != null)
                try { comp.Name2 = c.Str("name"); }
                catch (System.Runtime.InteropServices.COMException) { }
            comp.Transform2 = (MathTransform)mu.CreateTransform(ToArray(c.Vec("transform")));
            if (c.Bool("flexible"))
            {
                // a sub-assembly solved as flexible in the source (FORMAT-SPEC 5, components)
                // Solving is read-only: set through the selected component's properties (fully resolved, flexible, shown)
                bool flex = false;
                string why = null;
                try
                {
                    adoc.ClearSelection2(true);
                    if (!comp.Select4(false, null, false)) why = "not selectable";
                    else flex = asm.CompConfigProperties5(2, 1, true, true, comp.ReferencedConfiguration, false, false);
                    adoc.ClearSelection2(true);
                }
                catch (Exception e) { why = e.Message; }
                if (!flex || comp.Solving != 1)
                    Report.Add(docId, c.Str("name"), "approximate", "could not be made flexible (" + (why ?? (flex ? "solving " + comp.Solving : "refused")) + "): solved rigid");
            }
            if (c.Bool("fixed") != comp.IsFixed())
            {
                adoc.ClearSelection2(true);
                comp.Select4(false, null, false);
                if (c.Bool("fixed")) asm.FixComponent();
                else asm.UnfixComponent();
                adoc.ClearSelection2(true);
            }
            comps[c.Str("id")] = comp;
            Dictionary<string, string> names;
            if (!builtNames.TryGetValue(docId, out names)) builtNames[docId] = names = new Dictionary<string, string>();
            names[c.Str("id")] = comp.Name2.Split('/').Last();
            Report.Add(docId, c.Str("name"), "rebuilt");
        }

        /// <summary>A linear or circular component pattern. Which way an edge or axis runs can differ between systems
        /// and versions, so the pattern is checked against where the source has its instances, and rebuilt with the
        /// other direction(s) if they landed elsewhere.</summary>
        string ReplayComponentPattern(IModelDoc2 adoc, Dictionary<string, IComponent2> comps, JObj pf, List<JObj> instances)
        {
            var def = pf.Obj("def");
            bool linear = def.Str("kind") == "linear";
            var dir1 = def.Obj("direction");
            var dir2 = def.Obj("direction2");
            var fm = (IFeatureManager)adoc.FeatureManager;
            var asm = (IAssemblyDoc)adoc;
            // reverse flags to try: as exported first, then flipped
            var tries = new List<bool[]>();
            bool r1 = linear ? dir1.Bool("reverse") : def.Bool("reverse"), r2 = dir2 != null && dir2.Bool("reverse");
            tries.Add(new[] { r1, r2 });
            tries.Add(new[] { !r1, r2 });
            if (dir2 != null) { tries.Add(new[] { r1, !r2 }); tries.Add(new[] { !r1, !r2 }); }
            string last = null;
            foreach (var rev in tries)
            {
                adoc.ClearSelection2(true);
                // component patterns: first direction or axis mark 2, second direction mark 4, seeds mark 1
                string why = SelectAssemblyRef(adoc, comps, linear ? dir1.Obj("ref") : def.Obj("axis"), false, 2);
                if (why == null && dir2 != null) why = SelectAssemblyRef(adoc, comps, dir2.Obj("ref"), true, 4);
                foreach (string sid in def.Arr("seeds").Cast<string>())
                {
                    if (why != null) break;
                    IComponent2 seed;
                    if (!comps.TryGetValue(sid, out seed)) { why = "seed " + sid + " missing"; break; }
                    var sd = (SelectData)((ISelectionMgr)adoc.SelectionManager).CreateSelectData();
                    sd.Mark = 1;
                    if (!seed.Select4(true, sd, false)) why = "could not select seed " + seed.Name2;
                }
                if (why != null) return (linear ? "direction/seed: " : "axis/seed: ") + why;
                var before = new HashSet<string>(((object[])asm.GetComponents(true) ?? new object[0]).Cast<IComponent2>().Select(c => c.Name2));
                if (linear)
                    fm.FeatureLinearPattern5((int)dir1.Num("count"), U.M(dir1.Num("spacing")), dir2 == null ? 1 : (int)dir2.Num("count"),
                                             dir2 == null ? 0.01 : U.M(dir2.Num("spacing")),   // unused, but a zero spacing is refused
                                             rev[0], rev[1], "NULL", "NULL", false, false,
                                             false, false, false, false, false, false, false, false, 0, 0, false, false);
                else
                    fm.FeatureCircularPattern5((int)def.Num("count"), U.Rad(def.Num("angle")), rev[0], "NULL", false, false, false, false,
                                               false, false, 0, 0, "NULL", false);
                adoc.ClearSelection2(true);
                IFeature feat = null;
                for (var f = (IFeature)adoc.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
                    if ((f.GetTypeName2() == "LocalLPattern" || f.GetTypeName2() == "LocalCirPattern") && !Registered(f)) feat = f;
                if (feat == null) return "the pattern was not created";
                if (def.Arr("skipped").Count > 0) SkipComponentInstances(adoc, feat, def.Arr("skipped"));
                adoc.EditRebuild3();
                var made = ((object[])asm.GetComponents(true) ?? new object[0]).Cast<IComponent2>().Where(c => !before.Contains(c.Name2)).ToList();
                var match = MatchInstances(instances, made);
                if (match != null)
                {
                    Name(feat, pf.Str("name"));
                    features[pf.Str("id")] = feat;
                    foreach (var kv in match)
                    {
                        var c = kv.Value;
                        string want = kv.Key.Str("name");
                        if (want != null && c.Name2 != want)
                            try { c.Name2 = want; } catch (System.Runtime.InteropServices.COMException) { }
                        comps[kv.Key.Str("id")] = c;
                        Report.Add(docId, want, "rebuilt", null, "pattern instance");
                    }
                    return null;
                }
                last = made.Count + " instances made, " + instances.Count + " expected, not where the source has them";
                adoc.ClearSelection2(true);
                feat.Select2(false, 0);
                adoc.EditDelete();   // the pattern and its instances; try the other direction
            }
            return last;
        }

        /// <summary>Mirror Components: the same-part instances are mirrored again and matched to the source's by
        /// transform (opposite-hand copies are plain components of their own mirrored parts).</summary>
        string ReplayMirrorComponents(IModelDoc2 adoc, Dictionary<string, IComponent2> comps, JObj pf, List<JObj> instances)
        {
            var def = pf.Obj("def");
            var asm = (IAssemblyDoc)adoc;
            var seeds = new List<DispatchWrapper>();
            var orients = new List<int>();
            foreach (JObj i in def.Arr("instances"))
            {
                IComponent2 seed;
                if (!comps.TryGetValue(i.Str("seed"), out seed)) return "seed " + i.Str("seed") + " missing";
                seeds.Add(new DispatchWrapper(seed));
                orients.Add((int)i.Num("orientation"));
            }
            if (seeds.Count == 0) return null;   // only opposite-hand copies: nothing to mirror here
            adoc.ClearSelection2(true);
            string why = SelectAssemblyRef(adoc, comps, def.Obj("plane"), false, 0);
            if (why != null) return "mirror plane: " + why;
            object plane = ((ISelectionMgr)adoc.SelectionManager).GetSelectedObject6(1, -1);
            adoc.ClearSelection2(true);
            var before = new HashSet<string>(((object[])asm.GetComponents(true) ?? new object[0]).Cast<IComponent2>().Select(c => c.Name2));
            asm.MirrorComponents3(plane, seeds.ToArray(), orients.ToArray(), false, null, false, null, 0, "", "", 0, false, false, false);
            IFeature feat = null;
            for (var f = (IFeature)adoc.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
                if (f.GetTypeName2() == "MirrorCompFeat" && !Registered(f)) feat = f;
            if (feat == null) return "the mirror was not created";
            adoc.EditRebuild3();
            var made = ((object[])asm.GetComponents(true) ?? new object[0]).Cast<IComponent2>().Where(c => !before.Contains(c.Name2)).ToList();
            var match = MatchInstances(instances, made);
            if (match == null) return made.Count + " components mirrored, " + instances.Count + " expected, not where the source has them";
            Name(feat, pf.Str("name"));
            features[pf.Str("id")] = feat;
            foreach (var kv in match)
            {
                string want = kv.Key.Str("name");
                if (want != null && kv.Value.Name2 != want)
                    try { kv.Value.Name2 = want; } catch (COMException) { }
                comps[kv.Key.Str("id")] = kv.Value;
                Report.Add(docId, want, "rebuilt", null, "mirrored instance");
            }
            return null;
        }

        /// <summary>Pair each exported instance with a made component of the same document at the same transform;
        /// null unless every one is matched.</summary>
        Dictionary<JObj, IComponent2> MatchInstances(List<JObj> instances, List<IComponent2> made)
        {
            if (made.Count != instances.Count) return null;
            var free = new List<IComponent2>(made);
            var match = new Dictionary<JObj, IComponent2>();
            foreach (var c in instances)
            {
                var want = c.Vec("transform");
                var hit = free.FirstOrDefault(m =>
                {
                    var got = Exporter.ToMatrix((double[])((IMathTransform)m.Transform2).ArrayData);
                    for (int i = 0; i < 12; i++)
                        if (Math.Abs(got[i] - want[i]) > (i % 4 == 3 ? 1e-3 : 1e-6)) return false;
                    return true;
                });
                if (hit == null) return null;
                free.Remove(hit);
                match[c] = hit;
            }
            return match;
        }

        static void SkipComponentInstances(IModelDoc2 adoc, IFeature feat, List<object> skipped)
        {
            var data = feat.GetDefinition();
            var ints = skipped.Select(Convert.ToInt32).ToArray();
            var lin = data as ILocalLinearPatternFeatureData;
            var cir = data as ILocalCircularPatternFeatureData;
            if (lin != null) lin.SkippedItemArray = ints;
            else if (cir != null) cir.SkippedItemArray = ints;
            feat.ModifyDefinition(data, adoc, null);
        }

        /// <summary>The newest mate: mates live under the MateGroup, not at the end of the tree.</summary>
        static IFeature LastMate(IModelDoc2 adoc)
        {
            for (var f = (IFeature)adoc.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
            {
                if (f.GetTypeName2() != "MateGroup") continue;
                IFeature last = null;
                for (var s = (IFeature)f.GetFirstSubFeature(); s != null; s = (IFeature)s.GetNextSubFeature()) last = s;
                return last;
            }
            return null;
        }

        /// <summary>An origin plane (by position) or a feature (by its name in the payload) of a component.</summary>
        IFeature ComponentFeature(IComponent2 comp, JObj rf)
        {
            if (rf.Str("kind") == "feature")
            {
                var d = Model?.Arr("documents").Cast<JObj>().FirstOrDefault(x => x.Str("id") == rf.Str("document"));
                var nf = d?.Arr("features").Cast<JObj>().FirstOrDefault(x => x.Str("id") == rf.Str("id"));
                return nf == null ? null : (IFeature)comp.FeatureByName(nf.Str("name"));
            }
            int index = Array.IndexOf(new[] { "front", "top", "right" }, rf.Str("plane"));
            int planes = 0;
            for (var f = (IFeature)comp.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
                if (f.GetTypeName2() == "RefPlane" && planes++ == index) return f;
            return null;
        }

        void VerifyAssembly(JObj d, IModelDoc2 adoc, Dictionary<string, IComponent2> comps, JObj fingerprint)
        {
            // where every component ended up, against the source (mates may move unfixed components)
            var moved = new List<object>();
            foreach (JObj c in d.Arr("components"))
            {
                IComponent2 comp;
                if (!comps.TryGetValue(c.Str("id"), out comp)) continue;
                var got = Exporter.ToMatrix((double[])((IMathTransform)comp.Transform2).ArrayData);
                var want = c.Vec("transform");
                double dt = 0, dr = 0;
                for (int i = 0; i < 12; i++)
                    if (i % 4 == 3) dt = Math.Max(dt, Math.Abs(got[i] - want[i]));
                    else dr = Math.Max(dr, Math.Abs(got[i] - want[i]));
                if (dt > 1e-3 || dr > 1e-6) moved.Add(new JObj { { "component", c.Str("name") }, { "translation", dt }, { "rotation", dr } });
            }
            var v = new JObj { { "document", docId }, { "placement", moved.Count == 0 ? "match" : "moved" }, { "moved", moved } };
            if (fingerprint != null)
            {
                var actual = Host.DocFingerprint(adoc);
                var rep = (JObj)Json.Parse(NcxCore.FingerprintCompare(Json.Write(fingerprint, true), Json.Write(actual, true)).ReportJson);
                v["result"] = rep.Str("result");
                v["differences"] = rep["differences"];
            }
            else v["result"] = moved.Count == 0 ? "match" : "mismatch";
            Verification.Add(v);
        }
    }
}
