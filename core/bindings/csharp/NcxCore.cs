// P/Invoke binding to ncx_core (core/include/ncx_core.h) for the SolidWorks add-in (.NET Framework 4.8,
// x64). Strings cross as UTF-8 bytes; every buffer the core returns is copied and freed here.
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Ncx
{
    public enum NcxStatus
    {
        Ok = 0, NoPayload = 1, HeaderUnreadable = 2, ChunkMissing = 3, DigestMismatch = 4, SchemaTooNew = 5,
        InvalidJson = 6, InvalidModel = 7, StepUnparseable = 8, BadArgument = 9, Internal = 99
    }

    /// <summary>A call's status, its JSON report and (when the call returns one) its payload bytes.</summary>
    public sealed class NcxResult
    {
        public NcxStatus Status;
        public string ReportJson;
        public byte[] Data;
        public bool Ok { get { return Status == NcxStatus.Ok; } }
    }

    public static class NcxCore
    {
        const string Dll = "ncx_core.dll";

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] static extern IntPtr ncx_version();
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] static extern IntPtr ncx_schema_version();
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        static extern int ncx_payload_write(byte[] step, UIntPtr stepLen, byte[] model, UIntPtr modelLen, byte[] options,
                                            out IntPtr outStep, out UIntPtr outStepLen, out IntPtr outReport);
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        static extern int ncx_payload_read(byte[] step, UIntPtr stepLen, out IntPtr outJson, out UIntPtr outJsonLen,
                                           out IntPtr outReport);
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        static extern int ncx_model_validate(byte[] model, UIntPtr modelLen, out IntPtr outReport);
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        static extern int ncx_fingerprint_compare_ex(byte[] expected, byte[] actual, byte[] options, out IntPtr outReport);
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        static extern int ncx_reference_match(byte[] request, out IntPtr outResult);
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        static extern int ncx_face_compare(byte[] request, out IntPtr outResult);
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] static extern void ncx_free(IntPtr p);

        public static string Version { get { return Utf8Static(ncx_version()); } }
        public static string SchemaVersion { get { return Utf8Static(ncx_schema_version()); } }

        public static NcxResult PayloadWrite(byte[] step, byte[] modelJson, string optionsJson = null)
        {
            IntPtr outStep, rep;
            UIntPtr len;
            int st = ncx_payload_write(step, (UIntPtr)step.Length, modelJson, (UIntPtr)modelJson.Length,
                                       optionsJson == null ? null : Z(optionsJson), out outStep, out len, out rep);
            return new NcxResult { Status = (NcxStatus)st, Data = Take(outStep, (long)len), ReportJson = TakeString(rep) };
        }

        public static NcxResult PayloadRead(byte[] step)
        {
            IntPtr json, rep;
            UIntPtr len;
            int st = ncx_payload_read(step, (UIntPtr)step.Length, out json, out len, out rep);
            return new NcxResult { Status = (NcxStatus)st, Data = Take(json, (long)len), ReportJson = TakeString(rep) };
        }

        public static NcxResult Validate(byte[] modelJson)
        {
            IntPtr rep;
            int st = ncx_model_validate(modelJson, (UIntPtr)modelJson.Length, out rep);
            return new NcxResult { Status = (NcxStatus)st, ReportJson = TakeString(rep) };
        }

        /// <summary>Compare fingerprints. optionsJson picks the tolerances, e.g. {"profile":"exchange"} for
        /// B-rep that went through STEP; null is the native (same-kernel) profile.</summary>
        public static NcxResult FingerprintCompare(string expectedJson, string actualJson, string optionsJson = null)
        {
            IntPtr rep;
            int st = ncx_fingerprint_compare_ex(Z(expectedJson), Z(actualJson), optionsJson == null ? null : Z(optionsJson), out rep);
            return new NcxResult { Status = (NcxStatus)st, ReportJson = TakeString(rep) };
        }

        public static NcxResult MatchReference(string requestJson)
        {
            IntPtr res;
            int st = ncx_reference_match(Z(requestJson), out res);
            return new NcxResult { Status = (NcxStatus)st, ReportJson = TakeString(res) };
        }

        public static NcxResult FaceCompare(string requestJson)
        {
            IntPtr res;
            int st = ncx_face_compare(Z(requestJson), out res);
            return new NcxResult { Status = (NcxStatus)st, ReportJson = TakeString(res) };
        }

        // ------------------------------------------------------------------------- marshalling
        static byte[] Z(string s)  // UTF-8, NUL-terminated
        {
            var b = Encoding.UTF8.GetBytes(s);
            Array.Resize(ref b, b.Length + 1);
            return b;
        }

        static byte[] Take(IntPtr p, long len)
        {
            if (p == IntPtr.Zero) return null;
            var b = new byte[len];
            Marshal.Copy(p, b, 0, (int)len);
            ncx_free(p);
            return b;
        }

        static string TakeString(IntPtr p)
        {
            if (p == IntPtr.Zero) return null;
            string s = Utf8Static(p);
            ncx_free(p);
            return s;
        }

        static string Utf8Static(IntPtr p)
        {
            int n = 0;
            while (Marshal.ReadByte(p, n) != 0) n++;
            var b = new byte[n];
            Marshal.Copy(p, b, 0, n);
            return Encoding.UTF8.GetString(b);
        }
    }
}
