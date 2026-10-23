// Windows Forms used by the add-in: a progress window with a working Cancel, and the report window.
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

    /// <summary>What carried across, what was approximated, what fell back and why.</summary>
    static class ReportForm
    {
        static readonly Dictionary<string, Color> Colours = new Dictionary<string, Color>
        {
            { "mapped", Color.Honeydew }, { "rebuilt", Color.Honeydew }, { "note", Color.AliceBlue }, { "approximate", Color.LightYellow },
            { "fallback", Color.MistyRose }, { "opaque", Color.MistyRose }, { "failed", Color.LightPink }, { "cancelled", Color.Gainsboro }
        };

        // a note (e.g. an under-defined sketch) is information, not a problem: it does not open the window
        static readonly HashSet<string> CleanStatus = new HashSet<string> { "mapped", "rebuilt", "note" };
        // "topology": the same surfaces and areas, only cut into faces differently
        static readonly HashSet<string> CleanVerification = new HashSet<string> { "match", "count_only", "topology" };

        /// <summary>Anything approximated, dropped, failed or cancelled, or any verification that did not match.</summary>
        public static bool HasProblems(Report report, List<JObj> verification)
        {
            return report.Items.Any(i => !CleanStatus.Contains(i.Str("status") ?? "")) ||
                   (verification != null && verification.Any(v => !CleanVerification.Contains(v.Str("result") ?? "")));
        }

        public static void Show(IWin32Window owner, string title, Report report, List<JObj> verification, string reportPath)
        {
            using (var f = new Form { Text = title, Width = 900, Height = 560, StartPosition = FormStartPosition.CenterParent, ShowInTaskbar = false })
            {
                var summary = report.ToJson().Obj("summary");
                var top = new Label
                {
                    Dock = DockStyle.Top, Height = 44, Padding = new Padding(6),
                    Text = string.Join("   ", summary.Select(kv => kv.Key + ": " + kv.Value)) +
                           (verification == null ? "" : "\nVerification: " + string.Join(", ", verification.Select(v => v.Str("document") + " " + v.Str("result"))))
                };
                var list = new ListView { Dock = DockStyle.Fill, View = View.Details, FullRowSelect = true, GridLines = true };
                list.Columns.Add("Document", 160);
                list.Columns.Add("Feature", 200);
                list.Columns.Add("Status", 90);
                list.Columns.Add("Reason", 420);
                foreach (var it in report.Items)
                {
                    var row = new ListViewItem(new[] { it.Str("document"), it.Str("feature"), it.Str("status"), it.Str("reason") ?? "" });
                    Color c;
                    if (Colours.TryGetValue(it.Str("status") ?? "", out c)) row.BackColor = c;
                    list.Items.Add(row);
                }
                var bottom = new Label { Dock = DockStyle.Bottom, Height = 24, Padding = new Padding(6), Text = "Full report: " + reportPath };
                f.Controls.Add(list);
                f.Controls.Add(bottom);
                f.Controls.Add(top);
                f.ShowDialog(owner);
            }
        }
    }
}
