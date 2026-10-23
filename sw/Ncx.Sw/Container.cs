// Writing and reading NCX files from SolidWorks: the B-rep is SolidWorks' own AP214 export (Save As
// Copy, so the document is untouched) and the payload is added by the core (FORMAT-SPEC section 2).
using System;
using System.IO;
using System.Text;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public static class Container
    {
        const int I_STEP_AP = 75;   // swUserPreferenceIntegerValue_e.swStepAP

        /// <summary>Export `doc` to an NCX file. Returns {model, report, header}.</summary>
        public static JObj Export(ISldWorks sw, IModelDoc2 doc, string outPath, Exporter exp)
        {
            var model = exp.BuildModel(doc);
            string json = Json.Write(model, true);
            var valid = NcxCore.Validate(Encoding.UTF8.GetBytes(json));
            if (!valid.Ok) throw new InvalidOperationException("exported model fails validation: " + valid.ReportJson);

            string tmp = Path.Combine(Path.GetTempPath(), "ncx_" + Guid.NewGuid().ToString("N") + ".step");
            int savedAp = sw.GetUserPreferenceIntegerValue(I_STEP_AP);
            try
            {
                sw.SetUserPreferenceIntegerValue(I_STEP_AP, 214);   // AP242 assemblies hang SolidWorks 2018 (Stage 1)
                int err = 0, warn = 0;
                bool ok = doc.Extension.SaveAs(tmp, 0, 1 | 2, null, ref err, ref warn);   // silent, copy
                if (!ok || !File.Exists(tmp)) throw new InvalidOperationException("STEP export failed: error " + err);
                var w = NcxCore.PayloadWrite(File.ReadAllBytes(tmp), Encoding.UTF8.GetBytes(json),
                                             "{\"generator\":\"ncx-sw " + Exporter.Version + " SOLIDWORKS " + sw.RevisionNumber() + "\"}");
                if (!w.Ok) throw new InvalidOperationException("payload write failed: " + w.ReportJson);
                File.WriteAllBytes(outPath, w.Data);
                return new JObj { { "model", model }, { "report", exp.Report.ToJson() }, { "container", Json.Parse(w.ReportJson) } };
            }
            finally
            {
                sw.SetUserPreferenceIntegerValue(I_STEP_AP, savedAp);
                if (File.Exists(tmp)) File.Delete(tmp);
            }
        }

        /// <summary>Read the payload of an NCX file; throws with the core's status if there is none.</summary>
        public static JObj ReadModel(string path, out JObj readReport)
        {
            var r = NcxCore.PayloadRead(File.ReadAllBytes(path));
            readReport = (JObj)Json.Parse(r.ReportJson);
            if (!r.Ok) return null;
            return (JObj)Json.Parse(Encoding.UTF8.GetString(r.Data));
        }
    }
}
