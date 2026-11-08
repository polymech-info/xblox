// Stand-ins for host services the desktop Tanit process provides.
// Variable expansion, local copy, and HTTP stay in their real translation units.

#include "commands/command_registry.hpp"
#include "core/app_resolver.hpp"
#include "core/app_storage.hpp"
#include "core/cli_cancel.hpp"
#include "core/host_backends.hpp"
#include "core/local_model_catalog.hpp"
#include "core/openrouter_provider_models_cli.hpp"
#include "core/replicate_provider_models_cli.hpp"
#include "core/settings_portable.hpp"
#include "core/settings_runtime.hpp"
#include "core/settings_store.hpp"
#include "controllers/hotkeys.hpp"
#include "llm/llm_fs_guard.hpp"
#include "media/probe.hpp"
#include "net/url_fetch.hpp"
#include "string/string_utils.hpp"
#include "ui_defaults.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string g_settings_file;
std::string g_config_dir;

fs::path config_root()
{
    std::error_code ec;
    const char* appdata = std::getenv("APPDATA");
    fs::path dir = (appdata && appdata[0])
        ? fs::path(appdata) / "pm-xblox"
        : fs::temp_directory_path(ec) / "pm-xblox";
    fs::create_directories(dir, ec);
    return dir;
}

} // namespace

namespace pm_xblox_standalone {

void set_settings_file(std::string path)
{
    g_settings_file = std::move(path);
}

void set_config_dir(std::string path)
{
    g_config_dir = std::move(path);
}

} // namespace pm_xblox_standalone

namespace media::settings {

fs::path get_config_dir()
{
    if (!g_config_dir.empty())
        return fs::path(g_config_dir);
    return config_root();
}

fs::path get_models_dir()
{
    std::error_code ec;
    const fs::path dir = config_root() / "models";
    fs::create_directories(dir, ec);
    return dir;
}

bool load_subtree(const std::string& key, nlohmann::json& out, std::string&)
{
    out = nlohmann::json::object();
    if (g_settings_file.empty() || key.empty())
        return false;
    const fs::path settings_path(g_settings_file);
    std::ifstream in(settings_path);
    if (!in)
        return false;
    nlohmann::json root;
    try {
        in >> root;
    } catch (...) {
        return false;
    }
    const nlohmann::json* node = &root;
    std::string part;
    for (char c : key) {
        if (c == '.') {
            if (!node->is_object() || !node->contains(part))
                return false;
            node = &(*node)[part];
            part.clear();
        } else {
            part.push_back(c);
        }
    }
    if (!node->is_object() || part.empty() || !node->contains(part))
        return false;
    out = (*node)[part];
    return true;
}

} // namespace media::settings

namespace media::portable_settings {

fs::path settings_pixelwiz_profile_dir()
{
    return media::settings::get_config_dir();
}

} // namespace media::portable_settings

namespace media::runtime_settings {

fs::path effective_temp_dir()
{
    std::error_code ec;
    const fs::path dir = media::settings::get_config_dir() / "temp";
    fs::create_directories(dir, ec);
    return dir;
}

bool load_chat_provider_presets(std::vector<ChatProviderPreset>& out, std::string&)
{
    out.clear();
    return false;
}

bool resolve_chat_provider_preset(const std::string&, ChatProviderPreset&, std::string& err)
{
    err = "no chat presets in pm-xblox";
    return false;
}

void merge_provider_credentials(const std::string&, bool, std::string&, std::string&) {}

bool append_xblox_artifact_entries(const nlohmann::json&, std::string&, std::size_t)
{
    return true;
}

} // namespace media::runtime_settings

namespace pm::appdata {

fs::path local_cache()
{
    return media::settings::get_config_dir();
}

} // namespace pm::appdata

namespace media::ui_defaults {

std::vector<std::string> app_search_path_dirs()
{
    return {};
}

fs::path safe_default_folder(const char*)
{
    std::error_code ec;
    return fs::current_path(ec);
}

fs::path fallback_work_folder(const char*)
{
    return safe_default_folder(nullptr);
}

} // namespace media::ui_defaults

namespace media::commands {

void seed_app(CommandRegistry&) {}

} // namespace media::commands

namespace media::cli {

bool cancel_requested() noexcept
{
    return false;
}

} // namespace media::cli

namespace media::hotkeys {
namespace {

std::string trim_ascii(std::string s)
{
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    const auto b = std::find_if(s.begin(), s.end(), not_space);
    const auto e = std::find_if(s.rbegin(), s.rend(), not_space).base();
    if (b >= e)
        return {};
    return std::string(b, e);
}

#if defined(_WIN32)
std::wstring widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

bool key_down(UINT vk)
{
    return (::GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
}
#endif

} // namespace

bool parse_shortcut(const std::string& s, unsigned& mods_out, unsigned& vk_out)
{
#if !defined(_WIN32)
    (void)s;
    (void)mods_out;
    (void)vk_out;
    return false;
#else
    if (s.empty())
        return false;
    std::vector<std::string> parts;
    std::string tok;
    for (char c : s) {
        if (c == '+') {
            if (!tok.empty()) {
                parts.push_back(trim_ascii(tok));
                tok.clear();
            }
        } else {
            tok += c;
        }
    }
    if (!tok.empty())
        parts.push_back(trim_ascii(tok));
    if (parts.empty())
        return false;

    UINT mods = MOD_NOREPEAT;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        const auto m = media::strutil::lower_ascii(parts[i]);
        if (m == "ctrl" || m == "control")
            mods |= MOD_CONTROL;
        else if (m == "alt")
            mods |= MOD_ALT;
        else if (m == "shift")
            mods |= MOD_SHIFT;
        else if (m == "win" || m == "windows")
            mods |= MOD_WIN;
        else
            return false;
    }

    const std::string key = trim_ascii(parts.back());
    const std::string lower_key = media::strutil::lower_ascii(key);
    UINT vk = 0;
    if (key.size() == 1 && std::isalpha(static_cast<unsigned char>(key[0])))
        vk = static_cast<UINT>(std::toupper(static_cast<unsigned char>(key[0])));
    else if (key.size() == 1 && std::isdigit(static_cast<unsigned char>(key[0])))
        vk = static_cast<UINT>(key[0]);
    else {
        static const std::map<std::string, UINT> kSpecial = {
            {"f1", VK_F1}, {"f2", VK_F2}, {"f3", VK_F3}, {"f4", VK_F4},
            {"f5", VK_F5}, {"f6", VK_F6}, {"f7", VK_F7}, {"f8", VK_F8},
            {"f9", VK_F9}, {"f10", VK_F10}, {"f11", VK_F11}, {"f12", VK_F12},
            {"space", VK_SPACE}, {"enter", VK_RETURN}, {"return", VK_RETURN},
            {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE},
            {"delete", VK_DELETE}, {"del", VK_DELETE},
            {"tab", VK_TAB}, {"backspace", VK_BACK},
            {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
            {"plus", VK_OEM_PLUS}, {"minus", VK_OEM_MINUS},
        };
        const auto it = kSpecial.find(lower_key);
        if (it != kSpecial.end()) {
            vk = it->second;
        } else {
            const std::wstring wkey = widen(key);
            if (wkey.size() != 1)
                return false;
            const SHORT scan = ::VkKeyScanW(wkey[0]);
            if (scan == -1)
                return false;
            vk = LOBYTE(scan);
            const UINT implied = HIBYTE(scan);
            if (implied & 1)
                mods |= MOD_SHIFT;
            if (implied & 2)
                mods |= MOD_CONTROL;
            if (implied & 4)
                mods |= MOD_ALT;
        }
    }
    if (!vk)
        return false;
    mods_out = mods;
    vk_out = vk;
    return true;
#endif
}

bool shortcut_down(unsigned mods, unsigned vk)
{
#if !defined(_WIN32)
    (void)mods;
    (void)vk;
    return false;
#else
    if (!key_down(vk))
        return false;
    if ((mods & MOD_CONTROL) && !(key_down(VK_CONTROL) || key_down(VK_LCONTROL) || key_down(VK_RCONTROL)))
        return false;
    if ((mods & MOD_ALT) && !(key_down(VK_MENU) || key_down(VK_LMENU) || key_down(VK_RMENU)))
        return false;
    if ((mods & MOD_SHIFT) && !(key_down(VK_SHIFT) || key_down(VK_LSHIFT) || key_down(VK_RSHIFT)))
        return false;
    if ((mods & MOD_WIN) && !(key_down(VK_LWIN) || key_down(VK_RWIN)))
        return false;
    return true;
#endif
}

} // namespace media::hotkeys

namespace media::app {

std::optional<ResolvedApplication> resolve_first_application(std::string_view, const ResolveRequest&)
{
    return std::nullopt;
}

} // namespace media::app

namespace pm::host::cli {

std::vector<RegisteredCommandInfo> registered_cli_commands() { return {}; }
std::vector<RegisteredAppCommandInfo> registered_app_commands() { return {}; }
std::vector<CustomCommandInfo> visible_custom_commands() { return {}; }

} // namespace pm::host::cli

namespace media::local_models {

std::vector<fs::path> local_model_roots() { return {}; }
std::vector<LocalModelEntry> discover_llama_text_models(bool) { return {}; }
std::vector<LocalModelEntry> discover_llama_vlm_models(bool) { return {}; }

} // namespace media::local_models

namespace media::replicate_cli {

bool fetch_collections(const std::string&, const std::string&,
                       std::vector<CollectionInfo>& out, std::string& err, bool)
{
    out.clear();
    err = "replicate catalog is not built into pm-xblox";
    return false;
}

bool fetch_collection_models(const std::string&, const std::string&, const std::string&,
                             std::vector<ModelInfo>& out, std::string& err, bool)
{
    out.clear();
    err = "replicate catalog is not built into pm-xblox";
    return false;
}

void sort_collections_alpha(std::vector<CollectionInfo>& collections)
{
    std::sort(collections.begin(), collections.end(),
              [](const CollectionInfo& a, const CollectionInfo& b) { return a.name < b.name; });
}

void sort_models_alpha(std::vector<ModelInfo>& models)
{
    std::sort(models.begin(), models.end(),
              [](const ModelInfo& a, const ModelInfo& b) { return a.slug < b.slug; });
}

bool lookup_replicate_openapi_input_flat(const std::string&, nlohmann::json&, nlohmann::json&,
                                         std::string& err)
{
    err = "replicate catalog is not built into pm-xblox";
    return false;
}

} // namespace media::replicate_cli

namespace media::openrouter_cli {

bool list_openrouter_catalog_models(const std::string&, const std::string&,
                                     std::vector<OpenRouterCatalogModelRow>& out,
                                     std::string& err, bool)
{
    out.clear();
    err = "openrouter catalog is not built into pm-xblox";
    return false;
}

} // namespace media::openrouter_cli

namespace media::llm {

std::string llm_fs_guard_deny_reason(const fs::path&)
{
    return {};
}

} // namespace media::llm

namespace media::probe {

ProbeResult probe_file(const fs::path&, const ProbeOptions&)
{
    ProbeResult result;
    result.error = "file probe is not built into pm-xblox";
    return result;
}

} // namespace media::probe

namespace media {

std::string url_suggested_filename(const std::string& url)
{
    std::string path = url;
    const auto query = path.find_first_of("?#");
    if (query != std::string::npos)
        path.erase(query);
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= path.size())
        return {};
    return path.substr(slash + 1);
}

void url_template_variables(const std::string& url, std::string& src_dir,
                            std::string& src_name, std::string& src_ext)
{
    src_dir.clear();
    src_name.clear();
    src_ext.clear();
    const std::string file = url_suggested_filename(url);
    const auto dot = file.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        src_name = file.substr(0, dot);
        src_ext = file.substr(dot);
    } else {
        src_name = file;
    }
    const auto scheme = url.find("://");
    const auto path_at = scheme == std::string::npos ? url.find('/') : url.find('/', scheme + 3);
    if (path_at != std::string::npos)
        src_dir = url.substr(0, path_at);
}

} // namespace media
