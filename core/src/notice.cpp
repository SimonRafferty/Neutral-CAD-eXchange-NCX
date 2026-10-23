#include "notice.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>

namespace ncx {

using nlohmann::json;

namespace {

using P3 = std::array<double, 3>;

std::string num(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.6f", v);
    return b;
}

std::string ref(long id) { return "#" + std::to_string(id); }

// ------------------------------------------------------------------------------------------- the font
// 5x7 pixel capitals, digits and a little punctuation; rows top to bottom. Lower case prints as capitals,
// anything else as '?'.
const std::map<char, std::array<const char*, 7>>& font() {
    static const std::map<char, std::array<const char*, 7>> f = {
        {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
        {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
        {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
        {'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
        {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
        {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
        {'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."}},
        {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
        {'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
        {'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
        {'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
        {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
        {'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
        {'N', {"#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"}},
        {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
        {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
        {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
        {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
        {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
        {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
        {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
        {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
        {'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."}},
        {'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
        {'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
        {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
        {'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
        {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
        {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
        {'3', {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."}},
        {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
        {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
        {'6', {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}},
        {'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
        {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
        {'9', {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."}},
        {'-', {".....", ".....", ".....", ".###.", ".....", ".....", "....."}},
        {'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
        {',', {".....", ".....", ".....", ".....", ".##..", "..#..", ".#..."}},
        {':', {".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}},
        {'/', {".....", "....#", "...#.", "..#..", ".#...", "#....", "....."}},
        {'_', {".....", ".....", ".....", ".....", ".....", ".....", "#####"}},
        {'(', {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."}},
        {')', {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."}},
        {'?', {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."}},
        {'\'', {"..#..", "..#..", ".#...", ".....", ".....", ".....", "....."}},
        {'"', {".#.#.", ".#.#.", ".....", ".....", ".....", ".....", "....."}},
        {' ', {".....", ".....", ".....", ".....", ".....", ".....", "....."}},
    };
    return f;
}

constexpr int kGlyphW = 5, kGlyphH = 7, kAdvance = 6, kLineAdvance = 9, kMargin = 2;

// Word-wrap the text into lines of at most `width` characters.
std::vector<std::string> wrap(const std::string& text, size_t width) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flush_word = [&]() {
        while (word.size() > width) {   // a word longer than a line is split
            if (!line.empty()) { lines.push_back(line); line.clear(); }
            lines.push_back(word.substr(0, width));
            word.erase(0, width);
        }
        if (word.empty()) return;
        if (!line.empty() && line.size() + 1 + word.size() > width) { lines.push_back(line); line.clear(); }
        line += (line.empty() ? "" : " ") + word;
        word.clear();
    };
    for (char c : text) {
        if (c == ' ') flush_word();
        else word += c;
    }
    flush_word();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// The raised pixels: grid[y][x], y = 0 the bottom row. Diagonal-only contacts are filled in, so the raised
// region is a proper 2-manifold boundary (no vertex where two cells touch only at a corner).
struct Grid {
    int w = 0, h = 0;
    std::vector<std::vector<char>> on;
    bool at(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h && on[y][x]; }
};

Grid rasterize(const std::vector<std::string>& lines) {
    Grid g;
    size_t cols = 0;
    for (const auto& l : lines) cols = std::max(cols, l.size());
    g.w = static_cast<int>(cols) * kAdvance - 1 + 2 * kMargin;
    g.h = static_cast<int>(lines.size()) * kLineAdvance - 2 + 2 * kMargin;
    g.on.assign(g.h, std::vector<char>(g.w, 0));
    for (size_t li = 0; li < lines.size(); ++li)
        for (size_t ci = 0; ci < lines[li].size(); ++ci) {
            char c = static_cast<char>(std::toupper(static_cast<unsigned char>(lines[li][ci])));
            auto it = font().find(c);
            const auto& glyph = it != font().end() ? it->second : font().at('?');
            for (int r = 0; r < kGlyphH; ++r)
                for (int k = 0; k < kGlyphW; ++k)
                    if (glyph[r][k] == '#') {
                        int x = kMargin + static_cast<int>(ci) * kAdvance + k;
                        int y = g.h - 1 - (kMargin + static_cast<int>(li) * kLineAdvance + r);
                        g.on[y][x] = 1;
                    }
        }
    for (bool changed = true; changed;) {
        changed = false;
        for (int y = 0; y + 1 < g.h; ++y)
            for (int x = 0; x + 1 < g.w; ++x) {
                bool a = g.on[y][x], b = g.on[y][x + 1], c = g.on[y + 1][x], d = g.on[y + 1][x + 1];
                // bridge the corner by filling one of the two empty cells
                if (a && d && !b && !c) { g.on[y][x + 1] = 1; changed = true; }
                else if (b && c && !a && !d) { g.on[y][x] = 1; changed = true; }
            }
    }
    return g;
}

// Boundary loops of the raised region, traced with the region on the left (outer loops counter-clockwise,
// holes clockwise, seen from +z), collinear runs merged. Points are grid corners.
using Loop = std::vector<std::array<int, 2>>;

std::vector<Loop> trace(const Grid& g) {
    std::map<std::array<int, 2>, std::array<int, 2>> next;   // directed boundary edges: start -> end
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x) {
            if (!g.at(x, y)) continue;
            if (!g.at(x, y - 1)) next[{x, y}] = {x + 1, y};
            if (!g.at(x + 1, y)) next[{x + 1, y}] = {x + 1, y + 1};
            if (!g.at(x, y + 1)) next[{x + 1, y + 1}] = {x, y + 1};
            if (!g.at(x - 1, y)) next[{x, y + 1}] = {x, y};
        }
    std::vector<Loop> loops;
    while (!next.empty()) {
        Loop raw;
        auto p = next.begin()->first;
        while (next.count(p)) {
            raw.push_back(p);
            auto q = next[p];
            next.erase(p);
            p = q;
        }
        Loop loop;   // keep only the corners
        for (size_t i = 0; i < raw.size(); ++i) {
            const auto& a = raw[(i + raw.size() - 1) % raw.size()];
            const auto& b = raw[i];
            const auto& c = raw[(i + 1) % raw.size()];
            if ((b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0]) != 0) loop.push_back(b);
        }
        loops.push_back(loop);
    }
    return loops;
}

long long area2(const Loop& l) {
    long long s = 0;
    for (size_t i = 0; i < l.size(); ++i) {
        const auto& a = l[i];
        const auto& b = l[(i + 1) % l.size()];
        s += static_cast<long long>(a[0]) * b[1] - static_cast<long long>(b[0]) * a[1];
    }
    return s;
}

bool inside(const Loop& l, double x, double y) {
    bool in = false;
    for (size_t i = 0, j = l.size() - 1; i < l.size(); j = i++) {
        double xi = l[i][0], yi = l[i][1], xj = l[j][0], yj = l[j][1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
}

// ---------------------------------------------------------------------------------- polyhedral B-rep
// Planar faces given as loops of points with an outward normal; vertices and edges are shared between faces
// by position, and each loop is oriented to its face (outer counter-clockwise about the normal, inner the
// other way). The faces must close up into one solid.
class Brep {
public:
    Brep(long& id, std::vector<std::string>& out) : id_(id), out_(out) {}

    void face(const P3& normal, std::vector<std::vector<P3>> loops) {
        for (size_t i = 0; i < loops.size(); ++i) {
            P3 n = newell(loops[i]);
            double d = n[0] * normal[0] + n[1] * normal[1] + n[2] * normal[2];
            if ((i == 0) != (d > 0)) std::reverse(loops[i].begin(), loops[i].end());
        }
        std::string bounds;
        for (size_t i = 0; i < loops.size(); ++i) {
            std::string oes;
            const auto& l = loops[i];
            for (size_t k = 0; k < l.size(); ++k) {
                long a = vertex(l[k]), b = vertex(l[(k + 1) % l.size()]);
                bool fwd;
                long e = edge(a, b, l[k], l[(k + 1) % l.size()], fwd);
                oes += (oes.empty() ? "" : ",") + ref(add("ORIENTED_EDGE('',*,*," + ref(e) + "," + (fwd ? ".T." : ".F.") + ")"));
            }
            long el = add("EDGE_LOOP('',(" + oes + "))");
            long b = add(std::string(i == 0 ? "FACE_OUTER_BOUND" : "FACE_BOUND") + "(''," + ref(el) + ",.T.)");
            bounds += (bounds.empty() ? "" : ",") + ref(b);
        }
        const P3& o = loops[0][0];
        P3 r = std::fabs(normal[0]) < 0.9 ? P3{1, 0, 0} : P3{0, 1, 0};   // a direction in the plane
        double k = r[0] * normal[0] + r[1] * normal[1] + r[2] * normal[2];
        r = {r[0] - k * normal[0], r[1] - k * normal[1], r[2] - k * normal[2]};
        long ax = add("AXIS2_PLACEMENT_3D(''," + ref(point(o)) + "," + ref(dir(normal)) + "," + ref(dir(unit(r))) + ")");
        long pl = add("PLANE(''," + ref(ax) + ")");
        faces_.push_back(add("ADVANCED_FACE('',(" + bounds + ")," + ref(pl) + ",.T.)"));
    }

    long solid(const std::string& name) {
        std::string fl;
        for (long f : faces_) fl += (fl.empty() ? "" : ",") + ref(f);
        long shell = add("CLOSED_SHELL('',(" + fl + "))");
        return add("MANIFOLD_SOLID_BREP(" + step_string(name) + "," + ref(shell) + ")");
    }

private:
    long add(const std::string& body) {
        out_.push_back("#" + std::to_string(++id_) + "=" + body + ";");
        return id_;
    }
    long point(const P3& p) { return add("CARTESIAN_POINT('',(" + num(p[0]) + "," + num(p[1]) + "," + num(p[2]) + "))"); }
    long dir(const P3& d) { return add("DIRECTION('',(" + num(d[0]) + "," + num(d[1]) + "," + num(d[2]) + "))"); }
    static P3 unit(const P3& v) {
        double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        return {v[0] / n, v[1] / n, v[2] / n};
    }
    static P3 newell(const std::vector<P3>& l) {
        P3 n{0, 0, 0};
        for (size_t i = 0; i < l.size(); ++i) {
            const P3& a = l[i];
            const P3& b = l[(i + 1) % l.size()];
            n[0] += (a[1] - b[1]) * (a[2] + b[2]);
            n[1] += (a[2] - b[2]) * (a[0] + b[0]);
            n[2] += (a[0] - b[0]) * (a[1] + b[1]);
        }
        return n;
    }
    static std::string key(const P3& p) { return num(p[0]) + "," + num(p[1]) + "," + num(p[2]); }
    long vertex(const P3& p) {
        auto k = key(p);
        auto it = verts_.find(k);
        if (it != verts_.end()) return it->second;
        return verts_[k] = add("VERTEX_POINT(''," + ref(point(p)) + ")");
    }
    long edge(long a, long b, const P3& pa, const P3& pb, bool& fwd) {
        auto it = edges_.find({std::min(a, b), std::max(a, b)});
        if (it != edges_.end()) {
            fwd = it->second.second == a;
            return it->second.first;
        }
        P3 d{pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
        double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        long v = add("VECTOR(''," + ref(dir(unit(d))) + "," + num(len) + ")");
        long line = add("LINE(''," + ref(point(pa)) + "," + ref(v) + ")");
        long e = add("EDGE_CURVE(''," + ref(a) + "," + ref(b) + "," + ref(line) + ",.T.)");
        edges_[{std::min(a, b), std::max(a, b)}] = {e, a};
        fwd = true;
        return e;
    }

    long& id_;
    std::vector<std::string>& out_;
    std::map<std::string, long> verts_;
    std::map<std::pair<long, long>, std::pair<long, long>> edges_;   // vertex pair -> (edge, its start vertex)
    std::vector<long> faces_;
};

// The notice solid: a plate (x0..x1, y0..y1, z0..z1) with the text raised by `relief` on its top face.
long notice_solid(const std::string& text, const std::array<double, 6>& model, double unit_scale, long& id,
                  std::vector<std::string>& out) {
    // lines about five times wider than the block of text is tall
    size_t width = std::max<size_t>(12, static_cast<size_t>(std::ceil(std::sqrt(7.5 * text.size()))));
    Grid g = rasterize(wrap(text, width));
    double size = std::max({model[3] - model[0], model[4] - model[1], model[5] - model[2], 10.0});
    double len = std::max(0.5 * (model[3] - model[0]), 0.3 * size);
    double px = len / g.w;                                     // one font pixel, mm
    double depth = px * g.h, thick = 2 * px, relief = px, gap = 0.1 * size;
    double x0 = model[0], y1 = model[1] - gap, y0 = y1 - depth, z0 = model[2], z1 = z0 + thick, z2 = z1 + relief;
    auto P = [&](double x, double y, double z) { return P3{x * unit_scale, y * unit_scale, z * unit_scale}; };
    auto at = [&](const std::array<int, 2>& c, double z) { return P(x0 + c[0] * px, y0 + c[1] * px, z); };

    auto loops = trace(g);
    std::vector<size_t> outer, holes;
    for (size_t i = 0; i < loops.size(); ++i) (area2(loops[i]) > 0 ? outer : holes).push_back(i);

    Brep b(id, out);
    double x1 = x0 + len;
    // plate: bottom and four sides
    b.face({0, 0, -1}, {{P(x0, y0, z0), P(x1, y0, z0), P(x1, y1, z0), P(x0, y1, z0)}});
    b.face({0, -1, 0}, {{P(x0, y0, z0), P(x1, y0, z0), P(x1, y0, z1), P(x0, y0, z1)}});
    b.face({1, 0, 0}, {{P(x1, y0, z0), P(x1, y1, z0), P(x1, y1, z1), P(x1, y0, z1)}});
    b.face({0, 1, 0}, {{P(x1, y1, z0), P(x0, y1, z0), P(x0, y1, z1), P(x1, y1, z1)}});
    b.face({-1, 0, 0}, {{P(x0, y1, z0), P(x0, y0, z0), P(x0, y0, z1), P(x0, y1, z1)}});
    // plate top: the rectangle with a hole where each letter stands
    std::vector<std::vector<P3>> top{{P(x0, y0, z1), P(x1, y0, z1), P(x1, y1, z1), P(x0, y1, z1)}};
    for (size_t i : outer) {
        std::vector<P3> l;
        for (const auto& c : loops[i]) l.push_back(at(c, z1));
        top.push_back(l);
    }
    b.face({0, 0, 1}, top);
    // the floor inside a letter's hole (O, A, 8 ...) is plate top too
    for (size_t h : holes) {
        std::vector<P3> l;
        for (const auto& c : loops[h]) l.push_back(at(c, z1));
        b.face({0, 0, 1}, {l});
    }
    // letter tops: each outer outline with the holes inside it
    for (size_t i : outer) {
        std::vector<std::vector<P3>> f{{}};
        for (const auto& c : loops[i]) f[0].push_back(at(c, z2));
        for (size_t h : holes) {
            // a hole belongs to the smallest outline containing it (its corners lie strictly inside that
            // outline: outlines never touch, as diagonal contacts are bridged)
            double hx = loops[h][0][0] + 0.01, hy = loops[h][0][1] + 0.013;
            size_t best = SIZE_MAX;
            for (size_t o : outer)
                if (inside(loops[o], hx, hy) && (best == SIZE_MAX || std::llabs(area2(loops[o])) < std::llabs(area2(loops[best]))))
                    best = o;
            if (best != i) continue;
            std::vector<P3> l;
            for (const auto& c : loops[h]) l.push_back(at(c, z2));
            f.push_back(l);
        }
        b.face({0, 0, 1}, f);
    }
    // letter walls: one per straight run of every outline, facing away from the letter
    for (const auto& l : loops)
        for (size_t k = 0; k < l.size(); ++k) {
            const auto& a = l[k];
            const auto& c = l[(k + 1) % l.size()];
            double dx = c[0] - a[0], dy = c[1] - a[1], n = std::sqrt(dx * dx + dy * dy);
            P3 out_n{dy / n, -dx / n, 0};   // the letter is on the left of the run
            b.face(out_n, {{at(a, z1), at(c, z1), at(c, z2), at(a, z2)}});
        }
    return b.solid(text);
}

// mm per length unit of the file (SolidWorks writes mm; metres and inches are recognised too)
double unit_mm(const StepText& st) {
    for (const auto& e : st.entities()) {
        std::string b(e.body);
        b.erase(std::remove(b.begin(), b.end(), ' '), b.end());
        if (b.find("LENGTH_UNIT") == std::string::npos) continue;
        if (b.find(".MILLI.") != std::string::npos) return 1.0;
        if (b.find("CONVERSION_BASED_UNIT('INCH'") != std::string::npos) return 25.4;
        if (b.find("SI_UNIT($,.METRE.)") != std::string::npos) return 1000.0;
    }
    return 1.0;
}

// the model's extent in mm, from the payload fingerprint (what the B-rep holds)
std::array<double, 6> model_box(const json& model) {
    constexpr double inf = std::numeric_limits<double>::infinity();
    std::array<double, 6> b{inf, inf, inf, -inf, -inf, -inf};
    if (model.contains("fingerprint") && model["fingerprint"].contains("bodies"))
        for (const auto& body : model["fingerprint"]["bodies"]) {
            if (!body.contains("bbox") || !body["bbox"].is_array() || body["bbox"].size() != 6) continue;
            for (int i = 0; i < 3; ++i) {
                b[i] = std::min(b[i], body["bbox"][i].get<double>());
                b[i + 3] = std::max(b[i + 3], body["bbox"][i + 3].get<double>());
            }
        }
    if (!(b[0] <= b[3])) b = {0, 0, 0, 100, 100, 100};
    return b;
}

// every entity reachable from `id` (a notice owns all of its records; nothing else refers into them)
void reachable(const StepText& st, long id, std::set<long>& out) {
    if (!out.insert(id).second) return;
    const Entity* e = st.find(id);
    if (!e) return;
    std::string_view b = e->body;
    for (size_t i = 0; i < b.size(); ++i)
        if (b[i] == '#') {
            size_t j = i + 1;
            while (j < b.size() && b[j] >= '0' && b[j] <= '9') ++j;
            if (j > i + 1) reachable(st, std::stol(std::string(b.substr(i + 1, j - i - 1))), out);
            i = j - 1;
        }
}

}  // namespace


namespace {

bool is_pd(const Entity* e) {
    return e && (e->type == "PRODUCT_DEFINITION" || e->type == "PRODUCT_DEFINITION_WITH_ASSOCIATED_DOCUMENTS");
}



// Everything an earlier notice component owns: the records that refer to its product (upwards: formation,
// definition, shape, usage, placement), then all they refer to, stopping at what the file shares (contexts,
// units, the root's definition and representation).
void old_notice_component(const StepText& st, long product, long root_pd, long root_sr, std::set<long>& out) {
    static const std::set<std::string> upward = {
        "PRODUCT_RELATED_PRODUCT_CATEGORY", "PRODUCT_DEFINITION_FORMATION_WITH_SPECIFIED_SOURCE",
        "PRODUCT_DEFINITION_FORMATION", "PRODUCT_DEFINITION", "PRODUCT_DEFINITION_SHAPE",
        "SHAPE_DEFINITION_REPRESENTATION", "NEXT_ASSEMBLY_USAGE_OCCURRENCE", "CONTEXT_DEPENDENT_SHAPE_REPRESENTATION",
        "SHAPE_REPRESENTATION_RELATIONSHIP"};
    auto shared = [&](const Entity* e) {
        if (!e || e->id == root_pd || e->id == root_sr) return true;
        std::string b(e->body);
        return (e->type.find("CONTEXT") != std::string::npos && e->type != "CONTEXT_DEPENDENT_SHAPE_REPRESENTATION") ||
               (e->type.empty() && (b.find("CONTEXT") != std::string::npos || b.find("UNIT") != std::string::npos) &&
                b.find("REPRESENTATION_RELATIONSHIP") == std::string::npos);
    };
    // alternate until nothing new: records that refer up to what is collected (e.g. the shape-representation
    // relationship that ties the component's shape to its B-rep), then everything those refer to
    std::vector<long> todo{product};
    for (;;) {
        while (!todo.empty()) {
            long id = todo.back();
            todo.pop_back();
            const Entity* e = st.find(id);
            if (shared(e) || !out.insert(id).second) continue;
            std::string_view b = e->body;
            for (size_t i = 0; i < b.size(); ++i)
                if (b[i] == '#') {
                    size_t j = i + 1;
                    while (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j]))) ++j;
                    if (j > i + 1) todo.push_back(std::stol(std::string(b.substr(i + 1, j - i - 1))));
                    i = j - 1;
                }
        }
        for (const auto& e : st.entities()) {
            if (out.count(e.id) || !upward.count(e.type)) continue;
            std::string_view b = e.body;
            for (size_t i = 0; i < b.size(); ++i)
                if (b[i] == '#') {
                    size_t j = i + 1;
                    while (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j]))) ++j;
                    if (j > i + 1 && out.count(std::stol(std::string(b.substr(i + 1, j - i - 1))))) {
                        todo.push_back(e.id);
                        break;
                    }
                    i = j - 1;
                }
        }
        if (todo.empty()) break;
    }
}

// The notice as a new part component of the root assembly, placed at the assembly origin (its geometry is
// already in the assembly frame). Its records go first in the file.
NoticeEdit plan_assembly_notice(const StepText& st, const json& model, const std::string& text,
                                const std::function<bool(const std::string&)>& is_earlier, long& next_id) {
    NoticeEdit ed;
    // the root definition: never the component side of a usage
    std::set<long> used;
    for (const Entity* e : st.of_type("NEXT_ASSEMBLY_USAGE_OCCURRENCE")) {
        auto a = parse_args(e->body);
        if (a.size() > 4 && a[4].kind == Arg::Ref) used.insert(a[4].ref);
    }
    long root_pd = 0;
    for (const auto& e : st.entities())
        if (is_pd(&e) && !used.count(e.id)) { root_pd = e.id; break; }
    // its shape representation: PRODUCT_DEFINITION_SHAPE(root) <- SHAPE_DEFINITION_REPRESENTATION(pds, sr)
    long root_sr = 0;
    for (const Entity* pds : st.of_type("PRODUCT_DEFINITION_SHAPE")) {
        auto a = parse_args(pds->body);
        if (a.size() < 3 || a[2].kind != Arg::Ref || a[2].ref != root_pd) continue;
        for (const Entity* sdr : st.of_type("SHAPE_DEFINITION_REPRESENTATION")) {
            auto s = parse_args(sdr->body);
            if (s.size() >= 2 && s[0].kind == Arg::Ref && s[0].ref == pds->id && s[1].kind == Arg::Ref) root_sr = s[1].ref;
        }
    }
    const Entity* sr = st.find(root_sr);
    auto ra = sr ? parse_args(sr->body) : std::vector<Arg>{};
    auto pda = parse_args(st.find(root_pd) ? st.find(root_pd)->body : std::string_view());
    if (!root_pd || !sr || ra.size() < 3 || ra[1].kind != Arg::List || ra[2].kind != Arg::Ref || pda.size() < 4 ||
        pda[2].kind != Arg::Ref || pda[3].kind != Arg::Ref) {
        ed.skipped = "could not find the root assembly's definition and shape";
        return ed;
    }
    long geom_ctx = ra[2].ref, pd_ctx = pda[3].ref;
    long product_ctx = 0;   // root definition -> formation -> product -> its product context
    if (const Entity* pdf = st.find(pda[2].ref)) {
        auto fa = parse_args(pdf->body);
        if (fa.size() >= 3 && fa[2].kind == Arg::Ref)
            if (const Entity* prod = st.find(fa[2].ref)) {
                auto p = parse_args(prod->body);
                if (p.size() >= 4 && p[3].kind == Arg::List && !p[3].items.empty() && p[3].items[0].kind == Arg::Ref)
                    product_ctx = p[3].items[0].ref;
            }
    }
    if (!product_ctx) {
        ed.skipped = "root product has no context";
        return ed;
    }
    // an earlier notice component is replaced, never doubled
    for (const Entity* p : st.of_type("PRODUCT")) {
        auto a = parse_args(p->body);
        if (!a.empty() && a[0].kind == Arg::Str && is_earlier(a[0].text)) old_notice_component(st, p->id, root_pd, root_sr, ed.remove);
    }

    auto& out = ed.prepend;
    auto add = [&](const std::string& body) {
        out.push_back("#" + std::to_string(++next_id) + "=" + body + ";");
        return next_id;
    };
    auto placement = [&]() {   // identity placement at the origin
        long o = add("CARTESIAN_POINT('',(0.,0.,0.))");
        long z = add("DIRECTION('',(0.,0.,1.))");
        long x = add("DIRECTION('',(1.,0.,0.))");
        return add("AXIS2_PLACEMENT_3D(''," + ref(o) + "," + ref(z) + "," + ref(x) + ")");
    };
    // a component (and its file, for importers that write one per part) cannot carry path characters
    std::string pname = text;
    for (char& c : pname)
        if (std::string("/\\:*?\"<>|").find(c) != std::string::npos) c = '_';

    long msb = notice_solid(text, model_box(model), 1.0 / unit_mm(st), next_id, out);
    long own = placement();
    long absr = add("ADVANCED_BREP_SHAPE_REPRESENTATION('',(" + ref(msb) + "," + ref(own) + ")," + ref(geom_ctx) + ")");
    long csr = add("SHAPE_REPRESENTATION(" + step_string(pname) + ",(" + ref(own) + ")," + ref(geom_ctx) + ")");
    add("SHAPE_REPRESENTATION_RELATIONSHIP('NONE','NONE'," + ref(csr) + "," + ref(absr) + ")");
    long prod = add("PRODUCT(" + step_string(pname) + "," + step_string(pname) + ",'',(" + ref(product_ctx) + "))");
    add("PRODUCT_RELATED_PRODUCT_CATEGORY('part','',(" + ref(prod) + "))");
    long pdf = add("PRODUCT_DEFINITION_FORMATION_WITH_SPECIFIED_SOURCE('ANY',''," + ref(prod) + ",.NOT_KNOWN.)");
    long pd = add("PRODUCT_DEFINITION('UNKNOWN',''," + ref(pdf) + "," + ref(pd_ctx) + ")");
    long pds = add("PRODUCT_DEFINITION_SHAPE('NONE','NONE'," + ref(pd) + ")");
    add("SHAPE_DEFINITION_REPRESENTATION(" + ref(pds) + "," + ref(csr) + ")");
    long nauo = add("NEXT_ASSEMBLY_USAGE_OCCURRENCE('NAUO-NCX-NOTICE'," + step_string(pname) + ",''," + ref(root_pd) + "," + ref(pd) + ",$)");
    long npds = add("PRODUCT_DEFINITION_SHAPE('NONE','NONE'," + ref(nauo) + ")");
    long at = placement();   // where the component sits, in the root's representation
    long idt = add("ITEM_DEFINED_TRANSFORMATION('NONE','NONE'," + ref(at) + "," + ref(own) + ")");
    long rr = add("(REPRESENTATION_RELATIONSHIP('NONE','NONE'," + ref(root_sr) + "," + ref(csr) +
                  ")REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION(" + ref(idt) + ")SHAPE_REPRESENTATION_RELATIONSHIP())");
    add("CONTEXT_DEPENDENT_SHAPE_REPRESENTATION(" + ref(rr) + "," + ref(npds) + ")");

    // the root's representation gains the placement (and loses an earlier notice's)
    std::string items;
    for (const auto& it : ra[1].items)
        if (it.kind == Arg::Ref && !ed.remove.count(it.ref)) items += (items.empty() ? "" : ",") + ref(it.ref);
    items += (items.empty() ? "" : ",") + ref(at);
    ed.remove.insert(root_sr);
    ed.append.push_back("#" + std::to_string(root_sr) + "=SHAPE_REPRESENTATION(" + step_string(ra[0].text) + ",(" + items +
                        ")," + ref(geom_ctx) + ");");
    ed.name = text;
    return ed;
}

}  // namespace

NoticeEdit plan_notice(const StepText& st, const json& model, const std::string& text,
                       const std::function<bool(const std::string&)>& is_earlier, long& next_id) {
    if (!st.of_type("NEXT_ASSEMBLY_USAGE_OCCURRENCE").empty()) return plan_assembly_notice(st, model, text, is_earlier, next_id);
    NoticeEdit ed;
    auto reps = st.of_type("ADVANCED_BREP_SHAPE_REPRESENTATION");
    if (reps.size() != 1) {
        ed.skipped = std::to_string(reps.size()) + " B-rep representations (expected one)";
        return ed;
    }
    const Entity* rep = reps[0];
    auto a = parse_args(rep->body);
    if (a.size() < 3 || a[1].kind != Arg::List || a[2].kind != Arg::Ref) {
        ed.skipped = "unexpected B-rep representation";
        return ed;
    }
    // an earlier notice (a file written again) is replaced, never doubled
    std::string items;
    for (const auto& it : a[1].items) {
        if (it.kind != Arg::Ref) continue;
        if (const Entity* e = st.find(it.ref); e && e->type == "MANIFOLD_SOLID_BREP") {
            auto ea = parse_args(e->body);
            if (!ea.empty() && ea[0].kind == Arg::Str && is_earlier(ea[0].text)) {
                reachable(st, e->id, ed.remove);
                continue;
            }
        }
        items += (items.empty() ? "" : ",") + ref(it.ref);
    }

    long msb = notice_solid(text, model_box(model), 1.0 / unit_mm(st), next_id, ed.append);
    items += (items.empty() ? "" : ",") + ref(msb);

    ed.remove.insert(rep->id);   // rewritten under the same id with the notice added
    ed.append.push_back("#" + std::to_string(rep->id) + "=ADVANCED_BREP_SHAPE_REPRESENTATION(" + step_string(a[0].text) +
                        ",(" + items + ")," + ref(a[2].ref) + ");");
    ed.name = text;
    return ed;
}

}  // namespace ncx
