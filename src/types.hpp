#pragma once

#include <string>
#include <vector>

namespace media::xblox {

struct CommandRunOutput {
    std::string message;
    std::vector<std::string> stdout_lines;
    std::vector<std::string> stderr_lines;
    bool timed_out = false;
    bool cancelled = false;
};

} // namespace media::xblox
