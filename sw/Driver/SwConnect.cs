// Out-of-process connection to a running SolidWorks, for the developer driver only (the add-in gets
// ISldWorks from ConnectToSW). SolidWorks registers "SolidWorks_PID_<pid>" in the Running Object
// Table, which lets 2018 and 2026 be driven side by side.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using SolidWorks.Interop.sldworks;

namespace Ncx.Driver
{
    public static class SwConnect
    {
        [DllImport("ole32.dll")] static extern int GetRunningObjectTable(int reserved, out IRunningObjectTable prot);
        [DllImport("ole32.dll")] static extern int CreateBindCtx(int reserved, out IBindCtx ppbc);

        /// <summary>API major version (26 = 2018, 33 = 2025, 34 = 2026) -> running SolidWorks.</summary>
        public static Dictionary<int, ISldWorks> Running()
        {
            var found = new Dictionary<int, ISldWorks>();
            IRunningObjectTable rot;
            IBindCtx ctx;
            if (GetRunningObjectTable(0, out rot) != 0 || CreateBindCtx(0, out ctx) != 0) return found;
            IEnumMoniker e;
            rot.EnumRunning(out e);
            var mk = new IMoniker[1];
            while (e.Next(1, mk, IntPtr.Zero) == 0)
            {
                string name;
                mk[0].GetDisplayName(ctx, null, out name);
                if (!name.StartsWith("SolidWorks_PID_")) continue;
                object o;
                rot.GetObject(mk[0], out o);
                var sw = o as ISldWorks;
                if (sw == null) continue;
                int major = int.Parse(sw.RevisionNumber().Split('.')[0]);
                found[major] = sw;
            }
            return found;
        }

        public static ISldWorks Get(int major)
        {
            ISldWorks sw;
            if (!Running().TryGetValue(major, out sw))
                throw new InvalidOperationException("no running SolidWorks with API " + major + " (" + (major + 1992) + ")");
            return sw;
        }
    }
}
