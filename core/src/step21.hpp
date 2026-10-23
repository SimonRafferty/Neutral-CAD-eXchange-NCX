// Minimal ISO 10303-21 reader/patcher: just enough to find, read, remove and append entity
// records in the DATA section while leaving every other byte of the file untouched.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ncx {

// One top-level argument of a simple entity instance.
struct Arg {
    enum Kind { Str, Ref, List, Other } kind = Other;
    std::string text;        // decoded string for Str ('' -> ', CR/LF removed); raw token otherwise
    long ref = 0;            // for Ref
    std::vector<Arg> items;  // for List
};

struct Entity {
    long id = 0;
    std::string type;        // e.g. PRODUCT_DEFINITION; empty for complex (multi-type) instances
    size_t begin = 0;        // byte range of "#id = ... ;" in the file text
    size_t end = 0;
    size_t body_off = 0;     // offset/length of the body in the file text
    size_t body_len = 0;
    std::string_view body;   // text after '=' up to (not including) the ';' (re-pointed on copy/move)
};

class StepText {
public:
    StepText() = default;
    // Entity::body views point into text_, so copies and moves must re-point them.
    StepText(const StepText& o) : text_(o.text_), ents_(o.ents_), index_(o.index_), data_begin_(o.data_begin_), data_end_(o.data_end_),
                                  max_id_(o.max_id_), eol_(o.eol_) { repoint(); }
    StepText(StepText&& o) noexcept : text_(std::move(o.text_)), ents_(std::move(o.ents_)), index_(std::move(o.index_)),
                                      data_begin_(o.data_begin_), data_end_(o.data_end_), max_id_(o.max_id_), eol_(std::move(o.eol_)) { repoint(); }
    StepText& operator=(const StepText&) = delete;
    StepText& operator=(StepText&&) = delete;

    // Parses the first DATA section; returns nullopt (with *error set) if the text isn't Part 21.
    static std::optional<StepText> parse(std::string text, std::string* error);

    const std::string& text() const { return text_; }
    const std::vector<Entity>& entities() const { return ents_; }
    const Entity* find(long id) const;
    std::vector<const Entity*> of_type(std::string_view type) const;
    long max_id() const { return max_id_; }
    const std::string& eol() const { return eol_; }

    // Ids referenced (#n) anywhere in any entity body other than the ones in `exclude`.
    std::set<long> referenced_ids(const std::set<long>& exclude = {}) const;

    // New file text: entities in `remove` deleted, `append` records inserted before the
    // DATA section's ENDSEC and `prepend` records at its start (each element is a full record
    // without line terminator).
    std::string rebuild(const std::set<long>& remove, const std::vector<std::string>& append,
                        const std::vector<std::string>& prepend = {}) const;

private:
    void repoint() {
        for (auto& e : ents_) e.body = std::string_view(text_).substr(e.body_off, e.body_len);
    }
    std::string text_;
    std::vector<Entity> ents_;
    std::map<long, size_t> index_;
    size_t data_begin_ = 0;  // offset just after "DATA;" and its line end
    size_t data_end_ = 0;    // offset of the ENDSEC keyword closing DATA
    long max_id_ = 0;
    std::string eol_ = "\n";
};

// Top-level arguments of a simple entity body such as "NAME('a',#5,(#6,#7),.T.)".
std::vector<Arg> parse_args(std::string_view body);

// Every entity reference (#n, also the record's own id) outside string literals replaced by map(n).
std::string renumber(const std::string& text, const std::function<long(long)>& map);

// Quote a string for Part 21 (doubles apostrophes). Callers keep payload text ASCII.
std::string step_string(std::string_view s);

}  // namespace ncx
