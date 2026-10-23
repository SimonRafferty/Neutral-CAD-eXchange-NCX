// Framework-free self-test of the core's internals. The format-level conformance tests live in
// conformance/ and run from pytest against the built DLL.
#include <cstdio>
#include <string>

#include "codec.hpp"
#include "container.hpp"
#include "matcher.hpp"
#include "ncx_core.h"
#include "step21.hpp"

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

using nlohmann::json;

static const char* kTinyStep =
    "ISO-10303-21;\r\nHEADER;\r\nFILE_DESCRIPTION(('x'),'2;1');\r\nFILE_NAME('a','',(''),(''),'','','');\r\n"
    "FILE_SCHEMA(('AUTOMOTIVE_DESIGN'));\r\nENDSEC;\r\nDATA;\r\n"
    "#1=PRODUCT('P','P','',(#2));\r\n#2=PRODUCT_CONTEXT('',#3,'mechanical');\r\n"
    "#3=APPLICATION_CONTEXT('x');\r\n#4=PRODUCT_DEFINITION_FORMATION('','',#1);\r\n"
    "#5=PRODUCT_DEFINITION('design','it''s; tricky /* not a comment */',#4,#6);\r\n"
    "/* a real comment ; with ' quote */\r\n#6=PRODUCT_DEFINITION_CONTEXT('part definition',#3,'design');\r\n"
    "ENDSEC;\r\nEND-ISO-10303-21;\r\n";

static void test_codec() {
    CHECK(ncx::sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(ncx::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(ncx::sha256_hex(std::string(1000, 'a')) ==
          "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    CHECK(ncx::base64_encode("foobar") == "Zm9vYmFy");
    CHECK(ncx::base64_encode("fo") == "Zm8=");
    CHECK(ncx::base64_decode("Zm9vYg==").value_or("?") == "foob");
    CHECK(!ncx::base64_decode("Zm9v!g=="));
    CHECK(!ncx::base64_decode("Zm=vYg=="));
    std::string big;
    for (int i = 0; i < 100000; ++i) big += std::to_string(i * 7919 % 1000) + ",";
    auto z = ncx::deflate_raw(big);
    CHECK(z.size() < big.size() / 2);
    CHECK(ncx::inflate_raw(z).value_or("") == big);
}

static void test_step21() {
    std::string err;
    auto st = ncx::StepText::parse(kTinyStep, &err);
    CHECK(st.has_value());
    if (!st) return;
    CHECK(st->entities().size() == 6);
    CHECK(st->eol() == "\r\n");
    auto pd = st->of_type("PRODUCT_DEFINITION");
    CHECK(pd.size() == 1);
    auto a = ncx::parse_args(pd[0]->body);
    CHECK(a.size() == 4 && a[1].text == "it's; tricky /* not a comment */" && a[2].ref == 4);
    std::string out = st->rebuild({6}, {"#7=X();"});
    CHECK(out.find("#6=") == std::string::npos && out.find("#7=X();\r\nENDSEC;") != std::string::npos);
    CHECK(!ncx::StepText::parse("not step", &err));
}

static void test_container() {
    std::string model = R"({"format":"ncx","schema":"0.1","root":"d","documents":[{"id":"d","kind":"part"}]})";
    ncx::WriteOptions opt;
    opt.generator = "self'test;gen";
    opt.chunk_chars = 100;
    // make the payload span several chunks
    std::string big = model.substr(0, model.size() - 1) + ",\"pad\":\"";
    unsigned lcg = 12345;  // pseudo-random padding: must not compress into a single chunk
    for (int i = 0; i < 3000; ++i) {
        lcg = lcg * 1103515245u + 12345u;
        big += char('a' + (lcg >> 16) % 26);
    }
    big += "\"}";
    auto w = ncx::write_payload(kTinyStep, big, opt);
    CHECK(w.status == NCX_OK);
    CHECK(w.report["chunks"].get<size_t>() > 3);
    auto r = ncx::read_payload(w.out);
    CHECK(r.status == NCX_OK && r.out == big);
    // re-writing replaces, never duplicates
    auto w2 = ncx::write_payload(w.out, model, opt);
    CHECK(w2.status == NCX_OK && w2.report.value("replaced_previous_payload", false));
    size_t count = 0;
    for (size_t p = w2.out.find("PROPERTY_DEFINITION('NCX'"); p != std::string::npos;
         p = w2.out.find("PROPERTY_DEFINITION('NCX'", p + 1))
        ++count;
    CHECK(count == 1);
    CHECK(ncx::read_payload(w2.out).out == model);
    // a missing chunk and a corrupted chunk are told apart
    std::string cut = w.out;
    size_t p0 = cut.find("'NCX.0001'");
    CHECK(p0 != std::string::npos);
    if (p0 == std::string::npos) { std::printf("%s\n", w.out.c_str()); return; }
    size_t ls = cut.rfind('#', p0), le = cut.find(';', p0);
    std::string id = cut.substr(ls, cut.find('=', ls) - ls);
    cut.erase(ls, le - ls + 1);
    size_t li = cut.find(id + ",");  // drop it from the representation's item list (never the last item here)
    CHECK(li != std::string::npos);
    if (li == std::string::npos) return;
    cut.replace(li, id.size() + 1, "");
    CHECK(ncx::read_payload(cut).status == NCX_CHUNK_MISSING);
    std::string bad = w.out;
    size_t q = bad.find("'NCX.0002','") + 12;
    bad[q] = bad[q] == 'A' ? 'B' : 'A';
    CHECK(ncx::read_payload(bad).status == NCX_DIGEST_MISMATCH);
    CHECK(ncx::read_payload(kTinyStep).status == NCX_NO_PAYLOAD);
    // header stays parseable despite hostile generator text
    auto h = ncx::parse_header(w.report["header"].get<std::string>());
    CHECK(h["gen"] == "self_test_gen" && h["enc"] == "deflate-b64");
}

static void test_matcher() {
    json ref = {{"kind", "face"},
                {"geom", {{"type", "cylinder"}, {"params", {{"radius", 5.0}, {"axis", {0, 0, 1}}}},
                          {"samples", json::array({{5, 0, 0}, {-5, 0, 0}, {0, 5, 5}, {0, -5, 5}})}}}};
    // split cylinder: two halves each cover two samples; a plane covers nothing
    json req = {{"ref", ref},
                {"candidates", json::array({
                     {{"id", "a"}, {"type", "cylinder"}, {"params", {{"radius", 5.0}}}, {"distances", {0, 3.0, 0, 2.0}}},
                     {{"id", "b"}, {"type", "cylinder"}, {"params", {{"radius", 5.0}}}, {"distances", {4.0, 0, 3.0, 0}}},
                     {{"id", "p"}, {"type", "plane"}, {"distances", {1, 1, 1, 1}}}})}};
    json r = ncx::match_reference(req);
    CHECK(r["status"] == "resolved" && r["ids"].size() == 2);
    // a single exact cover
    req["candidates"].push_back({{"id", "w"}, {"type", "cylinder"}, {"distances", {0, 0, 0, 1e-5}}});
    r = ncx::match_reference(req);
    CHECK(r["status"] == "resolved" && r["ids"] == json::array({"w"}));
    // two identical full covers -> ambiguous
    req["candidates"].push_back({{"id", "w2"}, {"type", "cylinder"}, {"distances", {0, 0, 0, 1e-5}}});
    CHECK(ncx::match_reference(req)["status"] == "ambiguous");
    // nothing close -> unresolved with a nearest miss
    json far = {{"ref", ref}, {"candidates", json::array({{{"id", "z"}, {"type", "cylinder"}, {"distances", {1, 1, 1, 1}}}})}};
    r = ncx::match_reference(far);
    CHECK(r["status"] == "unresolved" && r.contains("nearest"));
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    for (auto [name, fn] : {std::pair{"codec", &test_codec}, std::pair{"step21", &test_step21},
                            std::pair{"container", &test_container}, std::pair{"matcher", &test_matcher}}) {
        try {
            fn();
        } catch (const std::exception& e) {
            std::printf("FAIL %s: exception %s\n", name, e.what());
            ++failures;
        }
    }
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "all passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
