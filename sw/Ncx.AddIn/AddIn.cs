// The SolidWorks add-in: menu items only (File > NCX Translator), as the user asked - no task pane,
// no toolbar. Export Neutral writes the active part/assembly (and every document it depends on) to one
// NCX file; Import Neutral rebuilds a model from one, shows the per-feature report and verification.
//
// Registration (administrator, SolidWorks closed): sw\install.ps1 copies the build to an install folder
// and runs RegAsm /codebase on it there.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using Microsoft.Win32;
using Ncx.Sw;
using SolidWorks.Interop.sldworks;
using SolidWorks.Interop.swpublished;

namespace Ncx.AddIn
{
    [ComVisible(true)]
    [Guid("3bfa0246-4189-47c8-b413-c8b41d2937cf")]
    [ProgId("Ncx.AddIn")]
    public class NcxAddIn : ISwAddin
    {
        const string Title = "NCX Translator";
        // File > NCX Translator, like Fusion's File menu commands (user 2026-10-27; it was at the bottom of Tools)
        const string Menu = "NCX Translator@&File";
        const int MenuPosition = 2;   // just below File > Open... (New, Open come first in every document type)
        static readonly int[] DocTypes = { 0, 1, 2 };   // no document, part, assembly

        ISldWorks sw;
        int cookie;

        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr LoadLibrary(string path);

        bool active;   // false: this SOLIDWORKS version was not chosen at install (no menu, nothing loaded)

        /// <summary>SOLIDWORKS registers an add-in for every installed version at once; the installer records the
        /// versions the user chose (HKLM\SOFTWARE\NCX Translator, SolidWorksVersions = "2026,2018"). No record (a
        /// developer install) means every version.</summary>
        static bool ChosenFor(ISldWorks app)
        {
            using (var k = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\NCX Translator"))
            {
                var list = k?.GetValue("SolidWorksVersions") as string;
                if (string.IsNullOrWhiteSpace(list)) return true;
                int year = int.Parse(app.RevisionNumber().Split('.')[0]) + 1992;
                return list.Split(',').Any(s => s.Trim() == year.ToString());
            }
        }

        public bool ConnectToSW(object ThisSW, int Cookie)
        {
            sw = (ISldWorks)ThisSW;
            cookie = Cookie;
            active = ChosenFor(sw);
            if (!active) return true;   // dormant in a version the user did not choose
            // P/Invoke looks for ncx_core.dll beside sldworks.exe, not beside the add-in: load it from here
            // first so the DllImports bind to the already-loaded module
            LoadLibrary(Path.Combine(Path.GetDirectoryName(typeof(NcxAddIn).Assembly.Location), "ncx_core.dll"));
            sw.SetAddinCallbackInfo2(0, this, cookie);
            foreach (int t in DocTypes)
            {
                sw.AddMenu(t, Menu, MenuPosition);
                if (t != 0)
                    sw.AddMenuItem4(t, cookie, "Export Neutral (NCX)...@" + Menu, -1, "OnExport", "OnEnableExport",
                                    "Write this model and its history to a single NCX file", "");
                sw.AddMenuItem4(t, cookie, "Import Neutral (NCX)...@" + Menu, -1, "OnImport", "",
                                "Rebuild a model, with its history, from an NCX file", "");
                sw.AddMenuItem4(t, cookie, "About NCX Translator@" + Menu, -1, "OnAbout", "", "Version and core", "");
            }
            return true;
        }

        public bool DisconnectFromSW()
        {
            if (active)
                foreach (int t in DocTypes) sw.RemoveMenu(t, Menu, "");
            Marshal.ReleaseComObject(sw);
            sw = null;
            GC.Collect();
            GC.WaitForPendingFinalizers();
            return true;
        }

        IWin32Window Owner { get { return new WindowHandle(new IntPtr(((IFrame)sw.Frame()).GetHWnd())); } }

        class WindowHandle : IWin32Window
        {
            public WindowHandle(IntPtr h) { Handle = h; }
            public IntPtr Handle { get; private set; }
        }

        // ---------------------------------------------------------------------------- commands
        public int OnEnableExport()
        {
            var d = sw?.ActiveDoc as IModelDoc2;
            return d != null && (d.GetType() == 1 || d.GetType() == 2) ? 1 : 0;
        }

        public void OnAbout()
        {
            MessageBox.Show(Owner, Title + " " + Exporter.Version + "\nCore " + NcxCore.Version + ", schema " + NcxCore.SchemaVersion +
                            "\nSolidWorks " + sw.RevisionNumber(), Title, MessageBoxButtons.OK, MessageBoxIcon.Information);
        }

        public void OnExport()
        {
            var d = sw.ActiveDoc as IModelDoc2;
            if (d == null) return;
            if (string.IsNullOrEmpty(d.GetPathName()))
            {
                MessageBox.Show(Owner, "Save the document first: the export refers to it by file.", Title);
                return;
            }
            string target;
            using (var dlg = new SaveFileDialog
            {
                Title = "Export Neutral (NCX)", Filter = "NCX STEP file (*.step)|*.step",
                InitialDirectory = Path.GetDirectoryName(d.GetPathName()),
                FileName = Path.GetFileNameWithoutExtension(d.GetPathName()) + ".ncx.step"
            })
            {
                if (dlg.ShowDialog(Owner) != DialogResult.OK) return;
                target = dlg.FileName;
            }
            var exp = new Exporter(sw);
            using (var progress = new ProgressForm("Exporting " + Path.GetFileName(d.GetPathName())))
            {
                progress.Show(Owner);
                exp.Progress = progress.Step;
                try
                {
                    var res = Container.Export(sw, d, target, exp);
                    string reportPath = Path.ChangeExtension(target, ".report.json");
                    File.WriteAllText(reportPath, Json.Write(res["report"]));
                    progress.Close();
                    Finished("NCX export report: " + Path.GetFileName(target), false, exp.Report, null, DocName(null), reportPath);
                }
                catch (OperationCanceledException)
                {
                    progress.Close();
                    MessageBox.Show(Owner, "Export cancelled; no file was written.", Title);
                }
                catch (Exception e)
                {
                    progress.Close();
                    MessageBox.Show(Owner, "Export failed: " + e.Message, Title, MessageBoxButtons.OK, MessageBoxIcon.Error);
                }
            }
        }

        public void OnImport()
        {
            string source;
            using (var dlg = new OpenFileDialog { Title = "Import Neutral (NCX)", Filter = "NCX / STEP file (*.step;*.stp)|*.step;*.stp" })
            {
                if (dlg.ShowDialog(Owner) != DialogResult.OK) return;
                source = dlg.FileName;
            }
            JObj readReport;
            var model = Container.ReadModel(source, out readReport);
            if (model == null)
            {
                MessageBox.Show(Owner, "This file carries no usable NCX payload (" + readReport.Str("status") + ": " +
                                readReport.Str("message") + ").\nOpen it with File > Open to import its plain geometry.", Title);
                return;
            }
            string outDir;
            using (var dlg = new FolderBrowserDialog
            {
                Description = "Folder for the rebuilt SolidWorks documents",
                SelectedPath = Path.Combine(Path.GetDirectoryName(source), Path.GetFileNameWithoutExtension(source).Replace(".ncx", "") + "_ncx")
            })
            {
                Directory.CreateDirectory(dlg.SelectedPath);
                if (dlg.ShowDialog(Owner) != DialogResult.OK) return;
                outDir = dlg.SelectedPath;
            }
            var rep = new Replayer(sw) { ContainerPath = source };
            int steps = model.Arr("documents").Cast<JObj>().Sum(x => x.Arr("features").Count + 2) + 1;   // + rebuild/verify per document
            using (var progress = new ProgressForm("Importing " + Path.GetFileName(source), steps))
            {
                progress.Show(Owner);
                rep.Progress = progress.Step;
                try
                {
                    var saved = rep.ReplayModel(model, outDir);
                    string reportPath = Path.Combine(outDir, "ncx-import-report.json");
                    File.WriteAllText(reportPath, Json.Write(new JObj { { "saved", saved }, { "report", rep.Report.ToJson() },
                                                                        { "verification", rep.Verification } }));
                    progress.Close();
                    if (saved.Count > 0)
                    {
                        int err = 0;
                        sw.ActivateDoc3(Path.GetFileName(saved.Last()), false, 0, ref err);
                    }
                    Finished("NCX import report: " + Path.GetFileName(source), true, rep.Report, rep.Verification, DocName(model), reportPath);
                }
                catch (Exception e)
                {
                    progress.Close();
                    MessageBox.Show(Owner, "Import failed: " + e.Message, Title, MessageBoxButtons.OK, MessageBoxIcon.Error);
                }
            }
        }

        // ---------------------------------------------------------------------------- automation
        /// <summary>For test scripts (no UI): run an export or import inside SOLIDWORKS, as the menu commands do,
        /// and return its report as JSON. A script reaches it through ISldWorks.GetAddInObject("{3bfa0246-...}"):
        /// one call crosses the process boundary, the work runs here (out-of-process API calls are 10-50 times
        /// slower). Job: {"op":"export","source":path,"target":path,"flex":"auto|off|N"} or
        /// {"op":"import","source":path,"outDir":path}.</summary>
        public string RunJob(string jobJson)
        {
            StreamWriter log = null;
            try
            {
                var job = (JObj)Json.Parse(jobJson);
                // a timing log, one line per step (feature), to find what is slow: the job's "log", else
                // ncx-job.log in the temp folder
                string logPath = job.Str("log") ?? Path.Combine(Path.GetTempPath(), "ncx-job.log");
                log = new StreamWriter(logPath, false) { AutoFlush = true };
                var clock = System.Diagnostics.Stopwatch.StartNew();
                double last = 0;
                Func<string, bool> step = s =>
                {
                    double now = clock.Elapsed.TotalSeconds;
                    log.WriteLine(now.ToString("F1") + "\t+" + (now - last).ToString("F2") + "\t" + s);
                    last = now;
                    return true;   // never cancels
                };
                if (job.Str("op") == "export")
                {
                    string src = job.Str("source");
                    var d = (IModelDoc2)sw.GetOpenDocumentByName(src);
                    if (d == null)
                    {
                        int err = 0, warn = 0;
                        d = (IModelDoc2)sw.OpenDoc6(src, src.EndsWith(".SLDASM", StringComparison.OrdinalIgnoreCase) ? 2 : 1, 1, "", ref err, ref warn);
                        if (d == null) return Json.Write(new JObj { { "ok", false }, { "error", "could not open " + src + " (error " + err + ")" } });
                    }
                    var exp = new Exporter(sw) { Progress = step };
                    string flex = job.Str("flex") ?? "auto";
                    exp.FlexProbes = flex == "auto" ? -1 : flex == "off" ? 0 : int.Parse(flex);
                    var t0 = DateTime.Now;
                    var res = Container.Export(sw, d, job.Str("target"), exp);   // the model stays open, as the menu leaves it
                    return Json.Write(new JObj { { "ok", true }, { "seconds", (DateTime.Now - t0).TotalSeconds }, { "report", res["report"] } });
                }
                if (job.Str("op") == "import")
                {
                    JObj readReport;
                    var model = Container.ReadModel(job.Str("source"), out readReport);
                    if (model == null) return Json.Write(new JObj { { "ok", false }, { "error", "no NCX payload: " + readReport.Str("status") } });
                    var rep = new Replayer(sw) { ContainerPath = job.Str("source"), Progress = step };
                    var t0 = DateTime.Now;
                    var saved = rep.ReplayModel(model, job.Str("outDir"));
                    return Json.Write(new JObj { { "ok", true }, { "seconds", (DateTime.Now - t0).TotalSeconds }, { "saved", saved },
                                                 { "report", rep.Report.ToJson() }, { "verification", rep.Verification } });
                }
                return Json.Write(new JObj { { "ok", false }, { "error", "unknown op " + job.Str("op") } });
            }
            catch (Exception e)
            {
                return Json.Write(new JObj { { "ok", false }, { "error", e.Message }, { "at", e.StackTrace } });
            }
            finally { log?.Dispose(); }
        }

        /// <summary>A document id ("doc:x") as its name in the payload, or the id without its prefix.</summary>
        static Func<string, string> DocName(JObj model)
        {
            var names = new Dictionary<string, string>();
            if (model != null)
                foreach (JObj d in model.Arr("documents"))
                    if (d.Str("id") != null && d.Str("name") != null) names[d.Str("id")] = d.Str("name");
            return id =>
            {
                string n;
                if (id != null && names.TryGetValue(id, out n)) return n;
                return id != null && id.StartsWith("doc:") ? id.Substring(4) : id;
            };
        }

        // the report box only when there is something to tell; a clean run just says so in the status bar. docName: a
        // document id ("doc:x") as the name the user knows
        void Finished(string title, bool import, Report report, List<JObj> verification, Func<string, string> docName, string reportPath)
        {
            // nothing serious: no box, one line on the status bar (the report file has the details)
            if (!ReportForm.Show(Owner, title, import, report, verification, docName, reportPath))
                ((IFrame)sw.Frame()).SetStatusBarText((import ? "NCX: model imported" : "NCX: file saved") + ". Details: " + reportPath);
        }

        // ------------------------------------------------------------------------ registration
        [ComRegisterFunction]
        public static void Register(Type t)
        {
            string key = @"SOFTWARE\SolidWorks\Addins\{" + t.GUID.ToString() + "}";
            using (var k = Registry.LocalMachine.CreateSubKey(key))
            {
                k.SetValue(null, 0);
                k.SetValue("Description", "Exports and imports models with their full history through single-file NCX (STEP) files");
                k.SetValue("Title", Title);
            }
            using (var k = Registry.CurrentUser.CreateSubKey(@"Software\SolidWorks\AddInsStartup\{" + t.GUID.ToString() + "}"))
                k.SetValue(null, 1);   // load at start-up
        }

        [ComUnregisterFunction]
        public static void Unregister(Type t)
        {
            Registry.LocalMachine.DeleteSubKey(@"SOFTWARE\SolidWorks\Addins\{" + t.GUID.ToString() + "}", false);
            Registry.CurrentUser.DeleteSubKey(@"Software\SolidWorks\AddInsStartup\{" + t.GUID.ToString() + "}", false);
        }
    }
}
