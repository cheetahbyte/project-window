#pragma once

#include "protocol/messages.h"
#include "protocol/ports.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string_view>
#include <type_traits>

namespace pw {

// Publishes a port under name in the per-user bootstrap namespace.
[[nodiscard]] Result<ReceiveRight> register_service(std::string_view name);
[[nodiscard]] Result<SendRight> look_up_service(std::string_view name);

enum class SendMode {
    Block,
    // Drop the message when the receiver's queue is full; for senders that must never stall.
    DropIfFull,
};

namespace detail {

[[nodiscard]] Result<void> send_message(mach_msg_header_t& header, SendMode mode);

template <class Payload>
[[nodiscard]] Result<void> send_with_port(const SendRight& destination, const Payload& payload,
                                          mach_port_t port, mach_msg_type_name_t disposition,
                                          SendMode mode) {
    PortMessage<Payload> message{};
    message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
    message.header.msgh_size = sizeof message;
    message.header.msgh_remote_port = destination.name();
    message.header.msgh_id = static_cast<mach_msg_id_t>(Payload::id);
    message.body.msgh_descriptor_count = 1;
    message.port.name = port;
    message.port.disposition = disposition;
    message.port.type = MACH_MSG_PORT_DESCRIPTOR;
    message.payload = payload;
    return send_message(message.header, mode);
}

}  // namespace detail

template <class Payload>
    requires(!Payload::carries_port)
[[nodiscard]] Result<void> send(const SendRight& destination, const Payload& payload,
                                SendMode mode = SendMode::Block) {
    PlainMessage<Payload> message{};
    message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    message.header.msgh_size = sizeof message;
    message.header.msgh_remote_port = destination.name();
    message.header.msgh_id = static_cast<mach_msg_id_t>(Payload::id);
    message.payload = payload;
    return detail::send_message(message.header, mode);
}

// Sends a message that hands right over to the receiver. The right is gone afterwards whether
// or not the send succeeded.
template <class Payload>
    requires Payload::carries_port
[[nodiscard]] Result<void> send(const SendRight& destination, const Payload& payload,
                                SendRight right, SendMode mode = SendMode::Block) {
    Result<void> result = detail::send_with_port(destination, payload, right.name(),
                                                 MACH_MSG_TYPE_MOVE_SEND, mode);
    // The kernel consumed the reference; a timed-out send has already released it again.
    if (result || result.error() == MACH_SEND_TIMED_OUT) static_cast<void>(right.release());
    return result;
}

// Sends a message that gives the receiver a new send right to port.
template <class Payload>
    requires Payload::carries_port
[[nodiscard]] Result<void> send(const SendRight& destination, const Payload& payload,
                                const ReceiveRight& port, SendMode mode = SendMode::Block) {
    return detail::send_with_port(destination, payload, port.name(), MACH_MSG_TYPE_MAKE_SEND,
                                  mode);
}

template <class Payload>
struct Received {
    Payload payload{};
    // The right the message carried; empty for payloads that carry none.
    SendRight port;
};

// One received message. Port rights it still carries are released on destruction.
class Message {
public:
    Message() = default;
    Message(Message&& other) noexcept
        : bytes_{other.bytes_}, holds_rights_{std::exchange(other.holds_rights_, false)} {}
    Message& operator=(Message&& other) noexcept {
        if (this != &other) {
            release_rights();
            bytes_ = other.bytes_;
            holds_rights_ = std::exchange(other.holds_rights_, false);
        }
        return *this;
    }
    ~Message() { release_rights(); }

    [[nodiscard]] mach_msg_id_t id() const { return header().msgh_id; }
    // The port the message arrived on; for a port set, the member port.
    [[nodiscard]] mach_port_t arrival_port() const { return header().msgh_local_port; }

    // The payload, if the message's identifier, exact size, and port descriptor all match the
    // layout of Payload and it names no reply port. On a match the carried right moves into
    // the result and the message is left empty.
    template <class Payload>
    [[nodiscard]] std::optional<Received<Payload>> take() {
        static_assert(std::is_trivially_copyable_v<Payload>);
        using Wire = std::conditional_t<Payload::carries_port, PortMessage<Payload>,
                                        PlainMessage<Payload>>;
        static_assert(sizeof(Wire) + sizeof(mach_msg_max_trailer_t) <= kCapacity);

        const std::optional<mach_port_t> port =
            match(static_cast<mach_msg_id_t>(Payload::id), sizeof(Wire), Payload::carries_port);
        if (!port) return std::nullopt;

        Received<Payload> received;
        std::memcpy(&received.payload, bytes_.data() + offsetof(Wire, payload), sizeof(Payload));
        if (Payload::carries_port) received.port = SendRight::adopt(*port);
        holds_rights_ = false;
        return received;
    }

private:
    friend Result<Message> receive(mach_port_t port,
                                   std::optional<std::chrono::milliseconds> timeout);

    static constexpr std::size_t kCapacity = 256;

    [[nodiscard]] mach_msg_header_t header() const;
    [[nodiscard]] std::optional<mach_port_t> match(mach_msg_id_t id, std::size_t size,
                                                   bool carries_port) const;
    void release_rights();

    alignas(8) std::array<std::byte, kCapacity> bytes_{};
    bool holds_rights_{false};
};

// Waits for a message on a receive right or port set, given by name; without a timeout it
// waits indefinitely. A timeout is reported as MACH_RCV_TIMED_OUT.
[[nodiscard]] Result<Message> receive(mach_port_t port,
                                      std::optional<std::chrono::milliseconds> timeout = std::nullopt);

}  // namespace pw
