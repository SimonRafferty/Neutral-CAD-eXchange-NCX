// The notice body (FORMAT-SPEC section 2.6): a plain solid plate written beside the model, named with a
// message for anyone who opens the file without an NCX add-in. It tells them the file carries a full
// parametric model; they can delete it and keep the plain geometry. NCX importers skip it: it is never part
// of the payload's fingerprint, and the header names it (notice=...).
#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "step21.hpp"

namespace ncx {

constexpr const char* kDefaultNotice = "READ ME - NCX file: Search 'Neutral CAD eXchange' on Github to import.";

struct NoticeEdit {
    std::set<long> remove;             // records replaced (the shape representation, any earlier notice)
    std::vector<std::string> append;   // new records (at the end of DATA)
    std::vector<std::string> prepend;  // new records that must come first (an assembly's notice component, so
                                       // importers that order components by file order list it at the top)
    std::string name;                  // the notice body's name, empty if none was added
    std::string skipped;               // why none was added
};

// Plan the notice for a STEP file: a plate with the text raised on it, beside the bounding box of the model's
// fingerprint (mm), in the file's length unit. A part file gets it as another body of the root part; an
// assembly file as a new part component of the root assembly, first in the file. `next_id` is advanced past
// the new records.
// `is_earlier(name)` recognises an earlier notice (a body or component name) to replace.
NoticeEdit plan_notice(const StepText& st, const nlohmann::json& model, const std::string& text,
                       const std::function<bool(const std::string&)>& is_earlier, long& next_id);


}  // namespace ncx
