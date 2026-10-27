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
            // Only a serious problem gets a box (user 2026-10-27: "Unless there's a serious problem, don't mention it"):
            // a part whose shape may differ from the original, or an import cancelled part way. Parts that came in
            // without history, items carried over another way and features exported as fixed shapes all have the
            // right shape: the report file lists them for whoever helps the user, the box does not.
            warn = false;
            if (!import) return "";
            var lines = new List<string>();
            // parts replaced by the file's own geometry: right shape, no feature history (not checked again)
            var fixedDocs = new HashSet<string>(report.Items.Where(i => i.Str("status") == "fallback").Select(i => i.Str("document")));
            // parts whose mass properties did not match and were not replaced. The face and structure checks
            // ("doc:x (faces)", "(structure)") are details for the report file: faces cut differently, sketches
            // less defined than the source's, with the shape the same
            var check = (verification ?? new List<JObj>())
                .Where(v => (v.Str("document") ?? "").StartsWith("doc:") && !v.Str("document").Contains(" (") &&
                            v.Str("result") == "mismatch" && !fixedDocs.Contains(v.Str("document")))
                .Select(v => docName(v.Str("document"))).Distinct().ToList();
            if (report.Items.Any(i => i.Str("status") == "cancelled"))
                lines.Add("The import was cancelled before the end: only what was finished by then was built.");
            if (check.Count > 0)
            {
                warn = true;
                lines.Add("Please look over " + (check.Count == 1 ? "this part" : "these parts") +
                          ", as the shape may differ from the original: " + NameList(check) + ".");
            }
            if (lines.Count == 0) return "";
            return "Your model has been imported into SOLIDWORKS.\n\n" + string.Join("\n\n", lines);
        }

        /// <summary>The report box, when there is something serious to say; returns false (nothing shown) otherwise.</summary>
        public static bool Show(IWin32Window owner, string title, bool import, Report report, List<JObj> verification,
                                Func<string, string> docName, string reportPath)
        {
            bool warn;
            string text = Summary(import, report, verification, docName, out warn);
            if (text.Length == 0) return false;
            text += "\n\nDetails, for anyone helping you, are saved in:\n" + reportPath;
            MessageBox.Show(owner, text, title, MessageBoxButtons.OK, warn ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
            return true;
        }
    }
}
