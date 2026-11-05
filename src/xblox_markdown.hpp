#pragma once

#include "xblox/xblox_diagram_styles.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace media::xblox {

// Rendering knobs for document_to_markdown. Kept here (not on the CLI) so the
// web UI / other hosts can reuse the generator with their own presentation.
struct MarkdownOptions {
    bool numbered = false;     // numeric bullets (1. / 2.) instead of dashes
    std::string title;         // optional H1, e.g. the source file name
    std::size_t max_text = 64; // free-text params are truncated to this length
    // Block kinds omitted from the script tree (the whole subtree is skipped).
    // stdout is noise in a structural outline, so it is filtered by default.
    std::vector<std::string> exclude{"stdout"};
    // Mermaid mode: render ## Script as a fenced flowchart instead of the
    // bullet tree (context variables stay as markdown bullets above it).
    bool mermaid = false;
    // Tanit-style SVG diagram type for --diagram (flow | sequence).
    std::string diagram_type = "flow";
    // Mermaid markdown diagram type (--md --mermaid only).
    std::string mermaid_type = "flow";
    std::string mermaid_direction = "TD"; // TD | LR | BT | RL (flow only)
    // Colouring: "edges" tints links by semantics (yes/no, loop, case),
    // "groups" tints node strokes by registry group (audio, input, ...) plus
    // flow diamonds, "both" combines them, "none" leaves the renderer theme.
    std::string mermaid_color = "both";
    // Include set block parameters in mermaid node labels (default: compact
    // labels  -  group/kind, `set <name>`, conditions on decision diamonds).
    // Legacy bool; prefer diagram_show_parameters ("none" | "set" | "all").
    bool expand_parameters = false;
    // Diagram parameter visibility (matches wiring toolbar: wired -> set, all -> all).
    std::string diagram_show_parameters = "none";
    bool        diagram_show_parameters_grouped = false;
    bool        diagram_show_parameter_values   = false;
    // Tanit-style SVG flowchart (alternative to mermaid). Requires flow type.
    bool diagram = false;
    // Styles JSON path or expression (default `${TANIT_SHARED}/xblox-diagram-constants.json`).
    std::string diagram_styles_path;
    // Populated by the caller after load_diagram_styles(); ignored when diagram is false.
    DiagramStyleConstants diagram_styles{};
    // Optional PNG raster path (libvips svgload). Resolved by the caller when set.
    std::string diagram_png;
    // Optional standalone .svg output path (raw SVG, not markdown).
    std::string diagram_dst;
    // Diagram content knobs (defaults also in xblox-diagram-constants.json "render").
    bool diagram_show_arrows         = true;
    bool diagram_show_links          = true;
    bool diagram_show_disabled       = false;
    bool diagram_show_variables      = true;
    bool diagram_show_groups              = true;
    bool diagram_humanize_node_names      = false;
    bool diagram_show_variable_delimiters = false;
    std::string diagram_variable_link_style  = "unicode";
    std::string diagram_variable_input_style = "at";
    std::string diagram_link_style           = "straight";
};

// Render a blocks-file document ({version, context?, roots}) as brief
// pseudo-code markdown:
//
//   # <title>                  (when options.title is set)
//   <description>              (when the blocks-file has a top-level description string)
//   ## Context                 (one bullet per context variable)
//   ## Script                  (bullet tree mirroring the block nesting)
//
// Pure formatting  -  nothing is resolved or executed.
std::string document_to_markdown(const nlohmann::json& blocks_file,
                                 const MarkdownOptions& options = {});

/// Load diagram styles and apply render knobs from @p options (web RPC / host UI).
/// Unset keys keep values from the styles JSON defaults.
bool apply_diagram_options_from_json(const nlohmann::json& options,
                                     const std::string& title,
                                     const std::string& cwd,
                                     MarkdownOptions& out,
                                     std::string& error);

} // namespace media::xblox
