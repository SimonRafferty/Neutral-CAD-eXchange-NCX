#include "step21.hpp"

#include <algorithm>
#include <cctype>

namespace ncx {

namespace {

bool is_ident(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Position just after the string literal starting at s[i] == '\''.
size_t skip_string(std::string_view s, size_t i) {
    ++i;
    while (i < s.size()) {
        if (s[i] == '\'') {
            if (i + 1 < s.size() && s[i + 1] == '\'') { i += 2; continue; }
            return i + 1;
        }
        ++i;
    }
    return s.size();
}

size_t skip_comment(std::string_view s, size_t i) {  // s[i..] == "/*"
    size_t e = s.find("*/", i + 2);
    return e == std::string_view::npos ? s.size() : e + 2;
}

size_t skip_ws_comments(std::string_view s, size_t i) {
    while (i < s.size()) {
        if (std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        else if (s.compare(i, 2, "/*") == 0) i = skip_comment(s, i);
        else break;
    }
    return i;
}

// Find keyword `kw` followed by optional whitespace and ';' at top level (outside strings/comments).
size_t find_keyword(std::string_view s, std::string_view kw, size_t from) {
    size_t i = from;
    while (i < s.size()) {
        char c = s[i];
        if (c == '\'') { i = skip_string(s, i); continue; }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') { i = skip_comment(s, i); continue; }
        if (s.compare(i, kw.size(), kw) == 0 && (i == 0 || !is_ident(s[i - 1]))) {
            size_t j = skip_ws_comments(s, i + kw.size());
            if (j < s.size() && s[j] == ';') return i;
        }
        ++i;
    }
    return std::string_view::npos;
}

std::string decode_string(std::string_view lit) {  // lit includes the quotes
    std::string out;
    for (size_t i = 1; i + 1 < lit.size(); ++i) {
        char c = lit[i];
        if (c == '\'' && lit[i + 1] == '\'') { out += '\''; ++i; continue; }
        if (c == '\r' || c == '\n') continue;  // line breaks inside strings are not significant
        out += c;
    }
    return out;
}

// Parse one argument starting at s[i]; returns position after it.
size_t parse_one(std::string_view s, size_t i, Arg& a);

size_t parse_list(std::string_view s, size_t i, std::vector<Arg>& out) {  // s[i] == '('
    ++i;
    for (;;) {
        i = skip_ws_comments(s, i);
        if (i >= s.size()) return i;
        if (s[i] == ')') return i + 1;
        Arg a;
        i = parse_one(s, i, a);
        out.push_back(std::move(a));
        i = skip_ws_comments(s, i);
        if (i < s.size() && s[i] == ',') ++i;
    }
}

size_t parse_one(std::string_view s, size_t i, Arg& a) {
    i = skip_ws_comments(s, i);
    if (i >= s.size()) return i;
    char c = s[i];
    if (c == '\'') {
        size_t e = skip_string(s, i);
        a.kind = Arg::Str;
        a.text = decode_string(s.substr(i, e - i));
        return e;
    }
    if (c == '#') {
        size_t e = i + 1;
        while (e < s.size() && std::isdigit(static_cast<unsigned char>(s[e]))) ++e;
        a.kind = Arg::Ref;
        a.text = std::string(s.substr(i, e - i));
        a.ref = std::stol(std::string(s.substr(i + 1, e - i - 1)));
        return e;
    }
    if (c == '(') {
        a.kind = Arg::List;
        return parse_list(s, i, a.items);
    }
    // keyword(...) typed parameter, enum, number, $ or *: take up to the next top-level ',' or ')'
    size_t e = i;
    int depth = 0;
    while (e < s.size()) {
        char d = s[e];
        if (d == '\'') { e = skip_string(s, e); continue; }
        if (d == '(') ++depth;
        else if (d == ')') { if (depth == 0) break; --depth; }
        else if (d == ',' && depth == 0) break;
        ++e;
    }
    a.kind = Arg::Other;
    a.text = std::string(s.substr(i, e - i));
    return e;
}

}  // namespace

std::vector<Arg> parse_args(std::string_view body) {
    std::vector<Arg> out;
    size_t i = 0;
    while (i < body.size() && is_ident(body[i])) ++i;  // entity name
    i = skip_ws_comments(body, i);
    if (i < body.size() && body[i] == '(') parse_list(body, i, out);
    return out;
}

std::string step_string(std::string_view s) {
    std::string out = "'";
    for (char c : s) {
        out += c;
        if (c == '\'') out += '\'';
    }
    out += '\'';
    return out;
}

std::optional<StepText> StepText::parse(std::string text, std::string* error) {
    StepText st;
    st.text_ = std::move(text);
    std::string_view s = st.text_;
    if (s.find("ISO-10303-21") == std::string_view::npos) {
        if (error) *error = "missing ISO-10303-21 marker";
        return std::nullopt;
    }
    if (s.find("\r\n") != std::string_view::npos) st.eol_ = "\r\n";
    size_t data = find_keyword(s, "DATA", 0);
    if (data == std::string_view::npos) {
        if (error) *error = "no DATA section";
        return std::nullopt;
    }
    size_t i = s.find(';', data) + 1;
    st.data_begin_ = i;
    if (s.compare(i, st.eol_.size(), st.eol_) == 0) st.data_begin_ += st.eol_.size();
    for (;;) {
        i = skip_ws_comments(s, i);
        if (i >= s.size()) {
            if (error) *error = "DATA section not terminated by ENDSEC";
            return std::nullopt;
        }
        if (s.compare(i, 6, "ENDSEC") == 0) {
            st.data_end_ = i;
            break;
        }
        if (s[i] != '#') {
            if (error) *error = "unexpected text in DATA section at offset " + std::to_string(i);
            return std::nullopt;
        }
        Entity e;
        e.begin = i;
        size_t j = i + 1;
        while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
        e.id = std::stol(std::string(s.substr(i + 1, j - i - 1)));
        j = skip_ws_comments(s, j);
        if (j >= s.size() || s[j] != '=') {
            if (error) *error = "expected '=' after #" + std::to_string(e.id);
            return std::nullopt;
        }
        j = skip_ws_comments(s, j + 1);
        size_t body_begin = j;
        while (j < s.size() && s[j] != ';') {
            if (s[j] == '\'') { j = skip_string(s, j); continue; }
            if (s[j] == '/' && j + 1 < s.size() && s[j + 1] == '*') { j = skip_comment(s, j); continue; }
            ++j;
        }
        if (j >= s.size()) {
            if (error) *error = "unterminated entity #" + std::to_string(e.id);
            return std::nullopt;
        }
        e.body_off = body_begin;
        e.body_len = j - body_begin;
        e.body = s.substr(body_begin, j - body_begin);
        e.end = j + 1;
        if (!e.body.empty() && e.body[0] != '(') {
            size_t k = 0;
            while (k < e.body.size() && is_ident(e.body[k])) ++k;
            e.type = std::string(e.body.substr(0, k));
        }
        st.index_[e.id] = st.ents_.size();
        st.max_id_ = std::max(st.max_id_, e.id);
        st.ents_.push_back(e);
        i = e.end;
    }
    return st;
}

const Entity* StepText::find(long id) const {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &ents_[it->second];
}

std::vector<const Entity*> StepText::of_type(std::string_view type) const {
    std::vector<const Entity*> out;
    for (const auto& e : ents_)
        if (e.type == type) out.push_back(&e);
    return out;
}

std::set<long> StepText::referenced_ids(const std::set<long>& exclude) const {
    std::set<long> out;
    for (const auto& e : ents_) {
        if (exclude.count(e.id)) continue;
        std::string_view b = e.body;
        for (size_t i = 0; i < b.size(); ++i) {
            if (b[i] == '\'') { i = skip_string(b, i) - 1; continue; }
            if (b[i] == '#') {
                size_t j = i + 1;
                while (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j]))) ++j;
                if (j > i + 1) out.insert(std::stol(std::string(b.substr(i + 1, j - i - 1))));
                i = j - 1;
            }
        }
    }
    return out;
}

std::string StepText::rebuild(const std::set<long>& remove, const std::vector<std::string>& append,
                              const std::vector<std::string>& prepend) const {
    std::string out;
    out.reserve(text_.size() + 64 * (append.size() + prepend.size()));
    size_t pos = 0;
    if (!prepend.empty()) {   // records that must come first in the file (before the exporter's own)
        out.append(text_, 0, data_begin_);
        for (const auto& rec : prepend) {
            out += rec;
            out += eol_;
        }
        pos = data_begin_;
    }
    for (const auto& e : ents_) {  // entities are in file order
        if (!remove.count(e.id)) continue;
        out.append(text_, pos, e.begin - pos);
        pos = e.end;
        // swallow the record's own line terminator so no blank line is left behind
        if (text_.compare(pos, eol_.size(), eol_) == 0) pos += eol_.size();
    }
    out.append(text_, pos, data_end_ - pos);
    for (const auto& rec : append) {
        out += rec;
        out += eol_;
    }
    out.append(text_, data_end_, std::string::npos);
    return out;
}

}  // namespace ncx

namespace ncx {

std::string renumber(const std::string& text, const std::function<long(long)>& map) {
    std::string out;
    out.reserve(text.size() + text.size() / 50);
    bool in_string = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\'') in_string = !in_string;   // a doubled '' toggles twice: still inside
        if (!in_string && c == '#' && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
            size_t j = i + 1;
            while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) ++j;
            out += '#';
            out += std::to_string(map(std::stol(text.substr(i + 1, j - i - 1))));
            i = j - 1;
            continue;
        }
        out += c;
    }
    return out;
}

}  // namespace ncx
