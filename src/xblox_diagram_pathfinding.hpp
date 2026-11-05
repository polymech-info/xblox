#pragma once

#include <vector>

namespace media::xblox::diagram_pathfinding {

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

struct Obstacle {
    Vec2 min;
    Vec2 max;

    bool intersects_segment(const Vec2& p1, const Vec2& p2) const;
    bool intersects_horizontal_line(double y, double x1, double x2) const;
    bool intersects_vertical_line(double x, double y1, double y2) const;
};

struct PinContext {
    Vec2 position;
    Vec2 direction;
};

struct NodeContext {
    Vec2 min;
    Vec2 max;
};

struct RoutingContext {
    PinContext              start_pin;
    PinContext              end_pin;
    NodeContext             start_node;
    NodeContext             end_node;
    double                  margin = 12.0;
    std::vector<Obstacle>   obstacles;
};

std::vector<Vec2> generate_waypoints(const RoutingContext& ctx);

} // namespace media::xblox::diagram_pathfinding
