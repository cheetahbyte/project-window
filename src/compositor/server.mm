#import "compositor/server.h"

#include "compositor/cf_owner.h"
#include "compositor/occlusion.h"

#import <IOSurface/IOSurface.h>

#include <dispatch/dispatch.h>
#include <mach/notify.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ranges>
#include <utility>

namespace pw {

namespace {

constexpr std::size_t kMaxConnections = 256;
constexpr std::size_t kMaxWindowsPerConnection = 1024;
constexpr OSType kPixelFormatBGRA = 0x42475241;
// Dragging inside this strip at the top of a window moves it; matches a standard title bar.
constexpr float kTitleBarHeight = 28;

// Sent by the server to its own service port to wake the receive thread so it sees a stop
// request. Private to the compositor; clients never send it, and one that does is ignored.
struct Wake {
    static constexpr MessageId id = static_cast<MessageId>(3000);
    static constexpr bool carries_port = false;
};

template <class Payload>
void post_event(const SendRight& events, const Payload& payload) {
    // Events are advisory: a client whose queue is full misses them rather than stalling the
    // compositor, and a client that has gone away is closed by its no-senders notification.
    static_cast<void>(send(events, payload, SendMode::DropIfFull));
}

}  // namespace

Server::Server(id<MTLDevice> device, ReleasePolicy policy, std::function<void()> on_damage)
    : device_{device}, policy_{policy}, on_damage_{std::move(on_damage)} {}

Server::~Server() {
    if (!receiver_.joinable()) return;
    receiver_.request_stop();
    // A full queue drops the wake, which is fine: the thread then has messages to receive
    // and checks the stop request after the next one.
    if (Result<void> woken = send(wake_, Wake{}, SendMode::DropIfFull);
        !woken && woken.error() != MACH_SEND_TIMED_OUT) {
        std::fprintf(stderr, "compositor: cannot wake receive thread: %s\n",
                     describe(woken.error()).c_str());
        // Destroying the port set ends the thread's pending receive with an error instead.
        port_set_.reset();
    }
    receiver_.join();
}

Result<void> Server::start() {
    Result<ReceiveRight> service = register_service(kServiceName);
    if (!service) return std::unexpected{service.error()};
    Result<PortSet> port_set = PortSet::allocate();
    if (!port_set) return std::unexpected{port_set.error()};
    if (Result<void> inserted = port_set->insert(*service); !inserted) return inserted;
    Result<SendRight> wake = service->make_send();
    if (!wake) return std::unexpected{wake.error()};

    service_ = std::move(*service);
    port_set_ = std::move(*port_set);
    wake_ = std::move(*wake);
    receiver_ = std::jthread{[this, set = port_set_.name()](std::stop_token stop) {
        receive_loop(std::move(stop), set);
    }};
    return {};
}

id<MTLTexture> Server::texture(WindowKey key) const {
    auto found = textures_.find(key);
    return found == textures_.end() ? nil : found->second;
}

id<MTLTexture> Server::drawn_texture(const Window& window) const {
    if (!window.mapped || !window.visible) return nil;
    return texture(window.key);
}

Region Server::take_damage() {
    return std::exchange(damage_, Region{});
}

void Server::damage_pixels(const Rect& rect) {
    if (!display_) return;
    const std::optional<Rect> shown = intersection(rect, display_->pixel_bounds());
    if (!shown) return;
    damage_.add(*shown);
    on_damage_();
}

void Server::damage_if_drawn(const Window& window) {
    if (!display_ || !drawn_texture(window)) return;
    damage_pixels(display_->frame_pixels(window.frame));
}

void Server::receive_loop(std::stop_token stop, mach_port_t port_set) {
    while (!stop.stop_requested()) {
        Result<Message> message = receive(port_set);
        if (stop.stop_requested()) return;
        if (!message) {
            // An oversized message is dequeued and destroyed by the kernel; keep serving.
            if (message.error() == MACH_RCV_TOO_LARGE) continue;
            std::fprintf(stderr, "compositor: receive failed: %s\n",
                         describe(message.error()).c_str());
            return;
        }
        post(std::move(*message));
    }
}

void Server::post(Message message) {
    bool schedule{false};
    {
        const std::scoped_lock lock{inbox_->mutex};
        inbox_->messages.push_back(std::move(message));
        schedule = !std::exchange(inbox_->wake_pending, true);
    }
    if (!schedule) return;
    // The block may run after the server is gone; the weak reference tells it so.
    const std::weak_ptr<Inbox> inbox{inbox_};
    dispatch_async(dispatch_get_main_queue(), ^{
        if (inbox.lock()) drain();
    });
}

void Server::drain() {
    std::deque<Message> messages;
    {
        const std::scoped_lock lock{inbox_->mutex};
        messages.swap(inbox_->messages);
        inbox_->wake_pending = false;
    }
    while (!messages.empty()) {
        Message message{std::move(messages.front())};
        messages.pop_front();
        handle(std::move(message));
    }
    update_visibility();
}

void Server::handle(Message message) {
    const mach_port_t local{message.arrival_port()};

    if (local == service_.name()) {
        std::optional<Received<Hello>> hello = message.take<Hello>();
        if (hello && hello->payload.version == kProtocolVersion &&
            connections_.size() < kMaxConnections) {
            accept(std::move(hello->port));
        }
        return;
    }

    if (!connections_.contains(local)) return;
    if (message.id() == MACH_NOTIFY_NO_SENDERS) {
        close(local);
        return;
    }
    if (!dispatch(local, message)) close(local);
}

void Server::accept(SendRight events) {
    Result<ReceiveRight> port = ReceiveRight::allocate();
    if (!port) return;
    if (!port_set_.insert(*port)) return;
    if (!send(events, Welcome{.version = kProtocolVersion}, *port, SendMode::DropIfFull)) return;
    // Fires once the client holds no send right any more, which includes the client dying.
    // Without it a dead client would never be cleaned up, so refuse the connection.
    if (!port->notify_when_no_senders()) return;
    const mach_port_t name{port->name()};
    connections_.emplace(name, Connection{std::move(*port), std::move(events)});
}

// Returns false on a protocol violation. Port rights of a rejected message are released when
// the message is destroyed.
bool Server::dispatch(mach_port_t connection, Message& message) {
    if (std::optional<Received<CreateWindow>> create = message.take<CreateWindow>()) {
        const CreateWindow& payload = create->payload;
        if (!valid_frame(payload.frame) || payload.scale < 1 || payload.scale > kMaxScale) {
            return false;
        }
        if (scene_.count(connection) >= kMaxWindowsPerConnection) return false;
        if (!scene_.add({connection, payload.window}, payload.frame, payload.scale)) return false;
        visibility_dirty_ = true;
        return true;
    }

    if (std::optional<Received<AttachBuffer>> attach = message.take<AttachBuffer>()) {
        const WindowKey key{connection, attach->payload.window};
        const Window* window = scene_.find(key);
        if (!window) return false;
        id<MTLTexture> texture = make_texture(attach->port, *window);
        if (!texture) return false;
        // The client sent it before reading that the window is hidden, and frees it once it does.
        if (!window->visible) return true;
        textures_[key] = texture;
        damage_if_drawn(*window);
        visibility_dirty_ = true;
        return true;
    }

    if (std::optional<Received<Commit>> commit = message.take<Commit>()) {
        const WindowKey key{connection, commit->payload.window};
        Window* window = scene_.find(key);
        if (!window) return false;
        id<MTLTexture> texture = this->texture(key);
        // A hidden window's buffer is dropped, and its client may commit before it learns that.
        if (!texture) return true;
        const Rect& damage = commit->payload.damage;
        if (damage.x < 0 || damage.y < 0 || damage.width < 0 || damage.height < 0 ||
            damage.width > kMaxDimension || damage.height > kMaxDimension ||
            static_cast<std::uint64_t>(damage.x) + static_cast<std::uint64_t>(damage.width) >
                texture.width ||
            static_cast<std::uint64_t>(damage.y) + static_cast<std::uint64_t>(damage.height) >
                texture.height) {
            return false;
        }
        if (window->reveal_started) {
            const std::chrono::microseconds elapsed{
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - *window->reveal_started)};
            window->reveal_started.reset();
            ++reveals_.count;
            reveals_.total += elapsed;
            reveals_.max = std::max(reveals_.max, elapsed);
        }
        if (display_ && drawn_texture(*window)) {
            damage_pixels(display_->damage_pixels(window->frame, window->scale, damage));
        }
        return true;
    }

    if (std::optional<Received<DestroyWindow>> destroy = message.take<DestroyWindow>()) {
        const WindowKey key{connection, destroy->payload.window};
        const Window* window = scene_.find(key);
        if (!window) return false;
        damage_if_drawn(*window);
        scene_.remove(key);
        textures_.erase(key);
        if (drag_ && drag_->key == key) drag_.reset();
        if (pointer_target_ == key) pointer_target_.reset();
        visibility_dirty_ = true;
        return true;
    }

    if (std::optional<Received<SetMapped>> set = message.take<SetMapped>()) {
        const WindowKey key{connection, set->payload.window};
        Window* window = scene_.find(key);
        if (!window || set->payload.mapped > 1) return false;
        const bool mapped{set->payload.mapped == 1};
        if (window->mapped == mapped) return true;
        damage_if_drawn(*window);
        window->mapped = mapped;
        damage_if_drawn(*window);
        if (!mapped) {
            if (drag_ && drag_->key == key) drag_.reset();
            if (pointer_target_ == key) pointer_target_.reset();
        }
        visibility_dirty_ = true;
        return true;
    }

    if (std::optional<Received<MoveWindow>> move = message.take<MoveWindow>()) {
        const WindowKey key{connection, move->payload.window};
        Window* window = scene_.find(key);
        if (!window) return false;
        Rect moved{window->frame};
        moved.x = move->payload.x;
        moved.y = move->payload.y;
        if (!valid_frame(moved)) return false;
        damage_if_drawn(*window);
        window->frame = moved;
        damage_if_drawn(*window);
        visibility_dirty_ = true;
        return true;
    }

    if (std::optional<Received<Sync>> sync = message.take<Sync>()) {
        post_event(connections_.at(connection).events, Synced{.serial = sync->payload.serial});
        return true;
    }

    return false;
}

void Server::close(mach_port_t connection) {
    for (const Window& window : scene_.back_to_front()) {
        if (window.key.connection == connection) damage_if_drawn(window);
    }
    scene_.remove_connection(connection);
    std::erase_if(textures_,
                  [&](const auto& entry) { return entry.first.connection == connection; });
    if (drag_ && drag_->key.connection == connection) drag_.reset();
    if (pointer_target_ && pointer_target_->connection == connection) pointer_target_.reset();
    connections_.erase(connection);
    visibility_dirty_ = true;
}

void Server::set_display(Display display) {
    if (display_ == display) return;
    display_ = display;
    damage_pixels(display.pixel_bounds());
    visibility_dirty_ = true;
    update_visibility();
}

// Repeats because closing a connection that missed an event can reveal what it covered.
// Each repeat follows a close, so this ends once every remaining client keeps up.
void Server::update_visibility() {
    if (!display_) return;
    while (visibility_dirty_) {
        visibility_dirty_ = false;
        // Holds keys only: a close below erases windows and connections.
        const std::vector<VisibilityChange> changes = recompute_visibility();
        for (const VisibilityChange& change : changes) {
            const auto connection = connections_.find(change.key.connection);
            if (connection == connections_.end()) continue;
            const Visibility event{.window = change.key.window,
                                   .visible = change.visible ? 1U : 0U};
            // Unlike other events this is state the client must see, so one that cannot
            // take it is treated as unresponsive.
            if (!send(connection->second.events, event, SendMode::DropIfFull)) {
                close(change.key.connection);
                continue;
            }
            if (!change.visible) continue;
            if (Window* window = scene_.find(change.key)) {
                window->reveal_started = std::chrono::steady_clock::now();
            }
        }
    }
}

// Updates every window's visible flag and drops the buffers of windows that became hidden,
// without touching connections. Returns the transitions the clients still need to hear about.
std::vector<Server::VisibilityChange> Server::recompute_visibility() {
    std::vector<VisibilityChange> changes;
    if (policy_ == ReleasePolicy::Never || !display_) return changes;
    Occlusion occlusion{display_->point_bounds()};
    for (Window& window : scene_.back_to_front() | std::views::reverse) {
        const bool visible{shows(window, occlusion)};
        if (visible != window.visible) {
            damage_if_drawn(window);
            window.visible = visible;
            changes.push_back({window.key, visible});
            if (!visible) {
                textures_.erase(window.key);
                window.reveal_started.reset();
                if (pointer_target_ == window.key) pointer_target_.reset();
                // A drag goes on, so a window dragged off the display can be dragged back.
            }
        }
        if (drawn_texture(window)) occlusion.cover(window.frame);
    }
    return changes;
}

bool Server::shows(const Window& window, const Occlusion& occlusion) const {
    switch (policy_) {
    case ReleasePolicy::Never:
        return true;
    case ReleasePolicy::Unmapped:
        return window.mapped;
    case ReleasePolicy::Occluded:
        return window.mapped && !occlusion.uncovered(window.frame).empty();
    }
    return true;
}

id<MTLTexture> Server::make_texture(const SendRight& surface_port, const Window& window) {
    const CfOwner<IOSurfaceRef> surface{IOSurfaceLookupFromMachPort(surface_port.name())};
    if (!surface) return nil;

    const std::size_t width{IOSurfaceGetWidth(surface.get())};
    const std::size_t height{IOSurfaceGetHeight(surface.get())};
    if (IOSurfaceGetPixelFormat(surface.get()) != kPixelFormatBGRA ||
        IOSurfaceGetPlaneCount(surface.get()) != 0 ||
        width > static_cast<std::size_t>(kMaxDimension) ||
        height > static_cast<std::size_t>(kMaxDimension) ||
        width != static_cast<std::size_t>(window.frame.width) * window.scale ||
        height != static_cast<std::size_t>(window.frame.height) * window.scale) {
        return nil;
    }
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    return [device_ newTextureWithDescriptor:descriptor iosurface:surface.get() plane:0];
}

void Server::send_input(WindowKey key, PointerPhase phase, float x, float y) {
    const Window* window = scene_.find(key);
    if (!window) return;
    post_event(connections_.at(key.connection).events,
               Input{.window = key.window,
                     .phase = phase,
                     .x = x - static_cast<float>(window->frame.x),
                     .y = y - static_cast<float>(window->frame.y)});
}

void Server::pointer(PointerPhase phase, float x, float y) {
    handle_pointer(phase, x, y);
    update_visibility();
}

void Server::handle_pointer(PointerPhase phase, float x, float y) {
    switch (phase) {
    case PointerPhase::Down: {
        const Window* hit = scene_.top_at(x, y);
        if (!hit) return;
        const WindowKey key{hit->key};
        const Rect frame{hit->frame};
        scene_.raise(key);
        if (const Window* raised = scene_.find(key)) damage_if_drawn(*raised);
        visibility_dirty_ = true;
        if (y - static_cast<float>(frame.y) < kTitleBarHeight) {
            drag_ = Drag{key, x - static_cast<float>(frame.x), y - static_cast<float>(frame.y)};
        } else {
            pointer_target_ = key;
            send_input(key, phase, x, y);
        }
        return;
    }
    case PointerPhase::Move:
        if (drag_) {
            Window* window = scene_.find(drag_->key);
            if (!window) return;
            Rect moved{window->frame};
            moved.x = static_cast<std::int32_t>(x - drag_->offset_x);
            moved.y = static_cast<std::int32_t>(y - drag_->offset_y);
            if (!valid_frame(moved)) return;
            damage_if_drawn(*window);
            window->frame = moved;
            damage_if_drawn(*window);
            visibility_dirty_ = true;
            post_event(connections_.at(drag_->key.connection).events,
                       Configure{.window = drag_->key.window, .frame = moved});
        } else if (pointer_target_) {
            send_input(*pointer_target_, phase, x, y);
        }
        return;
    case PointerPhase::Up:
        if (pointer_target_) send_input(*pointer_target_, phase, x, y);
        drag_.reset();
        pointer_target_.reset();
        return;
    }
}

}  // namespace pw
