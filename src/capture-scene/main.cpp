#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

constexpr int kMaxExtent = 16384;
constexpr int kMinCoordinate = -32768;
constexpr int kMaxCoordinate = 32768;
constexpr int kDisplayScale = 2;

struct CfRelease {
    void operator()(CFTypeRef object) const { CFRelease(object); }
};

template <class Ref>
using CfOwner = std::unique_ptr<std::remove_pointer_t<Ref>, CfRelease>;

struct WindowRecord {
    int x{0};
    int y{0};
    int width{0};
    int height{0};
    bool mapped{false};
    int layer{0};
};

int clamp_to_int(double value, int low, int high) {
    if (std::isnan(value)) return low;
    return static_cast<int>(std::clamp(value, static_cast<double>(low), static_cast<double>(high)));
}

CFTypeRef lookup(CFDictionaryRef dictionary, CFStringRef key, CFTypeID type) {
    const void* value = CFDictionaryGetValue(dictionary, key);
    if (value == nullptr || CFGetTypeID(value) != type) return nullptr;
    return value;
}

bool read_number(CFDictionaryRef dictionary, CFStringRef key, double& out) {
    const CFTypeRef value = lookup(dictionary, key, CFNumberGetTypeID());
    if (value == nullptr) return false;
    return CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberDoubleType, &out);
}

bool read_window(CFDictionaryRef info, WindowRecord& record) {
    const CFTypeRef bounds_value = lookup(info, kCGWindowBounds, CFDictionaryGetTypeID());
    if (bounds_value == nullptr) return false;
    const auto bounds = static_cast<CFDictionaryRef>(bounds_value);
    double x{0};
    double y{0};
    double width{0};
    double height{0};
    if (!read_number(bounds, CFSTR("X"), x) || !read_number(bounds, CFSTR("Y"), y) ||
        !read_number(bounds, CFSTR("Width"), width) ||
        !read_number(bounds, CFSTR("Height"), height)) {
        return false;
    }
    if (!(width >= 1.0) || !(height >= 1.0)) return false;
    record.x = clamp_to_int(x, kMinCoordinate, kMaxCoordinate);
    record.y = clamp_to_int(y, kMinCoordinate, kMaxCoordinate);
    record.width = clamp_to_int(width, 1, kMaxExtent);
    record.height = clamp_to_int(height, 1, kMaxExtent);

    record.mapped = false;
    if (const CFTypeRef onscreen = lookup(info, kCGWindowIsOnscreen, CFBooleanGetTypeID())) {
        record.mapped = CFBooleanGetValue(static_cast<CFBooleanRef>(onscreen));
    }
    double layer{0};
    record.layer = read_number(info, kCGWindowLayer, layer)
                       ? clamp_to_int(layer, kMinCoordinate, kMaxCoordinate)
                       : 0;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> arguments{argv, argv + argc};
    if (arguments.size() != 2) {
        std::fprintf(stderr, "usage: capture-scene <output-path>\n");
        return 2;
    }

    const CfOwner<CFArrayRef> list{
        CGWindowListCopyWindowInfo(kCGWindowListOptionAll, kCGNullWindowID)};
    if (!list) {
        std::fprintf(stderr, "capture-scene: cannot list windows\n");
        return 1;
    }

    std::vector<WindowRecord> windows;
    const CFIndex total = CFArrayGetCount(list.get());
    for (CFIndex index = total; index > 0; --index) {
        const void* entry = CFArrayGetValueAtIndex(list.get(), index - 1);
        if (entry == nullptr || CFGetTypeID(entry) != CFDictionaryGetTypeID()) continue;
        WindowRecord record;
        if (read_window(static_cast<CFDictionaryRef>(entry), record)) windows.push_back(record);
    }

    // The system list is front to back only within a layer; stacking goes by layer first.
    std::ranges::stable_sort(windows, {}, &WindowRecord::layer);

    const CGRect display = CGDisplayBounds(CGMainDisplayID());
    const int display_width = clamp_to_int(display.size.width, 1, kMaxExtent);
    const int display_height = clamp_to_int(display.size.height, 1, kMaxExtent);
    const auto mapped_count = std::ranges::count_if(
        windows, [](const WindowRecord& window) { return window.mapped; });

    std::ofstream out{std::string{arguments[1]}};
    if (!out) {
        std::fprintf(stderr, "capture-scene: cannot open the output file\n");
        return 1;
    }
    out << "# windows " << windows.size() << ", mapped " << mapped_count << "\n";
    out << "# x y width height mapped layer, back to front\n";
    out << "display " << display_width << ' ' << display_height << ' ' << kDisplayScale << '\n';
    for (const WindowRecord& window : windows) {
        out << window.x << ' ' << window.y << ' ' << window.width << ' ' << window.height << ' '
            << (window.mapped ? 1 : 0) << ' ' << window.layer << '\n';
    }
    out.flush();
    if (!out) {
        std::fprintf(stderr, "capture-scene: cannot write the output file\n");
        return 1;
    }
    std::printf("windows %zu, mapped %td\n", windows.size(), mapped_count);
    return 0;
}
