#import "compositor/renderer.h"

#include "compositor/occlusion.h"

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <simd/simd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <ranges>
#include <string>
#include <utility>

namespace pw {

namespace {

constexpr OSType kPixelFormatBGRA = 0x42475241;
constexpr std::int32_t kBytesPerPixel = 4;
// One buffer on screen, one queued for the window server, one being drawn; the same depth
// as CAMetalLayer's default maximumDrawableCount. Each is a full display of pixels.
constexpr std::size_t kMaxBuffers = 3;
// A buffer left unused gathers damage; beyond this many rectangles it is simply redrawn whole.
constexpr std::size_t kMaxStaleRects = 64;
constexpr simd_float4 kBackground{0.10F, 0.11F, 0.13F, 1.0F};
constexpr double kNanosecondsPerSecond = 1e9;
constexpr std::uint64_t kNanosecondsPerMicrosecond = 1000;

struct QuadUniforms {
    simd_float4 rect{};
    simd_float2 target{};
};

enum class Blending { Replace, PremultipliedOver };

struct DrawnWindow {
    id<MTLTexture> texture;
    Rect frame_pixels{};
    std::vector<Rect> opaque_rects;
    std::vector<Rect> translucent_rects;
};

std::string describe(NSError* error) {
    if (!error) return "unknown error";
    const char* text = error.localizedDescription.UTF8String;
    return text ? std::string{text} : std::string{"unknown error"};
}

bool set_number(CFMutableDictionaryRef dictionary, CFStringRef key, std::int32_t value) {
    const CfOwner<CFNumberRef> number{CFNumberCreate(nullptr, kCFNumberSInt32Type, &value)};
    if (!number) return false;
    CFDictionarySetValue(dictionary, key, number.get());
    return true;
}

id<MTLRenderPipelineState> make_pipeline(id<MTLDevice> device, id<MTLLibrary> library,
                                         NSString* fragment, Blending blending) {
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"quad_vertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:fragment];
    MTLRenderPipelineColorAttachmentDescriptor* colour = descriptor.colorAttachments[0];
    colour.pixelFormat = MTLPixelFormatBGRA8Unorm;
    if (blending == Blending::PremultipliedOver) {
        colour.blendingEnabled = YES;
        colour.rgbBlendOperation = MTLBlendOperationAdd;
        colour.sourceRGBBlendFactor = MTLBlendFactorOne;
        colour.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        // Whatever lies beneath has alpha 1, so the result does too; keeping it avoids rounding.
        colour.alphaBlendOperation = MTLBlendOperationAdd;
        colour.sourceAlphaBlendFactor = MTLBlendFactorZero;
        colour.destinationAlphaBlendFactor = MTLBlendFactorOne;
    }
    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor
                                                                                 error:&error];
    if (!pipeline) std::fprintf(stderr, "compositor: %s\n", describe(error).c_str());
    return pipeline;
}

std::optional<MTLScissorRect> scissor_of(const Rect& rect, const Rect& bounds) {
    const std::optional<Rect> inside = intersection(rect, bounds);
    if (!inside || inside->x < 0 || inside->y < 0) return std::nullopt;
    return MTLScissorRect{.x = static_cast<NSUInteger>(inside->x),
                          .y = static_cast<NSUInteger>(inside->y),
                          .width = static_cast<NSUInteger>(inside->width),
                          .height = static_cast<NSUInteger>(inside->height)};
}

std::vector<std::uint8_t> read_pixels(id<MTLTexture> texture) {
    const NSUInteger row_bytes{texture.width * static_cast<NSUInteger>(kBytesPerPixel)};
    std::vector<std::uint8_t> pixels(row_bytes * texture.height);
    [texture getBytes:pixels.data()
          bytesPerRow:row_bytes
           fromRegion:MTLRegionMake2D(0, 0, texture.width, texture.height)
          mipmapLevel:0];
    return pixels;
}

bool completed(id<MTLCommandBuffer> commands) {
    if (commands.status == MTLCommandBufferStatusCompleted) return true;
    std::fprintf(stderr, "compositor: GPU work failed: %s\n", describe(commands.error).c_str());
    return false;
}

}  // namespace

std::unique_ptr<Renderer> Renderer::create(id<MTLDevice> device, RedrawMode mode, bool verify) {
    NSURL* url = [NSBundle.mainBundle.executableURL.URLByDeletingLastPathComponent
        URLByAppendingPathComponent:@"composite.metallib"];
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:url error:&error];
    if (!library) {
        std::fprintf(stderr, "compositor: %s\n", describe(error).c_str());
        return nullptr;
    }
    id<MTLRenderPipelineState> opaque =
        make_pipeline(device, library, @"opaque_fragment", Blending::Replace);
    id<MTLRenderPipelineState> blended =
        make_pipeline(device, library, @"blended_fragment", Blending::PremultipliedOver);
    id<MTLRenderPipelineState> solid =
        make_pipeline(device, library, @"solid_fragment", Blending::Replace);
    if (!opaque || !blended || !solid) return nullptr;
    return std::make_unique<Renderer>(device, opaque, blended, solid, mode, verify);
}

Renderer::Renderer(id<MTLDevice> device, id<MTLRenderPipelineState> opaque,
                   id<MTLRenderPipelineState> blended, id<MTLRenderPipelineState> solid,
                   RedrawMode mode, bool verify)
    : device_{device},
      queue_{[device newCommandQueue]},
      opaque_{opaque},
      blended_{blended},
      solid_{solid},
      mode_{mode},
      verify_{verify && mode == RedrawMode::Damage} {}

RenderStats Renderer::stats() const {
    return {.frames = frames_,
            .gpu = std::chrono::microseconds{static_cast<std::chrono::microseconds::rep>(
                gpu_nanoseconds_->load(std::memory_order_relaxed) / kNanosecondsPerMicrosecond)},
            .verified = verified_,
            .mismatched = mismatched_};
}

std::optional<Renderer::Buffer> Renderer::make_buffer(PixelSize size) const {
    const CfOwner<CFMutableDictionaryRef> properties{CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)};
    if (!properties) return std::nullopt;
    if (!set_number(properties.get(), kIOSurfaceWidth, size.width) ||
        !set_number(properties.get(), kIOSurfaceHeight, size.height) ||
        !set_number(properties.get(), kIOSurfaceBytesPerElement, kBytesPerPixel) ||
        !set_number(properties.get(), kIOSurfacePixelFormat,
                    static_cast<std::int32_t>(kPixelFormatBGRA))) {
        return std::nullopt;
    }
    CfOwner<IOSurfaceRef> surface{IOSurfaceCreate(properties.get())};
    if (!surface) return std::nullopt;
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:static_cast<NSUInteger>(size.width)
                                    height:static_cast<NSUInteger>(size.height)
                                 mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget;
    descriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device_ newTextureWithDescriptor:descriptor
                                                     iosurface:surface.get()
                                                         plane:0];
    if (!texture) return std::nullopt;
    return Buffer{.surface = std::move(surface),
                  .texture = texture,
                  .stale = Region{Rect{0, 0, size.width, size.height}},
                  .presented = 0};
}

// Prefers the most recently shown buffer the window server has let go of, since it has the
// least to repaint; grows the pool only when every buffer is still in use.
std::optional<std::size_t> Renderer::acquire(const Display& display) {
    std::optional<std::size_t> best;
    for (std::size_t index{0}; index < pool_.size(); ++index) {
        const Buffer& buffer = pool_.at(index);
        if (index == on_screen_ || IOSurfaceIsInUse(buffer.surface.get())) continue;
        if (!best || buffer.presented > pool_.at(*best).presented) best = index;
    }
    if (best || pool_.size() >= kMaxBuffers) return best;
    std::optional<Buffer> buffer = make_buffer(display.pixels());
    if (!buffer) return std::nullopt;
    pool_.push_back(std::move(*buffer));
    return pool_.size() - 1;
}

FrameOutcome Renderer::frame(Server& server, CALayer* layer) {
    const Region damage = server.take_damage();
    if (!server.display()) return FrameOutcome::Idle;
    const Display display{*server.display()};
    const PixelSize size{display.pixels()};
    if (size.width <= 0 || size.height <= 0) return FrameOutcome::Idle;
    const Rect bounds{display.pixel_bounds()};

    if (size != pool_size_) {
        // The layer keeps its own reference to whatever it shows.
        pool_.clear();
        on_screen_.reset();
        scratch_.reset();
        pool_size_ = size;
        owed_ = true;
    }
    for (Buffer& buffer : pool_) {
        buffer.stale.add(damage);
        if (buffer.stale.size() > kMaxStaleRects) {
            buffer.stale.clear();
            buffer.stale.add(bounds);
        }
    }
    owed_ = owed_ || !damage.empty();
    if (!owed_) return FrameOutcome::Idle;

    const std::optional<std::size_t> chosen = acquire(display);
    if (!chosen) {
        if (pool_.size() >= kMaxBuffers) return FrameOutcome::Deferred;
        if (!std::exchange(reported_failure_, true)) {
            std::fprintf(stderr, "compositor: cannot create a %dx%d display buffer\n", size.width,
                         size.height);
        }
        return FrameOutcome::Idle;
    }
    Buffer& buffer = pool_.at(*chosen);

    id<MTLCommandBuffer> commands = [queue_ commandBuffer];
    if (mode_ == RedrawMode::Damage) {
        encode(commands, buffer.texture, MTLLoadActionLoad, server, display, buffer.stale, true);
    } else {
        encode(commands, buffer.texture, MTLLoadActionDontCare, server, display, Region{bounds},
               mode_ == RedrawMode::Full);
    }
    const std::shared_ptr<std::atomic<std::uint64_t>> gpu{gpu_nanoseconds_};
    [commands addCompletedHandler:^(id<MTLCommandBuffer> done) {
        if (!completed(done)) return;
        const CFTimeInterval seconds{done.GPUEndTime - done.GPUStartTime};
        if (!(seconds > 0)) return;
        gpu->fetch_add(static_cast<std::uint64_t>(std::llround(seconds * kNanosecondsPerSecond)),
                       std::memory_order_relaxed);
    }];
    [commands commit];
    if (verify_) {
        [commands waitUntilCompleted];
        if (commands.status == MTLCommandBufferStatusCompleted) verify(buffer, server, display);
    } else {
        // Once scheduled, the window server's reads of the surface wait for the GPU's writes.
        [commands waitUntilScheduled];
    }

    buffer.stale.clear();
    buffer.presented = ++presentations_;
    on_screen_ = *chosen;
    owed_ = false;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.contents = (__bridge id)buffer.surface.get();
    [CATransaction commit];
    ++frames_;
    return FrameOutcome::Presented;
}

void Renderer::encode(id<MTLCommandBuffer> commands, id<MTLTexture> target, MTLLoadAction load,
                      const Server& server, const Display& display, const Region& repaint,
                      bool cull) const {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = load;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];

    const Rect bounds{display.pixel_bounds()};
    const simd_float2 target_size{static_cast<float>(bounds.width),
                                  static_cast<float>(bounds.height)};
    const auto draw = [&](const Rect& quad, const Rect& clip) {
        const std::optional<MTLScissorRect> scissor = scissor_of(clip, bounds);
        if (!scissor) return;
        const QuadUniforms uniforms{
            .rect = simd_make_float4(static_cast<float>(quad.x), static_cast<float>(quad.y),
                                     static_cast<float>(quad.width),
                                     static_cast<float>(quad.height)),
            .target = target_size,
        };
        [encoder setScissorRect:*scissor];
        [encoder setVertexBytes:&uniforms length:sizeof uniforms atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    };
    const auto draw_background = [&](const std::vector<Rect>& rects) {
        [encoder setRenderPipelineState:solid_];
        [encoder setFragmentBytes:&kBackground length:sizeof kBackground atIndex:0];
        for (const Rect& rect : rects) draw(bounds, rect);
    };
    // Every pixel's bottom-most contribution replaces, so a partial repaint never depends on
    // what the buffer held before.
    const auto draw_window = [&](const DrawnWindow& window) {
        [encoder setFragmentTexture:window.texture atIndex:0];
        if (!window.opaque_rects.empty()) [encoder setRenderPipelineState:opaque_];
        for (const Rect& rect : window.opaque_rects) draw(window.frame_pixels, rect);
        if (!window.translucent_rects.empty()) [encoder setRenderPipelineState:blended_];
        for (const Rect& rect : window.translucent_rects) draw(window.frame_pixels, rect);
    };

    if (!cull) {
        draw_background({bounds});
        for (const Window& window : server.scene().back_to_front()) {
            id<MTLTexture> texture = server.drawn_texture(window);
            if (!texture) continue;
            const Rect frame{display.frame_pixels(window.frame)};
            const Rect opaque{server.opaque_pixels(window)};
            Region translucent{frame};
            translucent.subtract(opaque);
            draw_window({.texture = texture,
                         .frame_pixels = frame,
                         .opaque_rects = Region{opaque}.rects(),
                         .translucent_rects = translucent.rects()});
        }
        [encoder endEncoding];
        return;
    }

    Occlusion occlusion{bounds};
    std::vector<DrawnWindow> drawn;
    for (const Window& window : server.scene().back_to_front() | std::views::reverse) {
        id<MTLTexture> texture = server.drawn_texture(window);
        if (!texture) continue;
        const Rect frame{display.frame_pixels(window.frame)};
        const Rect opaque{server.opaque_pixels(window)};
        const Region shown{occlusion.uncovered(frame).intersected(repaint)};
        occlusion.cover(opaque);
        if (shown.empty()) continue;
        Region translucent{shown};
        translucent.subtract(opaque);
        drawn.push_back({.texture = texture,
                         .frame_pixels = frame,
                         .opaque_rects = shown.intersected(Region{opaque}).rects(),
                         .translucent_rects = translucent.rects()});
    }
    draw_background(occlusion.uncovered_display().intersected(repaint).rects());
    for (const DrawnWindow& window : drawn | std::views::reverse) draw_window(window);
    [encoder endEncoding];
}

// Draws the whole scene afresh into a scratch buffer and compares it with what the frame left
// in the buffer about to be shown. The frame's GPU work must be complete.
void Renderer::verify(const Buffer& buffer, const Server& server, const Display& display) {
    if (!scratch_) scratch_ = make_buffer(display.pixels());
    if (!scratch_) {
        std::fprintf(stderr, "compositor: cannot create a verification buffer\n");
        return;
    }
    id<MTLCommandBuffer> commands = [queue_ commandBuffer];
    encode(commands, scratch_->texture, MTLLoadActionDontCare, server, display,
           Region{display.pixel_bounds()}, true);
    [commands commit];
    [commands waitUntilCompleted];
    if (!completed(commands)) return;
    ++verified_;
    if (read_pixels(buffer.texture) != read_pixels(scratch_->texture)) ++mismatched_;
}

}  // namespace pw
