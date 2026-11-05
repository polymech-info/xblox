#pragma once

#include <string>

namespace media::xblox {

struct DiagramCanvasStyle {
    std::string background = "#FFFFFF";
    double      padding    = 48.0;
};

struct DiagramTitleStyle {
    std::string font_family = "Arial, Helvetica, sans-serif";
    double      font_size   = 24.0;
    std::string fill        = "#333333";
};

struct DiagramNodeStyle {
    std::string fill              = "#FFFFFF";
    std::string stroke            = "#E0E0E0";
    double      stroke_width      = 1.0;
    double      corner_radius     = 16.0;
    double      min_width         = 112.0;
    double      height            = 88.0;
    std::string label_font_family = "Arial, Helvetica, sans-serif";
    double      label_font_size   = 14.0;
    std::string label_fill           = "#666666";
    std::string variable_fill          = "#4A90A4";
    std::string variable_operator_fill = "#BBBBBB";
    std::string group_fill             = "#777777";
    double      variable_link_margin   = 8.0;
    double      shadow_blur       = 8.0;
    double      shadow_offset_y   = 4.0;
    double      shadow_opacity    = 0.12;
    // Inner param cards (microcontroller-style ins/outs inside the node shell).
    std::string param_card_fill   = "#F4F4F7";
    std::string param_card_stroke = "#E4E4EA";
    double      param_card_radius = 4.0;
    double      param_font_size   = 11.0;
    double      title_height      = 34.0;
    double      param_line_height = 14.0;
    double      param_card_padding = 6.0;
    double      param_card_gap    = 4.0;
    double      param_card_inset  = 4.0;
};

struct DiagramDecisionStyle {
    std::string fill            = "#FFFFFF";
    std::string stroke          = "#E0E0E0";
    double      stroke_width    = 1.0;
    double      size            = 72.0;
    double      label_padding   = 14.0;
    double      label_font_size = 12.0;
    std::string label_fill      = "#666666";
};

struct DiagramEdgeStyle {
    std::string stroke            = "#E0E0E0";
    double      stroke_width      = 1.5;
    double      arrow_size        = 8.0;
    double      route_margin      = 12.0;
    std::string label_font_family = "Arial, Helvetica, sans-serif";
    double      label_font_size   = 12.0;
    std::string label_fill        = "#999999";
};

struct DiagramLayoutStyle {
    double node_spacing_x   = 40.0;
    double node_spacing_y   = 56.0;
    double layer_spacing    = 96.0;
    double link_gap         = 20.0;
    double branch_lane_width = 48.0;
};

struct DiagramRenderOptions {
    bool show_arrows          = true;
    bool show_links           = true;
    bool show_disabled        = false;
    bool show_variables       = true;
    bool show_groups                 = true;
    bool humanize_node_names         = false;
    bool show_variable_delimiters    = false;
    std::string variable_link_style  = "unicode"; // arrow | unicode | colon | dot | equals | none
    std::string variable_input_style = "at";      // parens | at | bracket | none
    std::string link_style           = "straight"; // straight | bezier | waypoints
    std::string show_parameters      = "none";     // none | set | all
    bool        show_parameters_grouped = false;
    bool        show_parameter_values   = false;
};

struct DiagramStyleConstants {
    int                   version = 1;
    DiagramCanvasStyle    canvas;
    DiagramTitleStyle     title;
    DiagramNodeStyle      node;
    DiagramDecisionStyle  decision;
    DiagramEdgeStyle      edge;
    std::string           accent = "#4A90A4";
    DiagramLayoutStyle    layout;
    DiagramRenderOptions  render;
};

/// Default styles file: `${TANIT_SHARED}/xblox-diagram-constants.json`.
std::string default_diagram_styles_expression();

/// Resolve @p path_or_expression with command_variables (@p cwd for CWD/TANIT_*).
std::string resolve_diagram_styles_path(const std::string& path_or_expression,
                                        const std::string& cwd);

/// Load JSON constants from disk. Empty @p path_or_expression uses the default expression.
/// @p theme selects the palette from JSON "themes" (default "light"; falls back to JSON
/// "defaultTheme" when @p theme is empty). Root-level canvas/title/node/... are still
/// accepted for backward compatibility when "themes" is absent.
bool load_diagram_styles(const std::string& path_or_expression,
                         const std::string& cwd,
                         DiagramStyleConstants& out,
                         std::string& error,
                         const std::string& theme = "light");

} // namespace media::xblox
