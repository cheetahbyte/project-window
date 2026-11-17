#pragma once

#include "compositor/region.h"
#include "protocol/messages.h"

#include <vector>

namespace pw {

// Walks opaque windows from the top down. Each window asks what of it shows past the windows
// covered so far, then covers its own frame if it is drawn. All rectangles share one unit.
class Occlusion {
public:
    explicit Occlusion(const Rect& display) : display_{display} {}

    // The part of the frame on the display that nothing covered so far hides.
    [[nodiscard]] Region uncovered(const Rect& frame) const;
    // The part of the display that nothing covered so far hides.
    [[nodiscard]] Region uncovered_display() const { return uncovered(display_); }
    void cover(const Rect& frame) { covered_.push_back(frame); }

private:
    Rect display_;
    std::vector<Rect> covered_;
};

}  // namespace pw
