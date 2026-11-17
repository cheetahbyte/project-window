#pragma once

#include "protocol/messages.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pw {

struct WindowKey {
    // Names the connection's receive port; does not own it.
    mach_port_t connection{MACH_PORT_NULL};
    std::uint32_t window{};
    auto operator<=>(const WindowKey&) const = default;
};

struct Window {
    WindowKey key{};
    Rect frame{};
    std::uint32_t scale{};
    bool mapped{true};
};

// The windows on the display, in stacking order. Coordinates are points, origin top left.
class Scene {
public:
    bool add(WindowKey key, Rect frame, std::uint32_t scale);
    bool remove(WindowKey key);
    void remove_connection(mach_port_t connection);
    std::size_t count(mach_port_t connection) const;

    Window* find(WindowKey key);
    Window* top_at(float x, float y);
    void raise(WindowKey key);

    std::span<const Window> back_to_front() const { return windows_; }

private:
    std::vector<Window> windows_;
};

}  // namespace pw
