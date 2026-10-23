#include <cstdio>
#include "container.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "codec.hpp"
#include "fingerprint.hpp"
#include "notice.hpp"
#include "step21.hpp"
#include "version.hpp"

namespace ncx {

using nlohmann::json;

namespace {

bool is_product_definition(const Entity* e) {
    return e && (e->type == "PRODUCT_DEFINITION" || e->type == "PRODUCT_DEFINITION_WITH_ASSOCIATED_DOCUMENTS");
}

// Root product definitions: never the related (component) side of an assembly usage.
std::vector<long> root_product_definitions(const StepText& st) {
    std::set<long> used;
    for (const char* t : {"NEXT_ASSEMBLY_USAGE_OCCURRENCE", "ASSEMBLY_COMPONENT_USAGE"})
        for (const Entity* e : st.of_type(t)) {
            auto a = parse_args(e->body);
            if (a.size() > 4 && a[4].kind == Arg::Ref) used.insert(a[4].ref);
        }
    std::vector<long> roots;
    for (const auto& e : st.entities())
        if (is_product_definition(&e) && !used.count(e.id)) roots.push_back(e.id);
    return roots;
}

// Everything that belongs to one NCX property: PD, PDR, REPRESENTATION, its items, GPA, GP,
// and the representation context if nothing else uses it.
std::set<long> payload_entities(const StepText& st, long pd) {
    std::set<long> ids{pd};
    long ctx = 0;
    for (const Entity* e : st.of_type("PROPERTY_DEFINITION_REPRESENTATION")) {
        auto a = parse_args(e->body);
        if (a.size() >= 2 && a[0].kind == Arg::Ref && a[0].ref == pd && a[1].kind == Arg::Ref) {
            ids.insert(e->id);
            ids.insert(a[1].ref);
            if (const Entity* rep = st.find(a[1].ref)) {
                auto r = parse_args(rep->body);
                if (r.size() >= 3 && r[1].kind == Arg::List)
                    for (const auto& it : r[1].items)
                        if (it.kind == Arg::Ref) ids.insert(it.ref);
                if (r.size() >= 3 && r[2].kind == Arg::Ref) ctx = r[2].ref;
            }
        }
    }
    for (const Entity* e : st.of_type("GENERAL_PROPERTY_ASSOCIATION")) {
        auto a = parse_args(e->body);
        if (a.size() >= 4 && a[3].kind == Arg::Ref && a[3].ref == pd) {
            ids.insert(e->id);
            if (a[2].kind == Arg::Ref) ids.insert(a[2].ref);
        }
    }
    if (ctx && !st.referenced_ids(ids).count(ctx)) ids.insert(ctx);
    return ids;
}

std::vector<long> ncx_property_definitions(const StepText& st) {
    std::vector<long> out;
    for (const Entity* e : st.of_type("PROPERTY_DEFINITION")) {
        auto a = parse_args(e->body);
        if (a.size() >= 3 && a[0].kind == Arg::Str && a[0].text == "NCX" && a[2].kind == Arg::Ref &&
            is_product_definition(st.find(a[2].ref)))
            out.push_back(e->id);
    }
    return out;
}

std::string sanitize(std::string s) {
    // header values must not break the k=v;k=v syntax or need STEP escaping
    for (char& c : s)
        if (c == ';' || c == '\'' || c == '\\' || static_cast<unsigned char>(c) < 0x20 ||
            static_cast<unsigned char>(c) > 0x7E)
            c = '_';
    return s;
}

int major_of(const std::string& v) {
    try { return std::stoi(v.substr(0, v.find('.'))); } catch (...) { return -1; }
}

}  // namespace

std::map<std::string, std::string> parse_header(const std::string& header) {
    std::map<std::string, std::string> out;
    size_t i = header.find(';');
    while (i != std::string::npos && i < header.size()) {
        size_t j = header.find(';', i + 1);
        std::string kv = header.substr(i + 1, j == std::string::npos ? std::string::npos : j - i - 1);
        size_t eq = kv.find('=');
        if (eq != std::string::npos && eq > 0) out[kv.substr(0, eq)] = kv.substr(eq + 1);
        i = j;
    }
    return out;
}

Result write_payload(std::string step_text, const std::string& model_json, const WriteOptions& opt) {
    Result r;
    std::string err;
    auto st = StepText::parse(std::move(step_text), &err);
    if (!st) {
        r.status = NCX_STEP_UNPARSEABLE;
        r.report["message"] = err;
        return r;
    }
    json model;
    try {
        model = json::parse(model_json);
    } catch (const std::exception& e) {
        r.status = NCX_INVALID_JSON;
        r.report["message"] = e.what();
        return r;
    }
    auto roots = root_product_definitions(*st);
    if (roots.empty()) {
        r.status = NCX_STEP_UNPARSEABLE;
        r.report["message"] = "no root PRODUCT_DEFINITION to attach the payload to";
        return r;
    }
    if (roots.size() > 1) r.report["warnings"].push_back("several root products; payload attached to #" + std::to_string(roots[0]));

    // remove any earlier payload so a re-export never carries two
    std::set<long> remove;
    for (long pd : ncx_property_definitions(*st)) {
        auto ids = payload_entities(*st, pd);
        remove.insert(ids.begin(), ids.end());
    }
    if (!remove.empty()) r.report["replaced_previous_payload"] = true;

    // the notice body for readers without an NCX add-in (records first; the header names it)
    long id = st->max_id();
    NoticeEdit notice;
    if (opt.notice) {
        std::string text = opt.notice_text.empty() ? kDefaultNotice : opt.notice_text;
        for (char& c : text)   // STEP strings: printable ASCII ('' quoting is done when written)
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) c = '_';
        // an earlier notice is the one the earlier header names; names compare in their header form, with
        // path characters as an assembly component's name has them. Notices before 0.1 began "READ ME".
        auto norm = [](std::string s) {
            s = sanitize(s);
            for (char& c : s)
                if (std::string("/\\:*?\"<>|").find(c) != std::string::npos) c = '_';
            return s;
        };
        std::set<std::string> earlier;
        for (const Entity* e : st->of_type("DESCRIPTIVE_REPRESENTATION_ITEM")) {
            auto a = parse_args(e->body);
            if (a.size() >= 2 && a[0].kind == Arg::Str && a[0].text == "NCX" && a[1].kind == Arg::Str) {
                auto h = parse_header(a[1].text);
                if (h.count("notice")) earlier.insert(norm(h["notice"]));
            }
        }
        auto is_earlier = [&](const std::string& name) { return earlier.count(norm(name)) > 0 || name.rfind("READ ME", 0) == 0; };
        notice = plan_notice(*st, model, text, is_earlier, id);
        remove.insert(notice.remove.begin(), notice.remove.end());
        if (notice.name.empty()) r.report["warnings"].push_back("no notice body: " + notice.skipped);
    }

    std::string compressed = deflate_raw(model_json);
    std::string b64 = base64_encode(compressed);
    size_t chunk = std::clamp<size_t>(opt.chunk_chars, 100, kMaxChunkChars);
    size_t n = b64.empty() ? 0 : (b64.size() + chunk - 1) / chunk;

    std::string schema = model.contains("schema") && model["schema"].is_string() ? model["schema"].get<std::string>() : "";
    std::string fp = "none";
    if (model.contains("fingerprint") && model["fingerprint"].is_object()) fp = fingerprint_hash(model["fingerprint"]);
    std::string header = std::string(kContainerMarker) + ";schema=" + sanitize(schema) + ";enc=deflate-b64;chunks=" +
                         std::to_string(n) + ";b64len=" + std::to_string(b64.size()) + ";jsonlen=" +
                         std::to_string(model_json.size()) + ";sha256=" + sha256_hex(model_json) + ";fp=" + fp +
                         ";core=" + kVersion;
    if (!notice.name.empty()) {
        header += ";notice=" + sanitize(notice.name);   // ' and ; become _ (section 2.2)
        // its box, so a reader whose STEP importer drops body names can still find it (section 2.6)
        char box[200];
        std::snprintf(box, sizeof box, ";noticebox=%.3f,%.3f,%.3f,%.3f,%.3f,%.3f", notice.box[0], notice.box[1],
                      notice.box[2], notice.box[3], notice.box[4], notice.box[5]);
        header += box;
    }
    std::string gen = sanitize(opt.generator);
    size_t room = kMaxHeaderChars - std::min(kMaxHeaderChars, header.size() + 5);
    if (!gen.empty()) header += ";gen=" + gen.substr(0, room);

    std::vector<std::string> recs = notice.append;
    auto add = [&](const std::string& body) {
        ++id;
        recs.push_back("#" + std::to_string(id) + "=" + body + ";");
        return id;
    };
    long ctx = add("REPRESENTATION_CONTEXT('','')");
    long gp = add("GENERAL_PROPERTY('NCX','NCX','')");
    long pd = add("PROPERTY_DEFINITION('NCX','',#" + std::to_string(roots[0]) + ")");
    add("GENERAL_PROPERTY_ASSOCIATION('','',#" + std::to_string(gp) + ",#" + std::to_string(pd) + ")");
    std::string items = "#" + std::to_string(add("DESCRIPTIVE_REPRESENTATION_ITEM('NCX'," + step_string(header) + ")"));
    for (size_t k = 0; k < n; ++k) {
        char name[32];
        std::snprintf(name, sizeof name, "NCX.%04zu", k);
        long it = add("DESCRIPTIVE_REPRESENTATION_ITEM('" + std::string(name) + "','" + b64.substr(k * chunk, chunk) + "')");
        items += ",#" + std::to_string(it);
    }
    long rep = add("REPRESENTATION('NCX',(" + items + "),#" + std::to_string(ctx) + ")");
    add("PROPERTY_DEFINITION_REPRESENTATION(#" + std::to_string(pd) + ",#" + std::to_string(rep) + ")");

    r.out = st->rebuild(remove, recs, notice.prepend);
    if (!notice.prepend.empty()) {
        // the assembly notice's records also get the lowest ids: some importers (SolidWorks 2018) list
        // components in id order, others (2026) in file order; both then show the notice first
        const long first = st->max_id() + 1, k = static_cast<long>(notice.prepend.size());
        r.out = renumber(r.out, [&](long n) { return n >= first && n < first + k ? n - first + 1 : n < first ? n + k : n; });
    }
    r.report["header"] = header;
    if (!notice.name.empty()) r.report["notice"] = notice.name;
    r.report["chunks"] = n;
    r.report["root_product_definition"] = roots[0];
    r.report["compressed_bytes"] = compressed.size();
    return r;
}

Result read_payload(std::string step_text) {
    Result r;
    std::string err;
    auto st = StepText::parse(std::move(step_text), &err);
    if (!st) {
        r.status = NCX_STEP_UNPARSEABLE;
        r.report["message"] = err;
        return r;
    }
    auto pds = ncx_property_definitions(*st);
    if (pds.empty()) {
        r.status = NCX_NO_PAYLOAD;
        r.report["message"] = "no NCX property on any product definition";
        return r;
    }
    auto roots = root_product_definitions(*st);
    long pd = pds[0];
    for (long cand : pds) {  // prefer the one on the root product
        auto a = parse_args(st->find(cand)->body);
        if (std::find(roots.begin(), roots.end(), a[2].ref) != roots.end()) { pd = cand; break; }
    }
    if (pds.size() > 1) r.report["warnings"].push_back("several NCX properties; read #" + std::to_string(pd));

    // follow PDR -> REPRESENTATION -> items
    std::string header;
    std::map<long, std::string> chunks;  // index -> text
    bool have_rep = false;
    for (const Entity* e : st->of_type("PROPERTY_DEFINITION_REPRESENTATION")) {
        auto a = parse_args(e->body);
        if (a.size() < 2 || a[0].kind != Arg::Ref || a[0].ref != pd || a[1].kind != Arg::Ref) continue;
        const Entity* rep = st->find(a[1].ref);
        if (!rep) continue;
        auto ra = parse_args(rep->body);
        if (ra.size() < 2 || ra[1].kind != Arg::List) continue;
        have_rep = true;
        for (const auto& it : ra[1].items) {
            if (it.kind != Arg::Ref) continue;
            const Entity* dri = st->find(it.ref);
            if (!dri) continue;
            auto da = parse_args(dri->body);
            if (da.size() < 2 || da[0].kind != Arg::Str || da[1].kind != Arg::Str) continue;
            const std::string& nm = da[0].text;
            if (nm == "NCX") header = da[1].text;
            else if (nm.size() > 4 && nm.compare(0, 4, "NCX.") == 0 &&
                     std::all_of(nm.begin() + 4, nm.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
                chunks[std::stol(nm.substr(4))] = da[1].text;
        }
    }
    if (!have_rep || header.compare(0, std::char_traits<char>::length(kContainerMarker), kContainerMarker) != 0) {
        r.status = NCX_HEADER_UNREADABLE;
        r.report["message"] = have_rep ? "header item missing or not " + std::string(kContainerMarker) : "NCX property has no representation";
        return r;
    }
    auto h = parse_header(header);
    r.report["header"] = header;
    r.report["fields"] = h;
    size_t n = 0, b64len = 0, jsonlen = 0;
    try {
        n = std::stoul(h.at("chunks"));
        b64len = std::stoul(h.at("b64len"));
        jsonlen = std::stoul(h.at("jsonlen"));
        (void)h.at("sha256");
    } catch (...) {
        r.status = NCX_HEADER_UNREADABLE;
        r.report["message"] = "header lacks chunks/b64len/jsonlen/sha256";
        return r;
    }
    std::vector<size_t> missing;
    for (size_t k = 0; k < n; ++k)
        if (!chunks.count(static_cast<long>(k))) missing.push_back(k);
    if (!missing.empty()) {
        r.status = NCX_CHUNK_MISSING;
        r.report["missing"] = missing;
        r.report["message"] = std::to_string(missing.size()) + " of " + std::to_string(n) + " chunks missing";
        return r;
    }
    std::string b64;
    b64.reserve(b64len);
    for (size_t k = 0; k < n; ++k) b64 += chunks[static_cast<long>(k)];
    std::optional<std::string> raw;
    if (b64.size() == b64len)
        if (auto bin = base64_decode(b64)) raw = inflate_raw(*bin);
    if (!raw || raw->size() != jsonlen || sha256_hex(*raw) != h["sha256"]) {
        r.status = NCX_DIGEST_MISMATCH;
        r.report["message"] = "payload fails its length/SHA-256 check";
        return r;
    }
    std::string schema = h.count("schema") ? h["schema"] : "";
    if (major_of(schema) > major_of(kSchemaVersion)) {
        r.status = NCX_SCHEMA_TOO_NEW;
        r.report["message"] = "payload schema " + schema + " is newer than this core's " + kSchemaVersion;
        r.out = std::move(*raw);  // still handed back: a host may show what it can
        return r;
    }
    r.out = std::move(*raw);
    return r;
}

}  // namespace ncx
