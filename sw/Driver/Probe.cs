// Developer probe: which calls work out of process, typed (vtable) vs late-bound (IDispatch).
using System;
using SolidWorks.Interop.sldworks;

namespace Ncx.Driver
{
    static class Probe
    {
        static void Try(string name, Func<object> f)
        {
            try { Console.WriteLine("ok   " + name + " -> " + (f() ?? "null")); }
            catch (Exception e) { Console.WriteLine("FAIL " + name + " -> " + e.Message); }
        }

        public static int Run(ISldWorks sw)
        {
            var doc = (IModelDoc2)sw.NewDocument(sw.GetUserPreferenceStringValue(8), 0, 0, 0);
            try
            {
                IFeature front = null;
                for (var f = (IFeature)doc.FirstFeature(); f != null; f = (IFeature)f.GetNextFeature())
                    if (f.GetTypeName2() == "RefPlane") { front = f; break; }
                Try("select front (typed)", () => front.Select2(false, 0));
                var sm = (ISketchManager)doc.SketchManager;
                Try("InsertSketch typed", () => { sm.InsertSketch(true); return "done"; });
                Try("ActiveSketch typed", () => sm.ActiveSketch);
                Try("exit typed", () => { sm.InsertSketch(true); return "done"; });
                Try("select front again", () => front.Select2(false, 0));
                dynamic dsm = doc.SketchManager;
                Try("InsertSketch dynamic", () => { dsm.InsertSketch(true); return "done"; });
                Try("ActiveSketch dynamic", () => (object)dsm.ActiveSketch);
                Try("CreateLine dynamic", () => (object)dsm.CreateLine(0.0, 0.0, 0.0, 0.1, 0.0, 0.0));
                Try("CreateLine typed", () => sm.CreateLine(0.0, 0.01, 0.0, 0.1, 0.01, 0.0));
                Try("exit dynamic", () => { dsm.InsertSketch(true); return "done"; });
                Try("InsertSketch2 legacy (IModelDoc2)", () => { doc.InsertSketch2(true); return "done"; });
                Try("exit legacy", () => { doc.InsertSketch2(true); return "done"; });

                Try("select front 3", () => front.Select2(false, 0));
                Try("InsertSketch typed 3", () => { sm.InsertSketch(true); return "done"; });
                Try("AddToDB typed read (fresh sketch)", () => sm.AddToDB);
                Try("AddToDB dynamic read", () => (object)dsm.AddToDB);
                var sk = sm.ActiveSketch;
                Try("SketchFrame", () => new Ncx.Sw.SketchFrame(sw, sk).Z[2]);
                Try("AddToDB typed read (after frame)", () => sm.AddToDB);
                Try("DisplayWhenAdded typed read", () => sm.DisplayWhenAdded);
                Try("exit 3", () => { sm.InsertSketch(true); return "done"; });
            }
            finally
            {
                sw.CloseDoc(doc.GetTitle());
            }
            return 0;
        }
    }
}
