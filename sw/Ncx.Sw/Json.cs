// Minimal JSON writer and reader with no third-party dependency (no NuGet on this machine).
// Adapted from the AsmFEA add-in's Json.cs (same author): objects keep insertion order so payloads
// and reports diff cleanly; objects parse to JObj, arrays to List<object>, numbers to double.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace Ncx.Sw
{
    /// <summary>A JSON object that keeps insertion order.</summary>
    public class JObj : IEnumerable<KeyValuePair<string, object>>
    {
        private readonly List<KeyValuePair<string, object>> items = new List<KeyValuePair<string, object>>();

        public object this[string key]
        {
            get
            {
                foreach (var kv in items)
                    if (kv.Key == key) return kv.Value;
                return null;
            }
            set
            {
                for (int i = 0; i < items.Count; i++)
                    if (items[i].Key == key) { items[i] = new KeyValuePair<string, object>(key, value); return; }
                items.Add(new KeyValuePair<string, object>(key, value));
            }
        }

        /// <summary>Collection-initialiser support: new JObj { { "a", 1 }, { "b", "x" } }.</summary>
        public void Add(string key, object value) { this[key] = value; }

        public bool Has(string key)
        {
            foreach (var kv in items)
                if (kv.Key == key) return true;
            return false;
        }

        public bool Remove(string key) { return items.RemoveAll(kv => kv.Key == key) > 0; }

        // typed accessors for parsed JSON (numbers are double, arrays List<object>)
        public string Str(string key, string dflt = null) { return this[key] as string ?? dflt; }
        public double Num(string key, double dflt = 0) { var v = this[key]; return v is double ? (double)v : dflt; }
        public bool Bool(string key, bool dflt = false) { var v = this[key]; return v is bool ? (bool)v : dflt; }
        public JObj Obj(string key) { return this[key] as JObj; }
        public List<object> Arr(string key) { return this[key] as List<object> ?? new List<object>(); }
        public double[] Vec(string key)
        {
            var d = this[key] as double[];   // built in code rather than parsed
            if (d != null) return d;
            var a = this[key] as List<object>;
            if (a == null) return null;
            var r = new double[a.Count];
            for (int i = 0; i < a.Count; i++) r[i] = Convert.ToDouble(a[i], CultureInfo.InvariantCulture);
            return r;
        }

        public IEnumerator<KeyValuePair<string, object>> GetEnumerator() { return items.GetEnumerator(); }
        IEnumerator IEnumerable.GetEnumerator() { return items.GetEnumerator(); }
    }

    public static class Json
    {
        public static string Write(object value, bool compact = false)
        {
            var sb = new StringBuilder();
            WriteValue(sb, value, 0, compact);
            if (!compact) sb.Append('\n');
            return sb.ToString();
        }

        private static void Indent(StringBuilder sb, int level, bool compact) { if (!compact) sb.Append(' ', level * 1); }

        private static void WriteValue(StringBuilder sb, object v, int level, bool compact)
        {
            if (v == null) { sb.Append("null"); return; }
            if (v is string) { WriteString(sb, (string)v); return; }
            if (v is bool) { sb.Append((bool)v ? "true" : "false"); return; }
            if (v is double || v is float) { WriteNumber(sb, Convert.ToDouble(v)); return; }
            if (v is int || v is long || v is short) { sb.Append(Convert.ToInt64(v).ToString(CultureInfo.InvariantCulture)); return; }
            if (v is JObj) { WriteObject(sb, (JObj)v, level, compact); return; }
            if (v is double[]) { WriteNumberArray(sb, (double[])v); return; }
            if (v is IEnumerable) { WriteArray(sb, (IEnumerable)v, level, compact); return; }
            throw new ArgumentException("cannot write " + v.GetType().Name + " as JSON");
        }

        private static void WriteNumber(StringBuilder sb, double d)
        {
            if (double.IsNaN(d) || double.IsInfinity(d))
                throw new ArgumentException("JSON has no NaN or infinity");
            sb.Append(d.ToString("R", CultureInfo.InvariantCulture));
        }

        private static void WriteNumberArray(StringBuilder sb, double[] a)
        {
            sb.Append('[');
            for (int i = 0; i < a.Length; i++)
            {
                if (i > 0) sb.Append(',');
                WriteNumber(sb, a[i]);
            }
            sb.Append(']');
        }

        private static void WriteObject(StringBuilder sb, JObj o, int level, bool compact)
        {
            sb.Append('{');
            bool first = true;
            foreach (var kv in o)
            {
                if (kv.Value == null) continue;          // absent and null mean the same to the schema
                if (!first) sb.Append(',');
                if (!compact) sb.Append('\n');
                first = false;
                Indent(sb, level + 1, compact);
                WriteString(sb, kv.Key);
                sb.Append(compact ? ":" : ": ");
                WriteValue(sb, kv.Value, level + 1, compact);
            }
            if (!first && !compact) { sb.Append('\n'); Indent(sb, level, compact); }
            sb.Append('}');
        }

        private static void WriteArray(StringBuilder sb, IEnumerable a, int level, bool compact)
        {
            sb.Append('[');
            bool first = true;
            foreach (var item in a)
            {
                if (!first) sb.Append(',');
                if (!compact) sb.Append('\n');
                first = false;
                Indent(sb, level + 1, compact);
                WriteValue(sb, item, level + 1, compact);
            }
            if (!first && !compact) { sb.Append('\n'); Indent(sb, level, compact); }
            sb.Append(']');
        }

        private static void WriteString(StringBuilder sb, string s)
        {
            sb.Append('"');
            foreach (char c in s)
            {
                switch (c)
                {
                    case '"': sb.Append("\\\""); break;
                    case '\\': sb.Append("\\\\"); break;
                    case '\n': sb.Append("\\n"); break;
                    case '\r': sb.Append("\\r"); break;
                    case '\t': sb.Append("\\t"); break;
                    default:
                        if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4"));
                        else sb.Append(c);
                        break;
                }
            }
            sb.Append('"');
        }

        // ------------------------------------------------------------------------ reader
        public static object Parse(string text)
        {
            int i = 0;
            object v = ParseValue(text, ref i);
            SkipWs(text, ref i);
            if (i != text.Length) throw new FormatException("trailing characters at " + i);
            return v;
        }

        private static void SkipWs(string s, ref int i)
        {
            while (i < s.Length && char.IsWhiteSpace(s[i])) i++;
        }

        private static object ParseValue(string s, ref int i)
        {
            SkipWs(s, ref i);
            if (i >= s.Length) throw new FormatException("unexpected end of JSON");
            char c = s[i];
            if (c == '{') return ParseObject(s, ref i);
            if (c == '[') return ParseArray(s, ref i);
            if (c == '"') return ParseString(s, ref i);
            if (s.Length - i >= 4 && string.CompareOrdinal(s, i, "true", 0, 4) == 0) { i += 4; return true; }
            if (s.Length - i >= 5 && string.CompareOrdinal(s, i, "false", 0, 5) == 0) { i += 5; return false; }
            if (s.Length - i >= 4 && string.CompareOrdinal(s, i, "null", 0, 4) == 0) { i += 4; return null; }
            int start = i;
            while (i < s.Length && "+-0123456789.eE".IndexOf(s[i]) >= 0) i++;
            if (start == i) throw new FormatException("unexpected '" + c + "' at " + i);
            return double.Parse(s.Substring(start, i - start), NumberStyles.Float, CultureInfo.InvariantCulture);
        }

        private static JObj ParseObject(string s, ref int i)
        {
            var o = new JObj();
            i++;
            SkipWs(s, ref i);
            if (s[i] == '}') { i++; return o; }
            while (true)
            {
                SkipWs(s, ref i);
                string key = ParseString(s, ref i);
                SkipWs(s, ref i);
                if (s[i] != ':') throw new FormatException("expected ':' at " + i);
                i++;
                o[key] = ParseValue(s, ref i);
                SkipWs(s, ref i);
                if (s[i] == ',') { i++; continue; }
                if (s[i] == '}') { i++; return o; }
                throw new FormatException("expected ',' or '}' at " + i);
            }
        }

        private static List<object> ParseArray(string s, ref int i)
        {
            var a = new List<object>();
            i++;
            SkipWs(s, ref i);
            if (s[i] == ']') { i++; return a; }
            while (true)
            {
                a.Add(ParseValue(s, ref i));
                SkipWs(s, ref i);
                if (s[i] == ',') { i++; continue; }
                if (s[i] == ']') { i++; return a; }
                throw new FormatException("expected ',' or ']' at " + i);
            }
        }

        private static string ParseString(string s, ref int i)
        {
            if (s[i] != '"') throw new FormatException("expected string at " + i);
            i++;
            var sb = new StringBuilder();
            while (s[i] != '"')
            {
                char c = s[i++];
                if (c != '\\') { sb.Append(c); continue; }
                char e = s[i++];
                switch (e)
                {
                    case 'n': sb.Append('\n'); break;
                    case 'r': sb.Append('\r'); break;
                    case 't': sb.Append('\t'); break;
                    case 'b': sb.Append('\b'); break;
                    case 'f': sb.Append('\f'); break;
                    case 'u': sb.Append((char)Convert.ToInt32(s.Substring(i, 4), 16)); i += 4; break;
                    default: sb.Append(e); break;     // \" \\ \/
                }
            }
            i++;
            return sb.ToString();
        }
    }
}
