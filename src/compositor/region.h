#pragma once

#include "protocol/messages.h"

#include <cstdint>
#include <vector>

namespace pw {

// A set of points in the plane, kept as disjoint rectangles. A rectangle with no area is empty.
class Region {
public:
    Region() = default;
    explicit Region(const Rect& rect);

    void subtract(const Rect& rect);
    [[nodiscard]] bool empty() const { return boxes_.empty(); }

private:
    // Edges are widened so that x + width cannot overflow for any Rect.
    struct Box {
        std::int64_t left{};
        std::int64_t top{};
        std::int64_t right{};
        std::int64_t bottom{};
    };

    static Box box_of(const Rect& rect);

    std::vector<Box> boxes_;
};

}  // namespace pw
