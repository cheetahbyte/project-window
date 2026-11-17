#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "compositor/cf_owner.h"
#include "compositor/display.h"
#include "compositor/region.h"
#include "compositor/server.h"

#include <IOSurface/IOSurfaceRef.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace pw {

enum class RedrawMode {
    // Every frame repaints the display, drawing every window in full from the back.
    Overdraw,
    // Every frame repaints the display, drawing only what of each window shows.
    Full,
    // Every frame repaints only what changed since the chosen buffer was last shown.
    Damage,
};

struct RenderStats {
    std::uint64_t frames{};
    std::chrono::microseconds gpu{};
    std::uint64_t verified{};
    std::uint64_t mismatched{};
};

enum class FrameOutcome {
    // Nothing needed drawing, or drawing failed and was reported.
    Idle,
    Presented,
    // Every buffer is still being read; try again at the next display refresh.
    Deferred,
};

// Draws the server's scene into its own pool of IOSurfaces and shows them as a layer's contents.
// Main thread only.
class Renderer {
public:
    // Returns nullptr after reporting on stderr when the shaders cannot be loaded.
    static std::unique_ptr<Renderer> create(id<MTLDevice> device, RedrawMode mode, bool verify);

    Renderer(id<MTLDevice> device, id<MTLRenderPipelineState> textured,
             id<MTLRenderPipelineState> solid, RedrawMode mode, bool verify);
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Takes the server's damage and, if anything is owed, draws a frame and sets it as the
    // layer's contents.
    FrameOutcome frame(Server& server, CALayer* layer);
    RenderStats stats() const;

private:
    struct Buffer {
        CfOwner<IOSurfaceRef> surface;
        id<MTLTexture> texture;
        // Display pixels that changed since this buffer was last drawn.
        Region stale;
        // Which presentation last showed it; 0 when never shown.
        std::uint64_t presented{};
    };

    std::optional<Buffer> make_buffer(PixelSize size) const;
    std::optional<std::size_t> acquire(const Display& display);
    void encode(id<MTLCommandBuffer> commands, id<MTLTexture> target, MTLLoadAction load,
                const Server& server, const Display& display, const Region& repaint,
                bool cull) const;
    void verify(const Buffer& buffer, const Server& server, const Display& display);

    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    id<MTLRenderPipelineState> textured_;
    id<MTLRenderPipelineState> solid_;
    RedrawMode mode_;
    bool verify_;

    std::vector<Buffer> pool_;
    PixelSize pool_size_{};
    std::optional<std::size_t> on_screen_;
    std::optional<Buffer> scratch_;
    std::uint64_t presentations_{};
    bool owed_{false};
    bool reported_failure_{false};

    std::uint64_t frames_{};
    std::uint64_t verified_{};
    std::uint64_t mismatched_{};
    // Written by command buffer completion handlers, which run off the main thread.
    std::shared_ptr<std::atomic<std::uint64_t>> gpu_nanoseconds_{
        std::make_shared<std::atomic<std::uint64_t>>(0)};
};

}  // namespace pw
