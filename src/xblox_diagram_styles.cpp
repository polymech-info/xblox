#include "xblox/xblox_diagram_styles.hpp"

#include "core/command_variables.hpp"
#include "fs/path_sanitizer.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

namespace media::xblox {

namespace {

std::string json_string(const nlohmann::json& o, const char* key, std::string fallback = {})
{
    if (!o.is_object() || !o.contains(key))
        return fallback;
    if (o[key].is_string())
        return o[key].get<std::string>();
    return fallback;
}

double json_number(const nlohmann::json& o, const char* key, double fallback)
{
    if (!o.is_object() || !o.contains(key))
        return fallback;
    if (o[key].is_number())
        return o[key].get<double>();
    return fallback;
}

bool json_bool(const nlohmann::json& o, const char* key, bool fallback)
{
    if (!o.is_object() || !o.contains(key))
        return fallback;
    if (o[key].is_boolean())
        return o[key].get<bool>();
    return fallback;
}

void apply_canvas(const nlohmann::json& j, DiagramCanvasStyle& out)
{
    if (!j.is_object())
        return;
    out.background = json_string(j, "background", out.background);
    out.padding    = json_number(j, "padding", out.padding);
}

void apply_title(const nlohmann::json& j, DiagramTitleStyle& out)
{
    if (!j.is_object())
        return;
    out.font_family = json_string(j, "fontFamily", out.font_family);
    out.font_size   = json_number(j, "fontSize", out.font_size);
    out.fill        = json_string(j, "fill", out.fill);
}

void apply_node(const nlohmann::json& j, DiagramNodeStyle& out)
{
    if (!j.is_object())
        return;
    out.fill              = json_string(j, "fill", out.fill);
    out.stroke            = json_string(j, "stroke", out.stroke);
    out.stroke_width      = json_number(j, "strokeWidth", out.stroke_width);
    out.corner_radius     = json_number(j, "cornerRadius", out.corner_radius);
    out.min_width         = json_number(j, "minWidth", out.min_width);
    out.height            = json_number(j, "height", out.height);
    out.label_font_family = json_string(j, "labelFontFamily", out.label_font_family);
    out.label_font_size   = json_number(j, "labelFontSize", out.label_font_size);
    out.label_fill           = json_string(j, "labelFill", out.label_fill);
    out.variable_fill        = json_string(j, "variableFill", out.variable_fill);
    out.variable_operator_fill = json_string(j, "variableOperatorFill", out.variable_operator_fill);
    out.group_fill             = json_string(j, "groupFill", out.group_fill);
    out.variable_link_margin   = json_number(j, "variableLinkMargin", out.variable_link_margin);
    out.shadow_blur       = json_number(j, "shadowBlur", out.shadow_blur);
    out.shadow_offset_y   = json_number(j, "shadowOffsetY", out.shadow_offset_y);
    out.shadow_opacity    = json_number(j, "shadowOpacity", out.shadow_opacity);
    out.param_card_fill   = json_string(j, "paramCardFill", out.param_card_fill);
    out.param_card_stroke = json_string(j, "paramCardStroke", out.param_card_stroke);
    out.param_card_radius = json_number(j, "paramCardRadius", out.param_card_radius);
    out.param_font_size   = json_number(j, "paramFontSize", out.param_font_size);
    out.title_height      = json_number(j, "titleHeight", out.title_height);
    out.param_line_height = json_number(j, "paramLineHeight", out.param_line_height);
    out.param_card_padding = json_number(j, "paramCardPadding", out.param_card_padding);
    out.param_card_gap    = json_number(j, "paramCardGap", out.param_card_gap);
    out.param_card_inset  = json_number(j, "paramCardInset", out.param_card_inset);
}

void apply_decision(const nlohmann::json& j, DiagramDecisionStyle& out)
{
    if (!j.is_object())
        return;
    out.fill            = json_string(j, "fill", out.fill);
    out.stroke          = json_string(j, "stroke", out.stroke);
    out.stroke_width    = json_number(j, "strokeWidth", out.stroke_width);
    out.size            = json_number(j, "size", out.size);
    out.label_padding   = json_number(j, "labelPadding", out.label_padding);
    out.label_font_size = json_number(j, "labelFontSize", out.label_font_size);
    out.label_fill      = json_string(j, "labelFill", out.label_fill);
}

void apply_edge(const nlohmann::json& j, DiagramEdgeStyle& out)
{
    if (!j.is_object())
        return;
    out.stroke            = json_string(j, "stroke", out.stroke);
    out.stroke_width      = json_number(j, "strokeWidth", out.stroke_width);
    out.arrow_size        = json_number(j, "arrowSize", out.arrow_size);
    out.route_margin      = json_number(j, "routeMargin", out.route_margin);
    out.label_font_family = json_string(j, "labelFontFamily", out.label_font_family);
    out.label_font_size   = json_number(j, "labelFontSize", out.label_font_size);
    out.label_fill        = json_string(j, "labelFill", out.label_fill);
}

void apply_layout(const nlohmann::json& j, DiagramLayoutStyle& out)
{
    if (!j.is_object())
        return;
    out.node_spacing_x   = json_number(j, "nodeSpacingX", out.node_spacing_x);
    out.node_spacing_y   = json_number(j, "nodeSpacingY", out.node_spacing_y);
    out.layer_spacing    = json_number(j, "layerSpacing", out.layer_spacing);
    out.link_gap         = json_number(j, "linkGap", out.link_gap);
    out.branch_lane_width = json_number(j, "branchLaneWidth", out.branch_lane_width);
}

void apply_render(const nlohmann::json& j, DiagramRenderOptions& out)
{
    if (!j.is_object())
        return;
    out.show_arrows          = json_bool(j, "showArrows", out.show_arrows);
    out.show_links           = json_bool(j, "showLinks", out.show_links);
    out.show_disabled        = json_bool(j, "showDisabled", out.show_disabled);
    out.show_variables       = json_bool(j, "showVariables", out.show_variables);
    out.show_groups              = json_bool(j, "showGroups", out.show_groups);
    out.humanize_node_names      = json_bool(j, "humanizeNodeNames", out.humanize_node_names);
    out.show_variable_delimiters = json_bool(j, "showVariableDelimiters", out.show_variable_delimiters);
    out.variable_link_style  = json_string(j, "variableLinkStyle", out.variable_link_style);
    out.variable_input_style = json_string(j, "variableInputStyle", out.variable_input_style);
    out.link_style           = json_string(j, "linkStyle", out.link_style);
    out.show_parameters      = json_string(j, "showParameters", out.show_parameters);
    out.show_parameters_grouped = json_bool(j, "showParametersGrouped", out.show_parameters_grouped);
    out.show_parameter_values   = json_bool(j, "showParameterValues", out.show_parameter_values);
}

void apply_palette(const nlohmann::json& j, DiagramStyleConstants& out)
{
    if (!j.is_object())
        return;
    apply_canvas(j.value("canvas", nlohmann::json::object()), out.canvas);
    apply_title(j.value("title", nlohmann::json::object()), out.title);
    apply_node(j.value("node", nlohmann::json::object()), out.node);
    apply_decision(j.value("decision", nlohmann::json::object()), out.decision);
    apply_edge(j.value("edge", nlohmann::json::object()), out.edge);
    out.accent = json_string(j, "accent", out.accent);
}

} // namespace

std::string default_diagram_styles_expression()
{
    return "${TANIT_SHARED}/xblox-diagram-constants.json";
}

std::string resolve_diagram_styles_path(const std::string& path_or_expression, const std::string& cwd)
{
    media::commands::VariableContext ctx;
    ctx.cwd = cwd;
    const media::commands::VariableMap vars = media::commands::make_variable_map(ctx);
    const std::string expr = path_or_expression.empty()
        ? default_diagram_styles_expression()
        : path_or_expression;
    return media::path::normalize_local_path(
        media::commands::resolve_variables(expr, vars, nullptr), true);
}

bool load_diagram_styles(const std::string& path_or_expression,
                         const std::string& cwd,
                         DiagramStyleConstants& out,
                         std::string& error,
                         const std::string& theme)
{
    out = DiagramStyleConstants{};
    const std::string path = resolve_diagram_styles_path(path_or_expression, cwd);
    if (path.empty()) {
        error = "diagram styles: empty path after variable resolution";
        return false;
    }

    std::ifstream in(media::path::path_from_utf8(path), std::ios::binary);
    if (!in) {
        error = "diagram styles: cannot read " + path;
        return false;
    }

    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        error = std::string("diagram styles: invalid JSON in ") + path + ": " + e.what();
        return false;
    }

    if (!j.is_object())
        return true;

    out.version = static_cast<int>(json_number(j, "version", out.version));
    apply_layout(j.value("layout", nlohmann::json::object()), out.layout);
    apply_render(j.value("render", nlohmann::json::object()), out.render);

    std::string theme_name = theme.empty() ? json_string(j, "defaultTheme", "light") : theme;
    if (theme_name.empty())
        theme_name = "light";

    const nlohmann::json themes = j.value("themes", nlohmann::json::object());
    if (themes.is_object() && themes.contains(theme_name)) {
        apply_palette(themes[theme_name], out);
        return true;
    }

    if (themes.is_object() && !themes.empty()) {
        error = "diagram styles: unknown theme '" + theme_name + "' in " + path;
        return false;
    }

    // Backward compatibility: palette at document root (pre-themes JSON).
    apply_palette(j, out);
    return true;
}

} // namespace media::xblox
