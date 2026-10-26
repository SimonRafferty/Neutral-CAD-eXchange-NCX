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

        /// <summary>The running SolidWorks with this API major version (26 = 2018, 33 = 2025, 34 = 2026), or null. The
        /// version is read from each process's executable (its file version's major part is the API version), so no
        /// call is made into any other SolidWorks session: one the user is working in is never touched.</summary>
        public static ISldWorks Find(int major)
        {
            IRunningObjectTable rot;
            IBindCtx ctx;
            if (GetRunningObjectTable(0, out rot) != 0 || CreateBindCtx(0, out ctx) != 0) return null;
            IEnumMoniker e;
            rot.EnumRunning(out e);
            var mk = new IMoniker[1];
            while (e.Next(1, mk, IntPtr.Zero) == 0)
            {
                string name;
                mk[0].GetDisplayName(ctx, null, out name);
                int at = name.IndexOf("SolidWorks_PID_", StringComparison.Ordinal);
                int pid;
                if (at < 0 || !int.TryParse(name.Substring(at + "SolidWorks_PID_".Length), out pid)) continue;
                int procMajor;
                try { procMajor = System.Diagnostics.Process.GetProcessById(pid).MainModule.FileVersionInfo.FileMajorPart; }
                catch (Exception) { continue; }
                if (procMajor != major) continue;
                object o;
                rot.GetObject(mk[0], out o);
                return o as ISldWorks;
            }
            return null;
        }

        public static ISldWorks Get(int major)
        {
            ISldWorks sw = Find(major);
            if (sw == null)
                throw new InvalidOperationException("no running SolidWorks with API " + major + " (" + (major + 1992) + ")");
            return sw;
        }
    }
}
