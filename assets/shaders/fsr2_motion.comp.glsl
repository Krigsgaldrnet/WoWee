#version 450

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D depthBuffer;
layout(set = 0, binding = 1, rg16f) uniform writeonly image2D motionVectors;

layout(push_constant) uniform PushConstants {
    mat4 reprojMatrix;      // prevUnjitteredVP * inverse(currentUnjitteredVP)
    vec4 resolution;        // xy = internal size, zw = 1/internal size
    vec4 jitterOffset;      // xy = current jitter (NDC), zw = unused
} pc;

void main() {
    ivec2 pixelCoord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = ivec2(pc.resolution.xy);
    if (pixelCoord.x >= imgSize.x || pixelCoord.y >= imgSize.y) return;

    float depth = texelFetch(depthBuffer, pixelCoord, 0).r;

    // Pixel center UV and NDC
    vec2 uv = (vec2(pixelCoord) + 0.5) * pc.resolution.zw;
    vec2 ndc = uv * 2.0 - 1.0;

    // Unjitter the NDC: the scene was rendered with jitter applied to
    // projection[2][0/1]. For RH perspective (P[2][3]=-1, clip.w=-vz):
    //   jittered_ndc = unjittered_ndc - jitter
    //   unjittered_ndc = ndc + jitter
    vec2 unjitteredNDC = ndc + pc.jitterOffset.xy;

    // Reproject to previous frame via unjittered VP matrices
    vec4 clipPos = vec4(unjitteredNDC, depth, 1.0);
    vec4 prevClip = pc.reprojMatrix * clipPos;
    vec2 prevNdc = prevClip.xy / prevClip.w;
    vec2 prevUV = prevNdc * 0.5 + 0.5;

    // Current unjittered UV for this pixel's world content
    vec2 currentUnjitteredUV = unjitteredNDC * 0.5 + 0.5;

    // Motion between unjittered positions — jitter-free.
    // For a static scene (identity reprojMatrix), this is exactly zero.
    vec2 motion = prevUV - currentUnjitteredUV;

    imageStore(motionVectors, pixelCoord, vec4(motion, 0.0, 0.0));
}
