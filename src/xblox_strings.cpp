#include "xblox_strings.hpp"

#include "xblox_expressions.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include "string/string_utils.hpp"

namespace media::xblox {

std::vector<std::string> split_path(const std::string& path)
{
    return expr::split_reference(path);
}

std::string trim_ascii(std::string s)
{
    auto is_ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

std::vector<std::string> split_lines(std::string text)
{
    std::vector<std::string> lines;
    std::string current;
    for (char ch : text) {
        if (ch == '\r')
            continue;
        if (ch == '\n') {
            lines.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty())
        lines.push_back(std::move(current));
    return lines;
}

void append_pipe_text(std::vector<std::string>& lines, const std::string& text)
{
    auto more = split_lines(text);
    lines.insert(lines.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
}

std::string strip_quotes(std::string s)
{
    s = trim_ascii(std::move(s));
    if (s.size() >= 2 && ((s.front() == '\'' && s.back() == '\'') || (s.front() == '"' && s.back() == '"')))
        return s.substr(1, s.size() - 2);
    return s;
}

std::string lower_compact(std::string s)
{
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) {
        return c == '_' || c == '-' || c == ' ';
    }), s.end());
    s = media::strutil::lower_ascii(std::move(s));
    return s;
}

std::vector<std::string> split_cli_command_path(const std::string& cli)
{
    std::vector<std::string> out;
    std::string cur;
    bool in_quote = false;
    char quote = '\0';
    for (char ch : cli) {
        if ((ch == '"' || ch == '\'') && (!in_quote || quote == ch)) {
            in_quote = !in_quote;
            quote = in_quote ? ch : '\0';
            continue;
        }
        if (!in_quote && std::isspace(static_cast<unsigned char>(ch))) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            continue;
        }
        cur.push_back(ch);
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

} // namespace media::xblox
