#pragma once

#include "protocol/messages.h"
#include "protocol/ports.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <variant>

namespace pw {

struct CfRelease {
    void operator()(CFTypeRef object) const { CFRelease(object); }
};

template <class Ref>
using CfOwner = std::unique_ptr<std::remove_pointer_t<Ref>, CfRelease>;

// A BGRA8 pixel buffer that can be shared with the compositor. Pixels are 0xAARRGGBB.
class Surface {
public:
    [[nodiscard]] static std::optional<Surface> create(int width, int height);

    [[nodiscard]] IOSurfaceRef get() const { return surface_.get(); }
    [[nodiscard]] int width() const;
    [[nodiscard]] int height() const;

    // Calls draw(pixels, stride) with the buffer locked; pixels covers every row including
    // padding and stride is in pixels per row. The lock is released even if draw throws.
    template <class Draw>
    [[nodiscard]] Result<void> draw(Draw&& draw) {
        const Result<void> locked = lock();
        if (!locked) return locked;
        const UnlockGuard guard{surface_.get()};
        draw(pixels(), stride());
        return {};
    }

private:
    struct UnlockGuard {
        IOSurfaceRef surface;
        UnlockGuard(IOSurfaceRef locked) : surface{locked} {}
        UnlockGuard(const UnlockGuard&) = delete;
        UnlockGuard& operator=(const UnlockGuard&) = delete;
        ~UnlockGuard();
    };

    explicit Surface(CfOwner<IOSurfaceRef> surface) : surface_{std::move(surface)} {}
    [[nodiscard]] Result<void> lock();
    [[nodiscard]] std::span<std::uint32_t> pixels() const;
    [[nodiscard]] std::size_t stride() const;

    CfOwner<IOSurfaceRef> surface_;
};

struct Disconnected {};
using Event = std::variant<Configure, Input, Disconnected>;

class Client {
public:
    [[nodiscard]] static Result<Client> connect();

    Client(Client&&) noexcept = default;
    Client& operator=(Client&&) = delete;

    [[nodiscard]] Result<void> create_window(std::uint32_t window, Rect frame, std::uint32_t scale);
    [[nodiscard]] Result<void> attach_buffer(std::uint32_t window, const Surface& surface);
    [[nodiscard]] Result<void> commit(std::uint32_t window, Rect damage);
    [[nodiscard]] Result<void> destroy_window(std::uint32_t window);

    // Waits for the next event; without a timeout it waits indefinitely. Returns nothing when
    // the wait timed out or an unknown message was skipped.
    [[nodiscard]] std::optional<Event> next_event(
        std::optional<std::chrono::milliseconds> timeout = std::nullopt);

private:
    Client(SendRight connection, ReceiveRight events)
        : connection_{std::move(connection)}, events_{std::move(events)} {}

    SendRight connection_;
    ReceiveRight events_;
};

}  // namespace pw
