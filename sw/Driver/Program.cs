// Developer driver: runs the exporter/replayer against a running SolidWorks out of process.
//
//   ncxsw ping   --sw 34
//   ncxsw export --sw 34 [--flex N] [--results opaque|extended|all] [--opaque Type1,Type2] <part.SLDPRT> <out.step>
//                (N driving dimensions flexed per part; feature results per FORMAT-SPEC 4, "Feature results")
//   ncxsw replay --sw 26 <in.step> <outdir>
using System;
using System.Collections.Generic;
using System.IO;
using Ncx.Sw;
using SolidWorks.Interop.sldworks;

namespace Ncx.Driver
{
    static class Program
    {
        static int Main(string[] args)
        {
            try
            {
                var opts = new Dictionary<string, string>();
                var pos = new List<string>();
                for (int i = 0; i < args.Length; i++)
                    if (args[i].StartsWith("--")) opts[args[i].Substring(2)] = args[++i];
                    else pos.Add(args[i]);
                if (pos.Count == 0) { Console.Error.WriteLine("usage: ncxsw ping|export|replay --sw <api> ..."); return 2; }
                int major = opts.ContainsKey("sw") ? int.Parse(opts["sw"]) : 34;
                ISldWorks sw = SwConnect.Get(major);
                switch (pos[0])
                {
                    case "ping":
                        Console.WriteLine("SolidWorks " + sw.RevisionNumber() + ", core " + NcxCore.Version);
                        return 0;
                    case "probe":
                        return Probe.Run(sw);
                    case "export":
                        return Commands.Export(sw, pos[1], pos[2], opts);
                    case "replay":
                        return Commands.Replay(sw, pos[1], pos[2], opts);
                    default:
                        Console.Error.WriteLine("unknown command " + pos[0]);
                        return 2;
                }
            }
            catch (Exception e)
            {
                Console.Error.WriteLine("ERROR " + e);
                return 1;
            }
        }
    }
}
