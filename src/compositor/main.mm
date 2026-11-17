#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>
#import <simd/simd.h>

#import "compositor/server.h"

#include <dispatch/dispatch.h>

#include <charconv>
#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace {

constexpr int kUsageExitStatus = 2;
constexpr std::int32_t kMaxDisplayPoints = 16384;

struct DisplaySize {
    std::int32_t width{};
    std::int32_t height{};
};

std::optional<std::int32_t> parse_points(std::string_view text) {
    std::int32_t value{};
    const char* const end = text.data() + text.size();
    const auto [last, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || last != end || value < 1 || value > kMaxDisplayPoints) {
        return std::nullopt;
    }
    return value;
}

std::optional<DisplaySize> parse_display(std::string_view text) {
    const std::size_t separator = text.find('x');
    if (separator == std::string_view::npos) return std::nullopt;
    const std::optional<std::int32_t> width = parse_points(text.substr(0, separator));
    const std::optional<std::int32_t> height = parse_points(text.substr(separator + 1));
    if (!width || !height) return std::nullopt;
    return DisplaySize{.width = *width, .height = *height};
}

struct Options {
    std::optional<DisplaySize> display;
};

std::optional<Options> parse_options(std::span<char* const> arguments) {
    Options options{};
    for (std::size_t index{1}; index < arguments.size(); ++index) {
        const std::string_view argument{arguments[index]};
        if (argument != "--display" || options.display || index + 1 >= arguments.size()) {
            return std::nullopt;
        }
        ++index;
        options.display = parse_display(arguments[index]);
        if (!options.display) return std::nullopt;
    }
    return options;
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

struct QuadUniforms {
    simd_float4 rect{};
    simd_float2 display{};
};

id<MTLRenderPipelineState> make_pipeline(id<MTLDevice> device) {
    NSURL* url = [NSBundle.mainBundle.executableURL.URLByDeletingLastPathComponent
        URLByAppendingPathComponent:@"composite.metallib"];
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:url error:&error];
    if (!library) {
        std::fprintf(stderr, "compositor: %s\n", error.localizedDescription.UTF8String);
        return nil;
    }
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"quad_vertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"quad_fragment"];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor
                                                                                 error:&error];
    if (!pipeline) std::fprintf(stderr, "compositor: %s\n", error.localizedDescription.UTF8String);
    return pipeline;
}

}  // namespace

// Stands in for the display: draws the scene and feeds pointer events to the server.
@interface DisplayView : MTKView <MTKViewDelegate>
- (instancetype)initWithFrame:(NSRect)frame
                       server:(pw::Server*)server
                     pipeline:(id<MTLRenderPipelineState>)pipeline;
@property(readonly) std::uint64_t framesPresented;
@end

@implementation DisplayView {
    pw::Server* _server;
    id<MTLRenderPipelineState> _pipeline;
    id<MTLCommandQueue> _queue;
    std::uint64_t _framesPresented;
}

@synthesize framesPresented = _framesPresented;

- (instancetype)initWithFrame:(NSRect)frame
                       server:(pw::Server*)server
                     pipeline:(id<MTLRenderPipelineState>)pipeline {
    self = [super initWithFrame:frame device:pipeline.device];
    if (!self) return nil;
    _server = server;
    _pipeline = pipeline;
    _queue = [pipeline.device newCommandQueue];
    _framesPresented = 0;
    self.delegate = self;
    // Draw only when the server reports damage, never on a timer.
    self.paused = YES;
    self.enableSetNeedsDisplay = YES;
    self.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    self.clearColor = MTLClearColorMake(0.10, 0.11, 0.13, 1.0);
    return self;
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
}

- (void)drawInMTKView:(MTKView*)view {
    MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = view.currentDrawable;
    if (!pass || !drawable) return;

    id<MTLCommandBuffer> commands = [_queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:_pipeline];

    const CGSize display = view.bounds.size;
    for (const pw::Window& window : _server->scene().back_to_front()) {
        if (!window.mapped) continue;
        id<MTLTexture> texture = _server->texture(window.key);
        if (!texture) continue;
        const QuadUniforms uniforms{
            .rect = simd_make_float4(window.frame.x, window.frame.y, window.frame.width,
                                     window.frame.height),
            .display = simd_make_float2(static_cast<float>(display.width),
                                        static_cast<float>(display.height)),
        };
        [encoder setVertexBytes:&uniforms length:sizeof uniforms atIndex:0];
        [encoder setFragmentTexture:texture atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }

    [encoder endEncoding];
    [commands presentDrawable:drawable];
    [commands commit];
    ++_framesPresented;
}

- (void)sendPointer:(pw::PointerPhase)phase event:(NSEvent*)event {
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
        std::fprintf(stderr, "usage: compositor [--display WIDTHxHEIGHT]  (points, 1..%d)\n",
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
        id<MTLRenderPipelineState> pipeline = device ? make_pipeline(device) : nil;
        if (!pipeline) return 1;

        DisplayView* view = nil;
        pw::Server server{device, [&view] { view.needsDisplay = YES; }};
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
                                             pipeline:pipeline];
            window = [[DisplayWindow alloc] initWithContentRect:frame
                                                      styleMask:NSWindowStyleMaskBorderless
                                                        backing:NSBackingStoreBuffered
                                                          defer:NO];
            window.contentView = view;
            [window setFrame:frame display:NO];
        } else {
            const NSRect frame = NSMakeRect(0, 0, 1440, 900);
            view = [[DisplayView alloc] initWithFrame:frame server:&server pipeline:pipeline];
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
        [window makeKeyAndOrderFront:nil];

        dispatch_source_t frames_source = watch_signal(SIGUSR1, ^{
            std::fprintf(stdout, "frames %" PRIu64 "\n", view.framesPresented);
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
