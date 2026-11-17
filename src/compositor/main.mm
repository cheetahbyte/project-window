#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#import "compositor/renderer.h"
#import "compositor/server.h"

#include <dispatch/dispatch.h>

#include <algorithm>
#include <charconv>
#include <cinttypes>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace {

constexpr int kUsageExitStatus = 2;
constexpr std::int32_t kMaxDisplayPoints = 16384;

std::optional<std::int32_t> parse_points(std::string_view text) {
    std::int32_t value{};
    const char* const end = text.data() + text.size();
    const auto [last, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || last != end || value < 1 || value > kMaxDisplayPoints) {
        return std::nullopt;
    }
    return value;
}

std::optional<pw::DisplaySize> parse_display(std::string_view text) {
    const std::size_t separator = text.find('x');
    if (separator == std::string_view::npos) return std::nullopt;
    const std::optional<std::int32_t> width = parse_points(text.substr(0, separator));
    const std::optional<std::int32_t> height = parse_points(text.substr(separator + 1));
    if (!width || !height) return std::nullopt;
    return pw::DisplaySize{.width = *width, .height = *height};
}

std::optional<pw::ReleasePolicy> parse_release(std::string_view text) {
    if (text == "never") return pw::ReleasePolicy::Never;
    if (text == "unmapped") return pw::ReleasePolicy::Unmapped;
    if (text == "occluded") return pw::ReleasePolicy::Occluded;
    return std::nullopt;
}

std::optional<pw::RedrawMode> parse_redraw(std::string_view text) {
    if (text == "overdraw") return pw::RedrawMode::Overdraw;
    if (text == "full") return pw::RedrawMode::Full;
    if (text == "damage") return pw::RedrawMode::Damage;
    return std::nullopt;
}

struct Options {
    std::optional<pw::DisplaySize> display;
    std::optional<pw::ReleasePolicy> release;
    std::optional<pw::RedrawMode> redraw;
    bool input{true};
    bool verify_redraw{false};
};

std::optional<Options> parse_options(std::span<char* const> arguments) {
    Options options{};
    for (std::size_t index{1}; index < arguments.size(); ++index) {
        const std::string_view argument{arguments[index]};
        if (argument == "--no-input") {
            options.input = false;
            continue;
        }
        if (argument == "--verify-redraw") {
            options.verify_redraw = true;
            continue;
        }
        if (index + 1 >= arguments.size()) return std::nullopt;
        ++index;
        const std::string_view value{arguments[index]};
        if (argument == "--display" && !options.display) {
            options.display = parse_display(value);
            if (!options.display) return std::nullopt;
        } else if (argument == "--release" && !options.release) {
            options.release = parse_release(value);
            if (!options.release) return std::nullopt;
        } else if (argument == "--redraw" && !options.redraw) {
            options.redraw = parse_redraw(value);
            if (!options.redraw) return std::nullopt;
        } else {
            return std::nullopt;
        }
    }
    return options;
}

std::int32_t whole_points(CGFloat length) {
    // Rounds up so a partly shown last row or column still counts as on the display.
    if (!(length > 0)) return 0;
    return static_cast<std::int32_t>(
        std::ceil(std::min(length, static_cast<CGFloat>(kMaxDisplayPoints))));
}

pw::DisplaySize display_size_of(NSSize size) {
    return {.width = whole_points(size.width), .height = whole_points(size.height)};
}

// macOS backing scales are whole numbers; anything else is rounded to the nearest one.
std::int32_t whole_scale(CGFloat scale) {
    if (!(scale >= 1)) return 1;
    return static_cast<std::int32_t>(
        std::lround(std::min(scale, static_cast<CGFloat>(pw::kMaxDisplayScale))));
}

// Keeps the dispatch source alive and running for as long as the returned object lives.
dispatch_source_t watch_signal(int signal_number, dispatch_block_t handler) {
    // The source only observes the signal; the default action would otherwise still run.
    if (std::signal(signal_number, SIG_IGN) == SIG_ERR) return nil;
    dispatch_source_t source = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_SIGNAL, static_cast<std::uintptr_t>(signal_number), 0,
        dispatch_get_main_queue());
    if (!source) return nil;
    dispatch_source_set_event_handler(source, handler);
    dispatch_resume(source);
    return source;
}

}  // namespace

// Stands in for the display: shows the renderer's frames and feeds pointer events to the server.
@interface DisplayView : NSView
- (instancetype)initWithFrame:(NSRect)frame
                       server:(pw::Server*)server
                     renderer:(pw::Renderer*)renderer;
// Asks for a frame at the next display refresh.
- (void)requestFrame;
// When NO, pointer events are swallowed so a stray click cannot disturb a measurement.
@property BOOL inputEnabled;
@end

@implementation DisplayView {
    pw::Server* _server;
    pw::Renderer* _renderer;
    CADisplayLink* _link;
}

- (instancetype)initWithFrame:(NSRect)frame
                       server:(pw::Server*)server
                     renderer:(pw::Renderer*)renderer {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _server = server;
    _renderer = renderer;
    _inputEnabled = YES;
    CALayer* layer = [CALayer layer];
    layer.contentsGravity = kCAGravityTopLeft;
    layer.opaque = YES;
    // Hosting the layer, rather than backing the view with it, keeps AppKit from drawing into it.
    self.layer = layer;
    self.wantsLayer = YES;
    // Paused whenever no frame is owed, so a static scene schedules nothing.
    _link = [self displayLinkWithTarget:self selector:@selector(refresh:)];
    _link.paused = YES;
    [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    return self;
}

- (void)requestFrame {
    _link.paused = NO;
}

- (void)refresh:(CADisplayLink*)link {
    if (_renderer->frame(*_server, self.layer) != pw::FrameOutcome::Deferred) link.paused = YES;
}

- (void)reportDisplay {
    // Also reached from the superclass initialiser, before the server is set.
    if (!_server || !self.window) return;
    const CGFloat scale{self.window.backingScaleFactor};
    self.layer.contentsScale = scale;
    _server->set_display({.points = display_size_of(self.bounds.size),
                          .pixels_per_point = whole_scale(scale)});
    [self requestFrame];
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self reportDisplay];
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [self reportDisplay];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self reportDisplay];
}

- (void)sendPointer:(pw::PointerPhase)phase event:(NSEvent*)event {
    if (!_inputEnabled) return;
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    _server->pointer(phase, static_cast<float>(point.x),
                     static_cast<float>(self.bounds.size.height - point.y));
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    return YES;
}

- (void)mouseDown:(NSEvent*)event {
    [self sendPointer:pw::PointerPhase::Down event:event];
}

- (void)mouseDragged:(NSEvent*)event {
    [self sendPointer:pw::PointerPhase::Move event:event];
}

- (void)mouseUp:(NSEvent*)event {
    [self sendPointer:pw::PointerPhase::Up event:event];
}

@end

// AppKit would otherwise move or shrink a display window larger than the screen.
@interface DisplayWindow : NSWindow
@end

@implementation DisplayWindow
- (NSRect)constrainFrameRect:(NSRect)frameRect toScreen:(NSScreen*)screen {
    return frameRect;
}

- (BOOL)canBecomeKeyWindow {
    return YES;
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation AppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return YES;
}
@end

int main(int argc, char* argv[]) {
    const std::optional<Options> options =
        parse_options(std::span<char* const>{argv, static_cast<std::size_t>(argc)});
    if (!options) {
        std::fprintf(stderr,
                     "usage: compositor [--display WIDTHxHEIGHT] [--no-input] "
                     "[--release never|unmapped|occluded] [--redraw overdraw|full|damage] "
                     "[--verify-redraw]  (points, 1..%d)\n",
                     kMaxDisplayPoints);
        return kUsageExitStatus;
    }

    @autoreleasepool {
        NSApplication* app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        AppDelegate* delegate = [AppDelegate new];
        app.delegate = delegate;

        NSMenu* menu_bar = [NSMenu new];
        NSMenuItem* app_item = [menu_bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
        app_item.submenu = [NSMenu new];
        [app_item.submenu addItemWithTitle:@"Quit Compositor"
                                    action:@selector(terminate:)
                             keyEquivalent:@"q"];
        app.mainMenu = menu_bar;

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::fprintf(stderr, "compositor: no Metal device\n");
            return 1;
        }
        const std::unique_ptr<pw::Renderer> renderer = pw::Renderer::create(
            device, options->redraw.value_or(pw::RedrawMode::Damage), options->verify_redraw);
        if (!renderer) return 1;
        const pw::Renderer* const renderer_source{renderer.get()};

        DisplayView* view = nil;
        pw::Server server{device, options->release.value_or(pw::ReleasePolicy::Occluded),
                          [&view] { [view requestFrame]; }};
        const pw::Server* const stats_source{&server};
        if (const pw::Result<void> started = server.start(); !started) {
            const std::string service{pw::kServiceName};
            std::fprintf(stderr, "compositor: cannot register %s: %s\n", service.c_str(),
                         pw::describe(started.error()).c_str());
            return 1;
        }

        NSWindow* window = nil;
        if (options->display) {
            NSScreen* screen = NSScreen.screens.firstObject;
            if (!screen) {
                std::fprintf(stderr, "compositor: no screen\n");
                return 1;
            }
            const NSRect screen_frame = screen.frame;
            const CGFloat width{static_cast<CGFloat>(options->display->width)};
            const CGFloat height{static_cast<CGFloat>(options->display->height)};
            const NSRect frame = NSMakeRect(NSMinX(screen_frame), NSMaxY(screen_frame) - height,
                                            width, height);
            view = [[DisplayView alloc] initWithFrame:NSMakeRect(0, 0, width, height)
                                               server:&server
                                             renderer:renderer.get()];
            window = [[DisplayWindow alloc] initWithContentRect:frame
                                                      styleMask:NSWindowStyleMaskBorderless
                                                        backing:NSBackingStoreBuffered
                                                          defer:NO];
            window.contentView = view;
            [window setFrame:frame display:NO];
        } else {
            const NSRect frame = NSMakeRect(0, 0, 1440, 900);
            view = [[DisplayView alloc] initWithFrame:frame server:&server renderer:renderer.get()];
            window = [[NSWindow alloc]
                initWithContentRect:frame
                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                    NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                            backing:NSBackingStoreBuffered
                              defer:NO];
            window.title = @"Compositor";
            window.contentView = view;
            [window center];
        }
        view.inputEnabled = options->input;
        [window makeKeyAndOrderFront:nil];

        dispatch_source_t frames_source = watch_signal(SIGUSR1, ^{
            const pw::RevealStats& reveals = stats_source->reveal_stats();
            const pw::RenderStats rendered = renderer_source->stats();
            std::fprintf(stdout,
                         "frames %" PRIu64 "\nreveals %" PRIu64 " %" PRId64 " %" PRId64
                         "\ngpu %" PRId64 "\nverify %" PRIu64 " %" PRIu64 "\n",
                         rendered.frames, reveals.count,
                         static_cast<std::int64_t>(reveals.total.count()),
                         static_cast<std::int64_t>(reveals.max.count()),
                         static_cast<std::int64_t>(rendered.gpu.count()), rendered.verified,
                         rendered.mismatched);
            std::fflush(stdout);
        });
        dispatch_source_t terminate_source = watch_signal(SIGTERM, ^{
            [NSApp terminate:nil];
        });
        if (!frames_source || !terminate_source) {
            std::fprintf(stderr, "compositor: cannot watch signals\n");
            return 1;
        }

        [app activate];
        [app run];
    }
    return 0;
}
