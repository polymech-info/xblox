#pragma once

#include "xblox/xblox_diagram_styles.hpp"
#include "xblox/xblox_markdown.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace media::xblox {

/// Build a Tanit-style SVG flowchart from a blocks-file script tree.
/// @p direction uses the same tokens as mermaid (TD, LR, BT, RL).
/// @p standalone When true, prepend the XML declaration (for .svg files / rasterizers).
///               Markdown ```svg fences should pass false.
std::string render_flow_diagram_svg(const nlohmann::json& roots,
                                    const MarkdownOptions& options,
                                    const DiagramStyleConstants& styles,
                                    const std::string& direction,
                                    const std::string& title = {},
                                    bool standalone = false);

/// Write @p svg bytes to @p output_path (UTF-8, creates parent dirs best-effort).
bool save_diagram_svg_to_file(const std::string& svg,
                              const std::string& output_path,
                              std::string& error);

/// Rasterize SVG bytes to PNG via libvips svgload (requires librsvg in the vips bundle).
bool save_diagram_svg_as_png(const std::string& svg,
                             const std::string& output_path,
                             double dpi,
                             std::string& error);

/// Rasterize SVG to PNG bytes, base64-encoded (no data-URL prefix).
bool encode_diagram_svg_as_png_base64(const std::string& svg,
                                      double dpi,
                                      std::string& out_base64,
                                      std::string& error);

/// Tanit SVG for flow or sequence (@p options.diagram_type).
std::string render_document_diagram_svg(const nlohmann::json& blocks_file,
                                        const MarkdownOptions& options);

} // namespace media::xblox
