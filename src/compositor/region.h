#pragma once

#include "protocol/messages.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace pw {

// The overlap of two rectangles, or nothing when they share no area.
std::optional<Rect> intersection(const Rect& a, const Rect& b);

// A set of points in the plane, kept as disjoint rectangles. A rectangle with no area is empty.
class Region {
public:
    Region() = default;
    explicit Region(const Rect& rect);

    void add(const Rect& rect);
    void add(const Region& other);
    void subtract(const Rect& rect);
    [[nodiscard]] Region intersected(const Region& other) const;
    void clear() { boxes_.clear(); }

    [[nodiscard]] bool empty() const { return boxes_.empty(); }
    [[nodiscard]] std::size_t size() const { return boxes_.size(); }
    // The disjoint rectangles that make up the region, in no particular order.
    [[nodiscard]] std::vector<Rect> rects() const;

private:
    // Edges are widened so that x + width cannot overflow for any Rect.
    struct Box {
        std::int64_t left{};
        std::int64_t top{};
        std::int64_t right{};
        std::int64_t bottom{};
    };

    static Box box_of(const Rect& rect);
    void add(const Box& box);
    void subtract(const Box& cut);

    std::vector<Box> boxes_;
};

}  // namespace pw
