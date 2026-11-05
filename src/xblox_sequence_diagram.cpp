#include "xblox/xblox_sequence_diagram.hpp"

#include "xblox/blocks/block_registry.hpp"
#include "xblox/xblox_diagram_text.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <unordered_map>
#include <vector>
#include "string/string_utils.hpp"

namespace media::xblox {

namespace {

constexpr double kParticipantMinWidth = 96.0;
constexpr double kHeaderHeight        = 44.0;
constexpr double kFragmentIndent      = 14.0;
constexpr double kCharWidth           = 7.0;
constexpr double kNotePadding         = 8.0;

std::string scrub_label(std::string s);
std::string xml_escape(std::string s);
double estimate_text_width(const std::string& text, double char_w);

double sequence_row_height(const DiagramStyleConstants& styles)
{
    return std::max(32.0, styles.layout.node_spacing_y * 0.68);
}

double sequence_self_loop_width(const DiagramStyleConstants& styles)
{
    return std::max(20.0, styles.layout.branch_lane_width * 0.5);
}

double sequence_self_loop_drop(const DiagramStyleConstants& styles, double row_h)
{
    return std::clamp(row_h * 0.35, 8.0, 14.0);
}

double sequence_header_gap(const DiagramStyleConstants& styles)
{
    return std::max(16.0, styles.layout.link_gap * 0.85);
}

double interpolation_variable_margin(const DiagramStyleConstants& styles)
{
    return std::max(3.0, styles.node.variable_link_margin * 0.5);
}

double estimate_interpolated_text_width(const std::string& text,
                                        const DiagramStyleConstants& styles,
                                        double char_w)
{
    if (!string_has_interpolation(text))
        return estimate_text_width(text, char_w);
    std::vector<LabelSegment> segments;
    split_string_interpolation_segments(segments,
                                        text,
                                        styles.render.show_variable_delimiters,
                                        interpolation_variable_margin(styles));
    return estimate_segments_width(segments, char_w);
}

void append_sequence_event_label(std::ostringstream& svg,
                                 double x,
                                 double y,
                                 const std::string& text,
                                 bool treat_as_variable,
                                 const DiagramStyleConstants& styles,
                                 const std::string& font_family,
                                 double font_size)
{
    const std::string shown = scrub_label(text);
    if (shown.empty())
        return;

    const bool has_interpolation = string_has_interpolation(shown);
    if (treat_as_variable && !has_interpolation) {
        svg << "<text x=\"" << x << "\" y=\"" << y << "\" text-anchor=\"middle\" font-family=\""
            << xml_escape(font_family) << "\" font-size=\"" << font_size << "\" fill=\""
            << styles.node.variable_fill << "\">" << xml_escape(shown) << "</text>\n";
        return;
    }

    if (!has_interpolation) {
        svg << "<text x=\"" << x << "\" y=\"" << y << "\" text-anchor=\"middle\" font-family=\""
            << xml_escape(font_family) << "\" font-size=\"" << font_size << "\" fill=\""
            << styles.edge.label_fill << "\">" << xml_escape(shown) << "</text>\n";
        return;
    }

    std::vector<LabelSegment> segments;
    split_string_interpolation_segments(segments,
                                        shown,
                                        styles.render.show_variable_delimiters,
                                        interpolation_variable_margin(styles));
    svg << "<text x=\"" << x << "\" y=\"" << y << "\" text-anchor=\"middle\" font-family=\""
        << xml_escape(font_family) << "\" font-size=\"" << font_size << "\">";
    append_svg_text_tspans(svg,
                           segments,
                           true,
                           styles.node.label_fill,
                           styles.node.variable_fill);
    svg << "</text>\n";
}

std::string json_str(const nlohmann::json& o, const char* key)
{
    if (!o.is_object() || !o.contains(key) || !o[key].is_string())
        return {};
    return o[key].get<std::string>();
}

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

std::string dump_value(const nlohmann::json& v, std::size_t max_len)
{
    return clip(v.dump(), max_len);
}

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

    const std::string detail = generic_detail(block, opt.max_text);
    const std::string& group = kind_group(kind);
    std::string shown = kind.empty() ? "unknown" : (group.empty() ? kind : group + "/" + kind);
    if (!detail.empty())
        shown += " " + detail;
    if (block.contains("append") && block["append"].is_boolean() && block["append"].get<bool>())
        shown += " (append)";
    return shown;
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

std::string scrub_label(std::string s)
{
    s.erase(std::remove(s.begin(), s.end(), '`'), s.end());
    std::replace(s.begin(), s.end(), '\n', ' ');
    return s;
}

std::string scrub_for_mermaid(std::string s)
{
    s = scrub_label(std::move(s));
    std::replace(s.begin(), s.end(), ';', ',');
    return s;
}

std::string xml_escape(std::string s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c; break;
        }
    }
    return out;
}

std::string participant_name(const std::string& group)
{
    std::string name = group;
    if (!name.empty())
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    return name;
}

double estimate_text_width(const std::string& text, double char_w = kCharWidth)
{
    return text.empty() ? 0.0 : static_cast<double>(text.size()) * char_w;
}

class SequenceBuilder {
public:
    explicit SequenceBuilder(const MarkdownOptions& opt) : opt_(opt) {}

    SequenceModel build(const nlohmann::json& blocks_file)
    {
        SequenceModel model;
        const nlohmann::json roots = blocks_file.value("roots", nlohmann::json::array());

        if (blocks_file.contains("loop") && blocks_file["loop"].is_object()
            && blocks_file["loop"].value("enabled", false)) {
            model.doc_loop = true;
            const long long interval = blocks_file["loop"].value("intervalMs", 0LL);
            model.doc_loop_label = interval > 0 ? "every " + std::to_string(interval) + "ms"
                                                : "continuously";
            add_event(SequenceEventKind::LoopStart, {}, {}, model.doc_loop_label);
        }

        emit_list(roots);

        if (model.doc_loop)
            add_event(SequenceEventKind::LoopEnd);

        model.participants = std::move(participants_);
        model.events       = std::move(events_);
        return model;
    }

private:
    const MarkdownOptions&   opt_;
    std::vector<std::string> participants_;
    std::vector<SequenceEvent> events_;

    void add_event(SequenceEventKind kind,
                   std::string from = {},
                   std::string to   = {},
                   std::string text = {})
    {
        events_.push_back({kind, std::move(from), std::move(to), std::move(text)});
    }

    std::string participant_for(const std::string& kind)
    {
        const std::string& group = kind_group(kind);
        if (group.empty())
            return "Script";
        const std::string name = participant_name(group);
        if (std::find(participants_.begin(), participants_.end(), name) == participants_.end())
            participants_.push_back(name);
        return name;
    }

    std::string message_label(const nlohmann::json& block)
    {
        if (opt_.expand_parameters) {
            std::string s = scrub_for_mermaid(pseudo_line(block, opt_));
            const std::string& group = kind_group(json_str(block, "kind"));
            if (!group.empty() && s.rfind(group + "/", 0) == 0)
                s = s.substr(group.size() + 1);
            return s;
        }
        const std::string kind = json_str(block, "kind");
        if (kind == "stdout")           return "print";
        if (kind == "setVariable")      return "set " + json_str(block, "name");
        if (kind == "getVariable")      return "get " + json_str(block, "name");
        return kind;
    }

    void emit_list(const nlohmann::json& blocks)
    {
        if (!blocks.is_array())
            return;
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            const nlohmann::json& block = blocks[i];
            if (!block.is_object() || is_excluded(block, opt_))
                continue;
            const std::string kind = json_str(block, "kind");
            const bool disabled = is_block_disabled(block);
            const bool unknown = !is_known_block_kind(kind);
            if (disabled || unknown) {
                add_event(SequenceEventKind::Note, "Script", {}, scrub_for_mermaid(blocked_node_label(block, disabled, opt_)));
                continue;
            }

            if (kind == "if") {
                bool first = true;
                bool open  = false;
                for (; i < blocks.size(); ++i) {
                    const nlohmann::json& member = blocks[i];
                    const std::string mk = member.is_object() ? json_str(member, "kind") : std::string{};
                    if (!first && mk != "elseIf" && mk != "else")
                        break;
                    if (is_excluded(member, opt_)) {
                        first = false;
                        continue;
                    }
                    if (mk == "if" || mk == "elseIf") {
                        const std::string cond = scrub_for_mermaid(clip(json_str(member, "condition"), opt_.max_text));
                        add_event(first ? SequenceEventKind::AltStart : SequenceEventKind::AltElse,
                                  {}, {}, cond);
                        open = true;
                        emit_list(mk == "if" ? member.value("consequent", nlohmann::json::array())
                                             : member.value("items", nlohmann::json::array()));
                    } else {
                        add_event(SequenceEventKind::AltElse, {}, {}, "else");
                        emit_list(member.value("items", nlohmann::json::array()));
                        ++i;
                        break;
                    }
                    first = false;
                }
                --i;
                if (open)
                    add_event(SequenceEventKind::AltEnd);
            } else if (kind == "switch") {
                const std::string variable = scrub_for_mermaid(clip(json_str(block, "variable"), opt_.max_text));
                bool first = true;
                bool open  = false;
                for (const auto& item : block.value("items", nlohmann::json::array())) {
                    if (!item.is_object() || is_excluded(item, opt_))
                        continue;
                    std::string label;
                    if (json_str(item, "kind") == "switchDefault") {
                        label = "default";
                    } else {
                        const std::string comparator = json_str(item, "comparator");
                        const bool equality = comparator.empty() || comparator == "===" || comparator == "==";
                        label = variable + " " + (equality ? "==" : comparator) + " "
                            + scrub_for_mermaid(clip(json_str(item, "expression"), opt_.max_text));
                    }
                    add_event(first ? SequenceEventKind::AltStart : SequenceEventKind::AltElse,
                              {}, {}, label);
                    open = true;
                    first = false;
                    emit_list(item.value("consequent", nlohmann::json::array()));
                }
                if (open)
                    add_event(SequenceEventKind::AltEnd);
            } else if (kind == "while" || kind == "for") {
                std::string text;
                if (kind == "while") {
                    text = "while " + scrub_for_mermaid(clip(json_str(block, "condition"), opt_.max_text));
                } else {
                    const std::string variable = json_str(block, "variable");
                    text = "for " + variable + " = " + json_str(block, "initial")
                        + ", " + variable + " " + json_str(block, "comparator") + " " + json_str(block, "final")
                        + ", " + json_str(block, "modifier");
                }
                add_event(SequenceEventKind::LoopStart, {}, {}, text);
                emit_list(block.value("items", nlohmann::json::array()));
                add_event(SequenceEventKind::LoopEnd);
            } else if (kind == "break") {
                add_event(SequenceEventKind::Note, "Script", {}, "break");
            } else if (kind == "group" || kind == "runScript") {
                emit_list(block.value("items", nlohmann::json::array()));
            } else {
                const std::string target = participant_for(kind);
                add_event(SequenceEventKind::Message, "Script", target,
                          scrub_for_mermaid(message_label(block)));
                std::string store = json_str(block, "storeAs");
                if (store.empty()) store = json_str(block, "storeResult");
                if (store.empty() && kind != "getVariable") store = json_str(block, "target");
                if (!store.empty() && target != "Script")
                    add_event(SequenceEventKind::Return, target, "Script", scrub_for_mermaid(store));
                else if (!store.empty() && kind != "setVariable" && kind != "getVariable")
                    add_event(SequenceEventKind::Note, "Script", {}, "-> " + scrub_for_mermaid(store));
                if (block.contains("items") && block["items"].is_array())
                    emit_list(block["items"]);
            }
        }
    }
};

struct ParticipantLayout {
    std::string name;
    double      x      = 0.0;
    double      width  = 0.0;
    double      center = 0.0;
};

struct FragmentSpan {
    SequenceEventKind kind = SequenceEventKind::AltStart;
    int               start_row = 0;
    int               end_row   = 0;
    int               depth     = 0;
    std::string       label;
};

std::string mermaid_event_line(const SequenceEvent& event, int depth)
{
    const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
    switch (event.kind) {
        case SequenceEventKind::Message:
            return indent + event.from + "->>" + event.to + ": " + event.text + "\n";
        case SequenceEventKind::Return:
            return indent + event.from + "-->>" + event.to + ": " + event.text + "\n";
        case SequenceEventKind::Note:
            return indent + "Note over " + (event.from.empty() ? "Script" : event.from) + ": "
                + event.text + "\n";
        case SequenceEventKind::AltStart:
            return indent + "alt " + event.text + "\n";
        case SequenceEventKind::AltElse:
            if (event.text == "else")
                return indent + "else\n";
            return indent + "else " + event.text + "\n";
        case SequenceEventKind::AltEnd:
            return indent + "end\n";
        case SequenceEventKind::LoopStart:
            return indent + "loop " + event.text + "\n";
        case SequenceEventKind::LoopEnd:
            return indent + "end\n";
    }
    return {};
}

std::vector<ParticipantLayout> layout_participants(const SequenceModel& model,
                                                     const DiagramStyleConstants& styles)
{
    std::vector<std::string> names;
    names.push_back("Script");
    for (const auto& p : model.participants) {
        if (p != "Script")
            names.push_back(p);
    }

    const double spacing = styles.layout.node_spacing_x;
    const double pad     = styles.canvas.padding;
    std::vector<ParticipantLayout> out;
    out.reserve(names.size());

    double x = pad;
    for (const auto& name : names) {
        const double w = std::max(kParticipantMinWidth, estimate_text_width(name) + 24.0);
        out.push_back({name, x, w, x + w * 0.5});
        x += w + spacing;
    }
    return out;
}

std::vector<FragmentSpan> collect_fragment_spans(const SequenceModel& model)
{
    struct OpenFragment {
        SequenceEventKind kind;
        int               start_row;
        int               depth;
        std::string       label;
    };

    std::vector<FragmentSpan> spans;
    std::vector<OpenFragment> stack;
    int row = 0;

    for (const auto& event : model.events) {
        switch (event.kind) {
            case SequenceEventKind::AltStart:
            case SequenceEventKind::LoopStart:
                stack.push_back({event.kind, row, static_cast<int>(stack.size()), event.text});
                break;
            case SequenceEventKind::AltEnd:
            case SequenceEventKind::LoopEnd:
                if (!stack.empty()) {
                    const OpenFragment open = stack.back();
                    stack.pop_back();
                    spans.push_back({open.kind, open.start_row, row, open.depth, open.label});
                }
                break;
            default:
                ++row;
                break;
        }
    }
    return spans;
}

void append_sequence_arrow(std::ostringstream& svg,
                           double x1,
                           double x2,
                           double y,
                           bool dashed,
                           bool loop_left,
                           const DiagramStyleConstants& styles,
                           double loop_w,
                           double loop_drop)
{
    if (std::abs(x1 - x2) >= 0.5) {
        svg << "<line x1=\"" << x1 << "\" y1=\"" << y << "\" x2=\"" << x2 << "\" y2=\"" << y
            << "\" stroke=\"" << styles.edge.stroke << "\" stroke-width=\"" << styles.edge.stroke_width
            << "\"";
        if (dashed)
            svg << " stroke-dasharray=\"6 4\"";
        svg << " marker-end=\"url(#arrowHead)\"/>\n";
        return;
    }

    const double outward = loop_left ? -loop_w : loop_w;
    const double x_out   = x1 + outward;
    const double y_bot   = y + loop_drop;
    svg << "<polyline points=\"" << x1 << "," << y << " " << x_out << "," << y << " " << x_out << ","
        << y_bot << " " << x1 << "," << y_bot << "\" fill=\"none\" stroke=\"" << styles.edge.stroke
        << "\" stroke-width=\"" << styles.edge.stroke_width << "\"";
    if (dashed)
        svg << " stroke-dasharray=\"6 4\"";
    svg << " marker-end=\"url(#arrowHead)\"/>\n";
}

double sequence_message_label_x(double x1, double x2, bool self_message, bool loop_left, double loop_w)
{
    if (!self_message)
        return (x1 + x2) * 0.5;
    return loop_left ? x1 - loop_w * 0.55 : x1 + loop_w * 0.55;
}

} // namespace

SequenceModel build_sequence_model(const nlohmann::json& blocks_file, const MarkdownOptions& options)
{
    SequenceBuilder builder(options);
    return builder.build(blocks_file);
}

std::string sequence_model_to_mermaid(const SequenceModel& model)
{
    std::ostringstream out;
    out << "sequenceDiagram\n  participant Script\n";
    for (const auto& p : model.participants)
        out << "  participant " << p << "\n";

    int depth = 1;
    for (const auto& event : model.events) {
        switch (event.kind) {
            case SequenceEventKind::AltStart:
            case SequenceEventKind::LoopStart:
                out << mermaid_event_line(event, depth);
                ++depth;
                break;
            case SequenceEventKind::AltEnd:
            case SequenceEventKind::LoopEnd:
                --depth;
                if (depth < 1)
                    depth = 1;
                out << mermaid_event_line(event, depth);
                break;
            default:
                out << mermaid_event_line(event, depth);
                break;
        }
    }
    return out.str();
}

std::string render_sequence_diagram_svg(const nlohmann::json& blocks_file,
                                        const MarkdownOptions& options,
                                        const DiagramStyleConstants& styles,
                                        const std::string& title,
                                        bool standalone)
{
    const SequenceModel model = build_sequence_model(blocks_file, options);
    const std::vector<ParticipantLayout> participants = layout_participants(model, styles);

    int message_rows = 0;
    for (const auto& event : model.events) {
        switch (event.kind) {
            case SequenceEventKind::Message:
            case SequenceEventKind::Return:
            case SequenceEventKind::Note:
                ++message_rows;
                break;
            default:
                break;
        }
    }

    const double title_h = title.empty() ? 0.0 : styles.title.font_size + 24.0;
    const double pad     = styles.canvas.padding;
    const double top_y   = pad + title_h;
    const double header_bottom = top_y + kHeaderHeight;
    const double header_gap    = sequence_header_gap(styles);
    const double row_h   = sequence_row_height(styles);
    const double loop_w  = sequence_self_loop_width(styles);
    const double loop_drop = sequence_self_loop_drop(styles, row_h);
    const double content_h = header_gap + std::max(1, message_rows) * row_h + pad;
    const double svg_w   = participants.empty()
        ? pad * 2.0 + kParticipantMinWidth
        : participants.back().x + participants.back().width + pad;
    const double svg_h   = header_bottom + content_h + pad;

    std::unordered_map<std::string, double> lifeline_x;
    for (const auto& p : participants)
        lifeline_x.emplace(p.name, p.center);

    std::ostringstream svg;
    if (standalone)
        svg << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << svg_w << "\" height=\"" << svg_h
        << "\" viewBox=\"0 0 " << svg_w << " " << svg_h << "\">\n"
        << "<defs>\n"
        << "  <filter id=\"cardShadow\" x=\"-25%\" y=\"-25%\" width=\"150%\" height=\"150%\">\n"
        << "    <feGaussianBlur in=\"SourceAlpha\" stdDeviation=\"" << (styles.node.shadow_blur * 0.5)
        << "\" result=\"blur\"/>\n"
        << "    <feOffset in=\"blur\" dx=\"0\" dy=\"" << styles.node.shadow_offset_y
        << "\" result=\"offsetBlur\"/>\n"
        << "    <feFlood flood-color=\"#000000\" flood-opacity=\"" << styles.node.shadow_opacity
        << "\" result=\"offsetColor\"/>\n"
        << "    <feComposite in=\"offsetColor\" in2=\"offsetBlur\" operator=\"in\" result=\"shadow\"/>\n"
        << "    <feMerge>\n"
        << "      <feMergeNode in=\"shadow\"/>\n"
        << "      <feMergeNode in=\"SourceGraphic\"/>\n"
        << "    </feMerge>\n"
        << "  </filter>\n"
        << "  <marker id=\"arrowHead\" markerWidth=\"" << styles.edge.arrow_size
        << "\" markerHeight=\"" << styles.edge.arrow_size << "\" refX=\"" << (styles.edge.arrow_size - 1)
        << "\" refY=\"" << (styles.edge.arrow_size * 0.5)
        << "\" orient=\"auto\" markerUnits=\"userSpaceOnUse\">\n"
        << "    <path d=\"M0,0 L0," << styles.edge.arrow_size << " L" << styles.edge.arrow_size << ","
        << (styles.edge.arrow_size * 0.5) << " z\" fill=\"" << styles.edge.stroke << "\"/>\n"
        << "  </marker>\n"
        << "</defs>\n"
        << "<rect width=\"100%\" height=\"100%\" fill=\"" << styles.canvas.background << "\"/>\n";

    if (!title.empty()) {
        svg << "<text x=\"" << (svg_w * 0.5) << "\" y=\"" << (pad * 0.5 + styles.title.font_size)
            << "\" text-anchor=\"middle\" font-family=\"" << xml_escape(styles.title.font_family)
            << "\" font-size=\"" << styles.title.font_size << "\" fill=\"" << styles.title.fill << "\">"
            << xml_escape(title) << "</text>\n";
    }

    for (const auto& p : participants) {
        svg << "<line x1=\"" << p.center << "\" y1=\"" << header_bottom << "\" x2=\"" << p.center
            << "\" y2=\"" << (svg_h - pad) << "\" stroke=\"" << styles.edge.stroke
            << "\" stroke-width=\"" << styles.edge.stroke_width << "\"/>\n";
    }

    const std::vector<FragmentSpan> fragments = collect_fragment_spans(model);
    for (const auto& frag : fragments) {
        const double x = pad + static_cast<double>(frag.depth) * kFragmentIndent;
        const double y = header_bottom + header_gap + static_cast<double>(frag.start_row) * row_h + 4.0;
        const double h = std::max(row_h,
                                  static_cast<double>(frag.end_row - frag.start_row + 1) * row_h - 8.0);
        const double w = 18.0;
        const double r = 4.0;
        const bool is_loop = frag.kind == SequenceEventKind::LoopStart;
        const std::string fill = is_loop ? styles.decision.fill : styles.node.fill;
        const std::string stroke = is_loop ? styles.decision.stroke : styles.node.stroke;
        svg << "<rect x=\"" << x << "\" y=\"" << y << "\" width=\"" << w << "\" height=\"" << h
            << "\" rx=\"" << r << "\" ry=\"" << r << "\" fill=\"" << fill << "\" stroke=\"" << stroke
            << "\" stroke-width=\"" << styles.decision.stroke_width << "\"/>\n";
        const std::string tag = is_loop ? "loop" : "alt";
        svg << "<text x=\"" << (x + w + 6.0) << "\" y=\"" << (y + styles.decision.label_font_size + 2.0)
            << "\" font-family=\"" << xml_escape(styles.node.label_font_family)
            << "\" font-size=\"" << styles.decision.label_font_size << "\" fill=\""
            << styles.decision.label_fill << "\">" << xml_escape(tag + " " + frag.label) << "</text>\n";
    }

    for (const auto& p : participants) {
        svg << "<rect x=\"" << p.x << "\" y=\"" << top_y << "\" width=\"" << p.width
            << "\" height=\"" << kHeaderHeight << "\" rx=\"" << styles.node.corner_radius
            << "\" ry=\"" << styles.node.corner_radius << "\" fill=\"" << styles.node.fill
            << "\" stroke=\"" << styles.node.stroke << "\" stroke-width=\"" << styles.node.stroke_width
            << "\" filter=\"url(#cardShadow)\"/>\n"
            << "<text x=\"" << p.center << "\" y=\"" << (top_y + kHeaderHeight * 0.62)
            << "\" text-anchor=\"middle\" font-family=\"" << xml_escape(styles.node.label_font_family)
            << "\" font-size=\"" << styles.node.label_font_size << "\" fill=\"" << styles.node.label_fill
            << "\">" << xml_escape(p.name) << "</text>\n";
    }

    const double min_center = participants.empty() ? 0.0 : participants.front().center;
    const double max_center = participants.empty() ? 0.0 : participants.back().center;

    int row = 0;
    for (const auto& event : model.events) {
        const double y = header_bottom + header_gap + static_cast<double>(row) * row_h + row_h * 0.5;
        switch (event.kind) {
            case SequenceEventKind::Message:
            case SequenceEventKind::Return:
            case SequenceEventKind::Note: {
                if (event.kind == SequenceEventKind::Note) {
                    const auto it = lifeline_x.find(event.from.empty() ? "Script" : event.from);
                    const double cx = it == lifeline_x.end() ? lifeline_x["Script"] : it->second;
                    const double note_w = std::max(kParticipantMinWidth,
                                                   estimate_interpolated_text_width(
                                                       event.text, styles, kCharWidth)
                                                       + kNotePadding * 2.0);
                    const double note_x = cx - note_w * 0.5;
                    const double note_h = row_h - 6.0;
                    svg << "<rect x=\"" << note_x << "\" y=\"" << (y - note_h * 0.5)
                        << "\" width=\"" << note_w << "\" height=\"" << note_h << "\" rx=\""
                        << styles.node.param_card_radius << "\" ry=\"" << styles.node.param_card_radius
                        << "\" fill=\"" << styles.node.param_card_fill << "\" stroke=\""
                        << styles.node.param_card_stroke << "\" stroke-width=\""
                        << styles.node.stroke_width << "\"/>\n";
                    append_sequence_event_label(svg,
                                                cx,
                                                y + styles.node.param_font_size * 0.35,
                                                event.text,
                                                false,
                                                styles,
                                                styles.node.label_font_family,
                                                styles.node.param_font_size);
                } else {
                    const auto from_it = lifeline_x.find(event.from);
                    const auto to_it   = lifeline_x.find(event.to);
                    const double x1 = from_it == lifeline_x.end() ? lifeline_x["Script"] : from_it->second;
                    const double x2 = to_it == lifeline_x.end() ? lifeline_x["Script"] : to_it->second;
                    const bool dashed = event.kind == SequenceEventKind::Return;
                    const bool self_message = std::abs(x1 - x2) < 0.5;
                    const bool loop_left =
                        self_message ? ((x1 - min_center) <= (max_center - x1)) : false;
                    append_sequence_arrow(svg, x1, x2, y, dashed, loop_left, styles, loop_w, loop_drop);
                    if (!event.text.empty()) {
                        const double label_x = sequence_message_label_x(x1, x2, self_message, loop_left, loop_w);
                        append_sequence_event_label(svg,
                                                    label_x,
                                                    y - 6.0,
                                                    event.text,
                                                    event.kind == SequenceEventKind::Return,
                                                    styles,
                                                    styles.edge.label_font_family,
                                                    styles.edge.label_font_size);
                    }
                }
                ++row;
                break;
            }
            default:
                break;
        }
    }

    svg << "</svg>\n";
    return svg.str();
}

} // namespace media::xblox
