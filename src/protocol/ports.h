#pragma once

#include <mach/mach.h>

#include <expected>
#include <string>
#include <utility>

namespace pw {

template <class Value>
using Result = std::expected<Value, kern_return_t>;

// Owns one reference on a send right and releases it on destruction.
class SendRight {
public:
    SendRight() = default;
    // Takes over one reference the caller already holds on name.
    [[nodiscard]] static SendRight adopt(mach_port_t name) { return SendRight{name}; }

    SendRight(SendRight&& other) noexcept : name_{other.release()} {}
    SendRight& operator=(SendRight&& other) noexcept {
        if (this != &other) {
            reset();
            name_ = other.release();
        }
        return *this;
    }
    ~SendRight() { reset(); }

    [[nodiscard]] mach_port_t name() const { return name_; }
    explicit operator bool() const { return MACH_PORT_VALID(name_); }
    // Gives up ownership without releasing the reference.
    [[nodiscard]] mach_port_t release() { return std::exchange(name_, MACH_PORT_NULL); }
    void reset();

private:
    explicit SendRight(mach_port_t name) : name_{name} {}
    mach_port_t name_{MACH_PORT_NULL};
};

// Owns a receive right; destroying it destroys the port and every message still queued on it.
class ReceiveRight {
public:
    ReceiveRight() = default;
    // A new port with a large queue, so bursts of messages do not block the sender.
    [[nodiscard]] static Result<ReceiveRight> allocate();

    ReceiveRight(ReceiveRight&& other) noexcept
        : name_{std::exchange(other.name_, MACH_PORT_NULL)} {}
    ReceiveRight& operator=(ReceiveRight&& other) noexcept {
        if (this != &other) {
            reset();
            name_ = std::exchange(other.name_, MACH_PORT_NULL);
        }
        return *this;
    }
    ~ReceiveRight() { reset(); }

    [[nodiscard]] mach_port_t name() const { return name_; }
    explicit operator bool() const { return MACH_PORT_VALID(name_); }
    [[nodiscard]] Result<SendRight> make_send() const;
    // Asks the kernel to post MACH_NOTIFY_NO_SENDERS to this port once no send right to it
    // exists any more.
    [[nodiscard]] Result<void> notify_when_no_senders() const;
    void reset();

private:
    explicit ReceiveRight(mach_port_t name) : name_{name} {}
    mach_port_t name_{MACH_PORT_NULL};
};

// Owns a port set, which lets one thread wait on many receive rights at once.
class PortSet {
public:
    PortSet() = default;
    [[nodiscard]] static Result<PortSet> allocate();

    PortSet(PortSet&& other) noexcept : name_{std::exchange(other.name_, MACH_PORT_NULL)} {}
    PortSet& operator=(PortSet&& other) noexcept {
        if (this != &other) {
            reset();
            name_ = std::exchange(other.name_, MACH_PORT_NULL);
        }
        return *this;
    }
    ~PortSet() { reset(); }

    [[nodiscard]] mach_port_t name() const { return name_; }
    [[nodiscard]] Result<void> insert(const ReceiveRight& port) const;
    void reset();

private:
    explicit PortSet(mach_port_t name) : name_{name} {}
    mach_port_t name_{MACH_PORT_NULL};
};

// Asks the kernel to post MACH_NOTIFY_DEAD_NAME to notify once the port behind watched dies.
[[nodiscard]] Result<void> notify_when_dead(const SendRight& watched, const ReceiveRight& notify);

[[nodiscard]] inline Result<void> as_result(kern_return_t code) {
    if (code != KERN_SUCCESS) return std::unexpected{code};
    return {};
}

// A readable description of a Mach or bootstrap error.
[[nodiscard]] std::string describe(kern_return_t code);

}  // namespace pw
