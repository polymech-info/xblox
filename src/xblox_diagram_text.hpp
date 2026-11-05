#pragma once

#include <sstream>
#include <string>
#include <vector>

namespace media::xblox {

enum class LabelSegmentKind { Label, Group, Variable, Operator };

struct LabelSegment {
    std::string      text;
    LabelSegmentKind kind = LabelSegmentKind::Label;
    double           dx   = 0.0;
};

void append_segment(std::vector<LabelSegment>& segments,
                    std::string text,
                    LabelSegmentKind kind,
                    double dx = 0.0);

std::string segments_to_label(const std::vector<LabelSegment>& segments);

double estimate_segments_width(const std::vector<LabelSegment>& segments, double char_w);

bool string_has_interpolation(const std::string& text);

void split_string_interpolation_segments(std::vector<LabelSegment>& out,
                                         const std::string& text,
                                         bool show_delimiters,
                                         double variable_margin = 0.0);

std::string value_segment_fill(LabelSegmentKind kind,
                               bool has_interpolation,
                               const std::string& label_fill,
                               const std::string& variable_fill);

void append_svg_text_tspans(std::ostringstream& svg,
                            const std::vector<LabelSegment>& segments,
                            bool has_interpolation,
                            const std::string& label_fill,
                            const std::string& variable_fill,
                            double first_dx = 0.0);

} // namespace media::xblox
