#include "xblox_expressions.hpp"

#include "logger/logger.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>

namespace media::xblox::expr {
namespace {

bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string trim_ascii(std::string s)
{
    while (!s.empty() && is_ws(s.front()))
        s.erase(s.begin());
    while (!s.empty() && is_ws(s.back()))
        s.pop_back();
    return s;
}

void replace_all(std::string& s, std::string_view from, std::string_view to)
{
    if (from.empty())
        return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

bool all_digits(const std::string& s)
{
    if (s.empty())
        return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    return true;
}

bool ident_start(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool ident_char(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Characters that can appear in a reference term inside an expression.
bool term_char(char c)
{
    return ident_char(c) || c == '.' || c == '[' || c == ']';
}

const nlohmann::json* walk_segments(const nlohmann::json& root, const std::vector<std::string>& parts)
{
    if (parts.empty())
        return nullptr;
    const nlohmann::json* cur = &root;
    for (const auto& part : parts) {
        if (cur->is_object() && cur->contains(part)) {
            cur = &(*cur)[part];
        } else if (cur->is_array() && all_digits(part)) {
            const size_t idx = static_cast<size_t>(std::stoull(part));
            if (idx >= cur->size())
                return nullptr;
            cur = &(*cur)[idx];
        } else {
            return nullptr;
        }
    }
    return cur;
}

bool json_to_number(const nlohmann::json& value, double& out)
{
    if (value.is_number()) {
        out = value.get<double>();
        return true;
    }
    if (value.is_boolean()) {
        out = value.get<bool>() ? 1.0 : 0.0;
        return true;
    }
    if (value.is_string()) {
        try {
            size_t consumed = 0;
            const std::string s = value.get<std::string>();
            out = std::stod(s, &consumed);
            return consumed == s.size();
        } catch (...) {
            return false;
        }
    }
    return false;
}

bool ends_with(const std::string& s, std::string_view suffix)
{
    return s.size() > suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ── expression front-end stages ──────────────────────────────────────────────

// `${ref}` inside an expression field unwraps to the bare reference so both
// syntaxes mean the same thing (`${PREVIOUS}.data[0] == 2` ≡
// `PREVIOUS.data[0] == 2`). Keys with ':' (ENV:/KNOWNFOLDER:) or other
// non-reference characters are left alone.
void unwrap_interpolations(std::string& text)
{
    size_t i = 0;
    while ((i = text.find("${", i)) != std::string::npos) {
        const size_t close = text.find('}', i + 2);
        if (close == std::string::npos)
            return;
        const std::string key = text.substr(i + 2, close - i - 2);
        bool reference = !key.empty();
        for (char c : key) {
            if (!term_char(c)) {
                reference = false;
                break;
            }
        }
        if (reference) {
            text.replace(i, close - i + 1, key);
            i += key.size();
        } else {
            i = close + 1;
        }
    }
}

// Resolve a comparison term to its string form: quoted literals keep their
// content, references resolve through the context, and unresolved names
// degrade to their own text (mirrors switch/case matching semantics).
std::string resolve_term(const nlohmann::json& context, const std::string& term)
{
    std::string out;
    if (reference_string(context, term, out))
        return out;
    return term;
}

// Fold `term == "literal"` / `"literal" != term` (either side quoted) into
// 1/0 by comparing against the context now  -  muParser never sees the strings.
void fold_string_comparisons(std::string& text, const nlohmann::json& context)
{
    size_t i = 0;
    while (i < text.size()) {
        const char q = text[i];
        if (q != '"' && q != '\'') {
            ++i;
            continue;
        }
        const size_t close = text.find(q, i + 1);
        if (close == std::string::npos)
            return;
        const std::string literal = text.substr(i + 1, close - i - 1);

        // <term> ==|!= "literal"
        size_t l = i;
        while (l > 0 && is_ws(text[l - 1]))
            --l;
        if (l >= 2 && text[l - 1] == '=' && (text[l - 2] == '=' || text[l - 2] == '!')) {
            const bool ne = text[l - 2] == '!';
            size_t te = l - 2;
            while (te > 0 && is_ws(text[te - 1]))
                --te;
            size_t ts = te;
            std::string lhs;
            bool have_term = false;
            if (ts > 0 && (text[ts - 1] == '"' || text[ts - 1] == '\'')) {
                const char tq = text[ts - 1];
                const size_t topen = ts >= 2 ? text.rfind(tq, ts - 2) : std::string::npos;
                if (topen != std::string::npos) {
                    lhs = text.substr(topen + 1, ts - 2 - topen);
                    ts = topen;
                    have_term = true;
                }
            } else {
                while (ts > 0 && term_char(text[ts - 1]))
                    --ts;
                if (ts < te) {
                    lhs = resolve_term(context, text.substr(ts, te - ts));
                    have_term = true;
                }
            }
            if (have_term) {
                const bool equal = (lhs == literal);
                const char* repl = (equal != ne) ? "1" : "0";
                text.replace(ts, close + 1 - ts, repl);
                i = ts + 1;
                continue;
            }
        }

        // "literal" ==|!= <term>
        size_t r = close + 1;
        while (r < text.size() && is_ws(text[r]))
            ++r;
        if (r + 1 < text.size() && (text[r] == '=' || text[r] == '!') && text[r + 1] == '=') {
            const bool ne = text[r] == '!';
            size_t ts = r + 2;
            while (ts < text.size() && is_ws(text[ts]))
                ++ts;
            size_t te = ts;
            std::string rhs;
            bool have_term = false;
            if (ts < text.size() && (text[ts] == '"' || text[ts] == '\'')) {
                const char tq = text[ts];
                const size_t tclose = text.find(tq, ts + 1);
                if (tclose != std::string::npos) {
                    rhs = text.substr(ts + 1, tclose - ts - 1);
                    te = tclose + 1;
                    have_term = true;
                }
            } else {
                while (te < text.size() && term_char(text[te]))
                    ++te;
                if (te > ts) {
                    rhs = resolve_term(context, text.substr(ts, te - ts));
                    have_term = true;
                }
            }
            if (have_term) {
                const bool equal = (literal == rhs);
                const char* repl = (equal != ne) ? "1" : "0";
                text.replace(i, te - i, repl);
                i += 1;
                continue;
            }
        }

        i = close + 1; // lone literal  -  muParser will reject it, as before
    }
}

// Rewrite nested references (`a.b[0]`) to synthetic slot names and sanitize
// every remaining identifier for muParser. Plain names keep their legacy
// binding (scope vars, PREVIOUS, nowMs, selectionCount, ...).
void extract_references(const std::string& in, std::string& out, std::vector<ReferenceSlot>& refs)
{
    std::map<std::string, size_t> by_name;
    out.clear();
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        if (!ident_start(in[i])) {
            out.push_back(in[i++]);
            continue;
        }
        size_t j = i;
        bool has_path = false;
        std::string token;
        while (j < in.size()) {
            if (ident_char(in[j])) {
                token.push_back(in[j++]);
                continue;
            }
            if (in[j] == '.' && j + 1 < in.size() &&
                (ident_char(in[j + 1]))) {
                has_path = true;
                token.push_back(in[j++]);
                continue;
            }
            if (in[j] == '[') {
                size_t k = j + 1;
                while (k < in.size() && std::isdigit(static_cast<unsigned char>(in[k])))
                    ++k;
                if (k > j + 1 && k < in.size() && in[k] == ']') {
                    has_path = true;
                    token.append(in, j, k - j + 1);
                    j = k + 1;
                    continue;
                }
            }
            break;
        }
        if (has_path) {
            const std::string name = reference_slot_name(token);
            if (by_name.find(name) == by_name.end()) {
                by_name.emplace(name, refs.size());
                refs.push_back(ReferenceSlot{name, token});
            }
            out += name;
        } else {
            out += mu_var_name(std::move(token));
        }
        i = j;
    }
}

} // namespace

// ── reference paths ──────────────────────────────────────────────────────────

std::vector<std::string> split_reference(const std::string& path)
{
    std::vector<std::string> parts;
    std::string cur;
    for (char ch : path) {
        if (ch == '.' || ch == '[' || ch == ']') {
            if (!cur.empty()) {
                parts.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty())
        parts.push_back(cur);
    return parts;
}

const nlohmann::json* resolve_reference(const nlohmann::json& root, const std::string& path)
{
    if (path.find('.') == std::string::npos && path.find('[') == std::string::npos) {
        if (!root.is_object() || !root.contains(path))
            return nullptr;
        return &root[path];
    }
    return walk_segments(root, split_reference(path));
}

nlohmann::json* resolve_reference(nlohmann::json& root, const std::string& path)
{
    return const_cast<nlohmann::json*>(
        resolve_reference(static_cast<const nlohmann::json&>(root), path));
}

bool reference_number(const nlohmann::json& root, const std::string& path, double& out)
{
    if (const nlohmann::json* v = resolve_reference(root, path))
        return json_to_number(*v, out);

    // Array size pseudo-fields: `items.length` / `items.count`, plus the
    // legacy flattened spellings `items_length` / `items_count`.
    std::vector<std::string> parts = split_reference(path);
    if (parts.empty())
        return false;
    if (parts.size() >= 2 && (parts.back() == "length" || parts.back() == "count")) {
        std::vector<std::string> head(parts.begin(), parts.end() - 1);
        if (const nlohmann::json* v = walk_segments(root, head); v && v->is_array()) {
            out = static_cast<double>(v->size());
            return true;
        }
    }
    const std::string last = parts.back();
    for (std::string_view suffix : {std::string_view("_count"), std::string_view("_length")}) {
        if (!ends_with(last, suffix))
            continue;
        parts.back() = last.substr(0, last.size() - suffix.size());
        if (const nlohmann::json* v = walk_segments(root, parts); v && v->is_array()) {
            out = static_cast<double>(v->size());
            return true;
        }
        parts.back() = last;
    }
    return false;
}

bool reference_string(const nlohmann::json& root, const std::string& path, std::string& out)
{
    const nlohmann::json* v = resolve_reference(root, path);
    if (!v)
        return false;
    out = json_to_display_string(*v);
    return true;
}

std::string json_to_display_string(const nlohmann::json& v)
{
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number_float()) {
        // Whole-number floats print cleanly ("1" not "1.000000"); scope
        // numerics are stored as doubles so this keeps values readable.
        const double d = v.get<double>();
        if (std::isfinite(d) && d == std::floor(d) && std::abs(d) < 1e15)
            return std::to_string(static_cast<long long>(d));
        std::ostringstream os;
        os << d;
        return os.str();
    }
    if (v.is_number_integer())
        return std::to_string(v.get<long long>());
    if (v.is_number_unsigned())
        return std::to_string(v.get<unsigned long long>());
    if (v.is_boolean())
        return v.get<bool>() ? "true" : "false";
    if (v.is_null())
        return "null";
    return v.dump();
}

// ── muParser identifier sanitation ───────────────────────────────────────────

std::string mu_var_name(std::string s)
{
    for (char& ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != '_')
            ch = '_';
    }
    if (s.empty() || std::isdigit(static_cast<unsigned char>(s.front())))
        s.insert(s.begin(), '_');
    return s;
}

std::string reference_slot_name(const std::string& path)
{
    return "_p_" + mu_var_name(path);
}

// ── template pre-pass ────────────────────────────────────────────────────────

std::string interpolate_references(std::string_view text, const nlohmann::json& context, bool quiet)
{
    if (text.find("${") == std::string_view::npos)
        return std::string(text);

    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '$' && i + 1 < text.size() && text[i + 1] == '{') {
            const size_t close = text.find('}', i + 2);
            if (close != std::string_view::npos) {
                const std::string key(text.substr(i + 2, close - i - 2));
                const bool pathish = key.find_first_of(".[") != std::string::npos &&
                                     key.find(':') == std::string::npos;
                if (pathish) {
                    if (const nlohmann::json* v = resolve_reference(context, key)) {
                        out += json_to_display_string(*v);
                    } else {
                        if (!quiet)
                            logger::warn("[xblox] unresolved ${" + key + "}: path not found in context");
                        out += key;
                    }
                    i = close + 1;
                    continue;
                }
            }
        }
        out.push_back(text[i++]);
    }
    return out;
}

// ── expression front-end ─────────────────────────────────────────────────────

PreparedExpression prepare_expression(const std::string& source, const nlohmann::json& context)
{
    PreparedExpression prep;

    std::string text = trim_ascii(source);
    const std::string original = text;

    // Legacy host-expression aliases.
    replace_all(text, "this.", "");
    replace_all(text, "host.hasQueuedWork()", "hasQueuedWork");
    replace_all(text, "hasQueuedWork()", "hasQueuedWork");
    replace_all(text, "(selection?.files?.length ?? 0)", "selectionCount");
    replace_all(text, "selection?.files?.length ?? 0", "selectionCount");
    replace_all(text, "selection.files.length", "selectionCount");
    replace_all(text, "selection_files_count", "selectionCount");

    unwrap_interpolations(text);
    fold_string_comparisons(text, context);
    extract_references(text, prep.text, prep.refs);

    prep.changed = prep.text != original;
    return prep;
}

} // namespace media::xblox::expr
