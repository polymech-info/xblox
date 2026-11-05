#include "xblox_diagram.hpp"
#include "fs/path_sanitizer.hpp"

#include "xblox_sequence_diagram.hpp"

#include "blocks/block_registry.hpp"
#include "blocks/block_params.hpp"
#include "xblox_diagram_pathfinding.hpp"
#include "xblox_diagram_text.hpp"

#include <vips/vips.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <fstream>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "string/string_utils.hpp"

namespace media::xblox {

namespace {

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

std::string variable_ref_display(const std::string& raw, bool show_delimiters)
{
    if (raw.empty())
        return {};
    std::string inner = raw;
    if (raw.size() >= 3 && raw[0] == '$' && raw[1] == '{' && raw.back() == '}')
        inner = raw.substr(2, raw.size() - 3);
    if (show_delimiters)
        return "${" + inner + "}";
    return inner;
}

std::string variable_ref_parens(const std::string& raw, bool show_delimiters)
{
    const std::string shown = variable_ref_display(raw, show_delimiters);
    return shown.empty() ? std::string{} : " (" + shown + ")";
}

std::string variable_store_glyph(const std::string& style)
{
    if (style == "arrow")  return "->";
    if (style == "colon")  return ":";
    if (style == "dot")    return "\u00b7";
    if (style == "equals") return "=";
    if (style == "none")   return {};
    return "\u2192"; // unicode (default)
}

std::string variable_from_glyph(const std::string& style)
{
    if (style == "arrow")  return "<=";
    if (style == "colon")  return ":";
    if (style == "dot")    return "\u00b7";
    if (style == "equals") return "=";
    if (style == "none")   return {};
    return "\u2190"; // unicode
}

std::string variable_link_padding_chars(double margin_px, double char_w)
{
    if (margin_px <= 0.0 || char_w <= 0.0)
        return {};
    const int count = std::max(1, static_cast<int>(std::ceil(margin_px / char_w)));
    return std::string(static_cast<std::size_t>(count), ' ');
}

std::string variable_store_prefix(const std::string& style, double margin_px = 0.0, double char_w = 7.0)
{
    const std::string pad = variable_link_padding_chars(margin_px, char_w);
    const std::string glyph = variable_store_glyph(style);
    if (glyph.empty())
        return pad.empty() ? " " : pad;
    return pad + glyph + pad;
}

std::string variable_from_prefix(const std::string& style, double margin_px = 0.0, double char_w = 7.0)
{
    const std::string pad = variable_link_padding_chars(margin_px, char_w);
    const std::string glyph = variable_from_glyph(style);
    if (glyph.empty())
        return pad.empty() ? " " : pad;
    return pad + glyph + pad;
}

void append_variable_link(std::vector<LabelSegment>& segments,
                          const std::string& glyph,
                          double link_margin,
                          const std::string& variable,
                          bool show_delimiters)
{
    if (glyph.empty() && variable.empty())
        return;
    if (!glyph.empty())
        append_segment(segments, glyph, LabelSegmentKind::Operator, link_margin);
    const std::string shown = variable_ref_display(variable, show_delimiters);
    if (!shown.empty())
        append_segment(segments, shown, LabelSegmentKind::Variable, link_margin);
}

void append_variable_ref_input(std::vector<LabelSegment>& segments,
                               const std::string& raw,
                               bool show_delimiters,
                               const std::string& input_style)
{
    const std::string shown = variable_ref_display(raw, show_delimiters);
    if (shown.empty())
        return;
    if (input_style == "none") {
        append_segment(segments, " ", LabelSegmentKind::Operator);
        append_segment(segments, shown, LabelSegmentKind::Variable);
    } else if (input_style == "at") {
        append_segment(segments, " @", LabelSegmentKind::Operator);
        append_segment(segments, shown, LabelSegmentKind::Variable);
    } else if (input_style == "bracket") {
        append_segment(segments, " [", LabelSegmentKind::Operator);
        append_segment(segments, shown, LabelSegmentKind::Variable);
        append_segment(segments, "]", LabelSegmentKind::Operator);
    } else {
        append_segment(segments, " (", LabelSegmentKind::Operator);
        append_segment(segments, shown, LabelSegmentKind::Variable);
        append_segment(segments, ")", LabelSegmentKind::Operator);
    }
}

std::string humanize_token(std::string token)
{
    if (token.empty())
        return {};
    std::string out;
    out.reserve(token.size() + 8);
    auto flush_word = [&](std::string& word) {
        if (word.empty())
            return;
        if (!out.empty())
            out += ' ';
        word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
        for (std::size_t i = 1; i < word.size(); ++i)
            word[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(word[i])));
        out += word;
        word.clear();
    };
    std::string word;
    for (char c : token) {
        if (c == '_' || c == '-' || c == '/') {
            flush_word(word);
            continue;
        }
        if (!word.empty() && std::isupper(static_cast<unsigned char>(c)))
            flush_word(word);
        word += c;
    }
    flush_word(word);
    return out;
}

std::string format_registry_block_label(const std::string& kind,
                                        const std::string& group,
                                        bool show_groups,
                                        bool humanize)
{
    const std::string kind_label = humanize ? humanize_token(kind) : kind;
    if (!show_groups || group.empty())
        return kind_label.empty() ? kind : kind_label;
    const std::string group_label = humanize ? humanize_token(group) : group;
    if (group_label.empty())
        return kind_label.empty() ? kind : kind_label;
    if (kind_label.empty())
        return group_label;
    return group_label + (humanize ? " / " : "/") + kind_label;
}

void append_registry_block_label(std::vector<LabelSegment>& segments,
                                 const std::string& kind,
                                 const std::string& group,
                                 bool show_groups,
                                 bool humanize)
{
    const std::string kind_label = humanize ? humanize_token(kind) : kind;
    if (!show_groups || group.empty()) {
        append_segment(segments, kind_label.empty() ? kind : kind_label, LabelSegmentKind::Label);
        return;
    }
    const std::string group_label = humanize ? humanize_token(group) : group;
    if (group_label.empty()) {
        append_segment(segments, kind_label.empty() ? kind : kind_label, LabelSegmentKind::Label);
        return;
    }
    append_segment(segments, group_label, LabelSegmentKind::Group);
    append_segment(segments, humanize ? " / " : "/", LabelSegmentKind::Operator);
    append_segment(segments, kind_label.empty() ? kind : kind_label, LabelSegmentKind::Label);
}

std::string dump_value(const nlohmann::json& v, std::size_t max_len)
{
    return clip(v.dump(), max_len);
}

std::string code(const std::string& s)
{
    return "`" + s + "`";
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
    std::string shown = format_registry_block_label(kind, group,
                                                    opt.diagram_show_groups,
                                                    opt.diagram_humanize_node_names);
    if (shown.empty())
        shown = kind.empty() ? "unknown" : kind;
    if (!detail.empty())
        shown += " " + detail;
    if (block.contains("append") && block["append"].is_boolean() && block["append"].get<bool>())
        shown += " (append)";
    return shown;
}

std::string store_suffix(const nlohmann::json& block,
                         bool show_variables,
                         bool show_delimiters,
                         const std::string& link_style,
                         double link_margin = 0.0)
{
    if (!show_variables)
        return {};
    const std::string kind = json_str(block, "kind");
    if (kind == "getVariable" || kind == "setVariable")
        return {};
    std::string store = json_str(block, "storeAs");
    if (store.empty()) store = json_str(block, "storeResult");
    if (store.empty()) store = json_str(block, "target");
    if (store.empty())
        return {};
    return variable_store_prefix(link_style, link_margin) + variable_ref_display(store, show_delimiters);
}

void append_store_suffix(std::vector<LabelSegment>& segments,
                         const nlohmann::json& block,
                         bool show_variables,
                         bool show_delimiters,
                         const std::string& link_style,
                         double link_margin)
{
    if (!show_variables)
        return;
    const std::string kind = json_str(block, "kind");
    if (kind == "getVariable" || kind == "setVariable")
        return;
    std::string store = json_str(block, "storeAs");
    if (store.empty()) store = json_str(block, "storeResult");
    if (store.empty()) store = json_str(block, "target");
    if (store.empty())
        return;
    append_variable_link(segments, variable_store_glyph(link_style), link_margin, store, show_delimiters);
}

std::string used_variables_hint(const nlohmann::json& block,
                                bool show_variables,
                                bool show_delimiters,
                                const std::string& link_style,
                                const std::string& input_style,
                                double link_margin = 0.0)
{
    if (!show_variables)
        return {};
    const std::string kind = json_str(block, "kind");
    if (kind == "setVariable") {
        const std::string from = json_str(block, "valueFrom");
        if (!from.empty())
            return variable_from_prefix(link_style, link_margin) + variable_ref_display(from, show_delimiters);
    }
    if (kind == "getVariable") {
        const std::string target = json_str(block, "target");
        if (!target.empty())
            return variable_store_prefix(link_style, link_margin) + variable_ref_display(target, show_delimiters);
    }
    const std::string input = json_str(block, "input");
    if (!input.empty()) {
        std::vector<LabelSegment> segments;
        append_variable_ref_input(segments, input, show_delimiters, input_style);
        return segments_to_label(segments);
    }
    const std::string variable = json_str(block, "variable");
    if (!variable.empty() && kind != "setVariable" && kind != "getVariable" && kind != "for") {
        std::vector<LabelSegment> segments;
        append_variable_ref_input(segments, variable, show_delimiters, input_style);
        return segments_to_label(segments);
    }
    return {};
}

void append_used_variables_hint(std::vector<LabelSegment>& segments,
                                const nlohmann::json& block,
                                bool show_variables,
                                bool show_delimiters,
                                const std::string& link_style,
                                const std::string& input_style,
                                double link_margin)
{
    if (!show_variables)
        return;
    const std::string kind = json_str(block, "kind");
    if (kind == "setVariable") {
        const std::string from = json_str(block, "valueFrom");
        if (!from.empty())
            append_variable_link(segments, variable_from_glyph(link_style), link_margin, from, show_delimiters);
        return;
    }
    if (kind == "getVariable") {
        const std::string target = json_str(block, "target");
        if (!target.empty())
            append_variable_link(segments, variable_store_glyph(link_style), link_margin, target, show_delimiters);
        return;
    }
    const std::string input = json_str(block, "input");
    if (!input.empty()) {
        append_variable_ref_input(segments, input, show_delimiters, input_style);
        return;
    }
    const std::string variable = json_str(block, "variable");
    if (!variable.empty() && kind != "setVariable" && kind != "getVariable" && kind != "for")
        append_variable_ref_input(segments, variable, show_delimiters, input_style);
}

bool diagram_show_params(const MarkdownOptions& opt)
{
    return !opt.diagram_show_parameters.empty() && opt.diagram_show_parameters != "none";
}

bool is_block_meta_field(const std::string& key)
{
    static const std::unordered_set<std::string> kMeta = {
        "kind", "blockId", "enabled", "consequent", "alternate", "body", "cases", "children",
        "storeAs", "storeResult", "target", "event", "roots", "loop",
    };
    return kMeta.count(key) > 0;
}

bool json_param_is_empty(const nlohmann::json& value)
{
    if (value.is_null())
        return true;
    if (value.is_string())
        return value.get<std::string>().empty();
    if (value.is_array())
        return value.empty();
    if (value.is_object())
        return value.empty();
    return false;
}

bool json_param_equals_default(const nlohmann::json& value, const blocks::ParamDef& def)
{
    if (def.default_value.is_null())
        return false;
    return value == def.default_value;
}

bool is_param_field_set(const nlohmann::json& block, const blocks::ParamDef& def)
{
    if (!block.contains(def.name))
        return false;
    const nlohmann::json& value = block[def.name];
    if (json_param_is_empty(value))
        return false;
    if (json_param_equals_default(value, def))
        return false;
    return true;
}

std::string format_param_value(const nlohmann::json& value, std::size_t max_len)
{
    if (value.is_string())
        return clip(value.get<std::string>(), max_len);
    if (value.is_boolean())
        return value.get<bool>() ? "true" : "false";
    if (value.is_number_integer())
        return std::to_string(value.get<long long>());
    if (value.is_number_unsigned())
        return std::to_string(value.get<unsigned long long>());
    if (value.is_number_float()) {
        std::ostringstream out;
        out << value.get<double>();
        return clip(out.str(), max_len);
    }
    return clip(value.dump(), max_len);
}

void append_set_variable_value(std::vector<LabelSegment>& segments,
                               const nlohmann::json& block,
                               bool show_values,
                               std::size_t max_len,
                               double link_margin)
{
    if (!show_values)
        return;
    const std::string expression = json_str(block, "expression");
    const std::string value_from = json_str(block, "valueFrom");
    if (!expression.empty()) {
        append_segment(segments, " = ", LabelSegmentKind::Operator);
        append_segment(segments, clip(expression, max_len), LabelSegmentKind::Variable, link_margin);
        return;
    }
    if (!value_from.empty()) {
        append_segment(segments, " = ", LabelSegmentKind::Operator);
        append_segment(segments, value_from, LabelSegmentKind::Variable, link_margin);
        return;
    }
    if (block.contains("value")) {
        append_segment(segments, " = ", LabelSegmentKind::Operator);
        append_segment(segments, format_param_value(block["value"], max_len), LabelSegmentKind::Variable,
                        link_margin);
    }
}

void append_param_entry(std::vector<LabelSegment>& segments,
                        const std::string& group,
                        const std::string& name,
                        const std::string& value_text,
                        bool grouped,
                        bool show_values,
                        double link_margin,
                        bool& first)
{
    if (!first)
        append_segment(segments, " · ", LabelSegmentKind::Operator);
    first = false;
    if (grouped && !group.empty()) {
        append_segment(segments, group + "/", LabelSegmentKind::Group);
    }
    append_segment(segments, name, LabelSegmentKind::Label);
    if (show_values && !value_text.empty()) {
        append_segment(segments, ": ", LabelSegmentKind::Operator);
        append_segment(segments, value_text, LabelSegmentKind::Variable, link_margin);
    }
}

struct ParamLine {
    std::string               group;
    std::string               name;
    std::string               value;
    bool                      grouped    = false;
    bool                      show_value = false;
    double                    value_dx   = 0.0;
    std::vector<LabelSegment> freeform;
    std::string               plain;
};

std::string param_line_label_text(const ParamLine& line)
{
    if (!line.freeform.empty())
        return segments_to_label(line.freeform);
    std::string label = line.name;
    if (line.grouped && !line.group.empty())
        label = line.group + "/" + label;
    return label;
}

std::string param_line_plain(const ParamLine& line)
{
    if (!line.freeform.empty())
        return segments_to_label(line.freeform);
    std::string out = param_line_label_text(line);
    if (line.show_value && !line.value.empty())
        out += ": " + line.value;
    return out;
}

ParamLine make_param_line(const std::string& group,
                          const std::string& name,
                          const std::string& value_text,
                          bool grouped,
                          bool show_values,
                          double link_margin)
{
    ParamLine line;
    line.group      = group;
    line.name       = name;
    line.value      = value_text;
    line.grouped    = grouped;
    line.show_value = show_values && !value_text.empty();
    line.value_dx   = line.show_value ? link_margin : 0.0;
    line.plain      = param_line_plain(line);
    return line;
}

ParamLine make_param_segments_line(std::vector<LabelSegment> segments)
{
    ParamLine line;
    line.freeform = std::move(segments);
    line.plain    = segments_to_label(line.freeform);
    return line;
}

bool is_param_output_group(const std::string& group)
{
    return group == "output";
}

bool is_param_output_field(const std::string& key)
{
    return key == "storeAs" || key == "storeResult" || key == "target";
}

struct NodeParamCards {
    std::vector<ParamLine> inputs;
    std::vector<ParamLine> outputs;
};

struct ParamCardColumns {
    double name_w  = 0.0;
    double value_w = 0.0;
    double table_w = 0.0;
    bool   has_values = false;
};

ParamCardColumns measure_param_card_columns(const std::vector<ParamLine>& lines,
                                            const DiagramStyleConstants& styles)
{
    const double char_w = styles.node.param_font_size * 0.55;
    ParamCardColumns cols;
    for (const ParamLine& line : lines) {
        if (!line.freeform.empty()) {
            cols.table_w =
                std::max(cols.table_w, estimate_segments_width(line.freeform, char_w));
            continue;
        }
        const std::string label = param_line_label_text(line);
        cols.name_w             = std::max(cols.name_w, char_w * static_cast<double>(label.size()));
        if (line.show_value) {
            cols.has_values = true;
            const double var_margin =
                std::max(3.0, styles.node.variable_link_margin * 0.5);
            std::vector<LabelSegment> value_segments;
            split_string_interpolation_segments(value_segments, line.value,
                                                styles.render.show_variable_delimiters, var_margin);
            const double vw =
                estimate_segments_width(value_segments, char_w) + line.value_dx;
            cols.value_w = std::max(cols.value_w, vw);
        }
    }
    if (cols.has_values)
        cols.table_w = std::max(cols.table_w, cols.name_w + char_w * 2.0 + cols.value_w);
    else
        cols.table_w = std::max(cols.table_w, cols.name_w);
    return cols;
}

double estimate_param_card_width(const std::vector<ParamLine>& lines, const DiagramStyleConstants& styles)
{
    if (lines.empty())
        return 0.0;
    const ParamCardColumns cols = measure_param_card_columns(lines, styles);
    return std::max(styles.node.min_width * 0.55, cols.table_w + styles.node.param_card_padding * 2.0);
}

bool is_core_language_kind(const std::string& kind);

void append_core_language_parameters(std::vector<LabelSegment>& segments,
                                     const nlohmann::json& block,
                                     const MarkdownOptions& opt,
                                     double link_margin)
{
    const std::string kind = json_str(block, "kind");
    const bool grouped     = opt.diagram_show_parameters_grouped;
    const bool show_values = opt.diagram_show_parameter_values;
    bool first             = segments.empty();

    auto append_field = [&](const char* key, const char* label) {
        if (!block.contains(key) || json_param_is_empty(block.at(key)))
            return;
        append_param_entry(segments, {}, label, format_param_value(block.at(key), opt.max_text), grouped,
                           show_values, link_margin, first);
    };

    if (kind == "stdout" || kind == "log")
        append_field("message", "message");
    else if (kind == "wait")
        append_field("ms", "ms");
    else if (kind == "shell" || kind == "Shell")
        append_field("command", "command");
    else if (kind == "setVariable")
        append_set_variable_value(segments, block, show_values, opt.max_text, link_margin);
    else if (kind == "getVariable") {
        const std::string target = json_str(block, "target");
        if (!target.empty() && show_values)
            append_param_entry(segments, {}, "target", target, grouped, true, link_margin, first);
    }
}

void collect_core_language_parameters(NodeParamCards& cards,
                                      const nlohmann::json& block,
                                      const MarkdownOptions& opt,
                                      double link_margin)
{
    const std::string kind = json_str(block, "kind");
    const bool grouped     = opt.diagram_show_parameters_grouped;
    const bool show_values = opt.diagram_show_parameter_values;

    auto push_input = [&](const char* key, const char* label) {
        if (!block.contains(key) || json_param_is_empty(block.at(key)))
            return;
        cards.inputs.push_back(make_param_line({}, label, format_param_value(block.at(key), opt.max_text),
                                               grouped, show_values, link_margin));
    };

    if (kind == "stdout" || kind == "log")
        push_input("message", "message");
    else if (kind == "wait")
        push_input("ms", "ms");
    else if (kind == "shell" || kind == "Shell")
        push_input("command", "command");
    else if (kind == "setVariable") {
        if (show_values) {
            std::string value_text;
            const std::string expression = json_str(block, "expression");
            const std::string value_from = json_str(block, "valueFrom");
            if (!expression.empty())
                value_text = clip(expression, opt.max_text);
            else if (!value_from.empty())
                value_text = value_from;
            else if (block.contains("value"))
                value_text = format_param_value(block["value"], opt.max_text);
            if (!value_text.empty())
                cards.inputs.push_back(
                    make_param_line({}, "value", value_text, grouped, true, link_margin));
        }
    } else if (kind == "getVariable") {
        const std::string target = json_str(block, "target");
        if (!target.empty() && show_values)
            cards.inputs.push_back(make_param_line({}, "target", target, grouped, true, link_margin));
    }
}

void append_block_parameters(std::vector<LabelSegment>& segments,
                             const nlohmann::json& block,
                             const MarkdownOptions& opt,
                             double link_margin)
{
    if (!diagram_show_params(opt))
        return;

    const std::string kind = json_str(block, "kind");
    if (is_core_language_kind(kind)) {
        append_core_language_parameters(segments, block, opt, link_margin);
        return;
    }

    const bool show_all    = opt.diagram_show_parameters == "all";
    const bool grouped     = opt.diagram_show_parameters_grouped;
    const bool show_values = opt.diagram_show_parameter_values;
    const std::vector<blocks::ParamDef>* defs = blocks::builtin_block_param_defs(kind);
    bool first                   = segments.empty();

    if (defs && !defs->empty()) {
        for (const blocks::ParamDef& def : *defs) {
            if (def.hidden)
                continue;
            if (!show_all && !is_param_field_set(block, def))
                continue;
            const nlohmann::json& value =
                block.contains(def.name) ? block[def.name] : def.default_value;
            const std::string name = def.label.empty() ? def.name : def.label;
            const std::string value_text =
                show_values ? format_param_value(value, opt.max_text) : std::string{};
            append_param_entry(segments, def.group, name, value_text, grouped, show_values, link_margin,
                               first);
        }
        return;
    }

    if (!show_all) {
        for (auto it = block.begin(); it != block.end(); ++it) {
            if (!it.value().is_primitive() && !it.value().is_string())
                continue;
            if (is_block_meta_field(it.key()))
                continue;
            if (json_param_is_empty(it.value()))
                continue;
            append_param_entry(segments, {}, it.key(), format_param_value(it.value(), opt.max_text),
                                 grouped, show_values, link_margin, first);
        }
        return;
    }

    for (auto it = block.begin(); it != block.end(); ++it) {
        if (!it.value().is_primitive() && !it.value().is_string())
            continue;
        if (is_block_meta_field(it.key()))
            continue;
        append_param_entry(segments, {}, it.key(), format_param_value(it.value(), opt.max_text), grouped,
                             show_values, link_margin, first);
    }
}

void collect_block_parameters(NodeParamCards& cards,
                              const nlohmann::json& block,
                              const MarkdownOptions& opt,
                              double link_margin)
{
    if (!diagram_show_params(opt))
        return;

    const std::string kind = json_str(block, "kind");
    if (is_core_language_kind(kind)) {
        collect_core_language_parameters(cards, block, opt, link_margin);
        return;
    }

    const bool show_all    = opt.diagram_show_parameters == "all";
    const bool grouped     = opt.diagram_show_parameters_grouped;
    const bool show_values = opt.diagram_show_parameter_values;
    const std::vector<blocks::ParamDef>* defs = blocks::builtin_block_param_defs(kind);

    auto push_def = [&](const blocks::ParamDef& def) {
        if (def.hidden)
            return;
        if (!show_all && !is_param_field_set(block, def))
            return;
        const nlohmann::json& value =
            block.contains(def.name) ? block[def.name] : def.default_value;
        const std::string name = def.label.empty() ? def.name : def.label;
        const std::string value_text =
            show_values ? format_param_value(value, opt.max_text) : std::string{};
        ParamLine line =
            make_param_line(def.group, name, value_text, grouped, show_values, link_margin);
        if (is_param_output_group(def.group))
            cards.outputs.push_back(std::move(line));
        else
            cards.inputs.push_back(std::move(line));
    };

    if (defs && !defs->empty()) {
        for (const blocks::ParamDef& def : *defs)
            push_def(def);
        return;
    }

    auto push_field = [&](const std::string& key, const nlohmann::json& value) {
        ParamLine line = make_param_line({}, key, format_param_value(value, opt.max_text), grouped,
                                         show_values, link_margin);
        if (is_param_output_field(key))
            cards.outputs.push_back(std::move(line));
        else
            cards.inputs.push_back(std::move(line));
    };

    if (!show_all) {
        for (auto it = block.begin(); it != block.end(); ++it) {
            if (!it.value().is_primitive() && !it.value().is_string())
                continue;
            if (is_block_meta_field(it.key()) && !is_param_output_field(it.key()))
                continue;
            if (json_param_is_empty(it.value()))
                continue;
            push_field(it.key(), it.value());
        }
        return;
    }

    for (auto it = block.begin(); it != block.end(); ++it) {
        if (!it.value().is_primitive() && !it.value().is_string())
            continue;
        if (is_block_meta_field(it.key()) && !is_param_output_field(it.key()))
            continue;
        push_field(it.key(), it.value());
    }
}

void collect_store_output_line(NodeParamCards& cards,
                               const nlohmann::json& block,
                               bool show_variables,
                               bool show_delimiters,
                               const std::string& link_style,
                               double link_margin)
{
    if (!show_variables)
        return;
    const std::string kind = json_str(block, "kind");
    if (kind == "getVariable" || kind == "setVariable")
        return;
    std::string store = json_str(block, "storeAs");
    if (store.empty())
        store = json_str(block, "storeResult");
    if (store.empty())
        store = json_str(block, "target");
    if (store.empty())
        return;

    for (const ParamLine& existing : cards.outputs) {
        if (existing.plain.find(store) != std::string::npos)
            return;
    }

    std::vector<LabelSegment> segments;
    append_variable_link(segments, variable_store_glyph(link_style), link_margin, store, show_delimiters);
    cards.outputs.push_back(make_param_segments_line(std::move(segments)));
}

bool node_has_param_cards(const NodeParamCards& cards)
{
    return !cards.inputs.empty() || !cards.outputs.empty();
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
    if (opt.expand_parameters || diagram_show_params(opt) || is_core_language_kind(kind))
        return prefix + pseudo_line(block, opt);
    const std::string& group = kind_group(kind);
    return prefix + format_registry_block_label(kind, group,
                                                opt.diagram_show_groups,
                                                opt.diagram_humanize_node_names);
}

static std::string scrub_label(std::string s)
{
    s.erase(std::remove(s.begin(), s.end(), '`'), s.end());
    std::replace(s.begin(), s.end(), '\n', ' ');
    return s;
}

static std::string xml_escape(std::string s)
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

struct FlowNode {
    std::string                  id;
    std::string                  label;
    std::vector<LabelSegment>    segments;
    NodeParamCards               param_cards;
    bool                         decision = false;
};

struct FlowEdge {
    std::string from;
    std::string to;
    std::string label;
};

struct FlowGraph {
    std::vector<FlowNode> nodes;
    std::vector<FlowEdge> edges;
};

struct GraphExit {
    std::string id;
    std::string label;
};

class FlowGraphBuilder {
public:
    explicit FlowGraphBuilder(const MarkdownOptions& opt) : opt_(opt) {}

    FlowGraph build(const nlohmann::json& roots)
    {
        emit_list(roots, {}, nullptr);
        return graph_;
    }

private:
    const MarkdownOptions& opt_;
    FlowGraph              graph_;
    int                    next_id_ = 0;

    std::string add_node(const std::vector<LabelSegment>& segments,
                         bool decision,
                         NodeParamCards param_cards = {})
    {
        const std::string id = "B" + std::to_string(next_id_++);
        std::vector<LabelSegment> cleaned;
        cleaned.reserve(segments.size());
        for (auto segment : segments) {
            segment.text = scrub_label(segment.text);
            if (segment.text.empty())
                continue;
            if (!cleaned.empty() && cleaned.back().kind == segment.kind && cleaned.back().dx == 0.0
                && segment.dx == 0.0)
                cleaned.back().text += segment.text;
            else
                cleaned.push_back(std::move(segment));
        }
        if (cleaned.empty())
            append_segment(cleaned, decision ? "?" : "block", LabelSegmentKind::Label);
        graph_.nodes.push_back(
            {id, segments_to_label(cleaned), std::move(cleaned), std::move(param_cards), decision});
        return id;
    }

    std::string add_node(const std::string& text, bool decision)
    {
        return add_node(std::vector<LabelSegment>{{scrub_label(text), LabelSegmentKind::Label}}, decision);
    }

    void connect(const std::vector<GraphExit>& from, const std::string& to)
    {
        for (const auto& e : from)
            graph_.edges.push_back({e.id, to, e.label});
    }

    std::vector<LabelSegment> leaf_label_segments(const nlohmann::json& block,
                                                  NodeParamCards* param_cards_out = nullptr)
    {
        const bool show_vars       = opt_.diagram_show_variables;
        const bool show_delimiters = opt_.diagram_show_variable_delimiters;
        const std::string& link_style  = opt_.diagram_variable_link_style;
        const std::string& input_style = opt_.diagram_variable_input_style;
        const double link_margin       = opt_.diagram_styles.node.variable_link_margin;
        const bool show_params         = diagram_show_params(opt_);
        std::vector<LabelSegment> segments;
        const std::string kind = json_str(block, "kind");
        if (kind == "stdout")            append_segment(segments, "print", LabelSegmentKind::Label);
        else if (kind == "log")          append_segment(segments, "log", LabelSegmentKind::Label);
        else if (kind == "setVariable") {
            append_segment(segments, "set", LabelSegmentKind::Label);
            if (opt_.diagram_show_variables)
                append_segment(segments, json_str(block, "name"), LabelSegmentKind::Variable, link_margin);
        } else if (kind == "getVariable") {
            append_segment(segments, "get", LabelSegmentKind::Label);
            if (opt_.diagram_show_variables)
                append_segment(segments, json_str(block, "name"), LabelSegmentKind::Variable, link_margin);
        } else {
            const std::string& group = kind_group(kind);
            append_registry_block_label(segments, kind, group,
                                        opt_.diagram_show_groups,
                                        opt_.diagram_humanize_node_names);
            if (segments.empty())
                append_segment(segments, kind.empty() ? "unknown" : kind, LabelSegmentKind::Label);
        }

        NodeParamCards cards;
        if (show_params) {
            collect_block_parameters(cards, block, opt_, link_margin);
            collect_store_output_line(cards, block, show_vars, show_delimiters, link_style, link_margin);
            if (param_cards_out)
                *param_cards_out = std::move(cards);
        } else {
            append_store_suffix(segments, block, show_vars, show_delimiters, link_style, link_margin);
            append_used_variables_hint(segments, block, show_vars, show_delimiters, link_style, input_style,
                                       link_margin);
        }
        return segments;
    }

    std::string leaf_label(const nlohmann::json& block)
    {
        return segments_to_label(leaf_label_segments(block));
    }

    std::string case_edge_label(const nlohmann::json& block)
    {
        const std::string comparator = json_str(block, "comparator");
        const std::string expression = clip(json_str(block, "expression"), opt_.max_text);
        if (comparator.empty() || comparator == "===" || comparator == "==")
            return expression;
        return comparator + " " + expression;
    }

    std::vector<GraphExit> emit_list(const nlohmann::json& blocks,
                                     std::vector<GraphExit> incoming,
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
            if (disabled && !opt_.diagram_show_disabled)
                continue;
            if (disabled || unknown) {
                const std::string id = add_node(blocked_node_label(block, disabled, opt_), false);
                connect(incoming, id);
                incoming.clear();
                continue;
            }

            if (kind == "if") {
                std::vector<GraphExit> exits;
                std::vector<GraphExit> pending = std::move(incoming);
                bool first = true;
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
                        const std::string cond = add_node(clip(json_str(member, "condition"), opt_.max_text), true);
                        connect(pending, cond);
                        const auto body = mk == "if"
                            ? member.value("consequent", nlohmann::json::array())
                            : member.value("items", nlohmann::json::array());
                        auto body_exits = emit_list(body, {{cond, "yes"}}, break_sinks);
                        exits.insert(exits.end(), body_exits.begin(), body_exits.end());
                        pending = {{cond, "no"}};
                    } else {
                        auto body_exits = emit_list(member.value("items", nlohmann::json::array()),
                                                    std::move(pending), break_sinks);
                        exits.insert(exits.end(), body_exits.begin(), body_exits.end());
                        pending.clear();
                        ++i;
                        break;
                    }
                    first = false;
                }
                --i;
                exits.insert(exits.end(), pending.begin(), pending.end());
                incoming = std::move(exits);
            } else if (kind == "switch") {
                const std::string sw = add_node("switch " + clip(json_str(block, "variable"), opt_.max_text), true);
                connect(incoming, sw);
                std::vector<GraphExit> exits;
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
                                                {{sw, label}}, break_sinks);
                    exits.insert(exits.end(), body_exits.begin(), body_exits.end());
                }
                if (!has_default)
                    exits.push_back({sw, "no match"});
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
                                            {{loop, "do"}}, &local_breaks);
                connect(body_exits, loop);
                incoming = {{loop, "done"}};
                for (const auto& b : local_breaks)
                    incoming.push_back({b, ""});
            } else if (kind == "break" && break_sinks) {
                const std::string id = add_node("break", false);
                connect(incoming, id);
                break_sinks->push_back(id);
                incoming.clear();
            } else {
                NodeParamCards param_cards;
                const std::vector<LabelSegment> label_segments = leaf_label_segments(block, &param_cards);
                const std::string id = add_node(label_segments, false, std::move(param_cards));
                connect(incoming, id);
                incoming = {{id, ""}};
                if (block.contains("items") && block["items"].is_array())
                    incoming = emit_list(block["items"], std::move(incoming), break_sinks);
            }
        }
        return incoming;
    }
};

struct PositionedNode {
    std::string                  id;
    std::string                  label;
    std::vector<LabelSegment>    segments;
    NodeParamCards               param_cards;
    bool                         decision = false;
    double                       x = 0.0;
    double                       y = 0.0;
    double                       w = 0.0;
    double                       h = 0.0;
};

struct PositionedEdge {
    std::string from;
    std::string to;
    std::string label;
};

struct LayoutResult {
    std::vector<PositionedNode> nodes;
    std::vector<PositionedEdge> edges;
    double                      width  = 0.0;
    double                      height = 0.0;
};

double estimate_param_card_height(const std::vector<ParamLine>& lines, const DiagramStyleConstants& styles)
{
    if (lines.empty())
        return 0.0;
    return styles.node.param_card_padding * 2.0
        + styles.node.param_line_height * static_cast<double>(lines.size());
}

double estimate_node_with_cards_height(const FlowNode& node, const DiagramStyleConstants& styles)
{
    const double inset = styles.node.param_card_inset;
    double height      = styles.node.title_height + inset;
    const bool has_in  = !node.param_cards.inputs.empty();
    const bool has_out = !node.param_cards.outputs.empty();
    if (has_in)
        height += estimate_param_card_height(node.param_cards.inputs, styles);
    if (has_in && has_out)
        height += styles.node.param_card_gap;
    if (has_out)
        height += estimate_param_card_height(node.param_cards.outputs, styles);
    return height + inset;
}

double estimate_decision_size(const FlowNode& node, const DiagramStyleConstants& styles)
{
    const double char_w = styles.decision.label_font_size * 0.50;
    const double text_w = char_w * static_cast<double>(node.label.size());
    const double pad    = styles.decision.label_padding;
    const double fitted = text_w + pad * 2.0;
    const double min_side = styles.decision.label_font_size * 1.6 + pad * 2.0;
    return std::max(fitted, min_side);
}

double estimate_node_width(const FlowNode& node, const DiagramStyleConstants& styles)
{
    if (node.decision)
        return estimate_decision_size(node, styles);
    const double title_char_w = styles.node.label_font_size * 0.55;
    double extra_dx           = 0.0;
    for (const auto& segment : node.segments)
        extra_dx += segment.dx;
    const double title_w =
        estimate_segments_width(node.segments, title_char_w) + 32.0 + extra_dx;

    if (!node_has_param_cards(node.param_cards))
        return std::max(styles.node.min_width, title_w);

    const double cards_w = std::max(estimate_param_card_width(node.param_cards.inputs, styles),
                                    estimate_param_card_width(node.param_cards.outputs, styles))
        + styles.node.param_card_inset * 2.0;
    return std::max(styles.node.min_width, std::max(title_w, cards_w));
}

double node_height(const FlowNode& node, const DiagramStyleConstants& styles)
{
    if (node.decision)
        return estimate_decision_size(node, styles);
    if (node_has_param_cards(node.param_cards))
        return estimate_node_with_cards_height(node, styles);
    return styles.node.height;
}

LayoutResult layout_flow_graph(const FlowGraph& graph,
                               const DiagramStyleConstants& styles,
                               const std::string& direction)
{
    const bool horizontal = (direction == "LR" || direction == "RL");
    const bool reverse    = (direction == "BT" || direction == "RL");

    std::unordered_map<std::string, int> layer;
    std::unordered_map<std::string, int> order;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i)
        order[graph.nodes[i].id] = static_cast<int>(i);

    for (const auto& n : graph.nodes)
        layer[n.id] = 0;

    for (int pass = 0; pass < static_cast<int>(graph.nodes.size()) + 2; ++pass) {
        bool changed = false;
        for (const auto& e : graph.edges) {
            const int next = layer[e.from] + 1;
            if (layer[e.to] < next) {
                layer[e.to] = next;
                changed = true;
            }
        }
        if (!changed)
            break;
    }

    std::map<int, std::vector<const FlowNode*>> layers;
    for (const auto& n : graph.nodes)
        layers[layer[n.id]].push_back(&n);

    LayoutResult out;
    double cursor_main = styles.canvas.padding;
    const double title_reserve = 0.0;

    for (auto& layer_entry : layers) {
        auto& nodes = layer_entry.second;
        std::sort(nodes.begin(), nodes.end(), [&](const FlowNode* a, const FlowNode* b) {
            return order[a->id] < order[b->id];
        });

        double cross_total = 0.0;
        std::vector<double> widths;
        widths.reserve(nodes.size());
        for (const FlowNode* n : nodes) {
            const double w = estimate_node_width(*n, styles);
            const double h = node_height(*n, styles);
            widths.push_back(w);
            cross_total += h;
        }
        if (nodes.size() > 1)
            cross_total += styles.layout.node_spacing_y * static_cast<double>(nodes.size() - 1);

        double cross_cursor = styles.canvas.padding + title_reserve;
        double layer_main_extent = 0.0;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const FlowNode* n = nodes[i];
            const double w = widths[i];
            const double h = node_height(*n, styles);
            PositionedNode pn;
            pn.id       = n->id;
            pn.label       = n->label;
            pn.segments    = n->segments;
            pn.param_cards = n->param_cards;
            pn.decision    = n->decision;
            pn.w        = w;
            pn.h        = h;

            if (horizontal) {
                pn.x = cursor_main;
                pn.y = cross_cursor;
                layer_main_extent = std::max(layer_main_extent, w);
            } else {
                pn.x = cross_cursor;
                pn.y = cursor_main;
                layer_main_extent = std::max(layer_main_extent, w);
            }
            out.nodes.push_back(pn);
            cross_cursor += h + styles.layout.node_spacing_y;
        }

        cursor_main += layer_main_extent + styles.layout.layer_spacing;
    }

    if (horizontal) {
        out.width  = cursor_main + styles.canvas.padding;
        out.height = 0.0;
        for (const auto& n : out.nodes)
            out.height = std::max(out.height, n.y + n.h);
        out.height += styles.canvas.padding + title_reserve;
    } else {
        out.height = cursor_main + styles.canvas.padding + title_reserve;
        out.width  = 0.0;
        for (const auto& n : out.nodes)
            out.width = std::max(out.width, n.x + n.w);
        out.width += styles.canvas.padding;
    }

    if (reverse) {
        if (horizontal) {
            for (auto& n : out.nodes)
                n.x = out.width - styles.canvas.padding - n.x - n.w;
        } else {
            for (auto& n : out.nodes)
                n.y = out.height - styles.canvas.padding - title_reserve - n.y - n.h;
        }
    }

    for (const auto& e : graph.edges)
        out.edges.push_back({e.from, e.to, e.label});

    return out;
}

LayoutResult layout_flow_graph_compact(const FlowGraph& graph,
                                     const DiagramStyleConstants& styles,
                                     const std::string& direction,
                                     double main_gap_override = -1.0)
{
    const bool horizontal = (direction == "LR" || direction == "RL");
    const bool reverse    = (direction == "BT" || direction == "RL");

    std::unordered_map<std::string, int> order;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i)
        order[graph.nodes[i].id] = static_cast<int>(i);

    std::vector<const FlowNode*> sorted;
    sorted.reserve(graph.nodes.size());
    for (const auto& n : graph.nodes)
        sorted.push_back(&n);
    std::sort(sorted.begin(), sorted.end(), [&](const FlowNode* a, const FlowNode* b) {
        return order[a->id] < order[b->id];
    });

    const double gap = main_gap_override >= 0.0
        ? main_gap_override
        : std::min(styles.layout.node_spacing_y * 0.35, 20.0);

    LayoutResult out;
    double main_cursor = styles.canvas.padding;
    double cross_extent = 0.0;

    for (const FlowNode* n : sorted) {
        PositionedNode pn;
        pn.id          = n->id;
        pn.label       = n->label;
        pn.segments    = n->segments;
        pn.param_cards = n->param_cards;
        pn.decision    = n->decision;
        pn.w        = estimate_node_width(*n, styles);
        pn.h        = node_height(*n, styles);

        if (horizontal) {
            pn.x = main_cursor;
            pn.y = styles.canvas.padding;
            cross_extent = std::max(cross_extent, pn.h);
            main_cursor += pn.w + gap;
        } else {
            pn.x = styles.canvas.padding;
            pn.y = main_cursor;
            cross_extent = std::max(cross_extent, pn.w);
            main_cursor += pn.h + gap;
        }
        out.nodes.push_back(pn);
    }

    if (sorted.empty()) {
        out.width  = styles.canvas.padding * 2.0;
        out.height = styles.canvas.padding * 2.0;
        return out;
    }

    if (horizontal) {
        out.width  = main_cursor - gap + styles.canvas.padding;
        out.height = cross_extent + styles.canvas.padding * 2.0;
        for (auto& n : out.nodes)
            n.y = styles.canvas.padding + (cross_extent - n.h) * 0.5;
    } else {
        out.height = main_cursor - gap + styles.canvas.padding;
        out.width  = cross_extent + styles.canvas.padding * 2.0;
        for (auto& n : out.nodes)
            n.x = styles.canvas.padding + (cross_extent - n.w) * 0.5;
    }

    if (reverse) {
        if (horizontal) {
            for (auto& n : out.nodes)
                n.x = out.width - styles.canvas.padding - n.x - n.w;
        } else {
            for (auto& n : out.nodes)
                n.y = out.height - styles.canvas.padding - n.y - n.h;
        }
    }

    return out;
}

bool is_branch_lane_edge(const std::string& label)
{
    return label == "yes" || label == "do";
}

void assign_branch_lanes(const FlowGraph& graph,
                         std::unordered_map<std::string, int>& lane)
{
    std::unordered_map<std::string, int> indegree;
    std::unordered_map<std::string, std::vector<const FlowEdge*>> outgoing;
    for (const auto& n : graph.nodes)
        lane[n.id] = 0;
    for (const auto& e : graph.edges) {
        indegree[e.to]++;
        outgoing[e.from].push_back(&e);
    }
    for (const auto& n : graph.nodes) {
        if (indegree[n.id] > 1)
            lane[n.id] = 0;
    }
    for (const auto& e : graph.edges) {
        if (!is_branch_lane_edge(e.label))
            continue;
        std::deque<std::string> pending{e.to};
        std::unordered_set<std::string> seen;
        while (!pending.empty()) {
            const std::string id = pending.front();
            pending.pop_front();
            if (!seen.insert(id).second)
                continue;
            if (indegree[id] > 1 && id != e.to)
                continue;
            lane[id] = std::max(lane[id], 1);
            const auto out_it = outgoing.find(id);
            if (out_it == outgoing.end())
                continue;
            for (const FlowEdge* out_e : out_it->second) {
                if (out_e->label == "no")
                    continue;
                if (indegree[out_e->to] > 1)
                    continue;
                pending.push_back(out_e->to);
            }
        }
    }
}

LayoutResult layout_flow_graph_linked(const FlowGraph& graph,
                                      const DiagramStyleConstants& styles,
                                      const std::string& direction)
{
    const bool horizontal = (direction == "LR" || direction == "RL");
    if (horizontal)
        return layout_flow_graph(graph, styles, direction);

    LayoutResult out = layout_flow_graph_compact(graph, styles, direction, styles.layout.link_gap);

    std::unordered_map<std::string, int> lane;
    assign_branch_lanes(graph, lane);

    const double lane_w = styles.layout.branch_lane_width;
    double max_right = styles.canvas.padding;
    for (auto& pn : out.nodes) {
        const auto lane_it = lane.find(pn.id);
        if (lane_it != lane.end())
            pn.x += lane_it->second * lane_w;
        max_right = std::max(max_right, pn.x + pn.w);
    }
    out.width = max_right + styles.canvas.padding;

    for (const auto& e : graph.edges)
        out.edges.push_back({e.from, e.to, e.label});
    return out;
}

struct Point {
    double x = 0.0;
    double y = 0.0;
};

Point node_center(const PositionedNode& n)
{
    return {n.x + n.w * 0.5, n.y + n.h * 0.5};
}

Point node_exit(const PositionedNode& n, bool horizontal, bool reverse)
{
    if (horizontal)
        return reverse ? Point{n.x, n.y + n.h * 0.5} : Point{n.x + n.w, n.y + n.h * 0.5};
    return reverse ? Point{n.x + n.w * 0.5, n.y} : Point{n.x + n.w * 0.5, n.y + n.h};
}

Point node_entry(const PositionedNode& n, bool horizontal, bool reverse)
{
    if (horizontal)
        return reverse ? Point{n.x + n.w, n.y + n.h * 0.5} : Point{n.x, n.y + n.h * 0.5};
    return reverse ? Point{n.x + n.w * 0.5, n.y + n.h} : Point{n.x + n.w * 0.5, n.y};
}

struct EdgeRoute {
    std::string path;
    Point       label;
};

diagram_pathfinding::Vec2 pin_direction(bool horizontal, bool reverse, bool outgoing)
{
    if (horizontal) {
        if (outgoing)
            return reverse ? diagram_pathfinding::Vec2{-1.0, 0.0} : diagram_pathfinding::Vec2{1.0, 0.0};
        return reverse ? diagram_pathfinding::Vec2{1.0, 0.0} : diagram_pathfinding::Vec2{-1.0, 0.0};
    }
    if (outgoing)
        return reverse ? diagram_pathfinding::Vec2{0.0, -1.0} : diagram_pathfinding::Vec2{0.0, 1.0};
    return reverse ? diagram_pathfinding::Vec2{0.0, 1.0} : diagram_pathfinding::Vec2{0.0, -1.0};
}

diagram_pathfinding::RoutingContext build_routing_context(
    const PositionedNode& from,
    const PositionedNode& to,
    const std::unordered_map<std::string, PositionedNode>& nodes,
    const std::string& from_id,
    const std::string& to_id,
    bool horizontal,
    bool reverse,
    double margin)
{
    diagram_pathfinding::RoutingContext ctx;
    ctx.margin = margin;
    const Point exit = node_exit(from, horizontal, reverse);
    const Point entry = node_entry(to, horizontal, reverse);
    ctx.start_pin.position  = {exit.x, exit.y};
    ctx.start_pin.direction = pin_direction(horizontal, reverse, true);
    ctx.end_pin.position    = {entry.x, entry.y};
    ctx.end_pin.direction   = pin_direction(horizontal, reverse, false);
    ctx.start_node.min = {from.x, from.y};
    ctx.start_node.max = {from.x + from.w, from.y + from.h};
    ctx.end_node.min   = {to.x, to.y};
    ctx.end_node.max   = {to.x + to.w, to.y + to.h};
    ctx.obstacles.reserve(nodes.size());
    for (const auto& [id, node] : nodes) {
        if (id == from_id || id == to_id)
            continue;
        ctx.obstacles.push_back({{node.x, node.y}, {node.x + node.w, node.y + node.h}});
    }
    return ctx;
}

std::string path_from_points(const Point& p0,
                             const std::vector<diagram_pathfinding::Vec2>& waypoints,
                             const Point& p1)
{
    std::ostringstream path;
    path << "M " << p0.x << " " << p0.y;
    for (const auto& wp : waypoints)
        path << " L " << wp.x << " " << wp.y;
    path << " L " << p1.x << " " << p1.y;
    return path.str();
}

Point label_for_route(const Point& p0,
                      const Point& p1,
                      const std::vector<diagram_pathfinding::Vec2>& waypoints)
{
    if (!waypoints.empty()) {
        const auto& wp = waypoints[waypoints.size() / 2];
        return {wp.x, wp.y - 6.0};
    }
    return {(p0.x + p1.x) * 0.5, (p0.y + p1.y) * 0.5 - 6.0};
}

EdgeRoute route_edge_straight(const Point& p0, const Point& p1, bool horizontal)
{
    EdgeRoute route;
    route.label = {(p0.x + p1.x) * 0.5, (p0.y + p1.y) * 0.5};
    if (horizontal) {
        if (std::abs(p0.y - p1.y) < 1.0) {
            route.path = "M " + std::to_string(p0.x) + " " + std::to_string(p0.y)
                + " L " + std::to_string(p1.x) + " " + std::to_string(p1.y);
            return route;
        }
        const double mid_x = p0.x + (p1.x - p0.x) * 0.5;
        route.label = {mid_x, (p0.y + p1.y) * 0.5};
        route.path = "M " + std::to_string(p0.x) + " " + std::to_string(p0.y)
            + " L " + std::to_string(mid_x) + " " + std::to_string(p0.y)
            + " L " + std::to_string(mid_x) + " " + std::to_string(p1.y)
            + " L " + std::to_string(p1.x) + " " + std::to_string(p1.y);
        return route;
    }

    if (std::abs(p0.x - p1.x) < 1.0 && p1.y >= p0.y) {
        route.path = "M " + std::to_string(p0.x) + " " + std::to_string(p0.y)
            + " L " + std::to_string(p1.x) + " " + std::to_string(p1.y);
        route.label = {p0.x, (p0.y + p1.y) * 0.5};
        return route;
    }

    const double stub  = 10.0;
    const double mid_y = p0.y + std::max(stub, (p1.y - p0.y) * 0.5);
    route.label        = {(p0.x + p1.x) * 0.5, mid_y - 6.0};
    route.path         = "M " + std::to_string(p0.x) + " " + std::to_string(p0.y)
        + " L " + std::to_string(p0.x) + " " + std::to_string(mid_y)
        + " L " + std::to_string(p1.x) + " " + std::to_string(mid_y)
        + " L " + std::to_string(p1.x) + " " + std::to_string(p1.y);
    return route;
}

EdgeRoute route_edge_bezier(const Point& p0, const Point& p1, bool horizontal)
{
    EdgeRoute route;
    route.label = {(p0.x + p1.x) * 0.5, (p0.y + p1.y) * 0.5 - 6.0};
    if (horizontal) {
        if (std::abs(p0.y - p1.y) < 1.0) {
            route.path = "M " + std::to_string(p0.x) + " " + std::to_string(p0.y)
                + " L " + std::to_string(p1.x) + " " + std::to_string(p1.y);
            return route;
        }
        const double mid_x = p0.x + (p1.x - p0.x) * 0.5;
        route.label        = {mid_x, (p0.y + p1.y) * 0.5 - 6.0};
        route.path         = "M " + std::to_string(p0.x) + " " + std::to_string(p0.y)
            + " C " + std::to_string(mid_x) + " " + std::to_string(p0.y)
            + ", " + std::to_string(mid_x) + " " + std::to_string(p1.y)
            + ", " + std::to_string(p1.x) + " " + std::to_string(p1.y);
        return route;
    }

    if (std::abs(p0.x - p1.x) < 1.0 && p1.y >= p0.y) {
        route.path  = "M " + std::to_string(p0.x) + " " + std::to_string(p0.y)
            + " L " + std::to_string(p1.x) + " " + std::to_string(p1.y);
        route.label = {p0.x, (p0.y + p1.y) * 0.5 - 6.0};
        return route;
    }

    const double stub  = 10.0;
    const double mid_y = p0.y + std::max(stub, (p1.y - p0.y) * 0.5);
    route.label        = {(p0.x + p1.x) * 0.5, mid_y - 6.0};
    route.path         = "M " + std::to_string(p0.x) + " " + std::to_string(p0.y)
        + " C " + std::to_string(p0.x) + " " + std::to_string(mid_y)
        + ", " + std::to_string(p1.x) + " " + std::to_string(mid_y)
        + ", " + std::to_string(p1.x) + " " + std::to_string(p1.y);
    return route;
}

EdgeRoute route_edge_waypoints(const Point& p0,
                               const Point& p1,
                               const PositionedNode& from,
                               const PositionedNode& to,
                               const std::unordered_map<std::string, PositionedNode>& nodes,
                               const std::string& from_id,
                               const std::string& to_id,
                               bool horizontal,
                               bool reverse,
                               double margin)
{
    const diagram_pathfinding::RoutingContext ctx =
        build_routing_context(from, to, nodes, from_id, to_id, horizontal, reverse, margin);
    const std::vector<diagram_pathfinding::Vec2> waypoints = diagram_pathfinding::generate_waypoints(ctx);
    EdgeRoute route;
    route.path  = path_from_points(p0, waypoints, p1);
    route.label = label_for_route(p0, p1, waypoints);
    return route;
}

EdgeRoute route_edge(const Point& p0,
                     const Point& p1,
                     const PositionedNode& from,
                     const PositionedNode& to,
                     const std::unordered_map<std::string, PositionedNode>& nodes,
                     const std::string& from_id,
                     const std::string& to_id,
                     bool horizontal,
                     bool reverse,
                     const std::string& link_style,
                     double margin)
{
    if (link_style == "bezier")
        return route_edge_bezier(p0, p1, horizontal);
    if (link_style == "waypoints")
        return route_edge_waypoints(p0, p1, from, to, nodes, from_id, to_id, horizontal, reverse, margin);
    return route_edge_straight(p0, p1, horizontal);
}

void append_label_text(std::ostringstream& svg,
                       double x,
                       double y,
                       const std::vector<LabelSegment>& segments,
                       const std::string& font_family,
                       double font_size,
                       const std::string& default_fill,
                       const std::string& group_fill,
                       const std::string& variable_fill,
                       const std::string& operator_fill)
{
    if (segments.empty())
        return;
    svg << "<text x=\"" << x << "\" y=\"" << y
        << "\" text-anchor=\"middle\" font-family=\"" << xml_escape(font_family)
        << "\" font-size=\"" << font_size << "\">";
    for (const auto& segment : segments) {
        std::string fill = default_fill;
        switch (segment.kind) {
            case LabelSegmentKind::Group:    fill = group_fill; break;
            case LabelSegmentKind::Variable: fill = variable_fill; break;
            case LabelSegmentKind::Operator: fill = operator_fill; break;
            default: break;
        }
        svg << "<tspan";
        if (segment.dx > 0.0)
            svg << " dx=\"" << segment.dx << "\"";
        svg << " fill=\"" << fill << "\">" << xml_escape(segment.text) << "</tspan>";
    }
    svg << "</text>\n";
}

void append_param_line_row(std::ostringstream& svg,
                           double y,
                           const ParamLine& line,
                           const ParamCardColumns& cols,
                           double table_x,
                           const DiagramStyleConstants& styles)
{
    const std::string& font_family = styles.node.label_font_family;
    const double font_size         = styles.node.param_font_size;

    if (!line.freeform.empty()) {
        append_label_text(svg,
                          table_x + cols.table_w * 0.5,
                          y,
                          line.freeform,
                          font_family,
                          font_size,
                          styles.node.label_fill,
                          styles.node.group_fill,
                          styles.node.variable_fill,
                          styles.node.variable_operator_fill);
        return;
    }

    const std::string label = param_line_label_text(line);
    const double name_end_x = table_x + cols.name_w;
    const double colon_x    = name_end_x + 2.0;

    svg << "<text y=\"" << y << "\" font-family=\"" << xml_escape(font_family) << "\" font-size=\""
        << font_size << "\">";
    svg << "<tspan x=\"" << name_end_x << "\" text-anchor=\"end\" fill=\"" << styles.node.label_fill
        << "\">" << xml_escape(label) << "</tspan>";
    if (line.show_value) {
        svg << "<tspan x=\"" << colon_x << "\" fill=\"" << styles.node.variable_operator_fill
            << "\">: </tspan>";

        std::vector<LabelSegment> value_segments;
        const double var_margin = std::max(3.0, styles.node.variable_link_margin * 0.5);
        split_string_interpolation_segments(value_segments, line.value,
                                            styles.render.show_variable_delimiters, var_margin);
        const bool has_interpolation = string_has_interpolation(line.value);
        append_svg_text_tspans(svg,
                               value_segments,
                               has_interpolation,
                               styles.node.label_fill,
                               styles.node.variable_fill,
                               line.value_dx);
    }
    svg << "</text>\n";
}

void append_param_card(std::ostringstream& svg,
                       double x,
                       double y,
                       double width,
                       const std::vector<ParamLine>& lines,
                       const DiagramStyleConstants& styles)
{
    if (lines.empty())
        return;

    const double height = estimate_param_card_height(lines, styles);
    svg << "<rect x=\"" << x << "\" y=\"" << y << "\" width=\"" << width << "\" height=\"" << height
        << "\" rx=\"" << styles.node.param_card_radius << "\" ry=\"" << styles.node.param_card_radius
        << "\" fill=\"" << styles.node.param_card_fill << "\" stroke=\"" << styles.node.param_card_stroke
        << "\" stroke-width=\"" << styles.node.stroke_width << "\"/>\n";

    const ParamCardColumns cols = measure_param_card_columns(lines, styles);
    const double table_x        = x + (width - cols.table_w) * 0.5;
    double line_y               = y + styles.node.param_card_padding + styles.node.param_font_size * 0.85;
    for (const ParamLine& line : lines) {
        append_param_line_row(svg, line_y, line, cols, table_x, styles);
        line_y += styles.node.param_line_height;
    }
}

void append_node_body(std::ostringstream& svg,
                      const PositionedNode& n,
                      double x,
                      double y,
                      const DiagramStyleConstants& styles)
{
    const bool has_cards = node_has_param_cards(n.param_cards);
    const std::vector<LabelSegment>& title_segments = n.segments.empty()
        ? std::vector<LabelSegment>{{n.label, LabelSegmentKind::Label}}
        : n.segments;

    if (!has_cards) {
        append_label_text(svg,
                          x + n.w * 0.5,
                          y + n.h * 0.5 + styles.node.label_font_size * 0.35,
                          title_segments,
                          styles.node.label_font_family,
                          styles.node.label_font_size,
                          styles.node.label_fill,
                          styles.node.group_fill,
                          styles.node.variable_fill,
                          styles.node.variable_operator_fill);
        return;
    }

    append_label_text(svg,
                      x + n.w * 0.5,
                      y + styles.node.title_height * 0.65,
                      title_segments,
                      styles.node.label_font_family,
                      styles.node.label_font_size,
                      styles.node.label_fill,
                      styles.node.group_fill,
                      styles.node.variable_fill,
                      styles.node.variable_operator_fill);

    const double card_w = n.w - styles.node.param_card_inset * 2.0;
    const double card_x = x + styles.node.param_card_inset;
    double card_y       = y + styles.node.title_height + styles.node.param_card_inset * 0.5;

    if (!n.param_cards.inputs.empty()) {
        append_param_card(svg, card_x, card_y, card_w, n.param_cards.inputs, styles);
        card_y += estimate_param_card_height(n.param_cards.inputs, styles) + styles.node.param_card_gap;
    }
    if (!n.param_cards.outputs.empty())
        append_param_card(svg, card_x, card_y, card_w, n.param_cards.outputs, styles);
}

std::string render_svg(const LayoutResult& layout,
                       const DiagramStyleConstants& styles,
                       const std::string& direction,
                       const std::string& title,
                       bool standalone,
                       bool show_arrows,
                       bool show_links,
                       const std::string& link_style)
{
    const bool horizontal = (direction == "LR" || direction == "RL");
    const bool reverse    = (direction == "BT" || direction == "RL");

    const double title_h = title.empty() ? 0.0 : styles.title.font_size + 24.0;
    const double svg_w   = layout.width;
    const double svg_h   = layout.height + title_h;

    std::unordered_map<std::string, PositionedNode> nodes;
    for (const auto& n : layout.nodes)
        nodes.emplace(n.id, n);

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
        << "  </filter>\n";
    if (show_arrows && show_links) {
        svg << "  <marker id=\"arrowHead\" markerWidth=\"" << styles.edge.arrow_size
            << "\" markerHeight=\"" << styles.edge.arrow_size << "\" refX=\"" << (styles.edge.arrow_size - 1)
            << "\" refY=\"" << (styles.edge.arrow_size * 0.5)
            << "\" orient=\"auto\" markerUnits=\"userSpaceOnUse\">\n"
            << "    <path d=\"M0,0 L0," << styles.edge.arrow_size << " L" << styles.edge.arrow_size << ","
            << (styles.edge.arrow_size * 0.5) << " z\" fill=\"" << styles.edge.stroke << "\"/>\n"
            << "  </marker>\n";
    }
    svg << "</defs>\n"
        << "<rect width=\"100%\" height=\"100%\" fill=\"" << styles.canvas.background << "\"/>\n";

    if (!title.empty()) {
        svg << "<text x=\"" << (svg_w * 0.5) << "\" y=\"" << (styles.canvas.padding * 0.5 + styles.title.font_size)
            << "\" text-anchor=\"middle\" font-family=\"" << xml_escape(styles.title.font_family)
            << "\" font-size=\"" << styles.title.font_size << "\" fill=\"" << styles.title.fill << "\">"
            << xml_escape(title) << "</text>\n";
    }

    const double y_offset = title_h;

    if (show_links) {
        for (const auto& e : layout.edges) {
            const auto from_it = nodes.find(e.from);
            const auto to_it   = nodes.find(e.to);
            if (from_it == nodes.end() || to_it == nodes.end())
                continue;
            PositionedNode from = from_it->second;
            PositionedNode to   = to_it->second;
            from.y += y_offset;
            to.y += y_offset;

            const Point p0 = node_exit(from, horizontal, reverse);
            const Point p1 = node_entry(to, horizontal, reverse);
            const EdgeRoute route = route_edge(p0,
                                               p1,
                                               from,
                                               to,
                                               nodes,
                                               e.from,
                                               e.to,
                                               horizontal,
                                               reverse,
                                               link_style,
                                               styles.edge.route_margin);
            svg << "<path d=\"" << route.path << "\" fill=\"none\" stroke=\"" << styles.edge.stroke
                << "\" stroke-width=\"" << styles.edge.stroke_width << "\"";
            if (show_arrows)
                svg << " marker-end=\"url(#arrowHead)\"";
            svg << "/>\n";
            if (!e.label.empty()) {
                svg << "<text x=\"" << route.label.x << "\" y=\"" << route.label.y
                    << "\" text-anchor=\"middle\" font-family=\"" << xml_escape(styles.edge.label_font_family)
                    << "\" font-size=\"" << styles.edge.label_font_size << "\" fill=\"" << styles.edge.label_fill
                    << "\">" << xml_escape(e.label) << "</text>\n";
            }
        }
    }

    for (const auto& n : layout.nodes) {
        const double x = n.x;
        const double y = n.y + y_offset;
        if (n.decision) {
            const Point c = {x + n.w * 0.5, y + n.h * 0.5};
            const double r = n.w * 0.5;
            svg << "<polygon points=\"" << c.x << "," << (c.y - r) << " " << (c.x + r) << "," << c.y << " "
                << c.x << "," << (c.y + r) << " " << (c.x - r) << "," << c.y
                << "\" fill=\"" << styles.decision.fill << "\" stroke=\"" << styles.decision.stroke
                << "\" stroke-width=\"" << styles.decision.stroke_width << "\" filter=\"url(#cardShadow)\"/>\n";
            svg << "<text x=\"" << c.x << "\" y=\"" << (c.y + styles.decision.label_font_size * 0.35)
                << "\" text-anchor=\"middle\" font-family=\"" << xml_escape(styles.node.label_font_family)
                << "\" font-size=\"" << styles.decision.label_font_size << "\" fill=\"" << styles.decision.label_fill
                << "\">" << xml_escape(n.label) << "</text>\n";
        } else {
            svg << "<rect x=\"" << x << "\" y=\"" << y << "\" width=\"" << n.w << "\" height=\"" << n.h
                << "\" rx=\"" << styles.node.corner_radius << "\" ry=\"" << styles.node.corner_radius
                << "\" fill=\"" << styles.node.fill << "\" stroke=\"" << styles.node.stroke
                << "\" stroke-width=\"" << styles.node.stroke_width << "\" filter=\"url(#cardShadow)\"/>\n";
            append_node_body(svg, n, x, y, styles);
        }
    }

    svg << "</svg>\n";
    return svg.str();
}

void ensure_vips_once()
{
    static std::once_flag once;
    std::call_once(once, [] {
        if (vips_init("xblox_diagram"))
            std::abort();
    });
}

std::string vips_error_message()
{
    const char* buf = vips_error_buffer();
    std::string msg = buf ? buf : "vips error";
    vips_error_clear();
    return msg;
}

} // namespace

std::string render_flow_diagram_svg(const nlohmann::json& roots,
                                    const MarkdownOptions& options,
                                    const DiagramStyleConstants& styles,
                                    const std::string& direction,
                                    const std::string& title,
                                    bool standalone)
{
    FlowGraphBuilder builder(options);
    const FlowGraph graph = builder.build(roots);
    const std::string dir = direction.empty() ? "TD" : direction;
    const bool show_links = options.diagram_show_links;
    const bool show_arrows = options.diagram_show_arrows && show_links;
    const std::string link_style = options.diagram_link_style;
    if (graph.nodes.empty()) {
        LayoutResult empty{};
        empty.width  = styles.canvas.padding * 2.0;
        empty.height = styles.canvas.padding * 2.0;
        return render_svg(empty, styles, dir, title, standalone, show_arrows, show_links, link_style);
    }

    const LayoutResult layout = show_links
        ? layout_flow_graph_linked(graph, styles, dir)
        : layout_flow_graph_compact(graph, styles, dir);
    return render_svg(layout, styles, dir, title, standalone, show_arrows, show_links, link_style);
}

bool save_diagram_svg_to_file(const std::string& svg,
                              const std::string& output_path,
                              std::string& error)
{
    if (svg.empty()) {
        error = "diagram svg: empty SVG";
        return false;
    }
    if (output_path.empty()) {
        error = "diagram svg: empty output path";
        return false;
    }
    std::error_code ec;
    const std::filesystem::path out = media::path::path_from_utf8(output_path);
    if (out.has_parent_path())
        std::filesystem::create_directories(out.parent_path(), ec);
    std::ofstream file(out, std::ios::binary);
    if (!file) {
        error = "diagram svg: cannot write " + output_path;
        return false;
    }
    file.write(svg.data(), static_cast<std::streamsize>(svg.size()));
    if (!file) {
        error = "diagram svg: write failed for " + output_path;
        return false;
    }
    return true;
}

bool save_diagram_svg_as_png(const std::string& svg,
                             const std::string& output_path,
                             double dpi,
                             std::string& error)
{
    if (svg.empty()) {
        error = "diagram png: empty SVG";
        return false;
    }
    ensure_vips_once();

    VipsImage* im = vips_image_new_from_buffer(
        svg.data(), svg.size(), "", "dpi", dpi > 0.0 ? dpi : 144.0, nullptr);
    if (!im) {
        error = "diagram png: svgload failed: " + vips_error_message();
        return false;
    }

    const bool ok = vips_pngsave(im, output_path.c_str(), "strip", TRUE, "compression", 6, nullptr) == 0;
    g_object_unref(im);
    if (!ok) {
        error = "diagram png: pngsave failed: " + vips_error_message();
        return false;
    }
    return true;
}

namespace {

std::string base64_encode_bytes(const unsigned char* data, std::size_t len)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        const unsigned int b0 = data[i];
        const unsigned int b1 = i + 1 < len ? data[i + 1] : 0;
        const unsigned int b2 = i + 2 < len ? data[i + 2] : 0;
        const unsigned int n  = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(i + 1 < len ? kAlphabet[(n >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < len ? kAlphabet[n & 0x3F] : '=');
    }
    return out;
}

} // namespace

bool encode_diagram_svg_as_png_base64(const std::string& svg,
                                      double dpi,
                                      std::string& out_base64,
                                      std::string& error)
{
    if (svg.empty()) {
        error = "diagram png: empty SVG";
        return false;
    }
    ensure_vips_once();

    VipsImage* im = vips_image_new_from_buffer(
        svg.data(), svg.size(), "", "dpi", dpi > 0.0 ? dpi : 144.0, nullptr);
    if (!im) {
        error = "diagram png: svgload failed: " + vips_error_message();
        return false;
    }

    void*  buffer = nullptr;
    size_t length = 0;
    const bool ok =
        vips_pngsave_buffer(im, &buffer, &length, "strip", TRUE, "compression", 6, nullptr) == 0;
    g_object_unref(im);
    if (!ok || !buffer || length == 0) {
        if (buffer)
            g_free(buffer);
        error = "diagram png: pngsave_buffer failed: " + vips_error_message();
        return false;
    }
    out_base64 = base64_encode_bytes(static_cast<const unsigned char*>(buffer), length);
    g_free(buffer);
    return true;
}

std::string render_document_diagram_svg(const nlohmann::json& blocks_file,
                                        const MarkdownOptions& options)
{
    if (options.diagram_type == "sequence")
        return render_sequence_diagram_svg(
            blocks_file, options, options.diagram_styles, options.title, true);
    const nlohmann::json roots = blocks_file.value("roots", nlohmann::json::array());
    return render_flow_diagram_svg(roots,
                                   options,
                                   options.diagram_styles,
                                   options.mermaid_direction,
                                   options.title,
                                   true);
}

} // namespace media::xblox
