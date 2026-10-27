// Driver commands: thin wrappers that open documents and call the library.
using System;
using System.Collections.Generic;
using System.IO;
using Ncx.Sw;
using SolidWorks.Interop.sldworks;

namespace Ncx.Driver
{
    static class Commands
    {
        static IModelDoc2 Open(ISldWorks sw, string path, out bool openedHere)
        {
            path = Path.GetFullPath(path);
            var doc = (IModelDoc2)sw.GetOpenDocumentByName(path);
            openedHere = doc == null;
            if (doc != null) return doc;
            int type = path.EndsWith(".SLDASM", StringComparison.OrdinalIgnoreCase) ? 2 : 1;
            int err = 0, warn = 0;
            doc = (IModelDoc2)sw.OpenDoc6(path, type, 1, "", ref err, ref warn);   // silent
            if (doc == null) throw new InvalidOperationException("cannot open " + path + " (error " + err + ")");
            return doc;
        }

        public static int Export(ISldWorks sw, string docPath, string outStep, Dictionary<string, string> opts)
        {
            bool opened;
            var doc = Open(sw, docPath, out opened);
            try
            {
                var exp = new Exporter(sw);
                var t0 = DateTime.Now;
                if (opts.ContainsKey("phases")) exp.Phase = s => Console.Error.WriteLine((DateTime.Now - t0).TotalSeconds.ToString("F1") + " s  " + s);
                string flex;
                // parameter-flex probes: auto (default: only where needed) | off | N per part
                if (opts.TryGetValue("flex", out flex)) exp.FlexProbes = flex == "auto" ? -1 : flex == "off" ? 0 : int.Parse(flex);
                // feature results (FORMAT-SPEC 4, "Feature results"): opaque | extended (default) | all; --opaque Type1,Type2 for testing
                string results, opaque;
                if (opts.TryGetValue("results", out results)) exp.Results = results;
                if (opts.TryGetValue("opaque", out opaque)) exp.ForceOpaque = new HashSet<string>(opaque.Split(','));
                var res = Container.Export(sw, doc, Path.GetFullPath(outStep), exp);
                File.WriteAllText(Path.ChangeExtension(outStep, ".export.json"), Json.Write(res));
                Console.WriteLine(Json.Write(((JObj)res["report"])["summary"], true));
                foreach (JObj it in exp.Report.Items)
                    if (it.Str("status") != "mapped")
                        Console.WriteLine("  " + it.Str("status") + ": " + it.Str("feature") + " - " + it.Str("reason"));
                return 0;
            }
            finally
            {
                if (opened) sw.CloseDoc(doc.GetTitle());
            }
        }

        public static int Replay(ISldWorks sw, string inStep, string outDir, Dictionary<string, string> opts)
        {
            JObj readReport;
            var model = Container.ReadModel(inStep, out readReport);
            if (model == null)
            {
                Console.WriteLine("no usable payload: " + readReport.Str("status") + " " + readReport.Str("message"));
                return 3;
            }
            var rep = new Replayer(sw) { ContainerPath = Path.GetFullPath(inStep) };
            var saved = rep.ReplayModel(model, Path.GetFullPath(outDir));
            var res = new JObj { { "saved", saved }, { "report", rep.Report.ToJson() }, { "verification", rep.Verification } };
            File.WriteAllText(Path.Combine(outDir, "replay.json"), Json.Write(res));
            Console.WriteLine(Json.Write(rep.Report.ToJson()["summary"], true));
            foreach (JObj it in rep.Report.Items)
                if (it.Str("status") != "rebuilt")
                    Console.WriteLine("  " + it.Str("status") + ": " + it.Str("feature") + " - " + it.Str("reason"));
            foreach (var v in rep.Verification)
                Console.WriteLine("verify " + v.Str("document") + ": " + v.Str("result") + " " + Json.Write(v["differences"], true));
            if (!opts.ContainsKey("keep-open"))
                for (int i = saved.Count - 1; i >= 0; i--)   // assemblies first
                {
                    var d = sw.GetOpenDocumentByName(saved[i]) as IModelDoc2;
                    if (d != null) sw.CloseDoc(d.GetTitle());
                }
            return 0;
        }
    }
}
