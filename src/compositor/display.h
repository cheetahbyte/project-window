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

    // Maps a window's opaque rectangle, in window points and within the frame, to display
    // pixels. Where the display has more pixels per point than the buffer, linear filtering
    // mixes texels across an inner edge, so such edges move inward past every mixed pixel.
    [[nodiscard]] Rect opaque_pixels(const Rect& frame_points, std::uint32_t buffer_scale,
                                     const Rect& opaque_points) const {
        if (opaque_points.width <= 0 || opaque_points.height <= 0) return {};
        const std::int64_t scale{pixels_per_point};
        const std::int64_t buffer{std::max<std::int64_t>(buffer_scale, 1)};
        // A display pixel samples buffer texels within scale / (2 * buffer) - 1/2 of its own.
        const std::int64_t inset{scale > buffer ? (scale + buffer - 1) / (2 * buffer) : 0};
        const std::int64_t opaque_right{std::int64_t{opaque_points.x} + opaque_points.width};
        const std::int64_t opaque_bottom{std::int64_t{opaque_points.y} + opaque_points.height};
        const std::int64_t left{(std::int64_t{frame_points.x} + opaque_points.x) * scale +
                                (opaque_points.x > 0 ? inset : 0)};
        const std::int64_t top{(std::int64_t{frame_points.y} + opaque_points.y) * scale +
                               (opaque_points.y > 0 ? inset : 0)};
        const std::int64_t right{(std::int64_t{frame_points.x} + opaque_right) * scale -
                                 (opaque_right < frame_points.width ? inset : 0)};
        const std::int64_t bottom{(std::int64_t{frame_points.y} + opaque_bottom) * scale -
                                  (opaque_bottom < frame_points.height ? inset : 0)};
        if (left >= right || top >= bottom) return {};
        return {narrow(left), narrow(top), narrow(right - left), narrow(bottom - top)};
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
