#version 450

layout(local_size_x = 8, local_size_y = 8) in;

// Inputs (internal resolution)
layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(set = 0, binding = 1) uniform sampler2D depthBuffer;
layout(set = 0, binding = 2) uniform sampler2D motionVectors;

// History (display resolution)
layout(set = 0, binding = 3) uniform sampler2D historyInput;

// Output (display resolution)
layout(set = 0, binding = 4, rgba16f) uniform writeonly image2D historyOutput;

layout(push_constant) uniform PushConstants {
    vec4 internalSize;   // xy = internal resolution, zw = 1/internal
    vec4 displaySize;    // xy = display resolution, zw = 1/display
    vec4 jitterOffset;   // xy = current jitter (pixel-space), zw = unused
    vec4 params;         // x = resetHistory (1=reset), y = sharpness, zw = unused
} pc;

// RGB <-> YCoCg for neighborhood clamping
vec3 rgbToYCoCg(vec3 rgb) {
    float y  = 0.25 * rgb.r + 0.5 * rgb.g + 0.25 * rgb.b;
    float co = 0.5  * rgb.r                - 0.5  * rgb.b;
    float cg = -0.25 * rgb.r + 0.5 * rgb.g - 0.25 * rgb.b;
    return vec3(y, co, cg);
}

vec3 yCoCgToRgb(vec3 ycocg) {
    float y  = ycocg.x;
    float co = ycocg.y;
    float cg = ycocg.z;
    return vec3(y + co - cg, y + cg, y - co - cg);
}

void main() {
    ivec2 outPixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 outSize = ivec2(pc.displaySize.xy);
    if (outPixel.x >= outSize.x || outPixel.y >= outSize.y) return;

    // Output UV in display space
    vec2 outUV = (vec2(outPixel) + 0.5) * pc.displaySize.zw;

    // Map display pixel to internal resolution UV (accounting for jitter)
    vec2 internalUV = outUV;

    // Sample current frame color at internal resolution
    vec3 currentColor = texture(sceneColor, internalUV).rgb;

    // Sample motion vector at internal resolution
    vec2 inUV = outUV;  // Approximate — display maps to internal via scale
    vec2 motion = texture(motionVectors, inUV).rg;

    // Reproject: where was this pixel in the previous frame's history?
    vec2 historyUV = outUV - motion;

    // History reset: on teleport / camera cut, just use current frame
    if (pc.params.x > 0.5) {
        imageStore(historyOutput, outPixel, vec4(currentColor, 1.0));
        return;
    }

    // Sample reprojected history
    vec3 historyColor = texture(historyInput, historyUV).rgb;

    // Neighborhood clamping in YCoCg space to prevent ghosting
    // Sample 3x3 neighborhood from current frame
    vec2 texelSize = pc.internalSize.zw;
    vec3 samples[9];
    int idx = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            samples[idx] = rgbToYCoCg(texture(sceneColor, internalUV + vec2(dx, dy) * texelSize).rgb);
            idx++;
        }
    }

    // Compute AABB in YCoCg
    vec3 boxMin = samples[0];
    vec3 boxMax = samples[0];
    for (int i = 1; i < 9; i++) {
        boxMin = min(boxMin, samples[i]);
        boxMax = max(boxMax, samples[i]);
    }

    // Slightly expand the box to reduce flickering on edges
    vec3 boxCenter = (boxMin + boxMax) * 0.5;
    vec3 boxExtent = (boxMax - boxMin) * 0.5;
    boxMin = boxCenter - boxExtent * 1.25;
    boxMax = boxCenter + boxExtent * 1.25;

    // Clamp history to the neighborhood AABB
    vec3 historyYCoCg = rgbToYCoCg(historyColor);
    vec3 clampedHistory = clamp(historyYCoCg, boxMin, boxMax);
    historyColor = yCoCgToRgb(clampedHistory);

    // Check if history UV is valid (within [0,1])
    float historyValid = (historyUV.x >= 0.0 && historyUV.x <= 1.0 &&
                          historyUV.y >= 0.0 && historyUV.y <= 1.0) ? 1.0 : 0.0;

    // Blend factor: use more current frame for disoccluded regions
    // Luminance difference between clamped history and original → confidence
    float clampDist = length(historyYCoCg - clampedHistory);
    float blendFactor = mix(0.05, 0.3, clamp(clampDist * 4.0, 0.0, 1.0));

    // If history is off-screen, use current frame entirely
    blendFactor = mix(blendFactor, 1.0, 1.0 - historyValid);

    // Final blend
    vec3 result = mix(historyColor, currentColor, blendFactor);

    imageStore(historyOutput, outPixel, vec4(result, 1.0));
}
