#pragma once
// block_params.hpp  -  the ParamDef parameter-definition surface.
//
// ParamDef is the rich, self-describing parameter schema (kind, group, default,
// sourcing, range/enum, preconditions) that replaces opaque JSON param objects.
// It composes ParamKind (block_types.hpp) and ParamConstraint (block_validation.hpp),
// and is consumed by the resolver (block_resolver.hpp) and registry
// (block_registry.hpp). The `pd_*` factories are the terse builders used at
// registration sites.
//
// Split out of the former monolithic builtin_blocks.hpp (see docs/xblox-next.md
// §10.49).

#include "blocks/block_types.hpp"
#include "blocks/block_validation.hpp"
#include "commands/parameter_def.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace media::xblox::blocks {

using FlagDef = media::commands::FlagDef;

// ─────────────────────────────────────────────────────────────────────────────
// ParamSource  -  where a param's value comes from when not set on the block
// ─────────────────────────────────────────────────────────────────────────────

enum class ParamSource : std::uint8_t {
    Literal,   // the block's own field (default)
    Previous,  // the PREVIOUS chained result (== uses_previous)
    Variable,  // a named context variable (source_ref)
    Expression // evaluate source_ref as an expression (future)
};

// Explicit document-v2 binding sources accepted by a parameter. This is
// semantic runtime metadata, independent from the presentation-only `group`.
enum class ParamBindingKind : std::uint32_t {
    None               = 0,
    BlockOutput        = 1u << 0,
    ScopeVariable      = 1u << 1,
    Previous           = 1u << 2,
    ParameterOperation = 1u << 3,
};

inline constexpr ParamBindingKind kAllParamBindings =
    static_cast<ParamBindingKind>((1u << 4) - 1u);

inline constexpr ParamBindingKind operator|(ParamBindingKind a, ParamBindingKind b)
{
    return static_cast<ParamBindingKind>(
        static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
inline ParamBindingKind& operator|=(ParamBindingKind& a, ParamBindingKind b)
{
    a = a | b;
    return a;
}
inline bool binding_has(ParamBindingKind set, ParamBindingKind value)
{
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(value)) != 0;
}
inline std::vector<std::string> param_binding_tokens(ParamBindingKind bindings)
{
    std::vector<std::string> out;
    if (binding_has(bindings, ParamBindingKind::BlockOutput))        out.emplace_back("blockOutput");
    if (binding_has(bindings, ParamBindingKind::ScopeVariable))      out.emplace_back("scopeVariable");
    if (binding_has(bindings, ParamBindingKind::Previous))           out.emplace_back("previous");
    if (binding_has(bindings, ParamBindingKind::ParameterOperation)) out.emplace_back("parameterOperation");
    return out;
}

enum class ParamBindingSyntax : std::uint8_t {
    Value,         // Binding resolves directly to the typed parameter value.
    VariableName,  // Field contains a scope-variable name.
    Interpolation, // Field is a string template containing one or more refs.
    Expression,    // Field is parsed by the expression engine.
};

inline const char* param_binding_syntax_str(ParamBindingSyntax syntax) noexcept
{
    switch (syntax) {
    case ParamBindingSyntax::Value:         return "value";
    case ParamBindingSyntax::VariableName:  return "variableName";
    case ParamBindingSyntax::Interpolation: return "interpolation";
    case ParamBindingSyntax::Expression:    return "expression";
    default:                                return "value";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ParamResolve  -  composable resolution features applied (in order) by the
// executor's resolver after sourcing + casting, before the handler.
//
// This is feature *composition* on top of a ParamKind, not a new kind: a
// `file_path`/`dir_path` interpolates `${var}` by default, and `.globs()`
// layers glob expansion on the same param. The resolver runs the declared
// features left-to-right (Variables -> Globs -> ...); a glob-expanded param yields
// a JSON array of matched paths in the frame. Extensible (Url, Expression, ...).
// ─────────────────────────────────────────────────────────────────────────────

enum class ParamResolve : std::uint32_t {
    None      = 0,
    Variables = 1u << 0,  // ${var} / ${ENV:..} / ${KNOWNFOLDER:..} interpolation
    Globs     = 1u << 1,  // expand *, ?, ** into a list of matching paths
    Custom    = 1u << 2,  // run app-level resolve hooks (policy/guard/redirect)
    Deep      = 1u << 3,  // re-expand ${..} tokens carried in variable *values*
                          // (max 5 passes, circular refs abort). Opt-in per param:
                          // expanded values become templates, so only declare it on
                          // params that consume document-authored templates (paths),
                          // never on free-text passthroughs of untrusted data.
};

inline ParamResolve  operator|(ParamResolve a, ParamResolve b) { return static_cast<ParamResolve>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b)); }
inline ParamResolve& operator|=(ParamResolve& a, ParamResolve b) { a = a | b; return a; }
inline bool          resolve_has(ParamResolve set, ParamResolve f) { return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(f)) != 0; }

// Feature tokens for the manifest surface. Returns a plain string list; JSON is
// only materialized at the ParamDef::to_json() boundary (nlohmann converts it).
inline std::vector<std::string> param_resolve_tokens(ParamResolve r) {
    std::vector<std::string> t;
    if (resolve_has(r, ParamResolve::Variables)) t.emplace_back("variables");
    if (resolve_has(r, ParamResolve::Globs))     t.emplace_back("globs");
    if (resolve_has(r, ParamResolve::Custom))    t.emplace_back("custom");
    if (resolve_has(r, ParamResolve::Deep))      t.emplace_back("deep");
    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
// ParamDef  -  rich parameter definition, replaces opaque JSON param objects
// ─────────────────────────────────────────────────────────────────────────────

struct ParamDef : media::commands::ParameterDef {
    ParamDef() = default;
    ParamDef(std::string n, std::string l = {}, std::string d = {},
             ParamKind k = ParamKind::string_)
    {
        name = std::move(n);
        label = std::move(l);
        description = std::move(d);
        kind = k;
    }

    bool uses_previous = false;
    // Input sourcing: where the resolver pulls the value from when the block does
    // not set the field. `uses_previous` is the sugar for `Previous`.
    ParamSource source = ParamSource::Literal;
    std::string source_ref;   // Variable name / Expression text
    // Declared preconditions, validated by the executor's resolver (not the handler).
    ParamConstraint constraints = ParamConstraint::None;
    // Composable resolution features applied by the resolver (on top of the
    // kind's default  -  interpolatable kinds get Variables implicitly).
    ParamResolve resolve = ParamResolve::None;
    // Document-v2 wiring contract. Runtime value parameters are bindable by
    // default; output-name/configuration fields explicitly opt out.
    ParamBindingKind accepted_bindings = kAllParamBindings;
    ParamBindingSyntax binding_syntax = ParamBindingSyntax::Value;

    // ── Fluent builder ────────────────────────────────────────────────────────
    ParamDef& req()    { required = true;  return *this; }
    ParamDef& prev()   {
        uses_previous = true;
        source = ParamSource::Previous;
        accepted_bindings |= ParamBindingKind::Previous | ParamBindingKind::BlockOutput;
        return *this;
    }
    ParamDef& from_var(std::string v)  {
        source = ParamSource::Variable;
        source_ref = std::move(v);
        accepted_bindings |= ParamBindingKind::ScopeVariable | ParamBindingKind::BlockOutput;
        return *this;
    }
    ParamDef& from_expr(std::string e) { source = ParamSource::Expression; source_ref = std::move(e); return *this; }
    ParamDef& hide()   { hidden = true;    return *this; }
    ParamDef& in()     { group = "input"; return *this; }
    ParamDef& out()    {
        group = "output";
        accepted_bindings = ParamBindingKind::None;
        return *this;
    }
    ParamDef& adv()    { group = "advanced"; return *this; }
    ParamDef& opt()    { group = "options";  return *this; }
    ParamDef& grp(std::string g) { group = std::move(g); return *this; }
    ParamDef& lbl(std::string l)    { label = std::move(l); return *this; }
    ParamDef& desc(std::string d)   { description = std::move(d); return *this; }
    ParamDef& dflt(nlohmann::json v){ default_value = std::move(v); return *this; }
    ParamDef& dyn(nlohmann::json v) { dynamic_schema = std::move(v); return *this; }
    ParamDef& ui(nlohmann::json v)  { ui_meta = std::move(v); return *this; }
    ParamDef& control(std::string c) {
        if (!ui_meta.is_object())
            ui_meta = nlohmann::json::object();
        ui_meta["control"] = std::move(c);
        return *this;
    }
    ParamDef& wide(bool v = true) {
        if (!ui_meta.is_object())
            ui_meta = nlohmann::json::object();
        ui_meta["wide"] = v;
        return *this;
    }
    ParamDef& rows(int v) {
        if (!ui_meta.is_object())
            ui_meta = nlohmann::json::object();
        ui_meta["rows"] = v;
        return *this;
    }
    ParamDef& placeholder(std::string v) {
        if (!ui_meta.is_object())
            ui_meta = nlohmann::json::object();
        ui_meta["placeholder"] = std::move(v);
        return *this;
    }
    ParamDef& visible_when(nlohmann::json cond) {
        if (!ui_meta.is_object())
            ui_meta = nlohmann::json::object();
        ui_meta["visible_when"] = std::move(cond);
        return *this;
    }
    ParamDef& bindings(ParamBindingKind kinds,
                       ParamBindingSyntax syntax = ParamBindingSyntax::Value) {
        accepted_bindings = kinds;
        binding_syntax = syntax;
        return *this;
    }
    ParamDef& bindable(ParamBindingSyntax syntax = ParamBindingSyntax::Value) {
        accepted_bindings = kAllParamBindings;
        binding_syntax = syntax;
        return *this;
    }
    ParamDef& syntax(ParamBindingSyntax value) {
        binding_syntax = value;
        return *this;
    }
    ParamDef& literal_only() {
        accepted_bindings = ParamBindingKind::None;
        return *this;
    }
    ParamDef& accepts_binding(ParamBindingKind kind) {
        accepted_bindings |= kind;
        return *this;
    }
    ParamDef& enums(std::vector<std::string> vs) { enum_values = std::move(vs); return *this; }
    ParamDef& flag(long long value, std::string label, std::string description = {}) {
        flag_values.push_back({value, std::move(label), std::move(description)});
        return *this;
    }
    ParamDef& flags(std::vector<FlagDef> fs) { flag_values = std::move(fs); return *this; }
    ParamDef& range(double mn, double mx) {
        has_min = true; min_val = mn;
        has_max = true; max_val = mx;
        return *this;
    }
    ParamDef& plat(std::uint32_t mask) { platform_mask = mask; return *this; }

    // ── Precondition constraints (see ParamConstraint / validate_constraints) ──
    ParamDef& constrain(ParamConstraint c) { constraints |= c; return *this; }
    ParamDef& nonempty()      { constraints |= ParamConstraint::NonEmpty; return *this; }
    ParamDef& must_exist()    { constraints |= ParamConstraint::MustExist | ParamConstraint::Readable; return *this; }
    ParamDef& writable()      { constraints |= ParamConstraint::Writable; return *this; }
    ParamDef& as_file()       { constraints |= ParamConstraint::IsFile; return *this; }
    ParamDef& as_dir()        { constraints |= ParamConstraint::IsDir; return *this; }
    ParamDef& create_parents(){ constraints |= ParamConstraint::CreateParents; return *this; }

    // ── Resolution features (see ParamResolve / resolve_block_inputs) ──────────
    ParamDef& resolves(ParamResolve r) { resolve |= r; return *this; }
    ParamDef& vars()  { resolve |= ParamResolve::Variables; return *this; }
    ParamDef& globs() { resolve |= ParamResolve::Variables | ParamResolve::Globs; return *this; }
    ParamDef& guard() { resolve |= ParamResolve::Custom; return *this; }  // app-level resolve hooks
    ParamDef& deep()  { resolve |= ParamResolve::Variables | ParamResolve::Deep; return *this; }

    nlohmann::json to_json() const {
        nlohmann::json j = {
            {"name",  name},
            {"kind",  param_kind_str(kind)},
            {"group", group},
        };
        if (!label.empty())        j["label"]       = label;
        if (!description.empty())  j["description"] = description;
        if (!default_value.is_null()) j["default"]  = default_value;
        if (required)              j["required"]    = true;
        if (uses_previous)         j["uses_previous"] = true;
        if (source == ParamSource::Previous) {
            j["source"] = "previous";
        } else if (source == ParamSource::Variable && !source_ref.empty()) {
            j["source"]     = "variable";
            j["source_ref"] = source_ref;
        } else if (source == ParamSource::Expression && !source_ref.empty()) {
            j["source"]     = "expression";
            j["source_ref"] = source_ref;
        }
        if (hidden)                j["hidden"]      = true;
        if (!enum_values.empty())  j["enum_values"] = enum_values;
        if (!flag_values.empty()) {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& f : flag_values)
                arr.push_back(f.to_json());
            j["flags"] = std::move(arr);
        }
        if (has_min)               j["min"]         = min_val;
        if (has_max)               j["max"]         = max_val;
        if (platform_mask != 0xFFFFFFFFu) j["platform_mask"] = platform_mask;
        if (constraints != ParamConstraint::None) j["constraints"] = param_constraint_tokens(constraints);
        if (resolve != ParamResolve::None) j["resolve"] = param_resolve_tokens(resolve);
        if (!dynamic_schema.is_null()) j["dynamic_schema"] = dynamic_schema;
        if (!ui_meta.is_null()) j["ui"] = ui_meta;
        // All runtime params are bindable by default, declared once by the
        // containing manifest. Emit only per-param overrides to keep the large
        // native manifest compact.
        if (accepted_bindings == ParamBindingKind::None) {
            j["binding"] = false;
        } else if (accepted_bindings != kAllParamBindings ||
                   binding_syntax != ParamBindingSyntax::Value) {
            nlohmann::json binding = nlohmann::json::object();
            if (accepted_bindings != kAllParamBindings)
                binding["accepts"] = param_binding_tokens(accepted_bindings);
            if (binding_syntax != ParamBindingSyntax::Value)
                binding["syntax"] = param_binding_syntax_str(binding_syntax);
            j["binding"] = std::move(binding);
        }
        return j;
    }
};

// ── ParamDef factory helpers ─────────────────────────────────────────────────

inline ParamDef pd_str  (std::string n) { return {std::move(n), {}, {}, ParamKind::string_}; }
inline ParamDef pd_int  (std::string n) { return {std::move(n), {}, {}, ParamKind::integer_}; }
inline ParamDef pd_bool (std::string n) { return {std::move(n), {}, {}, ParamKind::boolean_}; }
inline ParamDef pd_float(std::string n) { return {std::move(n), {}, {}, ParamKind::float_}; }
inline ParamDef pd_expr (std::string n) { return {std::move(n), {}, {}, ParamKind::expression_}; }
inline ParamDef pd_file (std::string n) { return {std::move(n), {}, {}, ParamKind::file_path}; }
inline ParamDef pd_dir  (std::string n) { return {std::move(n), {}, {}, ParamKind::dir_path}; }
inline ParamDef pd_audio(std::string n) { return {std::move(n), {}, {}, ParamKind::audio_path}; }
inline ParamDef pd_image(std::string n) { return {std::move(n), {}, {}, ParamKind::image_path}; }
inline ParamDef pd_video(std::string n) { return {std::move(n), {}, {}, ParamKind::video_path}; }
inline ParamDef pd_outpath(std::string n){
    ParamDef p{std::move(n), {}, {}, ParamKind::output_path};
    // Output paths always auto-create their parent directory tree (CreateParents)
    // and check write permission on existing files (Writable).
    // Both are already supported by validate_constraints; baking them into the
    // factory means call sites get this for free without manual .create_parents().
    p.constraints |= ParamConstraint::Writable | ParamConstraint::CreateParents;
    p.ui({{"pick_mode", "save"}});
    return p;
}
inline ParamDef pd_screen_input(std::string n){ return {std::move(n), {}, {}, ParamKind::screen_input}; }
inline ParamDef pd_args (std::string n) { return {std::move(n), {}, {}, ParamKind::args_list}; }
inline ParamDef pd_json (std::string n) { return {std::move(n), {}, {}, ParamKind::json_value}; }
inline ParamDef pd_stt_route(std::string n) {
    return ParamDef{std::move(n), {}, {}, ParamKind::stt_route}
        .dflt(nlohmann::json::object())
        .dyn({
            {"kind", "stt_route"},
            {"preset_options_path", "agent.presets"},
            {"provider_options_path", "providers.stt"},
            {"model_options_path_template", "providers.stt.models.{provider}"},
            {"schema_method", "xbloxProviderOptionsSchemaGet"},
            {"mode", "stt"},
        });
}
inline ParamDef pd_flags(std::string n) { return {std::move(n), {}, {}, ParamKind::flags}; }
inline ParamDef pd_ms   (std::string n) { return {std::move(n), {}, {}, ParamKind::duration_ms}; }
inline ParamDef pd_dev  (std::string n) { return {std::move(n), {}, {}, ParamKind::device_name}; }
inline ParamDef pd_audio_input_device(std::string n) {
    return pd_dev(std::move(n)).ui({
        {"control", "select"},
        {"options_path", "audio.devices.input"},
        {"empty_label", "(system default microphone)"},
    });
}
inline ParamDef pd_audio_output_device(std::string n) {
    return pd_dev(std::move(n)).ui({
        {"control", "select"},
        {"options_path", "audio.devices.output"},
        {"empty_label", "(system default speaker)"},
    });
}
inline ParamDef pd_audio_loopback_device(std::string n) {
    return pd_dev(std::move(n)).ui({
        {"control", "select"},
        {"options_path", "audio.devices.loopback"},
        {"empty_label", "(default render device)"},
    });
}
inline ParamDef pd_sample_rate(std::string n) {
    return pd_int(std::move(n)).ui({
        {"control", "select"},
        {"options_path", "audio.sample_rates"},
        {"empty_label", "Default (48000 Hz)"},
    });
}
inline ParamDef pd_audio_channels(std::string n) {
    return pd_int(std::move(n)).ui({
        {"control", "select"},
        {"options_path", "audio.channels"},
        {"empty_label", "Default (stereo)"},
    });
}
inline ParamDef pd_video_fps(std::string n) {
    return pd_int(std::move(n)).ui({
        {"control", "select"},
        {"options_path", "video.fps"},
        {"empty_label", "Source default"},
    });
}
inline ParamDef pd_video_bitrate_kbps(std::string n) {
    return pd_int(std::move(n)).ui({
        {"control", "select"},
        {"options_path", "video.bitrate_kbps"},
        {"empty_label", "Auto (from resolution + fps)"},
    });
}
inline ParamDef pd_video_input(std::string n) { return {std::move(n), {}, {}, ParamKind::video_input}; }
inline ParamDef pd_prompt(std::string n){ return {std::move(n), {}, {}, ParamKind::prompt}; }
inline ParamDef pd_varref(std::string n){
    ParamDef p{std::move(n), {}, {}, ParamKind::variable_ref};
    return p.syntax(ParamBindingSyntax::VariableName);
}
inline ParamDef pd_shortcut(std::string n){ return {std::move(n), {}, {}, ParamKind::shortcut_}; }
inline ParamDef pd_color(std::string n){ return {std::move(n), {}, {}, ParamKind::color_}; }
inline ParamDef pd_enum (std::string n, std::vector<std::string> vs) {
    ParamDef p{std::move(n), {}, {}, ParamKind::enum_};
    p.enum_values = std::move(vs);
    return p;
}

inline std::vector<ParamDef> pd_status_hud_params(bool hud_default = false,
                                                  const char* mode_default = "overlay")
{
    return {
        pd_enum("hudMode", {"off", "taskbar", "overlay", "both"}).grp("hud").dflt(mode_default)
            .desc("Status HUD. taskbar = app taskbar button. overlay = standalone pill over the clock. both = combine. off = none."),
        pd_bool("hud").grp("hud").dflt(hud_default)
            .desc("Master switch for hudMode. Overlay Stop / taskbar Stop cancels the block."),
    };
}

inline void merge_status_hud_defaults(nlohmann::json& defaults,
                                      bool hud_default = false,
                                      const char* mode_default = "overlay")
{
    if (!defaults.contains("hud"))
        defaults["hud"] = hud_default;
    if (!defaults.contains("hudMode"))
        defaults["hudMode"] = mode_default;
}

inline std::vector<ParamDef> pd_screenshot_picker_params()
{
    return {
        pd_enum("pick", {"off", "interactive"}).grp("picker").dflt("off")
            .desc("interactive runs the window/element picker, then captures that "
                  "region. off uses input (or PREVIOUS). Click = window/UIA; drag = "
                  "free rectangle; Alt+wheel or [ ] cycles the stack; Esc cancels."),
        pd_bool("includeOpenWindows").grp("picker").dflt(true)
            .desc("Monitor/desktop picks: capture open apps on that display "
                  "(layer=monitor). Off = wallpaper only."),
        pd_bool("highlight").grp("picker").dflt(true)
            .desc("Translucent highlight overlay while picking."),
        pd_bool("hud").grp("picker").dflt(true)
            .desc("Picker HUD chip (stack name / shortcuts). Off keeps the highlight."),
        pd_bool("resolveUia").grp("picker").dflt(true)
            .desc("Resolve the UIA element under the cursor (web nodes, video players)."),
        pd_bool("showHidden").grp("picker").dflt(true)
            .desc("Include non-visible child windows (Chrome render widget, etc.)."),
        pd_bool("siblings").grp("picker").dflt(false)
            .desc("Alt+wheel / [ ] stack includes sibling panes and their union. "
                  "Off = leaf and ancestors only."),
        pd_bool("magnifier").grp("picker").dflt(false)
            .desc("Open the 8× loupe at pick start (Z toggles)."),
        pd_int("pickTimeoutMs").adv().grp("picker").dflt(60000).range(1000, 600000)
            .desc("Abort the pick after this many milliseconds. Default 60 s."),
    };
}

inline void merge_screenshot_picker_defaults(nlohmann::json& defaults)
{
    if (!defaults.contains("pick"))
        defaults["pick"] = "off";
    if (!defaults.contains("includeOpenWindows"))
        defaults["includeOpenWindows"] = true;
    if (!defaults.contains("highlight"))
        defaults["highlight"] = true;
    if (!defaults.contains("hud"))
        defaults["hud"] = true;
    if (!defaults.contains("resolveUia"))
        defaults["resolveUia"] = true;
    if (!defaults.contains("showHidden"))
        defaults["showHidden"] = true;
    if (!defaults.contains("siblings"))
        defaults["siblings"] = false;
    if (!defaults.contains("magnifier"))
        defaults["magnifier"] = false;
    if (!defaults.contains("pickTimeoutMs"))
        defaults["pickTimeoutMs"] = 60000;
}

// Common cross-block param definitions
inline ParamDef pd_store_as() {
    return pd_str("storeAs").out()
        .desc("Variable to also store the result in (always sets PREVIOUS).")
        .literal_only()
        .ui({{"semantic_role", "primary_output_name"}})
        .dflt("");
}
inline ParamDef pd_provider() {
    return pd_str("provider").adv()
        .desc("LLM / speech provider override. Empty = from App Settings.").dflt("");
}
inline ParamDef pd_model() {
    return pd_str("model").adv().deep()
        .desc("Model override. Empty = from App Settings.").dflt("");
}
inline ParamDef pd_api_key(std::string n = "apiKey") {
    ParamDef p{std::move(n), {}, {}, ParamKind::api_key};
    return p.adv().desc("API key override. Empty = from App Settings.").dflt("");
}

// ── Keystroke OSD bundle (group "key_overlay") ────────────────────────────────
// Flat fields so the resolver / property panel work unchanged. Any block can
// append this vector; keep the field names stable across videoCapture / stream.

inline std::vector<ParamDef> pd_key_overlay_params()
{
    const nlohmann::json when_on{{"visible_when", {{"keyOverlay", true}}}};
    return {
        pd_bool("keyOverlay").grp("key_overlay").dflt(false)
            .desc("Show typed / shortcut keystrokes as an on-screen overlay while recording."),
        pd_bool("keyOverlayBurnIn").adv().grp("key_overlay").dflt(false)
            .ui(when_on)
            .desc("Also burn keystrokes into the encoded MP4 (independent of hudCapture)."),
        pd_enum("keyOverlayAlign", {
                "center", "left", "right", "top", "bottom",
                "top-left", "top-right", "bottom-left", "bottom-right"
            }).grp("key_overlay").dflt("bottom")
            .ui(when_on)
            .desc("VLC-style subtitle / marquee position (0/1/2/4/5/6/8/9/10)."),
        pd_int("keyOverlayMarginX").adv().grp("key_overlay").dflt(0).range(0, 500)
            .ui(when_on)
            .desc("Horizontal margin in px. 0 = auto from short edge (~1.2%)."),
        pd_int("keyOverlayMarginY").adv().grp("key_overlay").dflt(0).range(0, 500)
            .ui(when_on)
            .desc("Vertical margin in px. 0 = auto from short edge (~1.2%)."),
        pd_int("keyOverlayFontSize").grp("key_overlay").dflt(0).range(0, 96)
            .ui(when_on)
            .desc("Chip font size in px. 0 = auto (~2.2% of short edge, clamped 14–32)."),
        pd_color("keyOverlayColor").grp("key_overlay").dflt("#F8F9FB").ui(when_on)
            .desc("Key label text color."),
        pd_color("keyOverlayBgColor").adv().grp("key_overlay").dflt("#181B21").ui(when_on)
            .desc("Chip background color."),
        pd_color("keyOverlayAccentColor").adv().grp("key_overlay").dflt("#EB445A").ui(when_on)
            .desc("Left accent bar color."),
        pd_float("keyOverlayOpacity").adv().grp("key_overlay").dflt(0.9).range(0.0, 1.0)
            .ui(when_on)
            .desc("Overlay opacity (0–1)."),
        pd_ms("keyOverlayHoldMs").grp("key_overlay").dflt(1200).range(100, 10000)
            .ui(when_on)
            .desc("How long each keychip stays fully visible before fading."),
        pd_ms("keyOverlayFadeMs").adv().grp("key_overlay").dflt(450).range(0, 5000)
            .ui(when_on)
            .desc("Fade-out duration after hold."),
        pd_int("keyOverlayMaxEntries").adv().grp("key_overlay").dflt(5).range(1, 16)
            .ui(when_on)
            .desc("Max stacked recent keychips; oldest are evicted."),
        pd_enum("keyOverlayFilter", {"all", "printable", "shortcuts"})
            .grp("key_overlay").dflt("all")
            .ui(when_on)
            .desc("all = every key; printable = letters/digits; shortcuts = modifier chords only."),
        pd_enum("keyOverlayStack", {"auto", "up", "down"})
            .grp("key_overlay").dflt("auto")
            .ui(when_on)
            .desc("Chip stack growth. auto = up for bottom-* (newest at screen edge), down for top/center. up/down force direction."),
        pd_bool("keyOverlayModifiers").adv().grp("key_overlay").dflt(true)
            .ui(when_on)
            .desc("Prefix Ctrl/Alt/Shift/Win on displayed chords."),
        pd_enum("keyOverlayCapture", {"inherit", "auto", "exclude", "visible"})
            .adv().grp("key_overlay").dflt("visible")
            .ui(when_on)
            .desc("Capture policy. Default visible so keys appear in the MP4 even when hudCapture excludes the zoom HUD. inherit uses hudCapture."),
    };
}

inline void append_key_overlay_params(std::vector<ParamDef>& params)
{
    auto ko = pd_key_overlay_params();
    params.insert(params.end(),
                  std::make_move_iterator(ko.begin()),
                  std::make_move_iterator(ko.end()));
}

inline void merge_key_overlay_defaults(nlohmann::json& defaults)
{
    const auto ko = nlohmann::json{
        {"keyOverlay", false},
        {"keyOverlayBurnIn", false},
        {"keyOverlayAlign", "bottom"},
        {"keyOverlayMarginX", 0},
        {"keyOverlayMarginY", 0},
        {"keyOverlayFontSize", 0},
        {"keyOverlayColor", "#F8F9FB"},
        {"keyOverlayBgColor", "#181B21"},
        {"keyOverlayAccentColor", "#EB445A"},
        {"keyOverlayOpacity", 0.9},
        {"keyOverlayHoldMs", 1200},
        {"keyOverlayFadeMs", 450},
        {"keyOverlayMaxEntries", 5},
        {"keyOverlayFilter", "all"},
        {"keyOverlayStack", "auto"},
        {"keyOverlayModifiers", true},
        {"keyOverlayCapture", "visible"},
    };
    for (auto it = ko.begin(); it != ko.end(); ++it) {
        if (!defaults.contains(it.key()))
            defaults[it.key()] = it.value();
    }
}

// ── Creator FX bundles (docs/video/video-next.md slices 1–3) ───────────────────
// Slices 1–2 (cursor, camera PiP) are live; slice 3 audio polish remains reserved.

inline std::vector<ParamDef> pd_cursor_fx_params()
{
    const nlohmann::json when_hl{{"visible_when", {{"cursorHighlight", true}}}};
    const nlohmann::json when_click{{"visible_when", {{"cursorClicks", true}}}};
    return {
        pd_bool("cursorHighlight").grp("cursor").dflt(false)
            .desc("Soft highlight ring around the pointer (capture-visible overlay)."),
        pd_color("cursorHighlightColor").grp("cursor").dflt("#F5C542").ui(when_hl)
            .desc("Cursor highlight ring color."),
        pd_int("cursorHighlightSize").adv().grp("cursor").dflt(0).range(0, 256).ui(when_hl)
            .desc("Highlight diameter in px. 0 = auto (~72)."),
        pd_bool("cursorClicks").grp("cursor").dflt(false)
            .desc("Animate a ripple at the click point (left/right/middle)."),
        pd_color("cursorClickColor").grp("cursor").dflt("#EB445A").ui(when_click)
            .desc("Left-click ripple color. Right/middle use complementary "
                  "hues (+180° / +120°) derived from this."),
        pd_bool("cursorTrail").adv().grp("cursor").dflt(false)
            .desc("Short motion trail behind the pointer."),
        pd_bool("hideCursorWhenTyping").adv().grp("cursor").dflt(false)
            .desc("Temporarily hide highlight/trail while typing."),
    };
}

inline void append_cursor_fx_params(std::vector<ParamDef>& params)
{
    auto p = pd_cursor_fx_params();
    params.insert(params.end(),
                  std::make_move_iterator(p.begin()),
                  std::make_move_iterator(p.end()));
}

inline void merge_cursor_fx_defaults(nlohmann::json& defaults)
{
    const auto d = nlohmann::json{
        {"cursorHighlight", false},
        {"cursorHighlightColor", "#F5C542"},
        {"cursorHighlightSize", 0},
        {"cursorClicks", false},
        {"cursorClickColor", "#EB445A"},
        {"cursorTrail", false},
        {"hideCursorWhenTyping", false},
    };
    for (auto it = d.begin(); it != d.end(); ++it) {
        if (!defaults.contains(it.key()))
            defaults[it.key()] = it.value();
    }
}

inline std::vector<ParamDef> pd_camera_overlay_params()
{
    const nlohmann::json when_on{{"visible_when", {{"cameraOverlay", true}}}};
    const nlohmann::json when_audio{{"visible_when", {{"cameraAudio", true}}}};
    return {
        pd_bool("cameraOverlay").grp("camera_overlay").dflt(false)
            .desc("Picture-in-picture webcam over the recording (capture-visible overlay). "
                  "When on, skin / lips / blur / mirror and the other face knobs apply to "
                  "this PiP camera only — the screen stays on the fast recorder."),
        pd_bool("cameraAudio").grp("camera_overlay").dflt(false)
            .desc("Include the webcam microphone in the recording mix "
                  "(matched from Camera; combines with audioSource)."),
        pd_float("cameraAudioGain").grp("camera_overlay").dflt(1.0).range(0.0, 4.0).ui(when_audio)
            .desc("Linear gain for the webcam mic (0 = mute, 1 = unity)."),
        pd_video_input("cameraInput").grp("camera_overlay").lbl("Camera").dflt("").ui(
            nlohmann::json{{"camera_only", true}})
            .desc("Webcam device for the PiP / camera mic. Empty = system default camera."),
        pd_str("cameraResolution").grp("camera_overlay").lbl("Capture").dflt("auto").ui(
            nlohmann::json{
                {"control", "select"},
                {"options_path_template", "video.camera.modes.{cameraInput}"},
                {"empty_label", "Auto (match PiP)"},
                {"visible_when", {{"cameraOverlay", true}}},
            })
            .desc("Webcam capture mode: auto (PiP-sized), max (highest native, "
                  "including 4K), or a device mode like 1920x1080@30."),
        pd_enum("cameraPosition", {
                "top-left", "top-right", "bottom-left", "bottom-right"
            }).grp("camera_overlay").dflt("bottom-right").ui(when_on)
            .desc("PiP corner on the capture monitor/window."),
        pd_float("cameraSize").grp("camera_overlay").dflt(22.0).range(4.0, 80.0).ui(when_on)
            .desc("PiP size as percent of the short capture edge."),
        pd_enum("cameraShape", {"circle", "rounded", "square"})
            .grp("camera_overlay").dflt("circle").ui(when_on)
            .desc("Webcam mask shape (circle / rounded / square)."),
        pd_bool("cameraBorder").adv().grp("camera_overlay").dflt(true).ui(when_on)
            .desc("Draw a border around the PiP."),
        pd_color("cameraBorderColor").adv().grp("camera_overlay").dflt("#FFFFFF").ui(when_on)
            .desc("PiP border color."),
        pd_bool("cameraShadow").adv().grp("camera_overlay").dflt(true).ui(when_on)
            .desc("Soft drop shadow under the PiP."),
    };
}

inline void append_camera_overlay_params(std::vector<ParamDef>& params)
{
    auto p = pd_camera_overlay_params();
    params.insert(params.end(),
                  std::make_move_iterator(p.begin()),
                  std::make_move_iterator(p.end()));
}

inline void merge_camera_overlay_defaults(nlohmann::json& defaults)
{
    const auto d = nlohmann::json{
        {"cameraOverlay", false},
        {"cameraInput", ""},
        {"cameraPosition", "bottom-right"},
        {"cameraSize", 22.0},
        {"cameraShape", "circle"},
        {"cameraBorder", true},
        {"cameraBorderColor", "#FFFFFF"},
        {"cameraShadow", true},
        {"cameraAudio", false},
        {"cameraAudioGain", 1.0},
        {"cameraResolution", "auto"},
    };
    for (auto it = d.begin(); it != d.end(); ++it) {
        if (!defaults.contains(it.key()))
            defaults[it.key()] = it.value();
    }
}

inline std::vector<ParamDef> pd_audio_polish_params()
{
    return {
        pd_enum("filter", {"off", "deepfilter", "gtcrn"}).grp("audio").dflt("off")
            .desc("Live DeepFilterNet on the AAC mux worker (mic/mix/cameraAudio). "
                  "off = dry. desktop-only loopback stays dry. gtcrn is reserved."),
        pd_str("filterModel").adv().grp("audio").dflt("")
            .desc("Optional DeepFilterNet tar.gz. Default: empty → ${MODELS_DIR}/deepfilter."),
        pd_float("attenLim").adv().grp("audio").dflt(100.0).range(0.0, 100.0)
            .desc("Attenuation limit in dB. Default: 100."),
        pd_bool("postFilter").adv().grp("audio").dflt(false)
            .desc("Over-attenuate very noisy sections. Implies deepfilter when filter is off."),
        pd_bool("compensateDelay").adv().grp("audio").dflt(true)
            .desc("Compensate STFT / model lookahead. Default: true."),
        pd_bool("audioNormalize").adv().grp("audio").dflt(false)
            .desc("Reserved (low risk): normalize mixed audio level before AAC encode."),
        pd_bool("audioAutoGain").adv().grp("audio").dflt(false)
            .desc("Reserved (low risk): gentle automatic gain on the microphone path."),
    };
}

inline void append_audio_polish_params(std::vector<ParamDef>& params)
{
    auto p = pd_audio_polish_params();
    params.insert(params.end(),
                  std::make_move_iterator(p.begin()),
                  std::make_move_iterator(p.end()));
}

inline void merge_audio_polish_defaults(nlohmann::json& defaults)
{
    const auto d = nlohmann::json{
        {"filter", "off"},
        {"filterModel", ""},
        {"attenLim", 100.0},
        {"postFilter", false},
        {"compensateDelay", true},
        {"audioNormalize", false},
        {"audioAutoGain", false},
    };
    for (auto it = d.begin(); it != d.end(); ++it) {
        if (!defaults.contains(it.key()))
            defaults[it.key()] = it.value();
    }
}

inline std::vector<ParamDef> pd_idle_pause_params()
{
    const nlohmann::json when_on{{"visible_when",
        {{"autoPause", nlohmann::json{"visual", "audio", "both"}}}}};
    const nlohmann::json when_vis{{"visible_when",
        {{"autoPause", nlohmann::json{"visual", "both"}}}}};
    const nlohmann::json when_aud{{"visible_when",
        {{"autoPause", nlohmann::json{"audio", "both"}}}}};
    return {
        pd_enum("autoPause", {"off", "visual", "audio", "both"}).grp("idle").dflt("off")
            .desc("Skip encoder writes while the capture is idle. Capture stays up. "
                  "Activity = user input OR speech-mic energy OR a substantial frame change."),
        pd_ms("autoPauseIdleMs").grp("idle").dflt(3000).range(0, 60000).ui(when_on)
            .desc("Overall idle timer. No activity for this long sets PauseIdle. 0 with autoPause off."),
        pd_ms("autoPauseResumeMs").adv().grp("idle").dflt(300).range(0, 5000).ui(when_vis)
            .desc("Visual must persist this long while auto-paused. Pre-roll covers the start of the action."),
        pd_int("autoPauseGridW").adv().grp("idle").dflt(32).range(8, 128).ui(when_vis)
            .desc("Logical tile columns."),
        pd_int("autoPauseGridH").adv().grp("idle").dflt(18).range(8, 72).ui(when_vis)
            .desc("Logical tile rows."),
        pd_int("autoPauseProbeW").adv().grp("idle").dflt(64).range(64, 192).ui(when_vis)
            .desc("Intermediate luma width (aggregated into logical tiles)."),
        pd_int("autoPauseProbeH").adv().grp("idle").dflt(36).range(36, 108).ui(when_vis)
            .desc("Intermediate luma height."),
        pd_int("autoPausePixelDelta").adv().grp("idle").dflt(12).range(1, 255).ui(when_vis)
            .desc("Luma abs-diff threshold (0–255)."),
        pd_int("autoPauseTilePercent").adv().grp("idle").dflt(8).range(1, 100).ui(when_vis)
            .desc("Percent of a tile’s probe samples over pixelDelta."),
        pd_int("autoPauseMinTiles").adv().grp("idle").dflt(4).range(1, 512).ui(when_vis)
            .desc("Minimum changed logical tiles. Typing is input activity, not this gate."),
        pd_float("autoPauseMinCoveragePercent").adv().grp("idle").dflt(2.0).range(0.0, 100.0).ui(when_vis)
            .desc("sum(activeTile.changedFraction) / gridTileCount as percent. Not bbox area."),
        pd_int("autoPauseSampleFps").adv().grp("idle").dflt(8).range(1, 30).ui(when_vis)
            .desc("Visual probe rate (independent of encode fps)."),
        pd_bool("autoPauseMicWake").grp("idle").dflt(true).ui(when_aud)
            .desc("Use speech-mic energy as activity (RMS/peak, not VAD). Desktop-only loopback ignored."),
        pd_float("autoPauseMicActivityDb").adv().grp("idle").dflt(-40.0).range(-90.0, 0.0).ui(when_aud)
            .desc("Mic activity threshold in dBFS (not speech). Swappable for WebRTC VAD later."),
        pd_ms("autoPauseMicHangoverMs").adv().grp("idle").dflt(500).range(0, 5000).ui(when_aud)
            .desc("Mic activity hold after the last hot window."),
        pd_ms("autoPauseKeyHoldMs").adv().grp("idle").dflt(750).range(0, 5000).ui(when_on)
            .desc("Key / click / wheel hold. Not the overall idle timer."),
        pd_ms("autoPauseMouseHoldMs").adv().grp("idle").dflt(350).range(0, 5000).ui(when_on)
            .desc("Mouse-move hold. A nudge must not keep recording for idleMs."),
        pd_bool("autoPauseWakeKey").adv().grp("idle").dflt(true).ui(when_on)
            .desc("Count keyboard as first-class activity (ignore pauseKey/stopKey)."),
        pd_bool("autoPauseWakeMouse").adv().grp("idle").dflt(true).ui(when_on)
            .desc("Count mouse move/click/wheel as activity."),
        pd_int("autoPauseMouseDeltaPx").adv().grp("idle").dflt(4).range(0, 64).ui(when_on)
            .desc("Ignore cursor jitter below this many screen pixels."),
        pd_ms("autoPausePrerollMs").grp("idle").dflt(500).range(0, 2000).ui(when_on)
            .desc("Frames kept while PauseIdle and flushed on resume."),
    };
}

inline void append_idle_pause_params(std::vector<ParamDef>& params)
{
    auto p = pd_idle_pause_params();
    params.insert(params.end(),
                  std::make_move_iterator(p.begin()),
                  std::make_move_iterator(p.end()));
}

inline void merge_idle_pause_defaults(nlohmann::json& defaults)
{
    const auto d = nlohmann::json{
        {"autoPause", "off"},
        {"autoPauseIdleMs", 3000},
        {"autoPauseResumeMs", 300},
        {"autoPauseGridW", 32},
        {"autoPauseGridH", 18},
        {"autoPauseProbeW", 64},
        {"autoPauseProbeH", 36},
        {"autoPausePixelDelta", 12},
        {"autoPauseTilePercent", 8},
        {"autoPauseMinTiles", 4},
        {"autoPauseMinCoveragePercent", 2.0},
        {"autoPauseSampleFps", 8},
        {"autoPauseMicWake", true},
        {"autoPauseMicActivityDb", -40.0},
        {"autoPauseMicHangoverMs", 500},
        {"autoPauseKeyHoldMs", 750},
        {"autoPauseMouseHoldMs", 350},
        {"autoPauseWakeKey", true},
        {"autoPauseWakeMouse", true},
        {"autoPauseMouseDeltaPx", 4},
        {"autoPausePrerollMs", 500},
    };
    for (auto it = d.begin(); it != d.end(); ++it) {
        if (!defaults.contains(it.key()))
            defaults[it.key()] = it.value();
    }
}

inline std::vector<ParamDef> pd_caption_params()
{
    const nlohmann::json when_on{{"visible_when",
        {{"captions", nlohmann::json{"sidecar", "srt", "vtt", "sbv", "all"}}}}};
    return {
        pd_enum("captions", {"off", "sidecar"}).grp("captions").dflt("off")
            .desc("After finalize, transcribe the encoded-timeline 16 kHz dump and write sidecars. "
                  "Not a live HUD (use audioTranscribe for that)."),
        pd_enum("captionFormat", {"srt", "vtt", "sbv", "all"}).grp("captions").dflt("srt").ui(when_on)
            .desc("YouTube-accepted sidecar: SubRip, WebVTT, SubViewer, or all three."),
        pd_stt_route("captionStt").grp("captions").lbl("STT").dflt(nlohmann::json::object()).ui(when_on)
            .desc("Dictation STT route. Empty {} inherits App Settings Voice & Audio."),
        pd_int("subtitleMaxChars").adv().grp("captions").dflt(42).range(8, 40000).ui(when_on)
            .desc("Cue wrap width (writer, not viewport)."),
        pd_int("subtitleMaxLines").adv().grp("captions").dflt(2).range(1, 4).ui(when_on)
            .desc("Cue wrap line count."),
    };
}

inline void append_caption_params(std::vector<ParamDef>& params)
{
    auto p = pd_caption_params();
    params.insert(params.end(),
                  std::make_move_iterator(p.begin()),
                  std::make_move_iterator(p.end()));
}

inline void merge_caption_defaults(nlohmann::json& defaults)
{
    const auto d = nlohmann::json{
        {"captions", "off"},
        {"captionFormat", "srt"},
        {"captionStt", nlohmann::json::object()},
        {"subtitleMaxChars", 42},
        {"subtitleMaxLines", 2},
    };
    for (auto it = d.begin(); it != d.end(); ++it) {
        if (!defaults.contains(it.key()))
            defaults[it.key()] = it.value();
    }
}

#if defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
inline std::vector<ParamDef> pd_beauty_params()
{
    const nlohmann::json when_skin{{"visible_when", {{"skin", true}}}};
    const nlohmann::json when_lips{{"visible_when", {{"lips", true}}}};
    const nlohmann::json when_teeth{{"visible_when", {{"teeth", true}}}};
    return {
        pd_bool("skin").grp("beauty").lbl("Skin").dflt(false)
            .desc("Smooth skin (face oval minus eyes/lips). Forces the compatibility CPU recorder."),
        pd_float("skinBlend").grp("beauty").lbl("Skin blend").dflt(0.5).range(0.0, 1.0).ui(when_skin)
            .desc("Skin mix 0..1 (0 dry / 1 wet). Same as CLI --skin-blend / --skin-strength."),
        pd_bool("lips").grp("beauty").lbl("Lips").dflt(false)
            .desc("Tint lips from the face mesh. Forces the compatibility CPU recorder."),
        pd_color("lipsColor").grp("beauty").lbl("Lip color").dflt("#C45C6A").ui(when_lips)
            .desc("Lip tint color."),
        pd_float("lipsBlend").grp("beauty").lbl("Lip blend").dflt(0.35).range(0.0, 1.0).ui(when_lips)
            .desc("Lip tint mix 0..1. Same as CLI --lips-blend / --lips-alpha."),
        pd_bool("teeth").grp("beauty").lbl("Teeth").dflt(false)
            .desc("Whiten teeth from the face mesh. Forces the compatibility CPU recorder."),
        pd_float("teethBlend").grp("beauty").lbl("Teeth blend").dflt(0.4).range(0.0, 1.0).ui(when_teeth)
            .desc("Teeth mix 0..1. Same as CLI --teeth-blend / --teeth-strength."),
        pd_bool("landmarks").grp("beauty").lbl("Landmarks").dflt(false)
            .desc("Draw face-mesh landmarks on the frame (debug)."),
        pd_float("faceGain").grp("beauty").lbl("Face gain").dflt(0.0).range(0.0, 1.0)
            .desc("Lift crushed face exposure (Lab L*). 0 = off. Same oval as skin. "
                  "Same as CLI --face-gain."),
        pd_float("warmth").grp("beauty").lbl("Warmth").dflt(0.0).range(0.0, 1.0)
            .desc("Warm the face oval (Lab b*). 0 = off. Same as CLI --warmth."),
        pd_bool("undereye").grp("beauty").lbl("Under-eye").dflt(false)
            .desc("Lighten under-eye bands (dark circles). Forces the compatibility CPU recorder."),
        pd_float("undereyeBlend").grp("beauty").lbl("Under-eye blend").dflt(0.45).range(0.0, 1.0)
            .ui(nlohmann::json{{"visible_when", {{"undereye", true}}}})
            .desc("Under-eye mix 0..1. Same as CLI --undereye-blend."),
        pd_bool("redness").grp("beauty").lbl("Redness mute").dflt(false)
            .desc("Mute flush / acne (Lab a* toward neutral). Forces the compatibility CPU recorder."),
        pd_float("rednessBlend").grp("beauty").lbl("Redness blend").dflt(0.4).range(0.0, 1.0)
            .ui(nlohmann::json{{"visible_when", {{"redness", true}}}})
            .desc("Redness mute mix 0..1. Same as CLI --redness-blend."),
        pd_bool("eyeSharpen").grp("beauty").lbl("Eye sharpen").dflt(false)
            .desc("Unsharp the eye polys after skin. Forces the compatibility CPU recorder."),
        pd_float("eyeSharpenBlend").grp("beauty").lbl("Eye sharpen blend").dflt(0.35).range(0.0, 1.0)
            .ui(nlohmann::json{{"visible_when", {{"eyeSharpen", true}}}})
            .desc("Eye sharpen mix 0..1. Same as CLI --eye-sharpen-blend."),
        pd_bool("mirror").grp("frame").lbl("Mirror").dflt(false)
            .desc("Horizontal flip (webcam mirror). No model."),
        pd_bool("preview").grp("frame").lbl("Preview").dflt(false)
            .desc("Live window at the captured frame size. Close the window to stop. "
                  "Forces the compatibility CPU recorder. Same as CLI --preview."),
        pd_bool("blur").grp("background").lbl("Background blur").dflt(false)
            .desc("Blur the non-person region (ImageSegmenter). Forces the compatibility CPU recorder."),
        pd_float("blurBlend").grp("background").lbl("Blur blend").dflt(0.65).range(0.0, 1.0)
            .ui(nlohmann::json{{"visible_when", {{"blur", true}}}})
            .desc("Background blur mix 0..1. Same as CLI --blur-strength."),
        pd_bool("background").grp("background").lbl("Replace background").dflt(false)
            .desc("Replace the non-person region with bgColor / bgImage."),
        pd_color("bgColor").grp("background").lbl("Background color").dflt("#1A1A1A")
            .ui(nlohmann::json{{"visible_when", {{"background", true}}}})
            .desc("Solid replacement color."),
        pd_image("bgImage").grp("background").lbl("Background image").dflt("")
            .ui(nlohmann::json{{"visible_when", {{"background", true}}}})
            .desc("Replacement image, scaled to the frame."),
        pd_int("featherPx").grp("background").lbl("Feather px").dflt(8)
            .desc("Person-mask feather in pixels."),
    };
}

inline void append_beauty_params(std::vector<ParamDef>& params)
{
    auto p = pd_beauty_params();
    params.insert(params.end(),
                  std::make_move_iterator(p.begin()),
                  std::make_move_iterator(p.end()));
}

inline void merge_beauty_defaults(nlohmann::json& defaults)
{
    const auto d = nlohmann::json{
        {"skin", false},
        {"skinBlend", 0.5},
        {"lips", false},
        {"lipsColor", "#C45C6A"},
        {"lipsBlend", 0.35},
        {"teeth", false},
        {"teethBlend", 0.4},
        {"landmarks", false},
        {"faceGain", 0.0},
        {"warmth", 0.0},
        {"undereye", false},
        {"undereyeBlend", 0.45},
        {"redness", false},
        {"rednessBlend", 0.4},
        {"eyeSharpen", false},
        {"eyeSharpenBlend", 0.35},
        {"mirror", false},
        {"preview", false},
        {"blur", false},
        {"blurBlend", 0.65},
        {"background", false},
        {"bgColor", "#1A1A1A"},
        {"bgImage", ""},
        {"featherPx", 8},
    };
    for (auto it = d.begin(); it != d.end(); ++it) {
        if (!defaults.contains(it.key()))
            defaults[it.key()] = it.value();
    }
}
#endif

#if defined(FEATURE_VIDEO_VCAM) && FEATURE_VIDEO_VCAM
inline std::vector<ParamDef> pd_vcam_params()
{
    const nlohmann::json when_on{{"visible_when", {{"virtualOut", true}}}};
    return {
        pd_bool("virtualOut").grp("virtual_camera").lbl("Virtual camera").dflt(false)
            .desc("Publish frames to Tanit Cam (Windows 11 MF Frame Server). "
                  "Zoom / Teams see it while this process runs. Forces the "
                  "compatibility CPU recorder. outputPath may be empty (vcam only)."),
        pd_str("virtualOutName").grp("virtual_camera").lbl("Device name").dflt("Tanit Cam").ui(when_on)
            .desc("Device name shown to Zoom / Teams (Windows adds “Windows Virtual Camera”)."),
    };
}

inline void append_vcam_params(std::vector<ParamDef>& params)
{
    auto p = pd_vcam_params();
    params.insert(params.end(),
                  std::make_move_iterator(p.begin()),
                  std::make_move_iterator(p.end()));
}

inline void merge_vcam_defaults(nlohmann::json& defaults)
{
    const auto d = nlohmann::json{
        {"virtualOut", false},
        {"virtualOutName", "Tanit Cam"},
    };
    for (auto it = d.begin(); it != d.end(); ++it) {
        if (!defaults.contains(it.key()))
            defaults[it.key()] = it.value();
    }
}
#endif

} // namespace media::xblox::blocks
