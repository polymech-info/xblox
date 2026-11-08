// XBlox LLM block  -  run an agent turn in-process (tool loop included).
//
// This is the native replacement for the old indirection where a flow reached
// the agent by spawning `pm-image-cli llm agent --prompt ...` through a `command`
// block. The agent loop is already library code (media::llm::agent::run_turn),
// and the launch defaults (provider/model/preset/timeout/max-iterations, preset
// tool & skill policy) live in agent_launch.cpp  -  so this block resolves and
// runs the same path the CLI and the Chat UI use, with **no duplicated defaults**.
//
//   • inputs    -  io.str/i64/flag over the resolved frame. `prompt` sources
//                PREVIOUS when unset; `cwd` defaults to the run's working dir.
//   • provider  -  launch::resolve_agent_launch() loads settings.json (default
//                chat preset + credentials); block params only override.
//   • primary   -  io.result(final_text) -> PREVIOUS / storeAs (the answer).
//   • outputs   -  io.set("usage", json, ...) (token/cost aggregate), plus model /
//                iterations as event metadata.
//   • stream    -  EventCallback maps tool-call / text-delta events to the trace
//                log; returning false (on io.cancelled()) aborts the turn.
//   • failure   -  io.error(...) + BlockResult::Error (policy decides flow);
//                cancellation returns BlockResult::Fatal (aborts the run).
//   • consent   -  native Win32 permission dialog by default (consentUi=win32),
//                including headless `xblox run`. Same surface as CLI --consent-ui.

#include "blocks/llm_blocks.hpp"

#if defined(FEATURE_MS_STORE_ENTITLEMENTS) && FEATURE_MS_STORE_ENTITLEMENTS
#include "licensing/features.hpp"
#endif

#include "blocks/block_variables.hpp"
#include "xblox_commands.hpp"
#include "llm/agent.hpp"
#include "llm/chat_host_events.hpp"
#include "llm/agent_launch.hpp"
#include "llm/agent_memory.hpp"
#include "llm/agent_runner.hpp"
#include "llm/agent_runner_session.hpp"
#include "llm/agent_turn_options.hpp"
#include "fs/fs_guard.hpp"
#include "fs/glob_filter.hpp"
#include "llm/llm_image_tool_defaults.hpp"
#include "win/overlay_hud.hpp"
#include "win/taskbar_status.hpp"
#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
#include "llm/security_review.hpp"
#include "security/consent_surface.hpp"
#include "security/ui/win/consent_ai_review.hpp"
#endif

#include <atomic>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace media::xblox::blocks {
namespace {

namespace agent = media::llm::agent;
namespace fs = std::filesystem;

static constexpr std::uintmax_t k_agent_embed_max_bytes = 1024 * 1024;

std::string trim(std::string s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> string_list_param(BlockIO& io, const char* name)
{
    std::vector<std::string> out;
    auto add = [&](std::string value) {
        value = trim(bv::resolve(value, io.runtime(), false, bv::kMaxDeepPasses));
        if (!value.empty())
            out.push_back(std::move(value));
    };

    if (const Value* v = io.value(name)) {
        const nlohmann::json j = v->to_json();
        if (j.is_array()) {
            for (const auto& item : j)
                if (item.is_string())
                    add(item.get<std::string>());
            return out;
        }
        if (j.is_string())
            add(j.get<std::string>());
        return out;
    }

    const nlohmann::json& raw = io.json(name);
    if (raw.is_array()) {
        for (const auto& item : raw)
            if (item.is_string())
                add(item.get<std::string>());
    } else if (raw.is_string()) {
        add(raw.get<std::string>());
    }
    return out;
}

bool read_embed_file(const fs::path& path, std::string& text, std::string& err)
{
    err.clear();
    text.clear();

    std::error_code ec;
    fs::path abs = fs::absolute(path, ec);
    if (ec) {
        err = "could not resolve path: " + media::path::path_to_utf8(path) + ": " + ec.message();
        return false;
    }
    fs::path resolved = fs::weakly_canonical(abs, ec);
    if (!ec && !resolved.empty())
        abs = std::move(resolved);

    if (!fs::is_regular_file(abs, ec)) {
        err = "path is not a regular file: " + media::path::path_to_utf8(abs);
        return false;
    }
    if (const std::string deny = media::llm::llm_fs_guard_deny_reason(abs); !deny.empty()) {
        err = deny;
        return false;
    }

    std::string ext = media::path::path_to_utf8(abs.extension());
    ext = media::strutil::lower_ascii(std::move(ext));
    if (media::llm::glob_file_extension_blocked_like_file_read(ext)) {
        err = "refusing binary or image extension";
        return false;
    }

    const std::uintmax_t sz = fs::file_size(abs, ec);
    if (ec) {
        err = "could not read file size: " + ec.message();
        return false;
    }
    if (sz > k_agent_embed_max_bytes) {
        err = "file exceeds 1 MiB embed limit";
        return false;
    }

    std::ifstream in(abs, std::ios::binary);
    if (!in) {
        err = "could not open file for reading";
        return false;
    }
    std::string buf(static_cast<std::size_t>(sz), '\0');
    if (sz > 0) {
        in.read(buf.data(), static_cast<std::streamsize>(sz));
        if (!in || static_cast<std::uintmax_t>(in.gcount()) != sz) {
            err = "could not read full file";
            return false;
        }
    }
    if (buf.find('\0') != std::string::npos) {
        err = "refusing NUL-containing binary content";
        return false;
    }

    text = std::move(buf);
    return true;
}

bool build_embedded_prompt(const std::string& prompt,
                           const std::vector<std::string>& embed_paths,
                           std::string& out_prompt,
                           std::string& err)
{
    err.clear();
    out_prompt = prompt;
    if (embed_paths.empty())
        return true;

    std::ostringstream embedded;
    embedded << "\n\n## Embedded Content\n\n";
    for (const auto& raw_path : embed_paths) {
        const std::string value = trim(raw_path);
        if (value.empty()) {
            err = "embed path is empty";
            return false;
        }

        std::error_code ec;
        fs::path fp = media::path::path_from_utf8(value);
        if (!fp.is_absolute())
            fp = fs::absolute(fp, ec);
        if (ec) {
            err = "embed could not resolve path: " + value + ": " + ec.message();
            return false;
        }

        std::string text;
        std::string read_err;
        if (!read_embed_file(fp, text, read_err)) {
            err = "embed " + media::path::path_to_utf8(fp) + ": " + read_err;
            return false;
        }

        std::error_code canon_ec;
        fs::path canon = fs::weakly_canonical(fp, canon_ec);
        if (canon_ec || canon.empty())
            canon = fp;
        const std::string path = media::path::path_to_generic_utf8(canon);
        embedded << "#File Content Start : " << path << "\n";
        embedded << text;
        if (text.empty() || text.back() != '\n')
            embedded << "\n";
        embedded << "#File Content End : " << path << "\n\n";
    }

    out_prompt += embedded.str();
    return true;
}

bool resolve_include_paths(const std::vector<std::string>& raw_paths,
                           std::vector<std::string>& out,
                           std::string& err)
{
    out.clear();
    err.clear();
    for (const std::string& raw : raw_paths) {
        const std::string value = trim(raw);
        if (value.empty())
            continue;
        std::error_code ec;
        fs::path fp = media::path::path_from_utf8(value);
        if (!fp.is_absolute())
            fp = fs::absolute(fp, ec);
        if (ec || !fs::exists(fp)) {
            err = "include path does not exist: " + value;
            return false;
        }
        out.push_back(media::path::path_to_utf8(fp));
    }
    return true;
}

void fill_launch_spec(BlockIO& io, agent::launch::AgentLaunchSpec& spec)
{
    spec.preset   = io.str("preset");
    spec.router   = io.str("router");
    spec.model    = io.str("model");
    spec.api_key  = io.str("apiKey");
    spec.base_url = io.str("baseUrl");
    spec.api_mode = io.str("type");
    spec.streaming = io.str("streaming");
    if (spec.streaming.empty())
        spec.streaming = "auto";

    spec.ai_consent_review = io.flag("aiConsentReview", false);
    spec.ai_consent_router = io.str("aiConsentRouter");
    spec.ai_consent_model  = io.str("aiConsentModel");
    if (!spec.ai_consent_router.empty() || !spec.ai_consent_model.empty())
        spec.ai_consent_review = true;

    spec.no_tools = !io.flag("tools", true);
    if (const long long n = io.i64("maxSteps", 0); n > 0)
        spec.max_iterations = static_cast<int>(n);
    if (const long long t = io.i64("timeoutMs", 0); t > 0)
        spec.timeout_ms = static_cast<int>(t);
    if (const long long b = io.i64("plannerBudget", 0); b > 0)
        spec.planner_schema_budget = static_cast<int>(b);

    spec.ocr_provider         = io.str("ocrProvider");
    spec.ocr_model            = io.str("ocrModel");
    spec.recognition_provider = io.str("recognitionProvider");
    spec.recognition_model    = io.str("recognitionModel");
    spec.image_provider       = io.str("imageProvider");
    spec.image_model          = io.str("imageModel");
    spec.video_provider       = io.str("videoProvider");
    spec.video_model          = io.str("videoModel");
    spec.stt_provider         = io.str("sttProvider");
    spec.stt_model            = io.str("sttModel");
    spec.tts_provider         = io.str("ttsProvider");
    spec.tts_model            = io.str("ttsModel");
    spec.tts_voice_id         = io.str("ttsVoiceId");
    spec.runner               = io.str("runner");
    if (spec.runner.empty()) {
        if (const char* env = std::getenv("PM_LLM_AGENT_RUNNER")) {
            if (*env)
                spec.runner = env;
        }
    }
}

std::string resolve_output_path(std::string path, const std::string& base_cwd)
{
    path = trim(std::move(path));
    if (path.empty())
        return {};
    std::error_code ec;
    fs::path dp = media::path::path_from_utf8(path);
    if (!dp.is_absolute()) {
        const fs::path base = base_cwd.empty() ? fs::current_path(ec) : media::path::path_from_utf8(base_cwd);
        dp = (base / dp).lexically_normal();
    }
    return media::path::path_to_utf8(dp);
}

bool write_text_file(const fs::path& path, const std::string& content, std::string& err)
{
    err.clear();
    std::error_code ec;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "could not write to " + media::path::path_to_utf8(path);
        return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) {
        err = "write failed: " + media::path::path_to_utf8(path);
        return false;
    }
    return true;
}

std::vector<ParamDef> llm_agent_params()
{
    const nlohmann::json when_ai_review{{"visible_when", {{"aiConsentReview", true}}}};
    const nlohmann::json empty_settings{{"empty_label", "(App Settings)"}};

    auto tool_provider = [&](const char* name, const char* options_path, const char* desc) {
        nlohmann::json ui = empty_settings;
        ui["control"] = "select";
        ui["options_path"] = options_path;
        return pd_str(name).adv().grp("tool_overrides").dflt("").ui(std::move(ui)).desc(desc);
    };
    auto tool_model = [&](const char* name, const char* models_prefix,
                          const char* provider_field, const char* desc) {
        return pd_str(name).adv().grp("tool_overrides").dflt("")
            .dyn({{"kind", "options_list"},
                  {"options_path_template", std::string(models_prefix) + ".{" + provider_field + "}"},
                  {"provider_field", provider_field}})
            .desc(desc);
    };

    std::vector<ParamDef> params = {
        pd_prompt("prompt").in().req().prev().deep()
            .desc("Instruction / question for the agent. Uses PREVIOUS when unset."),
        pd_prompt("systemPrompt").in().adv().dflt("").deep().rows(4)
            .desc("Full system-prompt override (CLI --system-prompt). Supports literal text, "
                  "null/none, @path, or file:path."),
        pd_str("system").in().adv().dflt("").deep()
            .desc("Optional extra system text appended after the resolved system prompt."),
        pd_prompt("plannerPrompt").in().adv().dflt("").deep().rows(3)
            .desc("Planner system-prompt override (CLI --planner-prompt). Same grammar as systemPrompt."),
        pd_args("include").in().dflt(nlohmann::json::array()).deep()
            .ui({{"picks_path", true}})
            .desc("Files or folders in the agent selection context (CLI --include)."),
        pd_args("embed").in().dflt(nlohmann::json::array()).deep()
            .ui({{"picks_path", true}})
            .desc("Text files inlined into the prompt (CLI --embed)."),
        pd_str("preset").opt().lbl("Preset").dflt("")
            .ui({{"control", "select"}, {"options_path", "agent.presets"}, {"empty_label", "(App Settings)"}})
            .desc("Chat provider preset name. Empty = default preset from App Settings."),
        pd_enum("runner", {"", "native", "claude-code", "codex" /*, "cursor" */}).adv().dflt("")
            .ui({{"options_path", "agent.runners"}, {"empty_label", "(preset / native)"}})
            .desc("Agent backend (CLI --runner). Empty = chat preset, then native. "
                  "Also reads PM_LLM_AGENT_RUNNER when unset."),
        pd_model().lbl("Model")
            .ui({{"empty_label", "(App Settings)"}})
            .dyn({{"kind", "options_list"},
                  {"options_path_template", "agent.models.{preset}.{router}"},
                  {"provider_field", "router"}}),
        pd_str("router").adv().lbl("Router").dflt("")
            .ui({{"control", "select"}, {"options_path", "agent.routers"}, {"empty_label", "(preset / settings)"}})
            .desc("Provider router override. Empty = from App Settings."),
        pd_enum("type", {"completion", "responses", "realtime"}).adv().dflt("completion")
            .desc("LLM API type."),
        pd_enum("streaming", {"auto", "on", "off"}).adv().dflt("auto")
            .desc("Streaming mode for Responses API turns."),
        pd_api_key().grp("auth"),
        pd_str("baseUrl").adv().grp("auth").dflt("")
            .desc("Router base URL override. Empty = from App Settings."),
        pd_bool("tools").opt().dflt(true)
            .desc("Allow tool calls. Off = plain completion (CLI --no-tools)."),
        pd_bool("mcp").adv().dflt(true)
            .desc("Allow MCP-backed tools. Off forces MCP off for this turn."),
        pd_bool("skills").adv().dflt(true)
            .desc("Allow agent skill discovery/injection."),
        pd_bool("planner").adv().dflt(true)
            .desc("Allow the planner pre-pass. Off forces planner off."),
        pd_int("plannerBudget").adv().dflt(0).range(0, 64)
            .desc("Max tool schemas the planner may pre-expand. 0 = default (8)."),
        pd_args("disableTools").adv().dflt(nlohmann::json::array())
            .ui({{"item_kind", "tool_name"}, {"options_path", "agent.path_tools"}})
            .desc("Path/MCP tool ids to omit for this turn (CLI --disable-tools)."),
        pd_args("enableTools").adv().dflt(nlohmann::json::array())
            .ui({{"item_kind", "tool_name"}, {"options_path", "agent.path_tools"}})
            .desc("Allowlist of path tools for this turn (CLI --enable-tools)."),
        pd_bool("parallelTools").adv().dflt(true)
            .desc("Dispatch multiple tool calls concurrently. Off = serial execution."),
        pd_int("maxSteps").adv().dflt(0).range(0, 100)
            .desc("Tool-loop iteration cap. 0 = resolved default."),
        pd_ms("timeoutMs").adv().dflt(0).range(0, 3600000)
            .desc("Per-LLM-call timeout in ms. 0 = resolved default."),
        pd_enum("consentUi", {
                "win32", "auto", "preset", "cli", "auto-deny", "auto-allow"
            }).opt().grp("security").dflt("win32")
            .ui({{"options_path", "agent.consent_ui"}})
            .desc("Consent surface for security-gated tools (CLI --consent-ui). "
                  "win32 = native permission dialog (default, including headless runs). "
                  "auto = cli when stdin is attached, else win32 on a desktop, else deny. "
                  "preset = chat preset consent_ui (Yolo / auto-deny). "
                  "cli = stdio prompt; auto-deny / auto-allow skip the dialog."),
        pd_str("consentOwner").adv().grp("security").dflt("")
            .desc("Target descriptor (ipc id) for consent-ui owner routing. Reserved."),
        pd_bool("aiConsentReview").adv().grp("security").dflt(false)
            .desc("Run the advisory SecurityAgent reviewer before consent prompts."),
        pd_str("aiConsentRouter").adv().grp("security").dflt("").ui(when_ai_review)
            .desc("Router for the advisory SecurityAgent reviewer."),
        pd_str("aiConsentModel").adv().grp("security").dflt("").ui(when_ai_review)
            .desc("Model for the advisory SecurityAgent reviewer."),
        pd_str("sessionId").adv().dflt("")
            .desc("Optional session id for multi-turn memory across runs (CLI --session-id)."),
        tool_provider("ocrProvider", "providers.ocr",
                      "OCR tool provider override for this turn."),
        tool_model("ocrModel", "providers.ocr.models", "ocrProvider",
                   "OCR tool model override for this turn."),
        tool_provider("recognitionProvider", "providers.vision",
                      "image_understand provider override."),
        tool_model("recognitionModel", "providers.vision.models", "recognitionProvider",
                   "image_understand model override."),
        tool_provider("imageProvider", "providers.image",
                      "image_create provider override."),
        tool_model("imageModel", "providers.image.models", "imageProvider",
                   "image_create model override."),
        tool_provider("videoProvider", "providers.image",
                      "create_video provider override."),
        tool_model("videoModel", "providers.image.models", "videoProvider",
                   "create_video model override."),
        tool_provider("sttProvider", "providers.stt",
                      "STT provider override (audio_transcribe / mic flows)."),
        tool_model("sttModel", "providers.stt.models", "sttProvider",
                   "STT model override."),
        tool_provider("ttsProvider", "providers.tts",
                      "TTS provider override."),
        tool_model("ttsModel", "providers.tts.models", "ttsProvider",
                   "TTS model override."),
        pd_str("ttsVoiceId").adv().grp("tool_overrides").dflt("")
            .desc("TTS voice id override."),
        pd_dir("cwd").in().dflt("").vars()
            .desc("Working folder the agent treats as context. Empty = the run cwd."),
        pd_enum("hudMode", {"off", "taskbar", "overlay", "both"}).grp("hud").dflt("both")
            .desc("Status HUD. taskbar = app taskbar button. overlay = movable pill over the clock. both = combine. off = none."),
        pd_bool("hud").grp("hud").dflt(true)
            .desc("Master switch for hudMode. Default: on."),
        pd_bool("streamLog").adv().dflt(false)
            .desc("Mirror streaming assistant text deltas to the trace log."),
        pd_bool("json").adv().dflt(false)
            .desc("Also emit the full agent transcript as a `transcript` output."),
        pd_outpath("dst").grp("output").lbl("Output file").dflt("")
            .desc("Write the agent response to this file (CLI --dst). "
                  "Without json: raw answer text. With json: full result JSON. "
                  "Parent directories are created automatically."),
        pd_json("usage").out().desc("Aggregated token usage / cost for the turn."),
        pd_json("transcript").out()
            .desc("Full turn transcript. Populated when `json` is on."),
        pd_store_as().dflt("answer"),
    };

#if defined(FEATURE_LLM_COMPRESSION) && FEATURE_LLM_COMPRESSION
    params.insert(params.end() - 3, {
        pd_enum("contextReduction", {"off", "tool-results", "all"}).adv().dflt("off")
            .desc("Pre-API context reduction mode (CLI --context-reduction)."),
        pd_float("contextReductionRatio").adv().dflt(0.5).range(0.0, 1.0)
            .desc("Target output/input byte ratio for reduced messages."),
        pd_int("contextReductionMinBytes").adv().dflt(2048).range(0, 1'000'000)
            .desc("Minimum message size before reduction applies."),
        pd_str("contextReductionObjective").adv().dflt("")
            .desc("Relevance objective for text reduction. Empty = user prompt."),
    });
#endif

    return params;
}

BlockResult llm_agent_block(BlockIO& io)
{
    const std::string event_kind = io.raw().is_object() ? io.raw().value("kind", std::string("llm")) : std::string("llm");
    io.event(event_kind, "running", "llm agent turn starting");

    std::string prompt = io.str("prompt");
    if (prompt.empty()) {
        io.error("llm", "llm agent block is missing a prompt", 2);
        return BlockResult::Error;
    }
    {
        std::string embed_err;
        std::string embedded_prompt;
        if (!build_embedded_prompt(prompt, string_list_param(io, "embed"), embedded_prompt, embed_err)) {
            io.error("llm", "llm agent block: " + embed_err, 2);
            return BlockResult::Error;
        }
        prompt = std::move(embedded_prompt);
    }

    agent::launch::AgentLaunchSpec spec;
    fill_launch_spec(io, spec);

    agent::launch::AgentLaunchResolved resolved;
    std::string err;
    if (!agent::launch::resolve_agent_launch(spec, resolved, err)) {
        const std::string msg = err.empty() ? "failed to resolve llm provider settings" : err;
        io.log("llm", msg);
        io.error("llm", msg, 2);
        std::cout << msg << "\n";
        return BlockResult::Error;
    }
    if (resolved.provider.model.empty()) {
        io.error("llm", "no llm model configured  -  set a model/preset in App Settings "
                        "or the block's model param", 2);
        return BlockResult::Error;
    }

    media::llm::ChatSettingsOverrideGuard tool_settings_guard(resolved.chat_settings);

    agent::Turn turn;
    turn.user_prompt = prompt;
    if (const std::string sys = io.str("system"); !sys.empty())
        turn.system_extra = sys;

    std::string cwd = io.str("cwd");
    if (cwd.empty())
        cwd = media::path::path_to_utf8(io.runtime().options.default_cwd);
    if (!cwd.empty())
        turn.folder_hint = cwd;

    {
        std::string include_err;
        if (!resolve_include_paths(string_list_param(io, "include"), turn.selection, include_err)) {
            io.error("llm", "llm agent block: " + include_err, 2);
            return BlockResult::Error;
        }
    }

    {
        std::string sys_err;
        if (!agent::options::apply_system_prompt_option(io.str("systemPrompt"), turn, sys_err)) {
            io.error("llm", "llm agent block: systemPrompt: " + sys_err, 2);
            return BlockResult::Error;
        }
        std::string planner_err;
        if (!agent::options::apply_planner_prompt_option(io.str("plannerPrompt"), turn, planner_err)) {
            io.error("llm", "llm agent block: plannerPrompt: " + planner_err, 2);
            return BlockResult::Error;
        }
    }

    agent::launch::apply_launch_policy_to_turn(spec, resolved, turn);

    if (const std::string session_id = io.str("sessionId"); !session_id.empty())
        turn.source_log_session_id = media::llm::agent::session_get_or_create(session_id);

    if (!io.flag("mcp", true)) {
        turn.mcp_tools_enabled = false;
        turn.tool_policy_override_set = true;
    }
    if (!io.flag("skills", true)) {
        turn.skills_enabled = false;
        turn.skill_policy_override_set = true;
    }
    if (!io.flag("planner", true)) {
        turn.planner_enabled = false;
        turn.planner_strategy = agent::PlannerStrategy::Disabled;
    }

    const bool no_tools = spec.no_tools;
    {
        const std::vector<std::string> disable_tools = string_list_param(io, "disableTools");
        if (!disable_tools.empty()) {
            std::vector<std::string> disabled_path;
            std::vector<std::string> disabled_mcp;
            agent::options::apply_disable_tools_list(disable_tools,
                                                     no_tools,
                                                     disabled_path,
                                                     &disabled_mcp);
            turn.disabled_path_tools = std::move(disabled_path);
            turn.disabled_mcp_tools  = std::move(disabled_mcp);
            turn.tool_policy_override_set = true;
        }
    }
    {
        const std::vector<std::string> enable_tools = string_list_param(io, "enableTools");
        if (!enable_tools.empty()) {
            agent::options::apply_enable_tools_list(enable_tools,
                                                    no_tools,
                                                    turn.allowed_path_tools,
                                                    turn.planner_path_tool_allowlist_set);
        }
    }
    turn.parallel_tool_dispatch = io.flag("parallelTools", true);

#if defined(FEATURE_LLM_COMPRESSION) && FEATURE_LLM_COMPRESSION
    if (const std::string mode_raw = io.str("contextReduction"); !mode_raw.empty()) {
        const auto mode = agent::options::parse_context_reduction_mode(mode_raw);
        turn.context_reduction_mode = mode;
        if (mode != pm::llm::ContextReductionMode::Off) {
            turn.context_reduction_ratio = io.f64("contextReductionRatio", 0.5);
            turn.context_reduction_min_bytes =
                static_cast<std::size_t>(std::max(0LL, io.i64("contextReductionMinBytes", 2048)));
            turn.context_reduction_objective = io.str("contextReductionObjective");
        }
    }
#endif

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    turn.cancel_signal = cancel;
    turn.cancel_check  = [&io, cancel]() -> bool {
        return io.cancelled() || cancel->load(std::memory_order_relaxed);
    };

    const std::string hud_mode = io.str("hudMode");
    const bool want_hud = io.flag("hud", true) && hud_mode != "off";
    const bool use_taskbar = want_hud
        && (hud_mode.empty() || hud_mode == "taskbar" || hud_mode == "both");
    const bool use_overlay = want_hud
        && (hud_mode.empty() || hud_mode == "overlay" || hud_mode == "both");
    std::unique_ptr<media::win::TaskbarJob> taskbar;
    std::unique_ptr<media::win::OverlayJob> overlay;
    auto hud_stop = [cancel]() { cancel->store(true, std::memory_order_relaxed); };
    if (use_taskbar) {
        taskbar = std::make_unique<media::win::TaskbarJob>("Agent working");
        taskbar->set_on_stop(hud_stop);
        taskbar->busy(resolved.provider.model);
    }
    if (use_overlay) {
        overlay = std::make_unique<media::win::OverlayJob>("Tanit AI");
        overlay->set_on_stop(hud_stop);
        overlay->busy(resolved.provider.model.empty() ? "working" : resolved.provider.model);
    }
    const int max_iter = resolved.provider.max_iterations;

    const bool stream_log = io.flag("streamLog", false);

#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
    // Headless xblox / CLI runs have no Chat WebView delegate. Default to the
    // native Win32 permission dialog so Ask-gated tools still prompt a human.
    pm::security::ConsentSurfaceScope::ReviewFn consent_review;
    if (resolved.provider.ai_consent_review) {
        const auto review_provider = resolved.provider;
        consent_review = [review_provider](const pm::security::ConsentRequest& cr)
            -> pm::security::ui::AiConsentReview {
            try {
                auto res = media::llm::agent::security_review::run_review(cr, review_provider);
                return pm::security::ui::ai_review_from_json(res.review, res.ok, res.error);
            } catch (const std::exception& e) {
                return pm::security::ui::ai_review_from_json(
                    nlohmann::json::object(), false, e.what());
            }
        };
    }
    const std::string consent_ui = pm::security::resolve_consent_ui_mode(
        io.str("consentUi"),
        resolved.has_preset ? resolved.selected_preset.consent_ui : std::string{},
        "win32");
    const pm::security::ConsentSurfaceScope consent_surface(
        consent_ui, io.str("consentOwner"), std::move(consent_review));
#endif

    auto on_event = [&io, &cancel, stream_log, &taskbar, &overlay, max_iter](const agent::Event& e) -> bool {
        if (io.cancelled() || cancel->load(std::memory_order_relaxed)
            || (overlay && overlay->stop_requested())) {
            cancel->store(true, std::memory_order_relaxed);
            return false;
        }
        switch (e.kind) {
        case agent::Event::ToolCall: {
            io.debug("llm tool-call: " + e.tool_name);
            const std::string detail = e.tool_name.empty() ? "tool" : e.tool_name;
            if (taskbar)
                taskbar->busy(detail);
            if (overlay)
                overlay->busy(detail);
            break;
        }
        case agent::Event::ToolResult:
            io.trace("llm tool-result: " + e.tool_name);
            break;
        case agent::Event::LlmRound: {
            const int round = e.payload.is_object() ? e.payload.value("round", 0) : 0;
            const std::string model = e.payload.is_object() ? e.payload.value("model", std::string{}) : std::string{};
            const std::string detail = model.empty() ? "thinking" : model;
            if (max_iter > 0 && round > 0) {
                if (taskbar)
                    taskbar->progress(static_cast<uint64_t>(round), static_cast<uint64_t>(max_iter), detail);
                if (overlay)
                    overlay->progress(static_cast<uint64_t>(round), static_cast<uint64_t>(max_iter), detail);
            } else {
                if (taskbar)
                    taskbar->busy(detail);
                if (overlay)
                    overlay->busy(detail);
            }
            break;
        }
        case agent::Event::Error:
            io.debug("llm error: " + e.text);
            break;
        case agent::Event::TextDelta:
            if (stream_log && !e.text.empty()) io.trace(e.text);
            break;
        default: break;
        }
        return true;
    };

    agent::AgentRunnerOptions runner_opts;
    runner_opts.kind = agent::AgentRunnerKind::Native;
    runner_opts.timeout_ms = resolved.provider.timeout_ms;
    {
        std::string runner_name = resolved.effective_runner;
        if (runner_name.empty())
            runner_name = io.str("runner");
        if (!runner_name.empty() && runner_name != "native") {
            if (const auto kind = agent::parse_agent_runner_kind(runner_name)) {
                runner_opts.kind = *kind;
            } else {
                io.error("llm", "unknown runner '" + runner_name
                                + "' (use native | claude-code | codex)", 2);
                return BlockResult::Error;
            }
        }
    }

    const agent::Result result =
        agent::run_agent_turn(turn, resolved.provider, runner_opts, on_event);

    if (result.cancelled) {
        io.error("llm", "llm agent turn cancelled", 130)
          .set("iterations", ParamKind::integer_, Value{static_cast<long long>(result.iterations)});
        return BlockResult::Fatal;
    }
    if (!result.ok) {
        if (taskbar)
            taskbar->error(result.error.empty() ? "llm agent turn failed" : result.error);
        if (overlay)
            overlay->error(result.error.empty() ? "failed" : result.error);
        io.error("llm", result.error.empty() ? "llm agent turn failed" : result.error, 1)
          .set("iterations", ParamKind::integer_, Value{static_cast<long long>(result.iterations)});
        return BlockResult::Error;
    }
    if (taskbar)
        taskbar->completed();
    if (overlay)
        overlay->completed();

    std::string resolved_dst;
    std::string written_dst;
    if (const std::string dst = io.str("dst"); !dst.empty()) {
        resolved_dst = resolve_output_path(dst, cwd);
        if (!resolved_dst.empty()) {
            std::string dst_content;
            if (io.flag("json", false)) {
                nlohmann::json result_json{
                    {"ok",         true},
                    {"final_text", result.final_text},
                    {"iterations", result.iterations},
                    {"transcript", result.transcript},
                };
                if (result.llm_usage_aggregate.is_object())
                    result_json["llm_usage"] = result.llm_usage_aggregate;
                dst_content = result_json.dump(2) + "\n";
            } else {
                dst_content = result.final_text;
                if (dst_content.empty() || dst_content.back() != '\n')
                    dst_content += '\n';
            }
            std::string dst_err;
            if (write_text_file(media::path::path_from_utf8(resolved_dst), dst_content, dst_err))
                written_dst = resolved_dst;
            else
                io.debug("llm: --dst: " + dst_err);
        }
    }

    if (io.flag("json", false)) {
        if (!written_dst.empty()) {
            io.ok("llm", "llm agent turn complete")
              .set("model", resolved.provider.model)
              .set("iterations", ParamKind::integer_, Value{static_cast<long long>(result.iterations)})
              .set("usage", ParamKind::json_value, Value::from_json(result.llm_usage_aggregate))
              .set("transcript", ParamKind::json_value, Value::from_json(result.transcript))
              .output("dst", ParamKind::file_path, Value{written_dst})
              .result(Value{result.final_text}, ParamKind::string_);
        } else {
            io.ok("llm", "llm agent turn complete")
              .set("model", resolved.provider.model)
              .set("iterations", ParamKind::integer_, Value{static_cast<long long>(result.iterations)})
              .set("usage", ParamKind::json_value, Value::from_json(result.llm_usage_aggregate))
              .set("transcript", ParamKind::json_value, Value::from_json(result.transcript))
              .result(Value{result.final_text}, ParamKind::string_);
        }
    } else if (!written_dst.empty()) {
        io.ok("llm", "llm agent turn complete")
          .set("model", resolved.provider.model)
          .set("iterations", ParamKind::integer_, Value{static_cast<long long>(result.iterations)})
          .set("usage", ParamKind::json_value, Value::from_json(result.llm_usage_aggregate))
          .output("dst", ParamKind::file_path, Value{written_dst})
          .result(Value{result.final_text}, ParamKind::string_);
    } else {
        io.ok("llm", "llm agent turn complete")
          .set("model", resolved.provider.model)
          .set("iterations", ParamKind::integer_, Value{static_cast<long long>(result.iterations)})
          .set("usage", ParamKind::json_value, Value::from_json(result.llm_usage_aggregate))
          .result(Value{result.final_text}, ParamKind::string_);
    }
    return BlockResult::Ok;
}

fs::path resolve_reference_path(const std::string& raw, const ExecutionOptions& options)
{
    fs::path path = media::path::path_from_utf8(raw);
    if (path.is_relative() && !options.default_cwd.empty())
        path = options.default_cwd / path;
    return path.lexically_normal();
}

BlockResult reference_in_chat_block(BlockIO& io)
{
    const std::string id = io.str("id");
    const std::string raw_path = io.str("path");
    std::string raw_text = trim(io.str("text"));
    if (raw_text.empty())
        raw_text = trim(io.str("prompt"));
    const bool send = io.flag("send", false);

    std::string target_string;
    if (!raw_path.empty())
        target_string = media::path::path_to_utf8(resolve_reference_path(raw_path, io.runtime().options));

    if (target_string.empty() && raw_text.empty()) {
        io.error("referenceInChat", "referenceInChat: path or text is required", 2)
            .id(id).type("referenceInChat");
        return BlockResult::Error;
    }

    if (!io.runtime().options.app_command_handler) {
        bool pushed = false;
        if (!target_string.empty()) {
            pushed = media::llm::emit_chat_host(
                "appendContextPaths",
                nlohmann::json::array({nlohmann::json::array({target_string})})) || pushed;
        }
        if (!raw_text.empty()) {
            pushed = media::llm::emit_chat_host(
                "appendComposerText", nlohmann::json::array({raw_text})) || pushed;
        }
        if (send) {
            pushed = media::llm::emit_chat_host("send", nlohmann::json::array()) || pushed;
        }
        if (!pushed) {
            io.error("referenceInChat", "referenceInChat: requires a host UI", 1)
                .id(id).type("referenceInChat")
                .payload({{"ok", false}, {"path", target_string}, {"text", raw_text}});
            return BlockResult::Error;
        }
        const std::string result = !target_string.empty() ? target_string : raw_text;
        io.ok("referenceInChat", "referenced in chat")
            .id(id).type("referenceInChat")
            .payload({
                {"ok", true},
                {"path", target_string},
                {"text", raw_text},
                {"send", send},
            })
            .result(Value{result}, !target_string.empty() ? ParamKind::file_path : ParamKind::string_);
        return BlockResult::Ok;
    }

    CommandInvocation invocation;
    invocation.path = io.path();
    invocation.id = id;
    invocation.action = "app";
    invocation.command = {{"appCommand", "referenceinchat"}};
    if (!target_string.empty()) {
        invocation.command["path"] = target_string;
        invocation.command["args"] = nlohmann::json::array({target_string});
    }
    // Prompt / send need a JSON extra so they survive encode_invocation
    // (path-only stays `referenceinchat|<path>` like the viewer action).
    if (!raw_text.empty() || send) {
        nlohmann::json extra = nlohmann::json::object();
        if (!target_string.empty())
            extra["path"] = target_string;
        if (!raw_text.empty())
            extra["prompt"] = raw_text;
        if (send)
            extra["send"] = true;
        invocation.command["extra"] = extra.dump();
    }
    invocation.payload = invocation.command;

    std::string message;
    const int code = io.runtime().options.app_command_handler(invocation, &message);
    nlohmann::json payload = {
        {"ok", code == 0},
        {"path", target_string},
        {"text", raw_text},
        {"send", send},
        {"message", message},
        {"exitCode", code},
    };
    if (code == 0) {
        const std::string result = !target_string.empty() ? target_string : raw_text;
        io.ok("referenceInChat", message.empty() ? "referenced in chat" : message)
            .id(id).type("referenceInChat")
            .payload(payload)
            .result(Value{result}, !target_string.empty() ? ParamKind::file_path : ParamKind::string_);
        return BlockResult::Ok;
    }
    io.error("referenceInChat", message.empty() ? "reference in chat failed" : message, code)
        .id(id).type("referenceInChat")
        .payload(std::move(payload));
    return BlockResult::Error;
}

} // namespace

BlockHandler llm_block_handler(const std::string& kind)
{
    if (kind == "referenceInChat")
        return reference_in_chat_block;
#if defined(FEATURE_COMMAND_LLM) && FEATURE_COMMAND_LLM
    if (kind == "llmAgent" || kind == "llm" || kind == "agent")
        return llm_agent_block;
#endif
    return nullptr;
}

void register_llm_blocks(BlockRegistry& registry)
{
    registry["referenceInChat"] = bdb(
        "referenceInChat", reference_in_chat_block, "Reference in Chat", "AI",
        "Add a file or folder to chat context, and/or seed the composer with text "
        "(same as UI launch `--prompt` / `--send`).",
        {{"kind", "referenceInChat"}, {"path", ""}, {"text", ""}, {"send", false},
         {"storeAs", "referencedPath"}})
        .params({
            pd_file("path").in().prev().deep()
                .ui({{"control", "path_picker"}})
                .desc("File or folder to add to chat context. Uses PREVIOUS when unset."),
            pd_str("text").in().deep().dflt("")
                .desc("Composer seed text (same as `--prompt`). Alias: prompt."),
            pd_bool("send").opt().dflt(false)
                .desc("Auto-send the seeded text when the composer is ready (same as `--send`)."),
            pd_store_as().dflt("referencedPath"),
        })
        .primary_output(ParamKind::file_path, "Referenced path or seeded text")
        .exec({false, false, true})
        .color("#0d9488");

#if defined(FEATURE_COMMAND_LLM) && FEATURE_COMMAND_LLM
    const std::vector<ParamDef> llm_params = llm_agent_params();

    const ExecFlags llm_flags{
        /*supports_background*/ true,
        /*supports_cancel*/     true,
        /*is_blocking*/         true,
        /*is_container*/        false,
        /*singleton*/           false,
        /*min_duration_hint_ms*/ 1500,
        /*has_side_effects*/    true,
        /*iterable*/            false,
    };

    registry["llmAgent"] = bdb(
        "llmAgent", llm_agent_block, "LLM Agent", "AI",
        "Run an LLM agent turn in-process (tool loop included) and store the answer in PREVIOUS. "
        "hudMode=both (default) shows the taskbar button plus a movable overlay over the clock.",
        {{"kind", "llmAgent"}, {"prompt", ""}, {"include", nlohmann::json::array()},
         {"embed", nlohmann::json::array()}, {"tools", true}, {"consentUi", "win32"},
         {"hud", true}, {"hudMode", "both"},
         {"storeAs", "answer"}})
        .params(llm_params)
        .exec(llm_flags)
        .primary_output(ParamKind::string_, "Answer")
        .output("iterations", ParamKind::integer_, "Iterations")
        .output("usage", ParamKind::json_value, "Usage")
        .output("transcript", ParamKind::json_value, "Transcript", {}, true)
        .color("#7c5cff");

    registry["llm"] = bdb(
        "llm", llm_agent_block, "LLM", "AI",
        "Run an LLM agent turn in-process (tool loop included) and store the answer in PREVIOUS. "
        "hudMode=both (default) shows the taskbar button plus a movable overlay over the clock.",
        {{"kind", "llm"}, {"prompt", ""}, {"include", nlohmann::json::array()},
         {"embed", nlohmann::json::array()}, {"tools", true}, {"consentUi", "win32"},
         {"hud", true}, {"hudMode", "both"},
         {"storeAs", "answer"}})
        .params(llm_params)
        .exec(llm_flags)
        .primary_output(ParamKind::string_, "Answer")
        .output("iterations", ParamKind::integer_, "Iterations")
        .output("usage", ParamKind::json_value, "Usage")
        .output("transcript", ParamKind::json_value, "Transcript", {}, true)
        .color("#7c5cff")
        .hidden();

    registry["agent"] = bdb(
        "agent", llm_agent_block, "Agent", "AI",
        "Run an LLM agent turn in-process (tool loop included) and store the answer in PREVIOUS. "
        "hudMode=both (default) shows the taskbar button plus a movable overlay over the clock.",
        {{"kind", "agent"}, {"prompt", ""}, {"include", nlohmann::json::array()},
         {"embed", nlohmann::json::array()}, {"tools", true}, {"consentUi", "win32"},
         {"hud", true}, {"hudMode", "both"},
         {"storeAs", "answer"}})
        .params(llm_params)
        .exec(llm_flags)
        .primary_output(ParamKind::string_, "Answer")
        .output("iterations", ParamKind::integer_, "Iterations")
        .output("usage", ParamKind::json_value, "Usage")
        .output("transcript", ParamKind::json_value, "Transcript", {}, true)
        .color("#7c5cff")
        .hidden();
#endif
}

} // namespace media::xblox::blocks
