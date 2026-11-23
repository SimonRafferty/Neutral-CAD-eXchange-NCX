// Update notice (user 2026-11-23: "a warning to the user with a click here to update link", never mandatory).
//
// When SOLIDWORKS loads the add-in, a background thread asks GitHub for the project's releases (at most once a day:
// the answer is kept in HKCU\Software\NCX Translator), so no command ever waits for the network. When the user next
// chooses Import or Export, and a newer SolidWorks release (tags "v<version>"; Fusion's are "fusion-v...") exists, one
// box offers the download page: Yes opens it in the browser, No carries on. Once per SOLIDWORKS session at most.
// Offline, a GitHub error or a rate limit: nothing is shown. No personal details are sent (a generic User-Agent).
// Off switch: UpdateCheck = 0 (DWORD) under HKCU or HKLM\Software\NCX Translator. Never in automated jobs (RunJob).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;
using Ncx.Sw;

namespace Ncx.AddIn
{
    static class UpdateCheck
    {
        const string Releases = "https://api.github.com/repos/SimonRafferty/Neutral-CAD-eXchange-NCX/releases?per_page=50";
        const string Key = @"Software\NCX Translator";
        const string TagPrefix = "v";   // the SolidWorks add-in's releases

        static volatile string latestVersion, latestUrl;
        static bool offered;

        /// <summary>Begin the check (call once, when the add-in loads).</summary>
        public static void Start()
        {
            try
            {
                if (Disabled()) return;
                using (var k = Registry.CurrentUser.CreateSubKey(Key))
                {
                    DateTime last;
                    if (DateTime.TryParse(k.GetValue("UpdateCheckedUtc") as string, CultureInfo.InvariantCulture,
                                          DateTimeStyles.RoundtripKind, out last) && DateTime.UtcNow - last < TimeSpan.FromDays(1))
                    {
                        latestVersion = k.GetValue("UpdateLatestVersion") as string;   // today's answer
                        latestUrl = k.GetValue("UpdateLatestUrl") as string;
                        return;
                    }
                }
                new Thread(() => { try { Fetch(); } catch (Exception) { } }) { IsBackground = true, Name = "NCX update check" }.Start();
            }
            catch (Exception) { }   // never let the check disturb SOLIDWORKS
        }

        static bool Disabled()
        {
            foreach (var root in new[] { Registry.CurrentUser, Registry.LocalMachine })
                using (var k = root.OpenSubKey(Key))
                    if (k?.GetValue("UpdateCheck") is int v && v == 0) return true;
            return false;
        }

        static void Fetch()
        {
            ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;   // .NET 4.8 under older Windows defaults
            var req = (HttpWebRequest)WebRequest.Create(Releases);
            req.UserAgent = "NCX-Translator-update-check";   // GitHub requires one; nothing about the user
            req.Accept = "application/vnd.github+json";
            req.Timeout = req.ReadWriteTimeout = 15000;
            string body;
            using (var r = req.GetResponse())
            using (var s = new StreamReader(r.GetResponseStream()))
                body = s.ReadToEnd();
            Version best = null;
            string bestUrl = null;
            foreach (var o in (Json.Parse(body) as List<object>) ?? new List<object>())
            {
                var rel = o as JObj;
                if (rel == null || rel.Bool("draft") || rel.Bool("prerelease")) continue;
                Version v = Parse(rel.Str("tag_name"));
                if (v != null && (best == null || v > best)) { best = v; bestUrl = rel.Str("html_url"); }
            }
            if (best == null) return;
            latestVersion = best.ToString();
            latestUrl = bestUrl;
            using (var k = Registry.CurrentUser.CreateSubKey(Key))
            {
                k.SetValue("UpdateLatestVersion", latestVersion);
                k.SetValue("UpdateLatestUrl", latestUrl ?? "");
                k.SetValue("UpdateCheckedUtc", DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture));
            }
        }

        /// <summary>"v0.6.7" -> 0.6.7; null for another add-in's tag ("fusion-v0.2.6") or anything else.</summary>
        static Version Parse(string tag)
        {
            Version v;
            if (tag == null || !tag.StartsWith(TagPrefix, StringComparison.Ordinal)) return null;
            return Version.TryParse(tag.Substring(TagPrefix.Length), out v) ? v : null;
        }

        /// <summary>Before Import or Export: offer the download page if a newer release exists (once per session).</summary>
        public static void Offer(IWin32Window owner, string title)
        {
            try
            {
                if (offered || latestVersion == null || string.IsNullOrEmpty(latestUrl)) return;
                Version newest, mine;
                if (!Version.TryParse(latestVersion, out newest) || !Version.TryParse(Exporter.Version, out mine) || newest <= mine) return;
                offered = true;
                var answer = MessageBox.Show(owner,
                    "A newer NCX Translator is available: " + newest + " (this computer has " + Exporter.Version + ").\n\n" +
                    "Open the download page now? (Close SOLIDWORKS before you run the new installer.)\n\n" +
                    "Choose No to carry on with this version.", title, MessageBoxButtons.YesNo, MessageBoxIcon.Information);
                if (answer == DialogResult.Yes) System.Diagnostics.Process.Start(latestUrl);
            }
            catch (Exception) { }
        }
    }
}
