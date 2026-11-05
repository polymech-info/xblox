#include "xblox_diagram_pathfinding.hpp"

#include <algorithm>
#include <cmath>

namespace media::xblox::diagram_pathfinding {

namespace {

enum class RoutingStrategy {
    SameBlock,
    ZShape,
    ZShapeAboveBlock,
    ZShapeBelowBlock,
    UShapeAbove,
    UShapeBelow,
    HorizontalFlow,
    AroundObstacles,
    LShape,
    Direct,
};

namespace Constants {
constexpr double MIN_PARALLEL_CLEARANCE     = 20.0;
constexpr double MIN_ABOVE_BLOCK_CLEARANCE  = 20.0;
constexpr double MIN_BELOW_START_CLEARANCE  = 15.0;
constexpr double MIN_SAME_BLOCK_CLEARANCE  = 25.0;
constexpr double HORIZONTAL_FLOW_HEIGHT_THRESHOLD = 10.0;
constexpr double SAME_BLOCK_POSITION_TOLERANCE    = 1.0;
constexpr double EDGE_CLEARANCE                   = 5.0;
} // namespace Constants

bool nearly_equal(double a, double b, double eps = 1.0)
{
    return std::abs(a - b) <= eps;
}

bool nodes_equal(const NodeContext& a, const NodeContext& b)
{
    return nearly_equal(a.min.x, b.min.x, Constants::SAME_BLOCK_POSITION_TOLERANCE)
        && nearly_equal(a.min.y, b.min.y, Constants::SAME_BLOCK_POSITION_TOLERANCE)
        && nearly_equal(a.max.x, b.max.x, Constants::SAME_BLOCK_POSITION_TOLERANCE)
        && nearly_equal(a.max.y, b.max.y, Constants::SAME_BLOCK_POSITION_TOLERANCE);
}

bool is_same_block(const NodeContext& start, const NodeContext& end)
{
    return nodes_equal(start, end);
}

bool nodes_overlap_x(const NodeContext& start, const NodeContext& end)
{
    return start.max.x > end.min.x && start.min.x < end.max.x;
}

bool end_is_below_start(const RoutingContext& ctx)
{
    return ctx.end_node.min.y > ctx.start_node.max.y;
}

bool pin_is_at_top(const PinContext& pin, const NodeContext& node)
{
    const double node_center_y = (node.min.y + node.max.y) * 0.5;
    return pin.position.y < node_center_y;
}

bool pin_is_on_left(const PinContext& pin, const NodeContext& node)
{
    const double distance_to_left  = std::abs(pin.position.x - node.min.x);
    const double distance_to_right = std::abs(pin.position.x - node.max.x);
    return distance_to_left < distance_to_right;
}

double calculate_horizontal_clearance_y(const RoutingContext& ctx, bool above_block)
{
    if (above_block)
        return ctx.end_node.min.y - Constants::MIN_ABOVE_BLOCK_CLEARANCE;

    double waypoint_y = ctx.start_node.max.y + Constants::MIN_PARALLEL_CLEARANCE;
    const double mid_y = (ctx.start_pin.position.y + ctx.end_pin.position.y) * 0.5;
    if (mid_y > waypoint_y)
        waypoint_y = mid_y;
    return std::max(waypoint_y, ctx.start_node.max.y + Constants::MIN_BELOW_START_CLEARANCE);
}

double calculate_vertical_clearance_x(const RoutingContext& ctx, bool left_side)
{
    if (left_side)
        return std::min(ctx.end_node.min.x, ctx.start_node.min.x) - ctx.margin;
    return std::max(ctx.end_node.max.x, ctx.start_node.max.x) + ctx.margin;
}

bool horizontal_segment_intersects_node(const NodeContext& node, double y, double x1, double x2)
{
    const Obstacle obstacle{node.min, node.max};
    const double seg_min_x = std::min(x1, x2);
    const double seg_max_x = std::max(x1, x2);
    return obstacle.intersects_horizontal_line(y, seg_min_x, seg_max_x);
}

bool segment_intersects_obstacles(const Vec2& p1, const Vec2& p2, const RoutingContext& ctx)
{
    for (const Obstacle& obstacle : ctx.obstacles) {
        if (nodes_equal({obstacle.min, obstacle.max}, ctx.start_node)
            || nodes_equal({obstacle.min, obstacle.max}, ctx.end_node))
            continue;
        if (obstacle.intersects_segment(p1, p2))
            return true;
    }
    return false;
}

bool determine_preferred_side(const RoutingContext& ctx)
{
    const bool pin_on_left  = pin_is_on_left(ctx.end_pin, ctx.end_node);
    const bool start_is_left  = ctx.start_pin.position.x < ctx.end_node.min.x;
    const bool start_is_right = ctx.start_pin.position.x > ctx.end_node.max.x;
    if (start_is_left && pin_on_left)
        return true;
    if (start_is_right && !pin_on_left)
        return false;
    return pin_on_left;
}

bool needs_waypoints(const Vec2& start_pos, const Vec2& end_pos, const Vec2& start_dir, const Vec2& end_dir)
{
    return !(start_dir.y > 0.0 && end_dir.y < 0.0 && end_pos.y > start_pos.y);
}

std::vector<Vec2> route_l_shape(const RoutingContext& ctx)
{
    const double mid_y = (ctx.start_pin.position.y + ctx.end_pin.position.y) * 0.5;
    return {{ctx.start_pin.position.x, mid_y}, {ctx.end_pin.position.x, mid_y}};
}

std::vector<Vec2> route_z_shape(const RoutingContext& ctx, float horizontal_y, bool prefer_left);

std::vector<Vec2> route_around_obstacles(const RoutingContext& ctx, bool prefer_left)
{
    std::vector<Vec2> waypoints;
    const bool start_within_end_node_x = ctx.start_pin.position.x >= ctx.end_node.min.x
        && ctx.start_pin.position.x <= ctx.end_node.max.x;
    const bool pin_at_top = pin_is_at_top(ctx.end_pin, ctx.end_node);

    if (start_within_end_node_x && !pin_at_top) {
        const double clear_y = std::max(ctx.start_node.max.y, ctx.end_node.max.y) + ctx.margin;
        waypoints.push_back({ctx.start_pin.position.x, clear_y});
        waypoints.push_back({ctx.end_pin.position.x, clear_y});
        return waypoints;
    }

    const bool route_left = prefer_left;
    const double horizontal_y = ctx.end_node.min.y - Constants::MIN_ABOVE_BLOCK_CLEARANCE;
    const double clear_x = route_left
        ? std::min(ctx.end_node.min.x, ctx.start_node.min.x) - ctx.margin
        : std::max(ctx.end_node.max.x, ctx.start_node.max.x) + ctx.margin;

    if (pin_at_top) {
        const double clear_y = std::max(ctx.start_node.max.y + Constants::MIN_PARALLEL_CLEARANCE,
                                        ctx.end_node.max.y + ctx.margin);
        waypoints.push_back({ctx.start_pin.position.x, clear_y});
        waypoints.push_back({clear_x, clear_y});
        waypoints.push_back({clear_x, horizontal_y});

        const bool intersects_start = horizontal_segment_intersects_node(
            ctx.start_node, horizontal_y, clear_x, ctx.end_pin.position.x);
        const bool intersects_obstacles = segment_intersects_obstacles(
            {clear_x, horizontal_y}, {ctx.end_pin.position.x, horizontal_y}, ctx);
        if (intersects_start || intersects_obstacles) {
            const double intermediate_x = route_left
                ? ctx.start_node.max.x + ctx.margin
                : ctx.start_node.min.x - ctx.margin;
            waypoints.push_back({intermediate_x, horizontal_y});
        }
        waypoints.push_back({ctx.end_pin.position.x, horizontal_y});
    } else {
        const double clear_y = std::max(ctx.start_node.max.y, ctx.end_node.max.y) + ctx.margin;
        waypoints.push_back({ctx.start_pin.position.x, clear_y});
        waypoints.push_back({clear_x, clear_y});
        waypoints.push_back({clear_x, ctx.end_pin.position.y});

        const bool intersects_start = horizontal_segment_intersects_node(
            ctx.start_node, ctx.end_pin.position.y, clear_x, ctx.end_pin.position.x);
        const bool intersects_obstacles = segment_intersects_obstacles(
            {clear_x, ctx.end_pin.position.y}, {ctx.end_pin.position.x, ctx.end_pin.position.y}, ctx);
        if (intersects_start || intersects_obstacles) {
            const double intermediate_x = route_left
                ? ctx.start_node.max.x + ctx.margin
                : ctx.start_node.min.x - ctx.margin;
            waypoints.push_back({intermediate_x, ctx.end_pin.position.y});
        }
        waypoints.push_back({ctx.end_pin.position.x, ctx.end_pin.position.y});
    }

    return waypoints;
}

std::vector<Vec2> route_z_shape(const RoutingContext& ctx, float horizontal_y, bool prefer_left)
{
    const Vec2 waypoint1{ctx.start_pin.position.x, horizontal_y};
    const Vec2 waypoint2{ctx.end_pin.position.x, horizontal_y};

    if (horizontal_segment_intersects_node(ctx.end_node, horizontal_y, waypoint1.x, waypoint2.x)
        || segment_intersects_obstacles(waypoint1, waypoint2, ctx))
        return route_around_obstacles(ctx, prefer_left);

    return {waypoint1, waypoint2};
}

std::vector<Vec2> route_u_shape(const RoutingContext& ctx, bool route_above)
{
    std::vector<Vec2> waypoints;
    if (route_above && pin_is_at_top(ctx.end_pin, ctx.end_node) && !nodes_overlap_x(ctx.start_node, ctx.end_node)) {
        const bool route_left = determine_preferred_side(ctx);
        const double horizontal_y = ctx.end_node.min.y - Constants::MIN_ABOVE_BLOCK_CLEARANCE;
        const double clear_x = calculate_vertical_clearance_x(ctx, route_left);
        const double clear_y = std::max(ctx.start_node.max.y + Constants::MIN_PARALLEL_CLEARANCE,
                                          ctx.end_node.max.y + ctx.margin);
        waypoints.push_back({ctx.start_pin.position.x, clear_y});
        waypoints.push_back({clear_x, clear_y});
        waypoints.push_back({clear_x, horizontal_y});
        waypoints.push_back({ctx.end_pin.position.x, horizontal_y});
        return waypoints;
    }

    const double bottom_y = std::max(ctx.start_node.max.y, ctx.end_node.max.y) + ctx.margin;
    waypoints.push_back({ctx.start_pin.position.x, bottom_y});
    waypoints.push_back({ctx.end_pin.position.x, bottom_y});
    return waypoints;
}

std::vector<Vec2> route_horizontal_flow(const RoutingContext& ctx)
{
    std::vector<Vec2> waypoints;
    const bool start_flows_right = ctx.start_pin.direction.x > 0.0;
    const bool end_accepts_from_left = ctx.end_pin.direction.x < 0.0;

    if (start_flows_right && end_accepts_from_left) {
        if (ctx.end_pin.position.x > ctx.start_pin.position.x) {
            if (std::abs(ctx.end_pin.position.y - ctx.start_pin.position.y)
                > Constants::HORIZONTAL_FLOW_HEIGHT_THRESHOLD) {
                const double mid_x = (ctx.start_pin.position.x + ctx.end_pin.position.x) * 0.5;
                waypoints.push_back({mid_x, ctx.start_pin.position.y});
                waypoints.push_back({mid_x, ctx.end_pin.position.y});
            }
            return waypoints;
        }

        const double right_x = std::max(ctx.start_node.max.x, ctx.end_node.max.x) + ctx.margin;
        const double left_x  = std::min(ctx.start_node.min.x, ctx.end_node.min.x) - ctx.margin;
        const double u_y     = std::max(ctx.start_node.max.y, ctx.end_node.max.y) + ctx.margin;
        waypoints.push_back({right_x, ctx.start_pin.position.y});
        waypoints.push_back({right_x, u_y});
        waypoints.push_back({left_x, u_y});
        waypoints.push_back({left_x, ctx.end_pin.position.y});
        return waypoints;
    }

    const double side_x = start_flows_right
        ? std::max(ctx.start_pin.position.x, ctx.end_pin.position.x) + ctx.margin
        : std::min(ctx.start_pin.position.x, ctx.end_pin.position.x) - ctx.margin;
    waypoints.push_back({side_x, ctx.start_pin.position.y});
    waypoints.push_back({side_x, ctx.end_pin.position.y});
    return waypoints;
}

std::vector<Vec2> route_same_block(const RoutingContext& ctx)
{
    enum class PinEdge { Top, Bottom, Left, Right };
    auto detect_edge = [](const Vec2& dir) -> PinEdge {
        if (std::abs(dir.y) > std::abs(dir.x))
            return dir.y > 0.0 ? PinEdge::Bottom : PinEdge::Top;
        return dir.x > 0.0 ? PinEdge::Right : PinEdge::Left;
    };

    const PinEdge start_edge = detect_edge(ctx.start_pin.direction);
    const PinEdge end_edge   = detect_edge(ctx.end_pin.direction);
    const double top_clear_y    = ctx.start_node.min.y - Constants::MIN_SAME_BLOCK_CLEARANCE;
    const double bottom_clear_y = ctx.start_node.max.y + Constants::MIN_SAME_BLOCK_CLEARANCE;
    const double left_clear_x   = ctx.start_node.min.x - Constants::MIN_SAME_BLOCK_CLEARANCE;
    const double right_clear_x  = ctx.start_node.max.x + Constants::MIN_SAME_BLOCK_CLEARANCE;
    std::vector<Vec2> waypoints;

    if (start_edge == PinEdge::Bottom && end_edge == PinEdge::Top) {
        const bool input_on_left = pin_is_on_left(ctx.end_pin, ctx.end_node);
        if (input_on_left) {
            waypoints.push_back({ctx.start_pin.position.x, bottom_clear_y});
            waypoints.push_back({left_clear_x, bottom_clear_y});
            waypoints.push_back({left_clear_x, top_clear_y});
            waypoints.push_back({ctx.end_pin.position.x, top_clear_y});
        } else {
            waypoints.push_back({ctx.start_pin.position.x, bottom_clear_y});
            waypoints.push_back({right_clear_x, bottom_clear_y});
            waypoints.push_back({right_clear_x, top_clear_y});
            waypoints.push_back({ctx.end_pin.position.x, top_clear_y});
        }
        return waypoints;
    }

    return route_l_shape(ctx);
}

RoutingStrategy select_strategy(const RoutingContext& ctx)
{
    if (is_same_block(ctx.start_node, ctx.end_node))
        return RoutingStrategy::SameBlock;

    if (ctx.start_pin.direction.y > 0.0 && ctx.end_pin.direction.y < 0.0) {
        const bool end_strictly_below = end_is_below_start(ctx);
        const bool end_at_similar_level = !end_strictly_below
            && ctx.end_pin.position.y >= ctx.start_pin.position.y;
        if (end_strictly_below || end_at_similar_level) {
            const double horizontal_y = calculate_horizontal_clearance_y(ctx, false);
            if (!horizontal_segment_intersects_node(ctx.end_node, horizontal_y,
                                                    ctx.start_pin.position.x, ctx.end_pin.position.x)
                && !segment_intersects_obstacles({ctx.start_pin.position.x, horizontal_y},
                                                 {ctx.end_pin.position.x, horizontal_y}, ctx))
                return RoutingStrategy::ZShape;

            const double above_y = calculate_horizontal_clearance_y(ctx, true);
            if (!horizontal_segment_intersects_node(ctx.end_node, above_y,
                                                    ctx.start_pin.position.x, ctx.end_pin.position.x)
                && !segment_intersects_obstacles({ctx.start_pin.position.x, above_y},
                                                 {ctx.end_pin.position.x, above_y}, ctx))
                return RoutingStrategy::ZShapeAboveBlock;
            return RoutingStrategy::AroundObstacles;
        }

        if (pin_is_at_top(ctx.end_pin, ctx.end_node) && !nodes_overlap_x(ctx.start_node, ctx.end_node))
            return RoutingStrategy::UShapeAbove;
        return RoutingStrategy::UShapeBelow;
    }

    if (std::abs(ctx.start_pin.direction.x) > 0.5 && std::abs(ctx.end_pin.direction.x) > 0.5)
        return RoutingStrategy::HorizontalFlow;

    if (!needs_waypoints(ctx.start_pin.position, ctx.end_pin.position,
                         ctx.start_pin.direction, ctx.end_pin.direction))
        return RoutingStrategy::Direct;

    return RoutingStrategy::LShape;
}

} // namespace

bool Obstacle::intersects_segment(const Vec2& p1, const Vec2& p2) const
{
    const double seg_min_x = std::min(p1.x, p2.x);
    const double seg_max_x = std::max(p1.x, p2.x);
    const double seg_min_y = std::min(p1.y, p2.y);
    const double seg_max_y = std::max(p1.y, p2.y);
    if (seg_max_x < min.x || seg_min_x > max.x || seg_max_y < min.y || seg_min_y > max.y)
        return false;
    if (nearly_equal(p1.x, p2.x))
        return intersects_vertical_line(p1.x, seg_min_y, seg_max_y);
    if (nearly_equal(p1.y, p2.y))
        return intersects_horizontal_line(p1.y, seg_min_x, seg_max_x);
    return true;
}

bool Obstacle::intersects_horizontal_line(double y, double x1, double x2) const
{
    if (y < (min.y + Constants::EDGE_CLEARANCE) || y > (max.y - Constants::EDGE_CLEARANCE))
        return false;
    const double seg_min_x = std::min(x1, x2);
    const double seg_max_x = std::max(x1, x2);
    return seg_max_x >= min.x && seg_min_x <= max.x;
}

bool Obstacle::intersects_vertical_line(double x, double y1, double y2) const
{
    if (x < min.x || x > max.x)
        return false;
    const double seg_min_y = std::min(y1, y2);
    const double seg_max_y = std::max(y1, y2);
    return seg_max_y >= min.y && seg_min_y <= max.y;
}

std::vector<Vec2> generate_waypoints(const RoutingContext& ctx)
{
    switch (select_strategy(ctx)) {
        case RoutingStrategy::SameBlock:
            return route_same_block(ctx);
        case RoutingStrategy::ZShape:
            return route_z_shape(ctx, static_cast<float>(calculate_horizontal_clearance_y(ctx, false)),
                                 determine_preferred_side(ctx));
        case RoutingStrategy::ZShapeAboveBlock:
            return route_z_shape(ctx, static_cast<float>(calculate_horizontal_clearance_y(ctx, true)),
                                 determine_preferred_side(ctx));
        case RoutingStrategy::ZShapeBelowBlock:
            return route_z_shape(ctx, static_cast<float>(calculate_horizontal_clearance_y(ctx, false)),
                                 determine_preferred_side(ctx));
        case RoutingStrategy::UShapeAbove:
            return route_u_shape(ctx, true);
        case RoutingStrategy::UShapeBelow:
            return route_u_shape(ctx, false);
        case RoutingStrategy::HorizontalFlow:
            return route_horizontal_flow(ctx);
        case RoutingStrategy::AroundObstacles:
            return route_around_obstacles(ctx, determine_preferred_side(ctx));
        case RoutingStrategy::Direct:
            return {};
        case RoutingStrategy::LShape:
        default:
            return route_l_shape(ctx);
    }
}

} // namespace media::xblox::diagram_pathfinding
