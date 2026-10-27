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
            var items = Grouped();
            var summary = new JObj();
            foreach (var g in items.GroupBy(i => i.Str("status"))) summary[g.Key] = g.Count();
            return new JObj { { "summary", summary }, { "items", items } };
        }

        /// <summary>The items with the parts that have no history (imported geometry, FORMAT-SPEC 4 plain) said once
        /// for all of them, where the first of them stood, instead of once per part (user 2026-10-27).</summary>
        List<JObj> Grouped()
        {
            var plain = Items.Where(IsNoHistory).ToList();
            if (plain.Count < 2) return Items;
            var names = plain.Select(i => i.Str("feature") ?? i.Str("document")).Distinct().ToList();   // the part's name
            var one = new JObj { { "document", names.Count + " parts" }, { "feature", string.Join(", ", names) }, { "status", "note" },
                                 { "reason", "no history in the source (imported geometry): carried as plain shapes, as the source has them" },
                                 { "detail", null } };
            var outList = new List<JObj>();
            foreach (var i in Items)
                if (!IsNoHistory(i)) outList.Add(i);
                else if (i == plain[0]) outList.Add(one);
            return outList;
        }

        static bool IsNoHistory(JObj i)
        {
            return i.Str("status") == "note" && (i.Str("reason") ?? "").StartsWith("no history");
        }
    }
}
