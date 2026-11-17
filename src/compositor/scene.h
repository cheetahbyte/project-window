#pragma once

#include "protocol/messages.h"

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
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
    // Whether the server keeps a buffer for the window and tells its client to draw.
    bool visible{true};
    // When the server last told the client the window is visible again, until it commits.
    std::optional<std::chrono::steady_clock::time_point> reveal_started;
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
    std::span<Window> back_to_front() { return windows_; }

private:
    std::vector<Window> windows_;
};

}  // namespace pw
