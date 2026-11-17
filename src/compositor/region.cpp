#include "compositor/region.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace pw {

namespace {

std::int32_t narrow(std::int64_t value) {
    // Only reachable for rectangles whose far edge already overflows int32 in the input.
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        value, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
}

}  // namespace

std::optional<Rect> intersection(const Rect& a, const Rect& b) {
    const std::int64_t left{std::max(std::int64_t{a.x}, std::int64_t{b.x})};
    const std::int64_t top{std::max(std::int64_t{a.y}, std::int64_t{b.y})};
    const std::int64_t right{std::min(std::int64_t{a.x} + a.width, std::int64_t{b.x} + b.width)};
    const std::int64_t bottom{
        std::min(std::int64_t{a.y} + a.height, std::int64_t{b.y} + b.height)};
    if (left >= right || top >= bottom) return std::nullopt;
    // Both edges lie within one of the inputs, so the result fits the int32 fields again.
    return Rect{static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
                static_cast<std::int32_t>(right - left), static_cast<std::int32_t>(bottom - top)};
}

Region::Box Region::box_of(const Rect& rect) {
    const std::int64_t left{rect.x};
    const std::int64_t top{rect.y};
    return {.left = left,
            .top = top,
            .right = left + std::int64_t{rect.width},
            .bottom = top + std::int64_t{rect.height}};
}

Region::Region(const Rect& rect) {
    const Box box = box_of(rect);
    if (box.left < box.right && box.top < box.bottom) boxes_.push_back(box);
}

void Region::add(const Rect& rect) {
    add(box_of(rect));
}

void Region::add(const Region& other) {
    for (const Box& box : other.boxes_) add(box);
}

// Adds only the parts of the box not already in the region, so the boxes stay disjoint.
void Region::add(const Box& box) {
    if (box.left >= box.right || box.top >= box.bottom) return;
    Region pieces{};
    pieces.boxes_.push_back(box);
    for (const Box& existing : boxes_) {
        pieces.subtract(existing);
        if (pieces.empty()) return;
    }
    boxes_.insert(boxes_.end(), pieces.boxes_.begin(), pieces.boxes_.end());
}

void Region::subtract(const Rect& rect) {
    subtract(box_of(rect));
}

void Region::subtract(const Box& cut) {
    if (cut.left >= cut.right || cut.top >= cut.bottom) return;

    std::vector<Box> remaining;
    remaining.reserve(boxes_.size());
    for (const Box& box : boxes_) {
        if (box.right <= cut.left || cut.right <= box.left || box.bottom <= cut.top ||
            cut.bottom <= box.top) {
            remaining.push_back(box);
            continue;
        }
        if (box.top < cut.top) remaining.push_back({box.left, box.top, box.right, cut.top});
        if (cut.bottom < box.bottom) {
            remaining.push_back({box.left, cut.bottom, box.right, box.bottom});
        }
        const std::int64_t top{std::max(box.top, cut.top)};
        const std::int64_t bottom{std::min(box.bottom, cut.bottom)};
        if (box.left < cut.left) remaining.push_back({box.left, top, cut.left, bottom});
        if (cut.right < box.right) remaining.push_back({cut.right, top, box.right, bottom});
    }
    boxes_ = std::move(remaining);
}

// Pairwise overlaps of two sets of disjoint boxes are themselves disjoint.
Region Region::intersected(const Region& other) const {
    Region result{};
    for (const Box& a : boxes_) {
        for (const Box& b : other.boxes_) {
            const Box overlap{.left = std::max(a.left, b.left),
                              .top = std::max(a.top, b.top),
                              .right = std::min(a.right, b.right),
                              .bottom = std::min(a.bottom, b.bottom)};
            if (overlap.left < overlap.right && overlap.top < overlap.bottom) {
                result.boxes_.push_back(overlap);
            }
        }
    }
    return result;
}

std::vector<Rect> Region::rects() const {
    std::vector<Rect> result;
    result.reserve(boxes_.size());
    for (const Box& box : boxes_) {
        result.push_back({narrow(box.left), narrow(box.top), narrow(box.right - box.left),
                          narrow(box.bottom - box.top)});
    }
    return result;
}

}  // namespace pw
