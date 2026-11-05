#include "Scope_Variables.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include "fs/path_sanitizer.hpp"

namespace fs = std::filesystem;

namespace media::xblox {

std::string scope_os_value()
{
#if defined(_WIN32)
    return "win32";
#elif defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#elif defined(__FreeBSD__)
    return "freebsd";
#elif defined(__OpenBSD__)
    return "openbsd";
#elif defined(__NetBSD__)
    return "netbsd";
#else
    return "unknown";
#endif
}

std::string scope_arch_value()
{
#if defined(__x86_64__) || defined(_M_X64)
    return "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#elif defined(__i386__) || defined(_M_IX86)
    return "ia32";
#elif defined(__ppc64__) || defined(__powerpc64__)
    return "ppc64";
#elif defined(__s390x__)
    return "s390x";
#elif defined(__riscv) && (__riscv_xlen == 64)
    return "riscv64";
#else
    return "unknown";
#endif
}

std::string iso_utc_date(std::time_t t)
{
    std::tm tm_buf{};
#if defined(_WIN32)
    if (gmtime_s(&tm_buf, &t) != 0)
        return {};
#else
    if (!gmtime_r(&t, &tm_buf))
        return {};
#endif
    char buf[32] = {};
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf) == 0)
        return {};
    return buf;
}

std::string scope_cwd_value(const ExecutionOptions& options)
{
    if (!options.default_cwd.empty())
        return media::path::path_to_utf8(options.default_cwd);
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    return ec ? std::string{} : media::path::path_to_utf8(cwd);
}

void seed_default_scope_vars(nlohmann::json& context, const ExecutionOptions& options)
{
    if (!context.is_object())
        context = nlohmann::json::object();
    if (!options.process_args.empty())
        context["argv"] = options.process_args;
    if (!options.process_command_line.empty())
        context["argv_str"] = options.process_command_line;
    context["os"] = scope_os_value();
    context["arch"] = scope_arch_value();
    context["cwd"] = scope_cwd_value(options);

    const auto now = std::chrono::system_clock::now();
    const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    context["epoch"] = static_cast<std::int64_t>(epoch);
    const std::string date = iso_utc_date(std::chrono::system_clock::to_time_t(now));
    if (!date.empty())
        context["date"] = date;
}

double steady_now_ms()
{
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace media::xblox
