#pragma once

#include "protocol/messages.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace pw {

inline constexpr std::int32_t kMaxDisplayScale = 4;

// In points.
struct DisplaySize {
    std::int32_t width{};
    std::int32_t height{};
    friend bool operator==(const DisplaySize&, const DisplaySize&) = default;
};

struct PixelSize {
    std::int32_t width{};
    std::int32_t height{};
    friend bool operator==(const PixelSize&, const PixelSize&) = default;
};

// The display's size in points and how many pixels make up a point along each axis.
// Points are at most kMaxDimension and the scale at most kMaxDisplayScale.
struct Display {
    DisplaySize points{};
    std::int32_t pixels_per_point{1};
    friend bool operator==(const Display&, const Display&) = default;

    [[nodiscard]] PixelSize pixels() const {
        return {.width = points.width * pixels_per_point,
                .height = points.height * pixels_per_point};
    }
    [[nodiscard]] Rect point_bounds() const { return {0, 0, points.width, points.height}; }
    [[nodiscard]] Rect pixel_bounds() const {
        const PixelSize size{pixels()};
        return {0, 0, size.width, size.height};
    }

    // For a window frame that passed valid_frame, so every product fits in int32.
    [[nodiscard]] Rect frame_pixels(const Rect& frame_points) const {
        return {frame_points.x * pixels_per_point, frame_points.y * pixels_per_point,
                frame_points.width * pixels_per_point, frame_points.height * pixels_per_point};
    }

    // Maps a damage rectangle in buffer pixels of a window drawn at buffer_scale pixels per
    // point to display pixels, rounding outward when the scales differ. The damage must lie
    // within the buffer, whose sides are at most kMaxDimension.
    [[nodiscard]] Rect damage_pixels(const Rect& frame_points, std::uint32_t buffer_scale,
                                     const Rect& damage_buffer) const {
        const std::int64_t scale{pixels_per_point};
        const std::int64_t divisor{std::max<std::int64_t>(buffer_scale, 1)};
        const auto floor_div = [divisor](std::int64_t value) { return value / divisor; };
        const auto ceil_div = [divisor](std::int64_t value) {
            return (value + divisor - 1) / divisor;
        };
        const std::int64_t origin_x{std::int64_t{frame_points.x} * scale};
        const std::int64_t origin_y{std::int64_t{frame_points.y} * scale};
        const std::int64_t left{origin_x + floor_div(std::int64_t{damage_buffer.x} * scale)};
        const std::int64_t top{origin_y + floor_div(std::int64_t{damage_buffer.y} * scale)};
        const std::int64_t right{
            origin_x +
            ceil_div((std::int64_t{damage_buffer.x} + damage_buffer.width) * scale)};
        const std::int64_t bottom{
            origin_y +
            ceil_div((std::int64_t{damage_buffer.y} + damage_buffer.height) * scale)};
        return {narrow(left), narrow(top), narrow(right - left), narrow(bottom - top)};
    }

private:
    static std::int32_t narrow(std::int64_t value) {
        return static_cast<std::int32_t>(std::clamp<std::int64_t>(
            value, std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max()));
    }
};

}  // namespace pw
