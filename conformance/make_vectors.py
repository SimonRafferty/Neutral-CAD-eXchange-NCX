"""Generate the container conformance vectors (FORMAT-SPEC.md section 2).

Every vector is a STEP file plus an expected read status (and, for OK, the exact model bytes).
Payloads are written with the *independent* Python writer (ncx.specread) and mutated by hand,
so the C++ core is tested against files it did not produce. Fixtures are real exports:
SolidWorks 2018 and 2026 AP214 (part, assembly) and OpenCASCADE AP242.

Usage:  .venv\\Scripts\\python.exe conformance\\make_vectors.py      (rewrites conformance/vectors/)
"""
import json
import os
import random
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))
from ncx import specread  # noqa: E402

FIX = os.path.join(HERE, "fixtures")
OUT = os.path.join(HERE, "vectors")


def model(n_pad=0, schema="0.1"):
    rnd = random.Random(7)
    m = {"format": "ncx", "schema": schema, "root": "doc:1",
         "documents": [{"id": "doc:1", "kind": "part", "name": "Test", "features": []}]}
    if n_pad:  # incompressible filler so the payload spans several chunks
        m["documents"][0]["properties"] = {"pad": "".join(rnd.choice("0123456789abcdef") for _ in range(n_pad))}
    return json.dumps(m, separators=(",", ":")).encode()


def fixture(name):
    with open(os.path.join(FIX, name), encoding="latin-1", newline="") as f:
        return f.read()


def item_record(text, name):
    """(start, end) of the DESCRIPTIVE_REPRESENTATION_ITEM record named `name`, and its id."""
    m = re.search(r"#(\d+)=DESCRIPTIVE_REPRESENTATION_ITEM\('%s'," % re.escape(name), text)
    end = text.index(";", m.end()) + 1
    return m.start(), end, m.group(1)


def main():
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        os.remove(os.path.join(OUT, f))
    vectors = []

    def emit(name, text, expect, model_bytes=None, note=""):
        path = os.path.join(OUT, name + ".step")
        with open(path, "w", encoding="latin-1", newline="") as f:
            f.write(text)
        v = {"name": name, "file": name + ".step", "expect": expect, "note": note}
        if model_bytes is not None:
            mpath = os.path.join(OUT, name + ".model.json")
            with open(mpath, "wb") as f:
                f.write(model_bytes)
            v["model"] = name + ".model.json"
        vectors.append(v)

    small, big = model(), model(n_pad=20000)
    for fx in sorted(os.listdir(FIX)):
        base = os.path.splitext(fx)[0]
        t = fixture(fx)
        emit(f"no_payload__{base}", t, "NO_PAYLOAD", note="real export, untouched")
        emit(f"ok_small__{base}", specread.write(t, small, generator="vectors"), "OK", small)
        emit(f"ok_multichunk__{base}", specread.write(t, big, chunk_chars=1000), "OK", big,
             note="many 1000-char chunks")

    t = fixture("sw2026_part_ap214.step")
    good = specread.write(t, big, chunk_chars=1000)

    # chunk items written in reverse order: readers must sort by the NCX.nnnn name, not file order
    recs = [item_record(good, "NCX.%04d" % k) for k in range(3)]
    rev = good
    texts = [good[s:e] for s, e, _ in recs]
    for (s, e, _), new in sorted(zip(recs, reversed(texts)), reverse=True):
        rev = rev[:s] + new + rev[e:]
    emit("ok_items_reordered", rev, "OK", big, note="chunk records 0..2 swapped in the file")

    # a long chunk string broken over several lines (Part 21: CR/LF inside strings are ignored)
    s, e, _ = item_record(good, "NCX.0001")
    rec = good[s:e]
    q = rec.index(",'") + 2
    wrapped = rec[:q] + "\r\n".join(rec[q:][i:i + 72] for i in range(0, len(rec) - q, 72))
    emit("ok_wrapped_string", good[:s] + wrapped + good[e:], "OK", big, note="NCX.0001 split over lines")

    # comments around and between the payload records
    emit("ok_comments", good.replace("PROPERTY_DEFINITION_REPRESENTATION(", "/* c;'x' */PROPERTY_DEFINITION_REPRESENTATION("),
         "OK", big)

    # LF-only line endings
    emit("ok_lf_only", good.replace("\r\n", "\n"), "OK", big)

    # payload on a component product as well as the root: the root's wins
    ta = fixture("sw2026_asm_ap214.step")
    comp_pd = re.findall(r"NEXT_ASSEMBLY_USAGE_OCCURRENCE\s*\([^;]*?,\s*#(\d+)\s*,\s*#(\d+)", ta)[0][1]
    other = model(schema="0.1").replace(b'"Test"', b'"Decoy"')
    dec = specread.write(ta, other)
    dec = re.sub(r"PROPERTY_DEFINITION\('NCX','',#\d+\)", f"PROPERTY_DEFINITION('NCX','',#{comp_pd})", dec)
    both = specread.write(dec, small)
    emit("ok_root_preferred", both, "OK", small, note="a decoy payload on a component product is ignored")

    # failures
    s, e, iid = item_record(good, "NCX.0003")
    cut = good[:s] + good[e:]
    cut = cut.replace(f"#{iid},", "", 1)
    emit("chunk_missing", cut, "CHUNK_MISSING")

    s, e, _ = item_record(good, "NCX.0002")
    rec = good[s:e]
    q = rec.index(",'") + 10
    flipped = rec[:q] + ("B" if rec[q] == "A" else "A") + rec[q + 1:]
    emit("digest_mismatch", good[:s] + flipped + good[e:], "DIGEST_MISMATCH", note="one base64 character changed")

    emit("header_wrong_marker", good.replace("'NCX/1;", "'NCX/9;", 1), "HEADER_UNREADABLE")

    newer = model(schema="9.0")
    hdr_newer = specread.write(t, newer).replace(";schema=0.1;", ";schema=9.0;", 1)
    emit("schema_too_new", hdr_newer, "SCHEMA_TOO_NEW", newer, note="readers must refuse a newer major schema")

    with open(os.path.join(OUT, "vectors.json"), "w") as f:
        json.dump(vectors, f, indent=1)
    print(f"{len(vectors)} vectors written to {OUT}")


if __name__ == "__main__":
    main()
