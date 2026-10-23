// Replay: global variables (user parameters) and equations (parameter expressions), and the parameter-flex
// check (FORMAT-SPEC 6.4).
//
// Fusion's parameters are design-wide; a document's global variable becomes a user parameter of the same name
// (prefixed with the document's name when another document already uses that name). Global variables are
// unitless, as SolidWorks' are; where one drives a length or an angle, the expression is multiplied by one unit
// of the driven parameter ("(W) * 1 mm"), which is what SolidWorks' equation means.
#include "host.h"
#include "replay.h"

#include <cctype>

namespace ncxf {

namespace {

std::string ident(const std::string& s)
{
    std::string r;
    for (unsigned char c : s) r += (std::isalnum(c) || c == '_') ? static_cast<char>(c) : '_';
    if (r.empty() || std::isdigit(static_cast<unsigned char>(r[0]))) r = "_" + r;
    return r;
}

} // namespace

// Translate a neutral expression: quoted (or bare) names -> Fusion parameter names. Returns "" and sets why when
// a name or a function has no counterpart.
std::string Replayer::translateExpression(const std::string& e, std::string& why)
{
    std::string out;
    size_t i = 0;
    auto lookup = [&](const std::string& name) -> std::string {
        auto it = paramByNative_.find(name);
        if (it != paramByNative_.end()) return it->second;
        // sketch dimension written "D1@SK_Base@part.Part": drop the document part
        size_t at2 = name.find('@', name.find('@') + 1);
        if (at2 != std::string::npos)
        {
            it = paramByNative_.find(name.substr(0, at2));
            if (it != paramByNative_.end()) return it->second;
        }
        return "";
    };
    while (i < e.size())
    {
        char c = e[i];
        if (c == '"')
        {
            size_t j = e.find('"', i + 1);
            if (j == std::string::npos)
            {
                why = "unbalanced quotes in " + e;
                return "";
            }
            std::string name = e.substr(i + 1, j - i - 1);
            std::string f = lookup(name);
            if (f.empty())
            {
                why = "name " + name + " has no Fusion parameter";
                return "";
            }
            out += f;
            i = j + 1;
        }
        else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
        {
            size_t j = i;
            while (j < e.size() && (std::isalnum(static_cast<unsigned char>(e[j])) || e[j] == '_' || e[j] == '@' || e[j] == '-')) j++;
            std::string w = e.substr(i, j - i);
            static const std::set<std::string> fns{"sin", "cos", "tan", "sqrt", "abs", "min", "max"};
            if (fns.count(w)) out += w;
            else if (w == "if")
            {
                why = "conditional (if) expressions have no Fusion counterpart";
                return "";
            }
            else
            {
                std::string f = lookup(w);
                if (f.empty())
                {
                    why = "name " + w + " has no Fusion parameter";
                    return "";
                }
                out += f;
            }
            i = j;
        }
        else if (c == '^')
        {
            out += '^';
            i++;
        }
        else
        {
            out += c;
            i++;
        }
    }
    return out;
}

void Replayer::replayParameters(const json& doc)
{
    if (!doc.contains("parameters")) return;
    Ptr<UserParameters> ups = design_->userParameters();
    for (auto& p : doc["parameters"])
    {
        std::string native = p.value("name", "");
        std::string name = ident(native);
        if (ups->itemByName(name)) name = ident(docName_ + "_" + native);
        paramByNative_[native] = name;
        std::string why;
        std::string expr = p.contains("expression") ? translateExpression(p["expression"].get<std::string>(), why) : "";
        if (expr.empty())
        {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.12g", p.value("value", 0.0));
            expr = buf;
        }
        // Fusion refuses names that are unit symbols (W, H, m, s, ...): such a variable gets a trailing "_"
        Ptr<UserParameter> up = ups->add(name, ValueInput::createByString(expr), "", "NCX: global variable " + native + " of " + docName_);
        if (!up && lastError().find("not valid") != std::string::npos)
        {
            name += "_";
            paramByNative_[native] = name;
            up = ups->add(name, ValueInput::createByString(expr), "", "NCX: global variable " + native + " of " + docName_);
        }
        if (!up)
        {
            report_.add(docName_, "parameter " + native, "failed", "could not add the user parameter: " + lastError());
            paramByNative_.erase(native);
            continue;
        }
        report_.add(docName_, "parameter " + native, why.empty() ? "rebuilt" : "approximate",
                    why.empty() ? (name != native ? "user parameter " + name : "") : "value only: " + why);
    }
}

void Replayer::replayEquations(const json& doc)
{
    if (!doc.contains("equations")) return;
    Ptr<Design> des = design_;
    for (auto& eq : doc["equations"])
    {
        std::string target = eq.value("target", "");
        std::string item = "equation " + target;
        // the target: by its neutral targetRef where the file has one (FORMAT-SPEC 4, "Equations"), else by
        // the source's name for it (D1@SK_Base, D1@Boss-Base)
        auto it = paramByNative_.end();
        if (eq.contains("targetRef") && eq["targetRef"].is_object())
        {
            const json& tr = eq["targetRef"];
            std::string kind = tr.value("kind", "");
            if (kind == "dimension") it = paramByNative_.find("dim:" + tr.value("feature", "") + "|" + tr.value("id", ""));
            else if (kind == "featureField") it = paramByNative_.find("field:" + tr.value("feature", "") + "|" + tr.value("field", ""));
        }
        if (it == paramByNative_.end()) it = paramByNative_.find(target);
        Ptr<Parameter> p;
        if (it != paramByNative_.end()) p = des->allParameters()->itemByName(it->second);
        if (!p)
        {
            report_.add(docName_, item, "failed", "its target has no Fusion parameter");
            continue;
        }
        std::string why;
        std::string expr = translateExpression(eq.value("expression", ""), why);
        if (expr.empty())
        {
            report_.add(docName_, item, "failed", why);
            continue;
        }
        std::string unit = p->unit();
        std::string full = unit.empty() ? expr : "(" + expr + ") * 1 " + unit;
        if (Ptr<UserParameter> up = p)
            full = expr;   // a global variable stays unitless
        if (!p->expression(full))
        {
            report_.add(docName_, item, "failed", "Fusion refused the expression " + full + ": " + lastError());
            continue;
        }
        report_.add(docName_, item, "rebuilt", p->name() + " = " + full);
    }
}

// The Fusion parameter a flex probe changes, or null.
Ptr<Parameter> Replayer::parameterFor(const std::string& native)
{
    auto it = paramByNative_.find(native);
    if (it == paramByNative_.end()) return nullptr;
    return design_->allParameters()->itemByName(it->second);
}

void Replayer::registerParameter(const std::string& native, const Ptr<Parameter>& p)
{
    if (!p || native.empty()) return;
    // a feature dimension keeps its native name too (D1@Boss-Base -> D1_Boss_Base) instead of Fusion's d<n>
    std::string n = p->name();
    if (n.size() > 1 && n[0] == 'd' && std::isdigit(static_cast<unsigned char>(n[1])) && native.find('@') != std::string::npos)
    {
        if (p->name(ident(native))) n = p->name();
        p->comment(native);
    }
    paramByNative_[native] = n;
}

} // namespace ncxf
