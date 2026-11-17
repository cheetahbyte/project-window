#pragma once

#import <Metal/Metal.h>

#include "compositor/display.h"
#include "compositor/effects.h"
#include "compositor/occlusion.h"
#include "compositor/region.h"
#include "compositor/scene.h"
#include "protocol/transport.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace pw {

// Which windows keep their buffers. Hidden windows are told so and their buffer is dropped.
enum class ReleasePolicy {
    // Every window stays visible.
    Never,
    // Unmapped windows are hidden.
    Unmapped,
    // Unmapped windows and windows nothing of which shows on the display are hidden.
    Occluded,
};

// Time from telling a client its window is visible again until its first commit after that.
struct RevealStats {
    std::uint64_t count{};
    std::chrono::microseconds total{};
    std::chrono::microseconds max{};
};

// Owns the client connections and the scene. Everything except the receive thread runs on
// the main thread.
class Server {
public:
    Server(id<MTLDevice> device, ReleasePolicy policy, EffectSet effects,
           std::function<void()> on_damage);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    [[nodiscard]] Result<void> start();

    const Scene& scene() const { return scene_; }
    id<MTLTexture> texture(WindowKey key) const;
    // The window's buffer if the window is drawn, otherwise nil. Of a drawn window, the part
    // opaque_pixels names hides what lies below it.
    id<MTLTexture> drawn_texture(const Window& window) const;
    // The part of the window treated as opaque, in display pixels; the whole frame when alpha
    // is off. May have no area. Requires a display.
    Rect opaque_pixels(const Window& window) const;

    void pointer(PointerPhase phase, float x, float y);
    // Call when the display is created and whenever its size or scale changes.
    void set_display(Display display);
    const std::optional<Display>& display() const { return display_; }
    // What changed on the display since the last call, in display pixels.
    [[nodiscard]] Region take_damage();
    const RevealStats& reveal_stats() const { return reveals_; }

private:
    struct Drag {
        WindowKey key{};
        float offset_x{};
        float offset_y{};
    };

    struct VisibilityChange {
        WindowKey key{};
        bool visible{};
    };

    struct Connection {
        ReceiveRight port;
        SendRight events;
    };

    // Messages handed from the receive thread to the main thread.
    struct Inbox {
        std::mutex mutex;
        std::deque<Message> messages;
        bool wake_pending{false};
    };

    void receive_loop(std::stop_token stop, mach_port_t port_set);
    void post(Message message);
    void drain();
    void handle(Message message);
    void accept(SendRight events);
    bool dispatch(mach_port_t connection, Message& message);
    void close(mach_port_t connection);
    id<MTLTexture> make_texture(const SendRight& surface_port, const Window& window);
    void send_input(WindowKey key, PointerPhase phase, float x, float y);
    void handle_pointer(PointerPhase phase, float x, float y);
    void update_visibility();
    std::vector<VisibilityChange> recompute_visibility();
    bool shows(const Window& window, const Occlusion& occlusion) const;
    void damage_pixels(const Rect& rect);
    void damage_if_drawn(const Window& window);

    id<MTLDevice> device_;
    ReleasePolicy policy_;
    EffectSet effects_;
    std::function<void()> on_damage_;
    ReceiveRight service_;
    SendRight wake_;
    PortSet port_set_;

    Scene scene_;
    std::map<mach_port_t, Connection> connections_;
    std::map<WindowKey, id<MTLTexture>> textures_;
    std::optional<Drag> drag_;
    std::optional<WindowKey> pointer_target_;
    // Unknown until the display reports its size; visibility is not computed before that.
    std::optional<Display> display_;
    Region damage_;
    bool visibility_dirty_{false};
    RevealStats reveals_;

    std::shared_ptr<Inbox> inbox_{std::make_shared<Inbox>()};
    std::jthread receiver_;
};

}  // namespace pw
