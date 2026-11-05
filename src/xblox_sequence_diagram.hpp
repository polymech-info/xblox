#pragma once

#include "xblox/xblox_diagram_styles.hpp"
#include "xblox/xblox_markdown.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace media::xblox {

enum class SequenceEventKind {
    Message,
    Return,
    Note,
    AltStart,
    AltElse,
    AltEnd,
    LoopStart,
    LoopEnd,
};

struct SequenceEvent {
    SequenceEventKind kind = SequenceEventKind::Message;
    std::string       from;
    std::string       to;
    std::string       text;
};

struct SequenceModel {
    std::vector<std::string> participants;
    std::vector<SequenceEvent> events;
    bool                     doc_loop = false;
    std::string              doc_loop_label;
};

SequenceModel build_sequence_model(const nlohmann::json& blocks_file, const MarkdownOptions& options);

std::string sequence_model_to_mermaid(const SequenceModel& model);

/// Tanit-style SVG sequence diagram (participants + lifelines + messages).
std::string render_sequence_diagram_svg(const nlohmann::json& blocks_file,
                                       const MarkdownOptions& options,
                                       const DiagramStyleConstants& styles,
                                       const std::string& title = {},
                                       bool standalone = false);

} // namespace media::xblox
