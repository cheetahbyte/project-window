#include "compositor/scene.h"

#include <algorithm>
#include <ranges>

namespace pw {

bool Scene::add(WindowKey key, Rect frame, std::uint32_t scale) {
    if (find(key)) return false;
    windows_.push_back({.key = key, .frame = frame, .scale = scale});
    return true;
}

bool Scene::remove(WindowKey key) {
    return std::erase_if(windows_, [&](const Window& window) { return window.key == key; }) > 0;
}

void Scene::remove_connection(mach_port_t connection) {
    std::erase_if(windows_,
                  [&](const Window& window) { return window.key.connection == connection; });
}

std::size_t Scene::count(mach_port_t connection) const {
    return static_cast<std::size_t>(std::ranges::count(
        windows_, connection, [](const Window& window) { return window.key.connection; }));
}

Window* Scene::find(WindowKey key) {
    auto found = std::ranges::find(windows_, key, &Window::key);
    return found == windows_.end() ? nullptr : &*found;
}

Window* Scene::top_at(float x, float y) {
    for (Window& window : windows_ | std::views::reverse) {
        if (!window.mapped) continue;
        const Rect& frame = window.frame;
        const float left{static_cast<float>(frame.x)};
        const float top{static_cast<float>(frame.y)};
        if (x >= left && x < left + static_cast<float>(frame.width) && y >= top &&
            y < top + static_cast<float>(frame.height)) {
            return &window;
        }
    }
    return nullptr;
}

void Scene::raise(WindowKey key) {
    auto found = std::ranges::find(windows_, key, &Window::key);
    if (found != windows_.end()) std::rotate(found, found + 1, windows_.end());
}

}  // namespace pw
