#pragma once

#include <array>
#include <cstddef>
#include <utility>

namespace pw {

enum class Effect { Alpha, Corners, Shadows, Blur, Animation };

inline constexpr std::array kAllEffects{Effect::Alpha, Effect::Corners, Effect::Shadows,
                                        Effect::Blur, Effect::Animation};

// Which visual effects the compositor applies. An effect that is off is ignored, not rejected.
class EffectSet {
public:
    static constexpr EffectSet all() {
        EffectSet set{};
        for (const Effect effect : kAllEffects) set.insert(effect);
        return set;
    }

    [[nodiscard]] constexpr bool contains(Effect effect) const {
        return members_.at(index(effect));
    }
    // Returns false when the effect was already in the set.
    constexpr bool insert(Effect effect) {
        return !std::exchange(members_.at(index(effect)), true);
    }

private:
    static constexpr std::size_t index(Effect effect) {
        return static_cast<std::size_t>(std::to_underlying(effect));
    }

    std::array<bool, kAllEffects.size()> members_{};
};

}  // namespace pw
