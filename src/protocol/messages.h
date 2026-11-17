#pragma once

#include <mach/mach.h>

#include <cstdint>
#include <string_view>

namespace pw {

inline constexpr std::string_view kServiceName{"dev.projectwindow.compositor"};
inline constexpr std::uint32_t kProtocolVersion = 1;

inline constexpr std::int32_t kMaxCoordinate = 32768;
inline constexpr std::int32_t kMaxDimension = 16384;
inline constexpr std::uint32_t kMaxScale = 4;

enum class MessageId : mach_msg_id_t {
    Hello = 1000,
    CreateWindow,
    AttachBuffer,
    Commit,
    DestroyWindow,
    SetMapped,
    MoveWindow,
    Sync,

    Welcome = 2000,
    Configure,
    Input,
    Synced,
};

enum class PointerPhase : std::uint32_t { Down, Move, Up };

#pragma pack(push, 4)

struct Rect {
    std::int32_t x, y, width, height;
};

// Client to server. Hello carries the client's event port; AttachBuffer carries an IOSurface port.

struct Hello {
    static constexpr MessageId id = MessageId::Hello;
    static constexpr bool carries_port = true;
    std::uint32_t version;
};

struct CreateWindow {
    static constexpr MessageId id = MessageId::CreateWindow;
    static constexpr bool carries_port = false;
    std::uint32_t window;
    Rect frame;
    std::uint32_t scale;
};

struct AttachBuffer {
    static constexpr MessageId id = MessageId::AttachBuffer;
    static constexpr bool carries_port = true;
    std::uint32_t window;
};

struct Commit {
    static constexpr MessageId id = MessageId::Commit;
    static constexpr bool carries_port = false;
    std::uint32_t window;
    Rect damage;
};

struct DestroyWindow {
    static constexpr MessageId id = MessageId::DestroyWindow;
    static constexpr bool carries_port = false;
    std::uint32_t window;
};

// An unmapped window keeps its buffer but is neither drawn nor given input. Windows start
// mapped; mapped must be 0 or 1.
struct SetMapped {
    static constexpr MessageId id = MessageId::SetMapped;
    static constexpr bool carries_port = false;
    std::uint32_t window;
    std::uint32_t mapped;
};

struct MoveWindow {
    static constexpr MessageId id = MessageId::MoveWindow;
    static constexpr bool carries_port = false;
    std::uint32_t window;
    std::int32_t x, y;
};

// Answered with Synced carrying the same serial once everything sent before it is processed.
struct Sync {
    static constexpr MessageId id = MessageId::Sync;
    static constexpr bool carries_port = false;
    std::uint32_t serial;
};

// Server to client. Welcome carries the port for this connection.

struct Welcome {
    static constexpr MessageId id = MessageId::Welcome;
    static constexpr bool carries_port = true;
    std::uint32_t version;
};

struct Configure {
    static constexpr MessageId id = MessageId::Configure;
    static constexpr bool carries_port = false;
    std::uint32_t window;
    Rect frame;
};

struct Input {
    static constexpr MessageId id = MessageId::Input;
    static constexpr bool carries_port = false;
    std::uint32_t window;
    PointerPhase phase;
    float x, y;
};

struct Synced {
    static constexpr MessageId id = MessageId::Synced;
    static constexpr bool carries_port = false;
    std::uint32_t serial;
};

template <class Payload>
struct PlainMessage {
    mach_msg_header_t header;
    Payload payload;
};

template <class Payload>
struct PortMessage {
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t port;
    Payload payload;
};

#pragma pack(pop)

constexpr bool valid_frame(const Rect& rect) {
    return rect.x >= -kMaxCoordinate && rect.x <= kMaxCoordinate &&
           rect.y >= -kMaxCoordinate && rect.y <= kMaxCoordinate &&
           rect.width > 0 && rect.width <= kMaxDimension &&
           rect.height > 0 && rect.height <= kMaxDimension;
}

}  // namespace pw
