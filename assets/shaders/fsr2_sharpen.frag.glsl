#version 450

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D inputImage;

layout(push_constant) uniform PushConstants {
    vec4 params;  // x = 1/width, y = 1/height, z = sharpness (0-2), w = unused
} pc;

void main() {
    vec2 texelSize = pc.params.xy;
    float sharpness = pc.params.z;

    // RCAS: Robust Contrast-Adaptive Sharpening
    // 5-tap cross pattern
    vec3 center = texture(inputImage, TexCoord).rgb;
    vec3 north  = texture(inputImage, TexCoord + vec2(0.0, -texelSize.y)).rgb;
    vec3 south  = texture(inputImage, TexCoord + vec2(0.0,  texelSize.y)).rgb;
    vec3 west   = texture(inputImage, TexCoord + vec2(-texelSize.x, 0.0)).rgb;
    vec3 east   = texture(inputImage, TexCoord + vec2( texelSize.x, 0.0)).rgb;

    // Compute local contrast (min/max of neighborhood)
    vec3 minRGB = min(center, min(min(north, south), min(west, east)));
    vec3 maxRGB = max(center, max(max(north, south), max(west, east)));

    // Adaptive sharpening weight based on local contrast
    // High contrast = less sharpening (prevent ringing)
    vec3 range = maxRGB - minRGB;
    vec3 rcpRange = 1.0 / (range + 0.001);

    // Sharpening amount: inversely proportional to contrast
    float luma = dot(center, vec3(0.299, 0.587, 0.114));
    float lumaRange = max(range.r, max(range.g, range.b));
    float w = clamp(1.0 - lumaRange * 2.0, 0.0, 1.0) * sharpness * 0.25;

    // Apply sharpening via unsharp mask
    vec3 avg = (north + south + west + east) * 0.25;
    vec3 sharpened = center + (center - avg) * w;

    // Clamp to prevent ringing artifacts
    sharpened = clamp(sharpened, minRGB, maxRGB);

    FragColor = vec4(sharpened, 1.0);
}
