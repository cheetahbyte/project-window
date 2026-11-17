#include "clientkit/client.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

constexpr std::uint32_t kScale = 2;
constexpr int kTitleBarHeight = 28;
constexpr int kBarWidth = 40;
constexpr auto kFrameInterval = std::chrono::milliseconds(16);
constexpr std::array<std::uint32_t, 6> kPalette = {0xFF3B82F6, 0xFF10B981, 0xFFF59E0B,
                                                   0xFFEF4444, 0xFF8B5CF6, 0xFF14B8A6};

constexpr std::uint32_t darken(std::uint32_t colour) {
    return 0xFF000000 | ((colour >> 1) & 0x007F7F7F);
}

struct DemoWindow {
    std::uint32_t id;
    pw::Rect frame;
    pw::Surface surface;
    std::size_t colour;
    int bar_x = 0;

    [[nodiscard]] pw::Result<void> fill(pw::Rect area, std::uint32_t colour_value) {
        const int left = std::clamp(area.x, 0, surface.width());
        const int right = std::clamp(area.x + area.width, left, surface.width());
        const int top = std::clamp(area.y, 0, surface.height());
        const int bottom = std::clamp(area.y + area.height, top, surface.height());
        if (left == right || top == bottom) return {};
        return surface.draw([&](std::span<std::uint32_t> pixels, std::size_t stride) {
            const auto count = static_cast<std::size_t>(right - left);
            for (int y = top; y < bottom; ++y) {
                const std::size_t offset = static_cast<std::size_t>(y) * stride +
                                           static_cast<std::size_t>(left);
                if (offset + count > pixels.size()) throw std::out_of_range{"row outside surface"};
                std::ranges::fill(pixels.subspan(offset, count), colour_value);
            }
        });
    }

    pw::Rect body() const {
        const int title = kTitleBarHeight * static_cast<int>(kScale);
        return {0, title, surface.width(), surface.height() - title};
    }

    [[nodiscard]] pw::Result<pw::Rect> paint() {
        const pw::Rect content = body();
        if (auto filled = fill({0, 0, surface.width(), content.y}, darken(kPalette.at(colour)));
            !filled) {
            return std::unexpected{filled.error()};
        }
        if (auto filled = fill(content, kPalette.at(colour)); !filled) {
            return std::unexpected{filled.error()};
        }
        return pw::Rect{0, 0, surface.width(), surface.height()};
    }

    // Moves the bar one step and returns the pixels that changed.
    [[nodiscard]] pw::Result<pw::Rect> step() {
        const pw::Rect content = body();
        const int previous = bar_x;
        bar_x = (bar_x + 8) % (content.width - kBarWidth);
        if (auto filled = fill({previous, content.y, kBarWidth, content.height},
                               kPalette.at(colour));
            !filled) {
            return std::unexpected{filled.error()};
        }
        if (auto filled = fill({bar_x, content.y, kBarWidth, content.height}, 0xFFFFFFFF);
            !filled) {
            return std::unexpected{filled.error()};
        }
        const int left = std::min(previous, bar_x);
        const int right = std::max(previous, bar_x) + kBarWidth;
        return pw::Rect{left, content.y, right - left, content.height};
    }
};

int fail(std::string_view what, kern_return_t code) {
    std::fprintf(stderr, "demo-client: %.*s: %s\n", static_cast<int>(what.size()), what.data(),
                 pw::describe(code).c_str());
    return 1;
}

std::optional<int> parse_count(std::string_view text) {
    int value{0};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return std::clamp(value, 1, 1000);
}

// Draws the window and sends the damage; shared by the first paint and colour changes.
pw::Result<void> repaint(pw::Client& client, DemoWindow& window) {
    const pw::Result<pw::Rect> damage = window.paint();
    if (!damage) return std::unexpected{damage.error()};
    return client.commit(window.id, *damage);
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> arguments{argv, static_cast<std::size_t>(argc)};
    int count = 3;
    bool animate = false;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string_view argument{arguments[index]};
        std::optional<int> parsed;
        if (argument == "--animate") {
            animate = true;
        } else if (argument == "--windows" && index + 1 < arguments.size() &&
                   (parsed = parse_count(arguments[++index]))) {
            count = *parsed;
        } else {
            std::fprintf(stderr, "usage: demo-client [--windows N] [--animate]\n");
            return 2;
        }
    }

    auto client = pw::Client::connect();
    if (!client) return fail("cannot connect", client.error());

    std::vector<DemoWindow> windows;
    for (int index = 0; index < count; ++index) {
        const pw::Rect frame{60 + index % 12 * 56, 60 + index % 12 * 44, 480, 320};
        auto surface = pw::Surface::create(frame.width * static_cast<int>(kScale),
                                           frame.height * static_cast<int>(kScale));
        if (!surface) {
            std::fprintf(stderr, "demo-client: cannot create a buffer\n");
            return 1;
        }
        DemoWindow& window = windows.emplace_back(
            static_cast<std::uint32_t>(index + 1), frame, std::move(*surface),
            static_cast<std::size_t>(index) % kPalette.size());
        const pw::Result<pw::Rect> damage = window.paint();
        if (!damage) return fail("cannot draw", damage.error());
        if (auto sent = client->create_window(window.id, frame, kScale); !sent) {
            return fail("cannot create a window", sent.error());
        }
        if (auto sent = client->attach_buffer(window.id, window.surface); !sent) {
            return fail("cannot attach a buffer", sent.error());
        }
        if (auto sent = client->commit(window.id, *damage); !sent) {
            return fail("cannot commit", sent.error());
        }
    }

    using Clock = std::chrono::steady_clock;
    auto next_frame = Clock::now();
    for (;;) {
        std::optional<std::chrono::milliseconds> timeout;
        if (animate) {
            timeout = std::max(std::chrono::milliseconds{0},
                               std::chrono::ceil<std::chrono::milliseconds>(next_frame - Clock::now()));
        }
        if (auto event = client->next_event(timeout)) {
            if (std::holds_alternative<pw::Disconnected>(*event)) return 0;
            if (const auto* configure = std::get_if<pw::Configure>(&*event)) {
                for (DemoWindow& window : windows) {
                    if (window.id == configure->window) window.frame = configure->frame;
                }
            } else if (const auto* input = std::get_if<pw::Input>(&*event);
                       input && input->phase == pw::PointerPhase::Down) {
                for (DemoWindow& window : windows) {
                    if (window.id != input->window) continue;
                    window.colour = (window.colour + 1) % kPalette.size();
                    if (auto sent = repaint(*client, window); !sent) {
                        return fail("cannot update a window", sent.error());
                    }
                }
            }
        }
        if (animate && Clock::now() >= next_frame) {
            for (DemoWindow& window : windows) {
                const pw::Result<pw::Rect> damage = window.step();
                if (!damage) return fail("cannot draw", damage.error());
                if (auto sent = client->commit(window.id, *damage); !sent) {
                    // The server going away races with its disconnect notification.
                    if (sent.error() == MACH_SEND_INVALID_DEST) return 0;
                    return fail("cannot commit", sent.error());
                }
            }
            next_frame = std::max(next_frame + kFrameInterval, Clock::now());
        }
    }
}
