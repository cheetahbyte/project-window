#include "clientkit/client.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <expected>
#include <fstream>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

constexpr std::uint32_t kDefaultScale = 2;
constexpr int kTitleBarHeight = 28;
constexpr int kBarWidth = 40;
constexpr auto kFrameInterval = std::chrono::milliseconds(16);
constexpr auto kSyncTimeout = std::chrono::seconds(30);
constexpr double kOrbitRadius = 100.0;
constexpr double kOrbitPeriodSeconds = 2.0;
constexpr int kMaxExtent = 16384;
constexpr int kMaxCoordinate = 32768;
constexpr int kMaxScale = 4;
constexpr int kMaxSliceCount = 1000;
constexpr std::size_t kMaxSceneWindows = 100000;
constexpr std::string_view kUsage =
    "usage: demo-client [--windows N] [--animate] | --scene PATH [--slice I/N] [--animate] "
    "[--animate-top | --orbit-top]\n";
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
    std::uint32_t scale = kDefaultScale;
    int bar_x = 0;
    bool animated = false;

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
        const int title = kTitleBarHeight * static_cast<int>(scale);
        return {0, title, surface.width(), surface.height() - title};
    }

    [[nodiscard]] bool can_animate() const {
        const pw::Rect content = body();
        return content.width > kBarWidth && content.height > 0;
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

struct SceneWindow {
    pw::Rect frame;
    bool mapped;
    int layer;
};

struct Scene {
    int scale{1};
    std::vector<SceneWindow> windows;
};

enum class TopMode { None, Bar, Orbit };

struct Slice {
    std::size_t index{0};
    std::size_t total{1};
};

struct Options {
    int count{3};
    bool animate{false};
    bool windows_given{false};
    std::optional<std::string> scene_path;
    std::optional<Slice> slice;
    TopMode top{TopMode::None};
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

std::optional<int> parse_int(std::string_view text, int low, int high) {
    int value{0};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    if (value < low || value > high) return std::nullopt;
    return value;
}

std::optional<Slice> parse_slice(std::string_view text) {
    const std::size_t slash = text.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    const auto index = parse_int(text.substr(0, slash), 0, kMaxSliceCount - 1);
    const auto total = parse_int(text.substr(slash + 1), 1, kMaxSliceCount);
    if (!index || !total || *index >= *total) return std::nullopt;
    return Slice{static_cast<std::size_t>(*index), static_cast<std::size_t>(*total)};
}

std::optional<Options> parse_options(std::span<char*> arguments) {
    Options options;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string_view argument{arguments[index]};
        const bool has_value = index + 1 < arguments.size();
        if (argument == "--animate") {
            options.animate = true;
        } else if (argument == "--animate-top" || argument == "--orbit-top") {
            if (options.top != TopMode::None) return std::nullopt;
            options.top = argument == "--orbit-top" ? TopMode::Orbit : TopMode::Bar;
        } else if (argument == "--windows" && has_value) {
            const auto parsed = parse_count(arguments[++index]);
            if (!parsed) return std::nullopt;
            options.count = *parsed;
            options.windows_given = true;
        } else if (argument == "--scene" && has_value && !options.scene_path) {
            options.scene_path = std::string{arguments[++index]};
        } else if (argument == "--slice" && has_value && !options.slice) {
            options.slice = parse_slice(arguments[++index]);
            if (!options.slice) return std::nullopt;
        } else {
            return std::nullopt;
        }
    }
    if (!options.scene_path) {
        if (options.slice || options.top != TopMode::None) return std::nullopt;
    } else if (options.windows_given ||
               (options.animate && options.top != TopMode::None)) {
        return std::nullopt;
    }
    return options;
}

std::vector<std::string_view> split_fields(std::string_view line) {
    constexpr std::string_view kSpace = " \t\r";
    std::vector<std::string_view> fields;
    for (;;) {
        const std::size_t begin = line.find_first_not_of(kSpace);
        if (begin == std::string_view::npos) return fields;
        line.remove_prefix(begin);
        const std::size_t end = std::min(line.find_first_of(kSpace), line.size());
        fields.push_back(line.substr(0, end));
        line.remove_prefix(end);
    }
}

std::string line_error(std::size_t line, std::string_view what) {
    return "line " + std::to_string(line) + ": " + std::string{what};
}

std::expected<Scene, std::string> parse_scene(const std::string& path) {
    std::ifstream file{path};
    if (!file) return std::unexpected{"cannot read " + path};
    Scene scene;
    bool have_display = false;
    std::string text;
    std::size_t line = 0;
    while (std::getline(file, text)) {
        ++line;
        const std::vector<std::string_view> fields = split_fields(text);
        if (fields.empty() || fields.front().starts_with('#')) continue;
        if (!have_display) {
            if (fields.size() != 4 || fields.at(0) != "display") {
                return std::unexpected{
                    line_error(line, "expected: display <width> <height> <scale>")};
            }
            const auto width = parse_int(fields.at(1), 1, kMaxExtent);
            const auto height = parse_int(fields.at(2), 1, kMaxExtent);
            const auto scale = parse_int(fields.at(3), 1, kMaxScale);
            if (!width || !height || !scale) {
                return std::unexpected{line_error(line, "display value out of range")};
            }
            scene.scale = *scale;
            have_display = true;
            continue;
        }
        if (fields.size() != 6) {
            return std::unexpected{
                line_error(line, "expected: <x> <y> <width> <height> <mapped> <layer>")};
        }
        if (scene.windows.size() >= kMaxSceneWindows) {
            return std::unexpected{line_error(line, "too many windows")};
        }
        const auto x = parse_int(fields.at(0), -kMaxCoordinate, kMaxCoordinate);
        const auto y = parse_int(fields.at(1), -kMaxCoordinate, kMaxCoordinate);
        const auto width = parse_int(fields.at(2), 1, kMaxExtent);
        const auto height = parse_int(fields.at(3), 1, kMaxExtent);
        const auto mapped = parse_int(fields.at(4), 0, 1);
        const auto layer = parse_int(fields.at(5), -kMaxCoordinate, kMaxCoordinate);
        if (!x || !y || !width || !height || !mapped || !layer) {
            return std::unexpected{line_error(line, "value out of range or not an integer")};
        }
        if (*width * scene.scale > kMaxExtent || *height * scene.scale > kMaxExtent) {
            return std::unexpected{line_error(line, "buffer larger than 16384 pixels")};
        }
        scene.windows.push_back({{*x, *y, *width, *height}, *mapped == 1, *layer});
    }
    if (file.bad()) return std::unexpected{"cannot read " + path};
    if (!have_display) return std::unexpected{"line " + std::to_string(line) + ": no display line"};
    return scene;
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
    const std::optional<Options> options = parse_options(arguments);
    if (!options) {
        std::fputs(kUsage.data(), stderr);
        return 2;
    }

    Scene scene;
    if (options->scene_path) {
        auto parsed = parse_scene(*options->scene_path);
        if (!parsed) {
            std::fprintf(stderr, "demo-client: %s\n", parsed.error().c_str());
            return 1;
        }
        scene = std::move(*parsed);
    } else {
        for (int index = 0; index < options->count; ++index) {
            scene.scale = static_cast<int>(kDefaultScale);
            scene.windows.push_back(
                {{60 + index % 12 * 56, 60 + index % 12 * 44, 480, 320}, true, 0});
        }
    }

    const std::size_t total = scene.windows.size();
    const Slice slice = options->slice.value_or(Slice{});
    const std::size_t first = slice.index * total / slice.total;
    const std::size_t last = (slice.index + 1) * total / slice.total;

    std::optional<std::size_t> top_index;
    for (std::size_t index = 0; index < total; ++index) {
        const SceneWindow& candidate = scene.windows.at(index);
        if (candidate.mapped && candidate.layer == 0) top_index = index;
    }
    const bool top_here = top_index && *top_index >= first && *top_index < last;

    auto client = pw::Client::connect();
    if (!client) return fail("cannot connect", client.error());

    std::vector<DemoWindow> windows;
    std::optional<std::size_t> orbit_slot;
    std::optional<pw::Rect> orbit_origin;
    for (std::size_t index = first; index < last; ++index) {
        const SceneWindow& entry = scene.windows.at(index);
        const auto scale = static_cast<std::uint32_t>(scene.scale);
        auto surface = pw::Surface::create(entry.frame.width * scene.scale,
                                           entry.frame.height * scene.scale);
        if (!surface) {
            std::fprintf(stderr, "demo-client: cannot create a buffer\n");
            return 1;
        }
        DemoWindow& window = windows.emplace_back(
            static_cast<std::uint32_t>(index + 1), entry.frame, std::move(*surface),
            index % kPalette.size(), scale);
        const pw::Result<pw::Rect> damage = window.paint();
        if (!damage) return fail("cannot draw", damage.error());
        if (options->scene_path) {
            const bool is_top = top_here && index == *top_index;
            if (is_top && options->top == TopMode::Orbit) {
                orbit_slot = windows.size() - 1;
                orbit_origin = entry.frame;
            }
            window.animated = options->top == TopMode::None ? options->animate
                                                            : is_top && options->top == TopMode::Bar;
        } else {
            window.animated = options->animate;
        }
        window.animated = window.animated && window.can_animate();
        if (auto sent = client->create_window(window.id, entry.frame, scale); !sent) {
            return fail("cannot create a window", sent.error());
        }
        if (!entry.mapped) {
            if (auto sent = client->set_mapped(window.id, false); !sent) {
                return fail("cannot unmap a window", sent.error());
            }
        }
        if (auto sent = client->attach_buffer(window.id, window.surface); !sent) {
            return fail("cannot attach a buffer", sent.error());
        }
        if (auto sent = client->commit(window.id, *damage); !sent) {
            return fail("cannot commit", sent.error());
        }
    }

    if (options->scene_path) {
        if (auto synced = client->sync(kSyncTimeout); !synced) {
            return fail("cannot sync", synced.error());
        }
        std::fputs("ready\n", stdout);
        std::fflush(stdout);
    }

    const bool ticking = orbit_slot.has_value() ||
                         std::ranges::any_of(windows, [](const DemoWindow& window) {
                             return window.animated;
                         });
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto next_frame = start;
    for (;;) {
        std::optional<std::chrono::milliseconds> timeout;
        if (ticking) {
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
        if (ticking && Clock::now() >= next_frame) {
            for (DemoWindow& window : windows) {
                if (!window.animated) continue;
                const pw::Result<pw::Rect> damage = window.step();
                if (!damage) return fail("cannot draw", damage.error());
                if (auto sent = client->commit(window.id, *damage); !sent) {
                    // The server going away races with its disconnect notification.
                    if (sent.error() == MACH_SEND_INVALID_DEST) return 0;
                    return fail("cannot commit", sent.error());
                }
            }
            if (orbit_slot && orbit_origin) {
                const std::chrono::duration<double> elapsed = Clock::now() - start;
                const double angle = 2.0 * std::numbers::pi * elapsed.count() / kOrbitPeriodSeconds;
                const auto x = orbit_origin->x +
                               static_cast<std::int32_t>(std::lround(kOrbitRadius * std::cos(angle)));
                const auto y = orbit_origin->y +
                               static_cast<std::int32_t>(std::lround(kOrbitRadius * std::sin(angle)));
                if (auto sent = client->move_window(windows.at(*orbit_slot).id, x, y); !sent) {
                    if (sent.error() == MACH_SEND_INVALID_DEST) return 0;
                    return fail("cannot move a window", sent.error());
                }
            }
            next_frame = std::max(next_frame + kFrameInterval, Clock::now());
        }
    }
}
