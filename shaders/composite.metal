#include <metal_stdlib>
using namespace metal;

// Both rectangles in pixels of the render target.
struct QuadUniforms {
    float4 rect;
    float2 target;
};

struct QuadVertex {
    float4 position [[position]];
    float2 uv;
};

vertex QuadVertex quad_vertex(uint vertex_id [[vertex_id]],
                              constant QuadUniforms& uniforms [[buffer(0)]]) {
    float2 corner = float2(vertex_id & 1, vertex_id >> 1);
    float2 point = uniforms.rect.xy + corner * uniforms.rect.zw;
    QuadVertex out;
    out.position = float4(point.x / uniforms.target.x * 2.0 - 1.0,
                          1.0 - point.y / uniforms.target.y * 2.0, 0.0, 1.0);
    out.uv = corner;
    return out;
}

fragment float4 quad_fragment(QuadVertex in [[stage_in]],
                              texture2d<float> contents [[texture(0)]]) {
    constexpr sampler linear_sampler(filter::linear);
    return float4(contents.sample(linear_sampler, in.uv).rgb, 1.0);
}

fragment float4 solid_fragment(constant float4& colour [[buffer(0)]]) {
    return colour;
}
