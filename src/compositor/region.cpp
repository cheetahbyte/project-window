#include "compositor/region.h"

#include <algorithm>
#include <utility>

namespace pw {

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

void Region::subtract(const Rect& rect) {
    const Box cut = box_of(rect);
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

}  // namespace pw
