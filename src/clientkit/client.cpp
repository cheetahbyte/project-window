#include "clientkit/client.h"

#include "protocol/transport.h"

#include <IOSurface/IOSurface.h>
#include <mach/notify.h>
#include <servers/bootstrap.h>

#include <thread>

namespace pw {

namespace {

constexpr std::int32_t kPixelFormatBGRA = 0x42475241;
constexpr auto kHandshakeTimeout = std::chrono::milliseconds{2000};
constexpr auto kStartupWait = std::chrono::milliseconds{3000};
constexpr auto kStartupPoll = std::chrono::milliseconds{50};

// A client launched alongside the compositor can run before the service name is published,
// so an unknown name is retried for a short while.
[[nodiscard]] Result<SendRight> look_up_compositor() {
    const auto deadline = std::chrono::steady_clock::now() + kStartupWait;
    for (;;) {
        Result<SendRight> service = look_up_service(kServiceName);
        if (service || service.error() != BOOTSTRAP_UNKNOWN_SERVICE ||
            std::chrono::steady_clock::now() >= deadline) {
            return service;
        }
        std::this_thread::sleep_for(kStartupPoll);
    }
}

[[nodiscard]] bool set_number(CFMutableDictionaryRef dictionary, CFStringRef key,
                              std::int32_t value) {
    const CfOwner<CFNumberRef> number{CFNumberCreate(nullptr, kCFNumberSInt32Type, &value)};
    if (!number) return false;
    CFDictionarySetValue(dictionary, key, number.get());
    return true;
}

}  // namespace

std::optional<Surface> Surface::create(int width, int height) {
    const CfOwner<CFMutableDictionaryRef> properties{CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)};
    if (!properties) return std::nullopt;
    if (!set_number(properties.get(), kIOSurfaceWidth, width) ||
        !set_number(properties.get(), kIOSurfaceHeight, height) ||
        !set_number(properties.get(), kIOSurfaceBytesPerElement, 4) ||
        !set_number(properties.get(), kIOSurfacePixelFormat, kPixelFormatBGRA)) {
        return std::nullopt;
    }
    CfOwner<IOSurfaceRef> surface{IOSurfaceCreate(properties.get())};
    if (!surface) return std::nullopt;
    return Surface{std::move(surface)};
}

int Surface::width() const {
    return static_cast<int>(IOSurfaceGetWidth(surface_.get()));
}

int Surface::height() const {
    return static_cast<int>(IOSurfaceGetHeight(surface_.get()));
}

std::size_t Surface::stride() const {
    return IOSurfaceGetBytesPerRow(surface_.get()) / sizeof(std::uint32_t);
}

std::span<std::uint32_t> Surface::pixels() const {
    auto* base = static_cast<std::uint32_t*>(IOSurfaceGetBaseAddress(surface_.get()));
    return {base, stride() * IOSurfaceGetHeight(surface_.get())};
}

Result<void> Surface::lock() {
    return as_result(IOSurfaceLock(surface_.get(), 0, nullptr));
}

Surface::UnlockGuard::~UnlockGuard() {
    IOSurfaceUnlock(surface, 0, nullptr);
}

Result<Client> Client::connect() {
    const Result<SendRight> service = look_up_compositor();
    if (!service) return std::unexpected{service.error()};
    Result<ReceiveRight> events = ReceiveRight::allocate();
    if (!events) return std::unexpected{events.error()};

    if (const Result<void> sent = send(*service, Hello{.version = kProtocolVersion}, *events);
        !sent) {
        return std::unexpected{sent.error()};
    }

    Result<Message> reply = receive(events->name(), kHandshakeTimeout);
    if (!reply) return std::unexpected{reply.error()};
    std::optional<Received<Welcome>> welcome = reply->take<Welcome>();
    if (!welcome || welcome->payload.version != kProtocolVersion) {
        return std::unexpected{KERN_FAILURE};
    }

    if (const Result<void> watched = notify_when_dead(welcome->port, *events); !watched) {
        return std::unexpected{watched.error()};
    }
    return Client{std::move(welcome->port), *std::move(events)};
}

Result<void> Client::create_window(std::uint32_t window, Rect frame, std::uint32_t scale) {
    return send(connection_, CreateWindow{.window = window, .frame = frame, .scale = scale});
}

Result<void> Client::attach_buffer(std::uint32_t window, const Surface& surface) {
    const mach_port_t port = IOSurfaceCreateMachPort(surface.get());
    if (!MACH_PORT_VALID(port)) return std::unexpected{KERN_FAILURE};
    return send(connection_, AttachBuffer{.window = window}, SendRight::adopt(port));
}

Result<void> Client::commit(std::uint32_t window, Rect damage) {
    return send(connection_, Commit{.window = window, .damage = damage});
}

Result<void> Client::destroy_window(std::uint32_t window) {
    return send(connection_, DestroyWindow{.window = window});
}

Result<void> Client::set_mapped(std::uint32_t window, bool mapped) {
    return send(connection_, SetMapped{.window = window, .mapped = mapped ? 1U : 0U});
}

Result<void> Client::move_window(std::uint32_t window, std::int32_t x, std::int32_t y) {
    return send(connection_, MoveWindow{.window = window, .x = x, .y = y});
}

Result<void> Client::sync(std::chrono::milliseconds timeout) {
    const std::uint32_t serial{next_serial_++};
    if (const Result<void> sent = send(connection_, Sync{.serial = serial}); !sent) return sent;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds{0}) return std::unexpected{MACH_RCV_TIMED_OUT};
        Result<Message> message = receive(events_.name(), remaining);
        if (!message) return std::unexpected{message.error()};
        if (const auto synced = message->take<Synced>()) {
            if (synced->payload.serial == serial) return {};
            continue;
        }
        if (std::optional<Event> event = decode(*message)) {
            const bool disconnected{std::holds_alternative<Disconnected>(*event)};
            pending_.push_back(*event);
            if (disconnected) return std::unexpected{MACH_SEND_INVALID_DEST};
        }
    }
}

std::optional<Event> Client::next_event(std::optional<std::chrono::milliseconds> timeout) {
    if (!pending_.empty()) {
        const Event event{pending_.front()};
        pending_.pop_front();
        return event;
    }
    Result<Message> message = receive(events_.name(), timeout);
    if (!message) return std::nullopt;
    return decode(*message);
}

std::optional<Event> Client::decode(Message& message) {
    if (const auto configure = message.take<Configure>()) return configure->payload;
    if (const auto input = message.take<Input>()) return input->payload;
    if (message.id() == MACH_NOTIFY_DEAD_NAME) {
        // The notification adds one user reference to the dead name; drop it so that connection_
        // is left with exactly the one reference it owns and releases on destruction.
        static_cast<void>(mach_port_deallocate(mach_task_self(), connection_.name()));
        return Disconnected{};
    }
    return std::nullopt;
}

}  // namespace pw
