#include "xblox/xblox_diagram_text.hpp"

#include <sstream>

namespace media::xblox {

namespace {

std::string xml_escape(std::string s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c; break;
        }
    }
    return out;
}

} // namespace

void append_segment(std::vector<LabelSegment>& segments,
                    std::string text,
                    LabelSegmentKind kind,
                    double dx)
{
    if (text.empty() && dx <= 0.0)
        return;
    if (dx > 0.0 || segments.empty() || segments.back().kind != kind || segments.back().dx > 0.0)
        segments.push_back({std::move(text), kind, dx});
    else
        segments.back().text += text;
}

std::string segments_to_label(const std::vector<LabelSegment>& segments)
{
    std::string out;
    for (const auto& segment : segments)
        out += segment.text;
    return out;
}

double estimate_segments_width(const std::vector<LabelSegment>& segments, double char_w)
{
    double width = 0.0;
    for (const auto& segment : segments) {
        width += char_w * static_cast<double>(segment.text.size());
        width += segment.dx;
    }
    return width;
}

bool string_has_interpolation(const std::string& text)
{
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] == '$' && text[i + 1] == '{') {
            const std::size_t end = text.find('}', i + 2);
            if (end != std::string::npos)
                return true;
        }
    }
    return false;
}

void split_string_interpolation_segments(std::vector<LabelSegment>& out,
                                         const std::string& text,
                                         bool show_delimiters,
                                         double variable_margin)
{
    out.clear();
    if (text.empty())
        return;

    bool prev_was_variable = false;
    std::size_t i          = 0;
    while (i < text.size()) {
        if (text[i] == '$' && i + 1 < text.size() && text[i + 1] == '{') {
            const std::size_t end = text.find('}', i + 2);
            if (end != std::string::npos) {
                const std::string inner = text.substr(i + 2, end - i - 2);
                const std::string shown =
                    show_delimiters ? text.substr(i, end - i + 1) : inner;
                append_segment(out, shown, LabelSegmentKind::Variable, variable_margin);
                prev_was_variable = true;
                i                 = end + 1;
                continue;
            }
        }
        const std::size_t start = i;
        while (i < text.size()) {
            if (text[i] == '$' && i + 1 < text.size() && text[i + 1] == '{')
                break;
            ++i;
        }
        append_segment(out,
                       text.substr(start, i - start),
                       LabelSegmentKind::Label,
                       prev_was_variable ? variable_margin : 0.0);
        prev_was_variable = false;
    }
}

std::string value_segment_fill(LabelSegmentKind kind,
                               bool has_interpolation,
                               const std::string& label_fill,
                               const std::string& variable_fill)
{
    if (kind == LabelSegmentKind::Variable)
        return variable_fill;
    return has_interpolation ? label_fill : variable_fill;
}

void append_svg_text_tspans(std::ostringstream& svg,
                            const std::vector<LabelSegment>& segments,
                            bool has_interpolation,
                            const std::string& label_fill,
                            const std::string& variable_fill,
                            double first_dx)
{
    bool first = true;
    for (const LabelSegment& segment : segments) {
        const std::string fill =
            value_segment_fill(segment.kind, has_interpolation, label_fill, variable_fill);
        svg << "<tspan fill=\"" << fill << "\"";
        if (first) {
            if (first_dx > 0.0)
                svg << " dx=\"" << first_dx << "\"";
            first = false;
        } else if (segment.dx > 0.0) {
            svg << " dx=\"" << segment.dx << "\"";
        }
        svg << ">" << xml_escape(segment.text) << "</tspan>";
    }
}

} // namespace media::xblox
