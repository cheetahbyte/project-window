#pragma once

#import <Metal/Metal.h>

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

struct DisplaySize {
    std::int32_t width{};
    std::int32_t height{};
    friend bool operator==(const DisplaySize&, const DisplaySize&) = default;
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
    Server(id<MTLDevice> device, ReleasePolicy policy, std::function<void()> on_damage);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    [[nodiscard]] Result<void> start();

    const Scene& scene() const { return scene_; }
    id<MTLTexture> texture(WindowKey key) const;

    void pointer(PointerPhase phase, float x, float y);
    // In points; call when the display is created and whenever its size changes.
    void set_display_size(DisplaySize size);
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
    bool shows(const Window& window, const std::vector<Rect>& occluders) const;

    id<MTLDevice> device_;
    ReleasePolicy policy_;
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
    std::optional<DisplaySize> display_;
    bool visibility_dirty_{false};
    RevealStats reveals_;

    std::shared_ptr<Inbox> inbox_{std::make_shared<Inbox>()};
    std::jthread receiver_;
};

}  // namespace pw
