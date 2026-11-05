// Brief pseudo-code markdown for blocks-file documents (`xblox run --md`).
// Pure formatting over the raw JSON tree: no input resolution, no execution.

#include "xblox/xblox_markdown.hpp"

#include "xblox/xblox_diagram.hpp"
#include "xblox/xblox_sequence_diagram.hpp"
#include "xblox/blocks/block_registry.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include "string/string_utils.hpp"

namespace media::xblox {

namespace {

std::string json_str(const nlohmann::json& o, const char* key)
{
    if (!o.is_object() || !o.contains(key) || !o[key].is_string())
        return {};
    return o[key].get<std::string>();
}

// kind -> lowercased registry group ("audio", "files", ...) for the logical
// `<group>/<kind>` prefix on non-language blocks. Core groups (Flow/Context)
// already render as pseudo-code keywords, so they map to "".
const std::string& kind_group(const std::string& kind)
{
    static const std::map<std::string, std::string> groups = [] {
        std::map<std::string, std::string> m;
        for (const auto& d : blocks::registered_block_definitions()) {
            if (d.group.empty() || d.group == "Flow" || d.group == "Context")
                continue;
            std::string g = d.group;
            g = media::strutil::lower_ascii(std::move(g));
            m[d.kind] = std::move(g);
        }
        return m;
    }();
    static const std::string empty;
    const auto it = groups.find(kind);
    return it == groups.end() ? empty : it->second;
}

// Truncate to max_len bytes without splitting a UTF-8 sequence; appends an
// ellipsis when something was cut.
std::string clip(std::string s, std::size_t max_len)
{
    if (max_len == 0 || s.size() <= max_len)
        return s;
    std::size_t cut = max_len;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0u) == 0x80u)
        --cut;
    s.resize(cut);
    return s + "...";
}

std::string code(const std::string& s)
{
    return "`" + s + "`";
}

// Compact one-line JSON for context values / scalar params.
std::string dump_value(const nlohmann::json& v, std::size_t max_len)
{
    return clip(v.dump(), max_len);
}

// First non-empty free-text param  -  the generic fallback detail for kinds
// without a dedicated pseudo-code shape (fsWrite, Parse, network, ...).
std::string generic_detail(const nlohmann::json& block, std::size_t max_len)
{
    static constexpr const char* kDetailKeys[] = {
        "label", "name", "path", "url", "message", "command", "method",
        "filter", "input", "expression", "condition", "variable",
        "shortcut", "keys", "session",
    };
    for (const char* key : kDetailKeys) {
        const std::string v = json_str(block, key);
        if (!v.empty())
            return code(clip(v, max_len));
    }
    return {};
}

// One pseudo-code line per block (children are rendered by the caller).
std::string pseudo_line(const nlohmann::json& block, const MarkdownOptions& opt)
{
    const std::string kind = json_str(block, "kind");
    const auto text = [&](const char* key) { return clip(json_str(block, key), opt.max_text); };

    if (kind == "if")
        return "if " + code(text("condition"));
    if (kind == "elseIf")
        return "else if " + code(text("condition"));
    if (kind == "else")
        return "else";
    if (kind == "switch")
        return "switch " + code(text("variable"));
    if (kind == "case") {
        const std::string comparator = json_str(block, "comparator");
        const std::string expression = text("expression");
        if (comparator.empty() || comparator == "===" || comparator == "==")
            return "case " + code(expression);
        return "case " + code(comparator + " " + expression);
    }
    if (kind == "switchDefault")
        return "default";
    if (kind == "while")
        return "while " + code(text("condition"));
    if (kind == "for") {
        const std::string variable = json_str(block, "variable");
        return "for " + code(variable + " = " + json_str(block, "initial")
            + "; " + variable + " " + json_str(block, "comparator") + " " + json_str(block, "final")
            + "; " + json_str(block, "modifier"));
    }
    if (kind == "break")
        return "break";
    if (kind == "stdout")
        return "print " + code("\"" + text("message") + "\"");
    if (kind == "log") {
        const std::string level = json_str(block, "level");
        return "log " + (level.empty() ? std::string{} : "[" + level + "] ") + code(text("message"));
    }
    if (kind == "wait") {
        if (block.contains("ms") && block["ms"].is_number())
            return "wait " + dump_value(block["ms"], opt.max_text) + "ms";
        return "wait";
    }
    if (kind == "setVariable") {
        const std::string name = json_str(block, "name");
        if (!json_str(block, "expression").empty())
            return "set " + code(name + " = " + text("expression"));
        if (!json_str(block, "valueFrom").empty())
            return "set " + code(name + " = ${" + json_str(block, "valueFrom") + "}");
        if (block.contains("value"))
            return "set " + code(name + " = " + dump_value(block["value"], opt.max_text));
        return "set " + code(name);
    }
    if (kind == "getVariable") {
        const std::string target = json_str(block, "target");
        return "get " + code(json_str(block, "name") + (target.empty() ? "" : " -> " + target));
    }
    if (kind == "command") {
        const nlohmann::json& payload = (block.contains("command") && block["command"].is_object())
            ? block["command"] : block;
        const std::string id = json_str(payload, "id").empty() ? json_str(block, "id") : json_str(payload, "id");
        std::string action;
        if (!json_str(payload, "cliCommand").empty())         action = "cli: " + json_str(payload, "cliCommand");
        else if (!json_str(payload, "appCommand").empty())    action = "app: " + json_str(payload, "appCommand");
        else if (!json_str(payload, "ribbonCommand").empty()) action = "ribbon: " + json_str(payload, "ribbonCommand");
        else if (payload.contains("externalCommand"))         action = "external";
        else if (!json_str(payload, "url").empty())           action = "url: " + json_str(payload, "url");
        else if (!json_str(payload, "path").empty())          action = "path: " + json_str(payload, "path");
        else                                                  action = "metadata";
        return "command " + code(id.empty() ? json_str(payload, "label") : id)
            + " (" + clip(action, opt.max_text) + ")";
    }
    if (kind == "shell" || kind == "Shell")
        return "shell " + code(text("command"));
    if (kind == "group" || kind == "runScript")
        return "group";

    // Fallback (non-language blocks): logical `<group>/<kind>` prefix from the
    // registry, plus the first descriptive scalar param, if any.
    const std::string detail = generic_detail(block, opt.max_text);
    const std::string& group = kind_group(kind);
    std::string shown = kind.empty() ? "unknown" : (group.empty() ? kind : group + "/" + kind);
    if (!detail.empty())
        shown += " " + detail;
    if (block.contains("append") && block["append"].is_boolean() && block["append"].get<bool>())
        shown += " (append)";
    return shown;
}

// `-> <var>` suffix for blocks that store their result (storeAs / storeResult /
// target). getVariable renders its target inline, so it is exempt here.
std::string store_suffix(const nlohmann::json& block)
{
    const std::string kind = json_str(block, "kind");
    if (kind == "getVariable" || kind == "setVariable")
        return {};
    std::string store = json_str(block, "storeAs");
    if (store.empty()) store = json_str(block, "storeResult");
    if (store.empty()) store = json_str(block, "target");
    return store.empty() ? std::string{} : " -> " + store;
}

bool is_excluded(const nlohmann::json& block, const MarkdownOptions& opt)
{
    const std::string kind = json_str(block, "kind");
    return std::find(opt.exclude.begin(), opt.exclude.end(), kind) != opt.exclude.end();
}

bool is_core_language_kind(const std::string& kind)
{
    static constexpr const char* kCore[] = {
        "if", "elseIf", "else", "switch", "case", "switchDefault",
        "while", "for", "break",
        "stdout", "log", "wait", "setVariable", "getVariable",
        "command", "shell", "Shell", "group", "runScript",
        "fetch", "network", "httpRequest", "Parse", "parse",
    };
    for (const char* k : kCore) {
        if (kind == k)
            return true;
    }
    return false;
}

bool is_known_block_kind(const std::string& kind)
{
    if (kind.empty())
        return false;
    if (is_core_language_kind(kind))
        return true;
    return blocks::builtin_block_descriptor(kind) != nullptr;
}

bool is_block_disabled(const nlohmann::json& block)
{
    return block.is_object() && block.contains("enabled") && block["enabled"].is_boolean()
        && !block["enabled"].get<bool>();
}

std::string blocked_node_label(const nlohmann::json& block, bool disabled, const MarkdownOptions& opt)
{
    const std::string kind = json_str(block, "kind");
    const std::string prefix = disabled ? "disabled: " : "unknown: ";
    if (kind.empty())
        return prefix + "block";
    if (opt.expand_parameters || is_core_language_kind(kind))
        return prefix + pseudo_line(block, opt);
    const std::string& group = kind_group(kind);
    std::string shown = group.empty() ? kind : group + "/" + kind;
    return prefix + shown;
}

// ── Mermaid flowchart emitter ───────────────────────────────────────────────
// Maps the block tree onto a flowchart: leaves are rectangles, conditions /
// switches / loops are decision diamonds. emit_list() threads "dangling
// exits" (node + edge label) through the walk so sibling order, chain
// branches, switch cases, loop back-edges and break targets all wire to
// whatever executes next.

// Semantic edge category, used for linkStyle colouring.
enum class EdgeKind { seq, yes, no, loop, choice };

struct MermaidExit {
    std::string id;
    std::string label;                  // edge label (|yes| / |no| / |done| / case value)
    EdgeKind edge = EdgeKind::seq;
};

class MermaidEmitter {
public:
    explicit MermaidEmitter(const MarkdownOptions& opt) : opt_(opt) {}

    std::string render(const nlohmann::json& roots)
    {
        emit_list(roots, {}, nullptr);
        std::ostringstream out;
        out << "graph " << opt_.mermaid_direction << "\n" << body_.str();
        const bool color_edges  = opt_.mermaid_color == "edges"  || opt_.mermaid_color == "both";
        const bool color_groups = opt_.mermaid_color == "groups" || opt_.mermaid_color == "both";
        if (color_edges)
            append_link_styles(out);
        if (color_groups)
            append_class_defs(out);
        return out.str();
    }

private:
    const MarkdownOptions& opt_;
    std::ostringstream body_;
    int next_id_ = 0;
    int next_edge_ = 0;
    // EdgeKind -> emitted edge indices (linkStyle addresses edges by index).
    std::map<EdgeKind, std::vector<int>> edge_index_;
    // class name (registry group / "flow") -> node ids, insertion-ordered.
    std::vector<std::pair<std::string, std::vector<std::string>>> node_class_;

    // Tokyo Night-ish palette, matching the sample the CLI help references.
    static const char* edge_color(EdgeKind k)
    {
        switch (k) {
            case EdgeKind::yes:    return "#9ece6a"; // green
            case EdgeKind::no:     return "#f7768e"; // red
            case EdgeKind::loop:   return "#7aa2f7"; // blue
            case EdgeKind::choice: return "#e0af68"; // yellow (switch cases)
            case EdgeKind::seq:    break;
        }
        return "#565f89"; // muted gray, applied via linkStyle default
    }

    void append_link_styles(std::ostringstream& out) const
    {
        for (const auto& [kind, indices] : edge_index_) {
            if (kind == EdgeKind::seq || indices.empty())
                continue;
            out << "  linkStyle ";
            for (std::size_t i = 0; i < indices.size(); ++i)
                out << (i ? "," : "") << indices[i];
            out << " stroke:" << edge_color(kind) << ",stroke-width:2px\n";
        }
        out << "  linkStyle default stroke:" << edge_color(EdgeKind::seq) << "\n";
    }

    void append_class_defs(std::ostringstream& out) const
    {
        // Stable per-group colours: "flow" is fixed, registry groups cycle the
        // palette in first-appearance order.
        static constexpr const char* kGroupPalette[] = {
            "#7dcfff", "#ff9e64", "#2ac3de", "#c0caf5", "#b4f9f8", "#d18616",
        };
        std::size_t next_color = 0;
        for (const auto& [name, ids] : node_class_) {
            const char* color = name == "flow" ? "#bb9af7"
                : name == "disabled" ? "#6b7280"
                : name == "unknown" ? "#f59e0b"
                : kGroupPalette[next_color++ % std::size(kGroupPalette)];
            out << "  classDef " << name << " stroke:" << color << ",stroke-width:1.5px\n";
            out << "  class ";
            for (std::size_t i = 0; i < ids.size(); ++i)
                out << (i ? "," : "") << ids[i];
            out << " " << name << "\n";
        }
    }

    void tag_node(const std::string& id, const std::string& class_name)
    {
        if (class_name.empty())
            return;
        for (auto& [name, ids] : node_class_) {
            if (name == class_name) {
                ids.push_back(id);
                return;
            }
        }
        node_class_.push_back({class_name, {id}});
    }

    // Mermaid label hygiene: quotes break "..." labels, pipes break |...|
    // edge labels, backticks are markdown affordances from pseudo_line.
    static std::string escape(std::string s)
    {
        std::replace(s.begin(), s.end(), '"', '\'');
        std::replace(s.begin(), s.end(), '|', '/');
        s.erase(std::remove(s.begin(), s.end(), '`'), s.end());
        return s;
    }

    std::string add_node(const std::string& text, bool decision,
                         const std::string& class_name = {})
    {
        const std::string id = "B" + std::to_string(next_id_++);
        body_ << "  " << id << (decision ? "{\"" : "[\"") << escape(text)
              << (decision ? "\"}" : "\"]") << "\n";
        tag_node(id, decision ? "flow" : class_name);
        return id;
    }

    void connect(const std::vector<MermaidExit>& from, const std::string& to)
    {
        for (const auto& e : from) {
            body_ << "  " << e.id
                  << (e.label.empty() ? " --> " : " -->|" + escape(e.label) + "| ")
                  << to << "\n";
            edge_index_[e.edge].push_back(next_edge_++);
        }
    }

    std::string leaf_label(const nlohmann::json& block)
    {
        if (opt_.expand_parameters)
            return pseudo_line(block, opt_) + store_suffix(block);
        const std::string kind = json_str(block, "kind");
        std::string label;
        if (kind == "stdout")            label = "print";
        else if (kind == "log")          label = "log";
        else if (kind == "setVariable")  label = "set " + json_str(block, "name");
        else if (kind == "getVariable")  label = "get " + json_str(block, "name");
        else {
            const std::string& group = kind_group(kind);
            label = group.empty() ? kind : group + "/" + kind;
        }
        return label + store_suffix(block);
    }

    std::string case_edge_label(const nlohmann::json& block)
    {
        const std::string comparator = json_str(block, "comparator");
        const std::string expression = clip(json_str(block, "expression"), opt_.max_text);
        if (comparator.empty() || comparator == "===" || comparator == "==")
            return expression;
        return comparator + " " + expression;
    }

    // Walks one sibling list. `incoming` are the dangling exits feeding the
    // first rendered block; the return value is the dangling exits after the
    // last one. `break_sinks` collects break-node ids inside the nearest loop.
    std::vector<MermaidExit> emit_list(const nlohmann::json& blocks,
                                       std::vector<MermaidExit> incoming,
                                       std::vector<std::string>* break_sinks)
    {
        if (!blocks.is_array())
            return incoming;
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            const nlohmann::json& block = blocks[i];
            if (!block.is_object() || is_excluded(block, opt_))
                continue;
            const std::string kind = json_str(block, "kind");
            const bool disabled = is_block_disabled(block);
            const bool unknown = !is_known_block_kind(kind);
            if (disabled || unknown) {
                const std::string id = add_node(
                    blocked_node_label(block, disabled, opt_), false, disabled ? "disabled" : "unknown");
                connect(incoming, id);
                incoming.clear();
                continue;
            }

            if (kind == "if") {
                // Consume the whole sibling chain (if / elseIf* / else?),
                // mirroring the executor's chain rule (docs/xblox/language.md D1).
                std::vector<MermaidExit> exits;
                std::vector<MermaidExit> pending = std::move(incoming);
                bool first = true;
                for (; i < blocks.size(); ++i) {
                    const nlohmann::json& member = blocks[i];
                    const std::string mk = member.is_object() ? json_str(member, "kind") : std::string{};
                    if (!first && mk != "elseIf" && mk != "else")
                        break; // non-chain sibling (or a fresh `if`) ends the chain
                    if (is_excluded(member, opt_)) {
                        first = false;
                        continue;
                    }
                    if (mk == "if" || mk == "elseIf") {
                        const std::string cond = add_node(clip(json_str(member, "condition"), opt_.max_text), true);
                        connect(pending, cond);
                        const auto body = mk == "if"
                            ? member.value("consequent", nlohmann::json::array())
                            : member.value("items", nlohmann::json::array());
                        auto body_exits = emit_list(body, {{cond, "yes", EdgeKind::yes}}, break_sinks);
                        exits.insert(exits.end(), body_exits.begin(), body_exits.end());
                        pending = {{cond, "no", EdgeKind::no}};
                    } else { // else  -  consumes the chain's remaining "no" path
                        auto body_exits = emit_list(member.value("items", nlohmann::json::array()),
                                                    std::move(pending), break_sinks);
                        exits.insert(exits.end(), body_exits.begin(), body_exits.end());
                        pending.clear();
                        ++i;
                        break;
                    }
                    first = false;
                }
                --i; // outer loop increments past the last consumed member
                exits.insert(exits.end(), pending.begin(), pending.end());
                incoming = std::move(exits);
            } else if (kind == "switch") {
                const std::string sw = add_node("switch " + clip(json_str(block, "variable"), opt_.max_text), true);
                connect(incoming, sw);
                std::vector<MermaidExit> exits;
                bool has_default = false;
                for (const auto& item : block.value("items", nlohmann::json::array())) {
                    if (!item.is_object() || is_excluded(item, opt_))
                        continue;
                    const std::string ik = json_str(item, "kind");
                    std::string label;
                    if (ik == "switchDefault") {
                        has_default = true;
                        label = "default";
                    } else {
                        label = case_edge_label(item);
                    }
                    auto body_exits = emit_list(item.value("consequent", nlohmann::json::array()),
                                                {{sw, label, EdgeKind::choice}}, break_sinks);
                    exits.insert(exits.end(), body_exits.begin(), body_exits.end());
                }
                if (!has_default)
                    exits.push_back({sw, "no match", EdgeKind::no});
                incoming = std::move(exits);
            } else if (kind == "while" || kind == "for") {
                std::string text;
                if (kind == "while") {
                    text = "while " + clip(json_str(block, "condition"), opt_.max_text);
                } else {
                    const std::string variable = json_str(block, "variable");
                    text = "for " + variable + " = " + json_str(block, "initial")
                        + "; " + variable + " " + json_str(block, "comparator") + " " + json_str(block, "final")
                        + "; " + json_str(block, "modifier");
                }
                const std::string loop = add_node(text, true);
                connect(incoming, loop);
                std::vector<std::string> local_breaks;
                auto body_exits = emit_list(block.value("items", nlohmann::json::array()),
                                            {{loop, "do", EdgeKind::loop}}, &local_breaks);
                for (auto& e : body_exits)
                    e.edge = EdgeKind::loop; // back-edges read as loop flow
                connect(body_exits, loop);
                incoming = {{loop, "done", EdgeKind::loop}};
                for (const auto& b : local_breaks)
                    incoming.push_back({b, "", EdgeKind::loop});
            } else if (kind == "break" && break_sinks) {
                const std::string id = add_node("break", false);
                connect(incoming, id);
                break_sinks->push_back(id);
                incoming.clear(); // siblings after break never run
            } else {
                const std::string id = add_node(leaf_label(block), false, kind_group(kind));
                connect(incoming, id);
                incoming = {{id, ""}};
                // Containers and iterable producers (group, fsList, ...) flow
                // through their items children.
                if (block.contains("items") && block["items"].is_array())
                    incoming = emit_list(block["items"], std::move(incoming), break_sinks);
            }
        }
        return incoming;
    }
};

// ── Mermaid sequence emitter (delegates to shared sequence model) ───────────

class SequenceEmitter {
public:
    explicit SequenceEmitter(const MarkdownOptions& opt) : opt_(opt) {}

    std::string render(const nlohmann::json& blocks_file)
    {
        return sequence_model_to_mermaid(build_sequence_model(blocks_file, opt_));
    }

private:
    const MarkdownOptions& opt_;
};


void append_blocks(const nlohmann::json& blocks,
                   int depth,
                   const MarkdownOptions& opt,
                   std::ostringstream& out)
{
    if (!blocks.is_array())
        return;
    std::size_t n = 0;
    for (const auto& block : blocks) {
        if (!block.is_object())
            continue;
        if (is_excluded(block, opt))
            continue; // filtered kinds vanish entirely (subtree included)
        ++n;
        // Numbered lists need wider indents so nested items stay attached to
        // their parent entry when the markdown is rendered.
        const std::string indent(static_cast<std::size_t>(depth) * (opt.numbered ? 3u : 2u), ' ');
        const std::string bullet = opt.numbered ? std::to_string(n) + ". " : "- ";
        out << indent << bullet << pseudo_line(block, opt) << store_suffix(block) << "\n";

        // Child lists: `consequent` (if/case/switchDefault) and `items`
        // (loops, group, else/elseIf, switch case containers, iterables).
        for (const char* key : {"consequent", "items"}) {
            if (block.contains(key) && block[key].is_array())
                append_blocks(block[key], depth + 1, opt, out);
        }
    }
}

} // namespace

std::string document_to_markdown(const nlohmann::json& blocks_file, const MarkdownOptions& options)
{
    std::ostringstream out;
    if (!options.title.empty())
        out << "# " << options.title << "\n\n";

    if (blocks_file.is_object() && blocks_file.contains("description")
        && blocks_file["description"].is_string()) {
        const std::string desc = blocks_file["description"].get<std::string>();
        if (!desc.empty())
            out << desc << "\n\n";
    }

    // Document loop settings change the whole execution model (controller-style
    // scripts re-run until cancelled), so surface them right under the title.
    if (blocks_file.is_object() && blocks_file.contains("loop") && blocks_file["loop"].is_object()) {
        const nlohmann::json& loop = blocks_file["loop"];
        if (loop.value("enabled", false)) {
            const long long interval = loop.value("intervalMs", 0LL);
            out << "> loops "
                << (interval > 0 ? "every " + std::to_string(interval) + "ms" : "continuously")
                << (loop.value("reset", false) ? "; state resets each pass" : "; state persists across passes")
                << "\n\n";
        }
    }

    const nlohmann::json context = (blocks_file.is_object() && blocks_file.contains("context")
                                    && blocks_file["context"].is_object())
        ? blocks_file["context"] : nlohmann::json::object();
    out << "## Context\n\n";
    if (context.empty()) {
        out << "_(no context variables)_\n";
    } else {
        for (auto it = context.begin(); it != context.end(); ++it)
            out << "- " << it.key() << ": " << code(dump_value(it.value(), options.max_text)) << "\n";
    }

    out << "\n## Script\n\n";
    const nlohmann::json roots = (blocks_file.is_object() && blocks_file.contains("roots")
                                  && blocks_file["roots"].is_array())
        ? blocks_file["roots"] : nlohmann::json::array();
    if (roots.empty()) {
        out << "_(empty)_\n";
    } else if (options.diagram && options.diagram_type == "sequence") {
        const std::string svg_embedded = render_sequence_diagram_svg(
            blocks_file, options, options.diagram_styles, options.title, false);
        out << "```svg\n" << svg_embedded << "```\n";
        if (!options.diagram_dst.empty() || !options.diagram_png.empty()) {
            const std::string svg_file = render_sequence_diagram_svg(
                blocks_file, options, options.diagram_styles, options.title, true);
            if (!options.diagram_dst.empty()) {
                std::string svg_err;
                if (!save_diagram_svg_to_file(svg_file, options.diagram_dst, svg_err))
                    out << "\n<!-- diagram svg error: " << svg_err << " -->\n";
            }
            if (!options.diagram_png.empty()) {
                std::string png_err;
                if (!save_diagram_svg_as_png(svg_file, options.diagram_png, 144.0, png_err))
                    out << "\n<!-- diagram png error: " << png_err << " -->\n";
            }
        }
    } else if (options.diagram) {
        const std::string svg_embedded = render_flow_diagram_svg(
            roots, options, options.diagram_styles,
            options.mermaid_direction, options.title, false);
        out << "```svg\n" << svg_embedded << "```\n";
        if (!options.diagram_dst.empty() || !options.diagram_png.empty()) {
            const std::string svg_file = render_flow_diagram_svg(
                roots, options, options.diagram_styles,
                options.mermaid_direction, options.title, true);
            if (!options.diagram_dst.empty()) {
                std::string svg_err;
                if (!save_diagram_svg_to_file(svg_file, options.diagram_dst, svg_err))
                    out << "\n<!-- diagram svg error: " << svg_err << " -->\n";
            }
            if (!options.diagram_png.empty()) {
                std::string png_err;
                if (!save_diagram_svg_as_png(svg_file, options.diagram_png, 144.0, png_err))
                    out << "\n<!-- diagram png error: " << png_err << " -->\n";
            }
        }
    } else if (options.mermaid && options.mermaid_type == "sequence") {
        SequenceEmitter emitter(options);
        out << "```mermaid\n" << emitter.render(blocks_file) << "```\n";
    } else if (options.mermaid) {
        MermaidEmitter emitter(options);
        out << "```mermaid\n" << emitter.render(roots) << "```\n";
    } else {
        append_blocks(roots, 0, options, out);
    }
    return out.str();
}

namespace {

std::string json_string_opt(const nlohmann::json& j, const char* key)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_string())
        return {};
    return j[key].get<std::string>();
}

bool json_bool_opt(const nlohmann::json& j, const char* key, bool fallback)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_boolean())
        return fallback;
    return j[key].get<bool>();
}

} // namespace

bool apply_diagram_options_from_json(const nlohmann::json& options,
                                     const std::string& title,
                                     const std::string& cwd,
                                     MarkdownOptions& out,
                                     std::string& error)
{
    out.title = title;
    const std::string diagram_type = json_string_opt(options, "diagramType");
    if (!diagram_type.empty())
        out.diagram_type = diagram_type;
    const std::string direction = json_string_opt(options, "mermaidDirection");
    if (!direction.empty())
        out.mermaid_direction = direction;

    std::string styles_path = json_string_opt(options, "stylesPath");
    if (styles_path.empty())
        styles_path = default_diagram_styles_expression();
    out.diagram_styles_path = styles_path;
    const std::string theme = json_string_opt(options, "theme");
    if (!load_diagram_styles(styles_path, cwd, out.diagram_styles, error, theme.empty() ? "light" : theme))
        return false;

    out.diagram_show_arrows              = out.diagram_styles.render.show_arrows;
    out.diagram_show_links               = out.diagram_styles.render.show_links;
    out.diagram_show_disabled            = out.diagram_styles.render.show_disabled;
    out.diagram_show_variables           = out.diagram_styles.render.show_variables;
    out.diagram_show_groups              = out.diagram_styles.render.show_groups;
    out.diagram_humanize_node_names      = out.diagram_styles.render.humanize_node_names;
    out.diagram_show_variable_delimiters = out.diagram_styles.render.show_variable_delimiters;
    out.diagram_variable_link_style      = out.diagram_styles.render.variable_link_style;
    out.diagram_variable_input_style     = out.diagram_styles.render.variable_input_style;
    out.diagram_link_style               = out.diagram_styles.render.link_style;
    out.diagram_show_parameters          = out.diagram_styles.render.show_parameters;
    out.diagram_show_parameters_grouped  = out.diagram_styles.render.show_parameters_grouped;
    out.diagram_show_parameter_values    = out.diagram_styles.render.show_parameter_values;

    if (options.contains("showArrows"))
        out.diagram_show_arrows = json_bool_opt(options, "showArrows", out.diagram_show_arrows);
    if (options.contains("showLinks"))
        out.diagram_show_links = json_bool_opt(options, "showLinks", out.diagram_show_links);
    if (options.contains("showDisabled"))
        out.diagram_show_disabled = json_bool_opt(options, "showDisabled", out.diagram_show_disabled);
    if (options.contains("showVariables"))
        out.diagram_show_variables = json_bool_opt(options, "showVariables", out.diagram_show_variables);
    if (options.contains("showGroups"))
        out.diagram_show_groups = json_bool_opt(options, "showGroups", out.diagram_show_groups);
    if (options.contains("humanizeNodeNames"))
        out.diagram_humanize_node_names =
            json_bool_opt(options, "humanizeNodeNames", out.diagram_humanize_node_names);
    if (options.contains("showVariableDelimiters"))
        out.diagram_show_variable_delimiters = json_bool_opt(
            options, "showVariableDelimiters", out.diagram_show_variable_delimiters);
    if (options.contains("expandParameters"))
        out.expand_parameters = json_bool_opt(options, "expandParameters", out.expand_parameters);
    if (options.contains("showParametersGrouped"))
        out.diagram_show_parameters_grouped = json_bool_opt(
            options, "showParametersGrouped", out.diagram_show_parameters_grouped);
    if (options.contains("showParameterValues"))
        out.diagram_show_parameter_values = json_bool_opt(
            options, "showParameterValues", out.diagram_show_parameter_values);

    const std::string show_params = json_string_opt(options, "showParameters");
    if (!show_params.empty())
        out.diagram_show_parameters = show_params;
    const std::string link_style = json_string_opt(options, "linkStyle");
    if (!link_style.empty())
        out.diagram_link_style = link_style;
    const std::string var_link = json_string_opt(options, "variableLinkStyle");
    if (!var_link.empty())
        out.diagram_variable_link_style = var_link;
    const std::string var_input = json_string_opt(options, "variableInputStyle");
    if (!var_input.empty())
        out.diagram_variable_input_style = var_input;

    if (out.expand_parameters)
        out.diagram_show_parameters = "all";
    out.expand_parameters = out.diagram_show_parameters == "all";

    if (options.contains("exclude") && options["exclude"].is_array()) {
        out.exclude.clear();
        for (const auto& item : options["exclude"]) {
            if (item.is_string())
                out.exclude.push_back(item.get<std::string>());
        }
    }
    return true;
}

} // namespace media::xblox
