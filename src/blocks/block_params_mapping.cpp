// block_params_mapping  -  x-xblox.kind overlay for dynamic provider schemas.
//
// Pipe design: an ordered array of matcher stages, cheap first.
//   stage 0 (gate)   -  structural: skip enums / non-strings / pre-annotated
//   stage 1 (glob)   -  name-glob + format rules (covers conventional names)
//   stage 2 (embed)  -  optional semantic fallback via the strutil embed
//                     backend (EmbedInference, same lazy bootstrap pattern
//                     as agent EmbedInferenceFeature / InfoTool fallbacks)
//
// The annotation is UI-only: the property panel maps `x-xblox.kind` to the
// native widget (file picker, prompt textarea, color input). Runtime
// behavior never depends on stage 2, so execution stays deterministic.

#include "blocks/block_params_mapping.hpp"

#include "string/string_utils.hpp"

#if defined(FEATURE_COMMAND_LLAMA_INFERENCE) && FEATURE_COMMAND_LLAMA_INFERENCE
#include "lib/llm/llama/EmbedInference.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <string>

namespace media::xblox::blocks::ui {
namespace {

// Master switch for the whole overlay. Local on purpose  -  flip to false to
// ship raw schemas without kind hints.
constexpr bool MAP_MODEL_PARAMS = true;

// Minimum cosine similarity for the embed stage to accept a kind.
constexpr float k_embed_threshold = 0.55f;

using media::strutil::lower_ascii;

// Minimal '*' wildcard matcher (case-sensitive; callers pass lowercase).
bool glob_match(const char* pat, const char* str)
{
    const char* star = nullptr;
    const char* ss = nullptr;
    while (*str) {
        if (*pat == *str) { ++pat; ++str; }
        else if (*pat == '*') { star = pat++; ss = str; }
        else if (star) { pat = star + 1; str = ++ss; }
        else return false;
    }
    while (*pat == '*') ++pat;
    return *pat == '\0';
}

struct FieldInfo {
    std::string name_lower;   // property key, lowercased
    std::string format;       // JSON Schema "format" (e.g. "uri")
    std::string title;
    std::string description;
};

// ── stage 1: glob rules (cheap) ───────────────────────────────────────────────
// First match wins. requires_format == nullptr -> any format.

struct GlobRule {
    const char* name_glob;
    const char* requires_format; // nullptr = no format requirement
    const char* kind;
};

constexpr GlobRule k_glob_rules[] = {
    // format:uri family  -  pick the media kind from the name
    {"*image*",    "uri",   "image_path"},
    {"*mask*",     "uri",   "image_path"},
    {"*img*",      "uri",   "image_path"},
    {"*photo*",    "uri",   "image_path"},
    {"*audio*",    "uri",   "audio_path"},
    {"*voice*",    "uri",   "audio_path"},
    {"*speech*",   "uri",   "audio_path"},
    {"*video*",    "uri",   "video_path"},
    {"*",          "uri",   "file_path"},   // any other uri -> generic file
    // plain strings with strong name signals
    {"*prompt*",   nullptr, "prompt"},
    {"*negative*", nullptr, "prompt"},
    {"*color*",    nullptr, "color"},
};

std::string glob_stage(const FieldInfo& f)
{
    for (const auto& rule : k_glob_rules) {
        if (rule.requires_format && f.format != rule.requires_format)
            continue;
        if (glob_match(rule.name_glob, f.name_lower.c_str()))
            return rule.kind;
    }
    return {};
}

// ── stage 2: embedding inference (optional) ───────────────────────────────────
// Same bootstrap pattern as EmbedInferenceFeature: embed_inference_init() is
// call_once and a transparent no-op when no GGUF embed model is present.
// Repeated strings are cached inside EmbedInference, so re-resolves of the
// same schema cost nothing.

#if defined(FEATURE_COMMAND_LLAMA_INFERENCE) && FEATURE_COMMAND_LLAMA_INFERENCE

std::string embed_stage(const FieldInfo& f)
{
    media::llama::embed_inference_init();   // lazy; no-op without a model
    if (!media::strutil::has_embed_backend())
        return {};

    static constexpr struct { const char* prototype; const char* kind; } k_protos[] = {
        {"input image picture file upload",      "image_path"},
        {"audio sound recording voice file",     "audio_path"},
        {"video clip movie footage file",        "video_path"},
        {"text prompt instruction description",  "prompt"},
        {"color hex value rgb",                  "color"},
    };

    const std::string query = f.name_lower + " " + f.title + " " + f.description;
    float best = 0.f;
    const char* best_kind = nullptr;
    for (const auto& p : k_protos) {
        const float score = media::strutil::embed_match_score(query, p.prototype);
        if (score > best) { best = score; best_kind = p.kind; }
    }
    if (best >= k_embed_threshold && best_kind)
        return best_kind;
    return {};
}

#else

std::string embed_stage(const FieldInfo&) { return {}; }

#endif // FEATURE_COMMAND_LLAMA_INFERENCE

// ── pipe: cheap options first, inference last ─────────────────────────────────

using MatchStage = std::string (*)(const FieldInfo&);

constexpr MatchStage k_pipe[] = {
    glob_stage,
    embed_stage,
};

} // namespace

void annotate_schema_param_kinds(nlohmann::json& schema_props)
{
    if (!MAP_MODEL_PARAMS || !schema_props.is_object())
        return;

    for (auto& [key, prop] : schema_props.items()) {
        if (!prop.is_object())
            continue;
        // stage 0  -  structural gate
        if (prop.contains("enum"))
            continue;                                   // select widget already correct
        if (prop.contains("x-xblox") && prop["x-xblox"].contains("kind"))
            continue;                                   // respect explicit annotations

        const std::string type = prop.value("type", std::string{});

        // Arrays of strings (e.g. Replicate `images` / `videos` uri lists) are
        // matched on the item format and annotated with list=true so the UI
        // renders rows of paths (same widget as the native references param).
        bool is_list = false;
        std::string format;
        if (type == "string") {
            format = prop.value("format", std::string{});
        } else if (type == "array" && prop.contains("items") && prop["items"].is_object()
                   && prop["items"].value("type", std::string{}) == "string") {
            is_list = true;
            format = prop["items"].value("format", std::string{});
        } else {
            continue;                                   // numeric/boolean widgets already correct
        }

        const FieldInfo info{
            lower_ascii(key),
            format,
            prop.value("title", std::string{}),
            prop.value("description", std::string{}),
        };

        for (const MatchStage stage : k_pipe) {
            const std::string kind = stage(info);
            if (!kind.empty()) {
                prop["x-xblox"]["kind"] = kind;
                if (is_list)
                    prop["x-xblox"]["list"] = true;
                break;
            }
        }
    }
}

} // namespace media::xblox::blocks::ui
