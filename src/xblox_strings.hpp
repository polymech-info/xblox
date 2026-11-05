#pragma once

#include <string>
#include <vector>

namespace media::xblox {

std::vector<std::string> split_path(const std::string& path);
std::string trim_ascii(std::string s);
std::vector<std::string> split_lines(std::string text);
void append_pipe_text(std::vector<std::string>& lines, const std::string& text);
std::string strip_quotes(std::string s);
std::string lower_compact(std::string s);
std::vector<std::string> split_cli_command_path(const std::string& cli);

} // namespace media::xblox
