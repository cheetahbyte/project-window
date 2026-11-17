#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>
#import <simd/simd.h>

#import "compositor/server.h"

#include <cstdio>
#include <string>

namespace {

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
@end

@implementation DisplayView {
    pw::Server* _server;
    id<MTLRenderPipelineState> _pipeline;
    id<MTLCommandQueue> _queue;
}

- (instancetype)initWithFrame:(NSRect)frame
                       server:(pw::Server*)server
                     pipeline:(id<MTLRenderPipelineState>)pipeline {
    self = [super initWithFrame:frame device:pipeline.device];
    if (!self) return nil;
    _server = server;
    _pipeline = pipeline;
    _queue = [pipeline.device newCommandQueue];
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

@interface AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation AppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return YES;
}
@end

int main() {
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

        const NSRect frame = NSMakeRect(0, 0, 1440, 900);
        view = [[DisplayView alloc] initWithFrame:frame server:&server pipeline:pipeline];
        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        window.title = @"Compositor";
        window.contentView = view;
        [window center];
        [window makeKeyAndOrderFront:nil];
        [app activate];
        [app run];
    }
    return 0;
}
