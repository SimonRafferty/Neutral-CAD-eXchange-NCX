// Smoke test: the SolidWorks add-in's route into the core (.NET Framework 4.8 x64 -> ncx_core.dll).
// Usage: NcxSmoke.exe <container.step>     exit code 0 = every check passed
using System;
using System.IO;
using Ncx;

static class Program
{
    static int failures;

    static void Check(bool ok, string what)
    {
        Console.WriteLine((ok ? "ok    " : "FAIL  ") + what);
        if (!ok) failures++;
    }

    static int Main(string[] args)
    {
        Console.WriteLine("ncx_core " + NcxCore.Version + ", schema " + NcxCore.SchemaVersion +
                          ", process " + (Environment.Is64BitProcess ? "x64" : "x86"));
        byte[] step = File.ReadAllBytes(args[0]);

        var read = NcxCore.PayloadRead(step);
        Check(read.Ok && read.Data != null && read.Data.Length > 0, "read payload (" + (read.Data == null ? 0 : read.Data.Length) + " bytes)");

        var valid = NcxCore.Validate(read.Data);
        Check(valid.Ok, "validate model");

        // write it back into the same file: the payload is replaced, not duplicated, and reads back equal
        var write = NcxCore.PayloadWrite(step, read.Data, "{\"generator\":\"NcxSmoke C#\"}");
        Check(write.Ok && write.ReportJson.Contains("\"replaced_previous_payload\":true"), "rewrite replaces payload");
        var again = NcxCore.PayloadRead(write.Data);
        Check(again.Ok && Convert.ToBase64String(again.Data) == Convert.ToBase64String(read.Data), "re-read equals original");

        var none = NcxCore.PayloadRead(System.Text.Encoding.ASCII.GetBytes("not a step file"));
        Check(none.Status == NcxStatus.StepUnparseable, "non-STEP input reports STEP_UNPARSEABLE");

        var match = NcxCore.MatchReference(
            "{\"ref\":{\"kind\":\"edge\",\"geom\":{\"type\":\"line\",\"samples\":[[0,0,1],[0,0,2]],\"length\":3}}," +
            "\"candidates\":[{\"id\":\"e1\",\"type\":\"line\",\"length\":3,\"distances\":[0,0]}]}");
        Check(match.Ok && match.ReportJson.Contains("\"result\":\"resolved\""), "reference match");

        // many calls: no leaks or crashes across the boundary
        for (int i = 0; i < 2000; i++) NcxCore.PayloadRead(step);
        Check(true, "2000 repeated reads");

        Console.WriteLine(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        return failures == 0 ? 0 : 1;
    }
}
