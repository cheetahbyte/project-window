#include "protocol/transport.h"

#include <mach/notify.h>
#include <servers/bootstrap.h>

#include <string>

namespace pw {

void SendRight::reset() {
    if (MACH_PORT_VALID(name_)) mach_port_deallocate(mach_task_self(), name_);
    name_ = MACH_PORT_NULL;
}

Result<ReceiveRight> ReceiveRight::allocate() {
    const mach_port_t task = mach_task_self();
    mach_port_t name{MACH_PORT_NULL};
    if (const kern_return_t code = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &name);
        code != KERN_SUCCESS) {
        return std::unexpected{code};
    }
    ReceiveRight right{name};
    mach_port_limits_t limits{.mpl_qlimit = MACH_PORT_QLIMIT_LARGE};
    if (const kern_return_t code = mach_port_set_attributes(
            task, name, MACH_PORT_LIMITS_INFO, reinterpret_cast<mach_port_info_t>(&limits),
            MACH_PORT_LIMITS_INFO_COUNT);
        code != KERN_SUCCESS) {
        return std::unexpected{code};
    }
    return right;
}

Result<SendRight> ReceiveRight::make_send() const {
    if (const kern_return_t code = mach_port_insert_right(mach_task_self(), name_, name_,
                                                          MACH_MSG_TYPE_MAKE_SEND);
        code != KERN_SUCCESS) {
        return std::unexpected{code};
    }
    return SendRight::adopt(name_);
}

Result<void> ReceiveRight::notify_when_no_senders() const {
    mach_port_t previous{MACH_PORT_NULL};
    return as_result(mach_port_request_notification(mach_task_self(), name_,
                                                    MACH_NOTIFY_NO_SENDERS, 1, name_,
                                                    MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous));
}

void ReceiveRight::reset() {
    if (MACH_PORT_VALID(name_)) {
        mach_port_mod_refs(mach_task_self(), name_, MACH_PORT_RIGHT_RECEIVE, -1);
    }
    name_ = MACH_PORT_NULL;
}

Result<PortSet> PortSet::allocate() {
    mach_port_t name{MACH_PORT_NULL};
    if (const kern_return_t code =
            mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &name);
        code != KERN_SUCCESS) {
        return std::unexpected{code};
    }
    return PortSet{name};
}

Result<void> PortSet::insert(const ReceiveRight& port) const {
    return as_result(mach_port_insert_member(mach_task_self(), port.name(), name_));
}

void PortSet::reset() {
    if (MACH_PORT_VALID(name_)) {
        mach_port_mod_refs(mach_task_self(), name_, MACH_PORT_RIGHT_PORT_SET, -1);
    }
    name_ = MACH_PORT_NULL;
}

Result<void> notify_when_dead(const SendRight& watched, const ReceiveRight& notify) {
    mach_port_t previous{MACH_PORT_NULL};
    return as_result(mach_port_request_notification(mach_task_self(), watched.name(),
                                                    MACH_NOTIFY_DEAD_NAME, 0, notify.name(),
                                                    MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous));
}

std::string describe(kern_return_t code) {
    // Bootstrap errors live outside the Mach error space, so mach_error_string cannot name them.
    if (code >= BOOTSTRAP_NOT_PRIVILEGED && code <= BOOTSTRAP_NO_CHILDREN) {
        return bootstrap_strerror(code);
    }
    return mach_error_string(code);
}

Result<ReceiveRight> register_service(std::string_view name) {
    Result<ReceiveRight> port = ReceiveRight::allocate();
    if (!port) return port;
    Result<SendRight> published = port->make_send();
    if (!published) return std::unexpected{published.error()};

    std::string service_name{name};
// bootstrap_register is deprecated in favour of launchd jobs, but it is the only way to
// publish a name without installing anything on the system.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const kern_return_t code =
        bootstrap_register(bootstrap_port, service_name.data(), published->name());
#pragma clang diagnostic pop
    if (code != KERN_SUCCESS) return std::unexpected{code};
    return port;
}

Result<SendRight> look_up_service(std::string_view name) {
    const std::string service_name{name};
    mach_port_t port{MACH_PORT_NULL};
    if (const kern_return_t code = bootstrap_look_up(bootstrap_port, service_name.c_str(), &port);
        code != KERN_SUCCESS) {
        return std::unexpected{code};
    }
    return SendRight::adopt(port);
}

Result<void> detail::send_message(mach_msg_header_t& header, SendMode mode) {
    mach_msg_option_t options{MACH_SEND_MSG};
    if (mode == SendMode::DropIfFull) options |= MACH_SEND_TIMEOUT;
    const kern_return_t code =
        mach_msg(&header, options, header.msgh_size, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
    // A timed-out send hands the message's port rights back to the sender as extra references.
    if (code == MACH_SEND_TIMED_OUT) mach_msg_destroy(&header);
    return as_result(code);
}

mach_msg_header_t Message::header() const {
    mach_msg_header_t header{};
    std::memcpy(&header, bytes_.data(), sizeof header);
    return header;
}

std::optional<mach_port_t> Message::match(mach_msg_id_t id, std::size_t size,
                                          bool carries_port) const {
    const mach_msg_header_t message_header = header();
    const bool complex = (message_header.msgh_bits & MACH_MSGH_BITS_COMPLEX) != 0;
    if (!holds_rights_ || message_header.msgh_id != id || message_header.msgh_size != size ||
        complex != carries_port || message_header.msgh_remote_port != MACH_PORT_NULL) {
        return std::nullopt;
    }
    if (!carries_port) return MACH_PORT_NULL;

    mach_msg_body_t body{};
    mach_msg_port_descriptor_t descriptor{};
    std::memcpy(&body, bytes_.data() + sizeof(mach_msg_header_t), sizeof body);
    std::memcpy(&descriptor, bytes_.data() + sizeof(mach_msg_header_t) + sizeof body,
                sizeof descriptor);
    if (body.msgh_descriptor_count != 1 || descriptor.type != MACH_MSG_PORT_DESCRIPTOR ||
        descriptor.disposition != MACH_MSG_TYPE_PORT_SEND || !MACH_PORT_VALID(descriptor.name)) {
        return std::nullopt;
    }
    return descriptor.name;
}

void Message::release_rights() {
    if (holds_rights_) mach_msg_destroy(reinterpret_cast<mach_msg_header_t*>(bytes_.data()));
    holds_rights_ = false;
}

Result<Message> receive(mach_port_t port, std::optional<std::chrono::milliseconds> timeout) {
    mach_msg_option_t options{MACH_RCV_MSG};
    mach_msg_timeout_t milliseconds{MACH_MSG_TIMEOUT_NONE};
    if (timeout) {
        options |= MACH_RCV_TIMEOUT;
        milliseconds = static_cast<mach_msg_timeout_t>(timeout->count());
    }
    Message message;
    const kern_return_t code =
        mach_msg(reinterpret_cast<mach_msg_header_t*>(message.bytes_.data()), options, 0,
                 static_cast<mach_msg_size_t>(message.bytes_.size()), port, milliseconds,
                 MACH_PORT_NULL);
    if (code != KERN_SUCCESS) return std::unexpected{code};
    message.holds_rights_ = true;
    return message;
}

}  // namespace pw
