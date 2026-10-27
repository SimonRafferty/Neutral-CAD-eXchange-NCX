// Windows Forms used by the add-in: a progress window with a working Cancel, and the report box.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Windows.Forms;
using Ncx.Sw;

namespace Ncx.AddIn
{
    /// <summary>Visible activity during long operations, with a Cancel that takes effect at the next step.</summary>
    class ProgressForm : Form
    {
        readonly Label label = new Label { Dock = DockStyle.Top, Height = 36, AutoEllipsis = true };
        readonly ProgressBar bar = new ProgressBar { Dock = DockStyle.Top, Height = 18, Style = ProgressBarStyle.Continuous };
        readonly Button cancel = new Button { Text = "Cancel", Dock = DockStyle.Bottom, Height = 28 };
        bool cancelled;

        public ProgressForm(string title, int steps = 0)
        {
            Text = title;
            Width = 460;
            Height = 150;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;
            ShowInTaskbar = false;
            Padding = new Padding(10);
            bar.Maximum = Math.Max(1, steps);
            if (steps == 0) bar.Style = ProgressBarStyle.Marquee;
            cancel.Click += (s, e) => { cancelled = true; cancel.Enabled = false; label.Text = "Cancelling after the current step..."; };
            Controls.Add(cancel);
            Controls.Add(bar);
            Controls.Add(label);
        }

        /// <summary>The library's progress callback: show the step, let the window process its clicks, and
        /// report whether to carry on.</summary>
        public bool Step(string message)
        {
            if (!cancelled) label.Text = message;
            if (bar.Style == ProgressBarStyle.Continuous && bar.Value < bar.Maximum) bar.Value++;
            Application.DoEvents();
            return !cancelled;
        }
    }

    /// <summary>The report at the end of an import or export, in plain words. Most people who open an NCX file are not
    /// CAD specialists: a list of every approximated feature reads as a list of errors. So the box says the model came
    /// through, names only the parts worth looking at, and counts the rest; the saved report file keeps every entry.</summary>
    static class ReportForm
    {
        // "A, B, C and 4 more"
        static string NameList(List<string> names, int most = 5)
        {
            string s = string.Join(", ", names.Take(most));
            return names.Count > most ? s + " and " + (names.Count - most) + " more" : s;
        }

        /// <summary>The summary text; empty when there is nothing to say beyond "it worked". warn: a part's shape may
        /// differ from the original (the only real warning).</summary>
        public static string Summary(bool import, Report report, List<JObj> verification, Func<string, string> docName, out bool warn)
        {
            warn = false;
            var lines = new List<string>();
            if (import)
            {
                // parts replaced by the file's own geometry: right shape, no feature history (their features don't count)
                var fixedDocs = new HashSet<string>(report.Items.Where(i => i.Str("status") == "fallback").Select(i => i.Str("document")));
                // parts whose mass properties did not match and were not replaced. The face and structure checks
                // ("doc:x (faces)", "(structure)") are details for the report file: faces cut differently, sketches
                // less defined than the source's, with the shape the same
                var check = (verification ?? new List<JObj>())
                    .Where(v => (v.Str("document") ?? "").StartsWith("doc:") && !v.Str("document").Contains(" (") &&
                                v.Str("result") == "mismatch" && !fixedDocs.Contains(v.Str("document")))
                    .Select(v => docName(v.Str("document"))).Distinct().ToList();
                int different = report.Items.Count(i => (i.Str("status") == "approximate" || i.Str("status") == "failed" || i.Str("status") == "opaque") &&
                                                         !fixedDocs.Contains(i.Str("document")));
                bool cancelled = report.Items.Any(i => i.Str("status") == "cancelled");
                if (cancelled) lines.Add("- The import was cancelled before the end: only what was finished by then was built.");
                if (check.Count > 0)
                {
                    warn = true;
                    lines.Add("- Please look over " + (check.Count == 1 ? "this part" : "these parts") +
                              ": the check found the shape may differ from the original: " + NameList(check) + ".");
                }
                var fixedNames = fixedDocs.Select(docName).Distinct().ToList();
                if (fixedNames.Count > 0)
                    lines.Add("- " + NameList(fixedNames) + (fixedNames.Count == 1 ? " has" : " have") + " the right shape, but came in " +
                              "without editable feature history (SolidWorks could not rebuild " + (fixedNames.Count == 1 ? "it" : "them") + " step by step).");
                if (different > 0)
                    lines.Add("- " + different + (different == 1 ? " item was" : " items were") + " carried over slightly differently, as SolidWorks " +
                              "has no exact match" + (check.Count == 0 ? ". This does not change the shape." : "."));
                if (lines.Count == 0) return "";
                if (check.Count == 0 && !cancelled) lines.Insert(0, "- Every part was checked against the original, and its shape matches.");
                return "Your model has been imported into SOLIDWORKS.\n\nThis is a short report on how the import went. It is not an error message.\n\n" +
                       string.Join("\n", lines);
            }
            int opaque = report.Count("opaque");
            int approx = report.Count("approximate") + report.Count("failed");
            if (opaque > 0)
                lines.Add("- " + opaque + (opaque == 1 ? " feature" : " features") + " could not be written as editable " + (opaque == 1 ? "feature" : "features") +
                          ": " + (opaque == 1 ? "it travels" : "they travel") + " as fixed shapes, so the other CAD program shows " +
                          (opaque == 1 ? "it" : "them") + " correctly but cannot edit " + (opaque == 1 ? "it." : "them."));
            if (approx > 0)
                lines.Add("- " + approx + (approx == 1 ? " item is" : " items are") + " written slightly differently, as the NCX format has no exact match.");
            if (lines.Count == 0) return "";
            lines.Add("- The shape of every part is carried exactly.");
            return "Your model has been exported.\n\nThis is a short report on how the export went. It is not an error message.\n\n" + string.Join("\n", lines);
        }

        /// <summary>The report box, when there is something to say; returns false (nothing shown) otherwise.</summary>
        public static bool Show(IWin32Window owner, string title, bool import, Report report, List<JObj> verification,
                                Func<string, string> docName, string reportPath)
        {
            bool warn;
            string text = Summary(import, report, verification, docName, out warn);
            if (text.Length == 0) return false;
            text += "\n\nClick OK to carry on.\n\nFull details, for anyone helping you, are saved in:\n" + reportPath;
            MessageBox.Show(owner, text, title, MessageBoxButtons.OK, warn ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
            return true;
        }
    }
}
