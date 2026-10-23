// Per-feature outcome of an export or replay: what carried across, what was approximated, what fell
// back and why (the plan's "explained failures" requirement).
using System.Collections.Generic;
using System.Linq;

namespace Ncx.Sw
{
    public class Report
    {
        public readonly List<JObj> Items = new List<JObj>();

        public void Add(string document, string feature, string status, string reason = null, string detail = null)
        {
            Items.Add(new JObj { { "document", document }, { "feature", feature }, { "status", status },
                                 { "reason", reason }, { "detail", detail } });
        }

        public int Count(string status) { return Items.Count(i => i.Str("status") == status); }

        public JObj ToJson()
        {
            var summary = new JObj();
            foreach (var g in Items.GroupBy(i => i.Str("status"))) summary[g.Key] = g.Count();
            return new JObj { { "summary", summary }, { "items", Items } };
        }
    }
}
