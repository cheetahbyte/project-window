#include "compositor/occlusion.h"

#include <optional>

namespace pw {

Region Occlusion::uncovered(const Rect& frame) const {
    const std::optional<Rect> shown = intersection(frame, display_);
    if (!shown) return {};
    Region region{*shown};
    for (const Rect& occluder : covered_) {
        region.subtract(occluder);
        if (region.empty()) break;
    }
    return region;
}

}  // namespace pw
