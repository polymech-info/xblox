#pragma once

#include "runtime.hpp"

#include <nlohmann/json.hpp>

#include <ctime>
#include <string>

namespace media::xblox {

std::string scope_os_value();
std::string scope_arch_value();
std::string iso_utc_date(std::time_t t);
std::string scope_cwd_value(const ExecutionOptions& options);
void seed_default_scope_vars(nlohmann::json& context, const ExecutionOptions& options);
double steady_now_ms();

} // namespace media::xblox
