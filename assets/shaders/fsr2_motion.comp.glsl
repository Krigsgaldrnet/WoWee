#version 450

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D depthBuffer;
layout(set = 0, binding = 1, rg16f) uniform writeonly image2D motionVectors;

layout(push_constant) uniform PushConstants {
    mat4 invViewProj;       // Inverse of current jittered VP
    mat4 prevViewProj;      // Previous frame unjittered VP
    vec4 resolution;        // xy = internal size, zw = 1/internal size
    vec4 jitterOffset;      // xy = current jitter (NDC), zw = previous jitter
} pc;

void main() {
    ivec2 pixelCoord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = ivec2(pc.resolution.xy);
    if (pixelCoord.x >= imgSize.x || pixelCoord.y >= imgSize.y) return;

    // Sample depth (Vulkan: 0 = near, 1 = far)
    float depth = texelFetch(depthBuffer, pixelCoord, 0).r;

    // Pixel center in NDC [-1, 1]
    vec2 uv = (vec2(pixelCoord) + 0.5) * pc.resolution.zw;
    vec2 ndc = uv * 2.0 - 1.0;

    // Reconstruct world position from depth
    vec4 clipPos = vec4(ndc, depth, 1.0);
    vec4 worldPos = pc.invViewProj * clipPos;
    worldPos /= worldPos.w;

    // Project into previous frame's clip space (unjittered)
    vec4 prevClip = pc.prevViewProj * worldPos;
    vec2 prevNdc = prevClip.xy / prevClip.w;
    vec2 prevUV = prevNdc * 0.5 + 0.5;

    // Remove jitter from current UV to get unjittered position
    vec2 unjitteredUV = uv - pc.jitterOffset.xy * 0.5;

    // Motion = previous position - current unjittered position (in UV space)
    vec2 motion = prevUV - unjitteredUV;

    imageStore(motionVectors, pixelCoord, vec4(motion, 0.0, 0.0));
}
