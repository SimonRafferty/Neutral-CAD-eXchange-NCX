"""Independent implementation of the NCX container (FORMAT-SPEC.md section 2), written from the
spec and sharing no code with the C++ core. The conformance tests check that both agree.

read(step_text)  -> (status, header_fields, model_json_bytes | None)
write(step_text, model_json_bytes, chunk_chars=8000, generator="") -> step_text
"""
import base64
import hashlib
import re
import zlib

SUPPORTED_SCHEMA_MAJOR = 0
ENT = re.compile(r"#(\d+)\s*=\s*([A-Z0-9_]*)\s*(\(.*)", re.S)


def _records(text):
    """(id, type, body) for each DATA-section record; strings and comments respected."""
    m = re.search(r"(?<![A-Z0-9_])DATA\s*;", text)
    if not m:
        return None, None, None
    i, n, out, start = m.end(), len(text), [], None
    buf_start = i
    in_str = False
    while i < n:
        c = text[i]
        if in_str:
            if c == "'":
                if i + 1 < n and text[i + 1] == "'":
                    i += 2
                    continue
                in_str = False
        elif c == "'":
            in_str = True
        elif text.startswith("/*", i):
            i = text.index("*/", i) + 2
            continue
        elif c == ";":
            rec = text[buf_start:i].strip()
            rec = re.sub(r"/\*.*?\*/", "", rec, flags=re.S).strip()
            if rec.startswith("ENDSEC"):
                return out, buf_start, i
            mm = ENT.match(rec)
            if mm:
                out.append((int(mm.group(1)), mm.group(2), mm.group(3)))
            buf_start = i + 1
        i += 1
    return None, None, None


def _string_end(body, i):
    """Index just past the string literal starting at body[i] == "'"."""
    j = i + 1
    while True:
        if body[j] == "'":
            if j + 1 < len(body) and body[j + 1] == "'":
                j += 2
                continue
            return j + 1
        j += 1


def _args(body):
    """Top-level args of '(a,b,...)': strings decoded, refs as int, lists as lists of refs, others raw."""
    out, i, n = [], 1, len(body)
    while i < n:
        c = body[i]
        if c in ", \r\n\t":
            i += 1
        elif c == ")":
            break
        elif c == "'":
            j = _string_end(body, i)
            out.append(body[i + 1:j - 1].replace("''", "'").replace("\r", "").replace("\n", ""))
            i = j
        elif c == "#":
            m = re.match(r"#(\d+)", body[i:])
            out.append(int(m.group(1)))
            i += len(m.group(0))
        else:  # list or typed/enum/number: scan to the matching close at this level
            depth, j = 0, i
            while j < n:
                d = body[j]
                if d == "'":
                    j = _string_end(body, j)
                    continue
                if d == "(":
                    depth += 1
                elif d == ")":
                    if depth == 0:
                        break
                    depth -= 1
                    if depth == 0 and c == "(":
                        j += 1
                        break
                elif d == "," and depth == 0:
                    break
                j += 1
            tok = body[i:j]
            out.append([int(x) for x in re.findall(r"#(\d+)", tok)] if c == "(" else tok.strip())
            i = j
    return out


def _roots(recs):
    used = {_args(b)[4] for _, t, b in recs if t == "NEXT_ASSEMBLY_USAGE_OCCURRENCE"}
    return [i for i, t, _ in recs if t == "PRODUCT_DEFINITION" and i not in used]


def read(text):
    recs, _, _ = _records(text)
    if recs is None:
        return "STEP_UNPARSEABLE", {}, None
    by_id = {i: (t, b) for i, t, b in recs}
    pds = [i for i, t, b in recs if t == "PROPERTY_DEFINITION" and _args(b)[0] == "NCX"
           and by_id.get(_args(b)[2], ("",))[0] == "PRODUCT_DEFINITION"]
    if not pds:
        return "NO_PAYLOAD", {}, None
    roots = _roots(recs)
    pd = next((p for p in pds if _args(by_id[p][1])[2] in roots), pds[0])
    rep = next((_args(b)[1] for _, t, b in recs if t == "PROPERTY_DEFINITION_REPRESENTATION" and _args(b)[0] == pd), None)
    if rep is None:
        return "HEADER_UNREADABLE", {}, None
    header, chunks = None, {}
    for item in _args(by_id[rep][1])[1]:
        name, value = _args(by_id[item][1])[:2]
        if name == "NCX":
            header = value
        elif re.fullmatch(r"NCX\.\d+", name):
            chunks[int(name[4:])] = value
    if not header or not header.startswith("NCX/1"):
        return "HEADER_UNREADABLE", {}, None
    fields = dict(kv.split("=", 1) for kv in header.split(";")[1:] if "=" in kv)
    n = int(fields["chunks"])
    if any(k not in chunks for k in range(n)):
        return "CHUNK_MISSING", fields, None
    b64 = "".join(chunks[k] for k in range(n))
    try:
        raw = zlib.decompress(base64.b64decode(b64, validate=True), -15)  # wbits -15 = raw DEFLATE
    except Exception:
        return "DIGEST_MISMATCH", fields, None
    if len(b64) != int(fields["b64len"]) or len(raw) != int(fields["jsonlen"]) or \
            hashlib.sha256(raw).hexdigest() != fields["sha256"]:
        return "DIGEST_MISMATCH", fields, None
    if int(fields.get("schema", "0").split(".")[0] or 0) > SUPPORTED_SCHEMA_MAJOR:
        return "SCHEMA_TOO_NEW", fields, raw
    return "OK", fields, raw


def write(text, model_json, chunk_chars=8000, generator=""):
    """Append a payload (assumes none present; the conformance tests cover replacement via the core)."""
    recs, _, data_end_semicolon = _records(text)
    root = _roots(recs)[0]
    nxt = max(i for i, _, _ in recs) + 1
    comp = zlib.compressobj(9, zlib.DEFLATED, -15)
    b64 = base64.b64encode(comp.compress(model_json) + comp.flush()).decode()
    chunks = [b64[i:i + chunk_chars] for i in range(0, len(b64), chunk_chars)]
    header = (f"NCX/1;schema=0.1;enc=deflate-b64;chunks={len(chunks)};b64len={len(b64)};"
              f"jsonlen={len(model_json)};sha256={hashlib.sha256(model_json).hexdigest()};fp=none;gen={generator}")
    lines, ids = [], {}

    def add(body):
        nonlocal nxt
        lines.append(f"#{nxt}={body};")
        nxt += 1
        return nxt - 1

    ctx = add("REPRESENTATION_CONTEXT('','')")
    gp = add("GENERAL_PROPERTY('NCX','NCX','')")
    pd = add(f"PROPERTY_DEFINITION('NCX','',#{root})")
    add(f"GENERAL_PROPERTY_ASSOCIATION('','',#{gp},#{pd})")
    items = [add(f"DESCRIPTIVE_REPRESENTATION_ITEM('NCX','{header}')")]
    items += [add(f"DESCRIPTIVE_REPRESENTATION_ITEM('NCX.{k:04d}','{c}')") for k, c in enumerate(chunks)]
    rep = add(f"REPRESENTATION('NCX',({','.join('#%d' % i for i in items)}),#{ctx})")
    add(f"PROPERTY_DEFINITION_REPRESENTATION(#{pd},#{rep})")
    endsec = text.rfind("ENDSEC", 0, data_end_semicolon + 1)
    eol = "\r\n" if "\r\n" in text else "\n"
    return text[:endsec] + eol.join(lines) + eol + text[endsec:]
