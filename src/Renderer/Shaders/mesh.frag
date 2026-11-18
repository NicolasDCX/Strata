#version 450

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;

layout(push_constant) uniform FrameConstants {
    mat4 viewProjection;
    vec4 eyePosition;
    vec4 brushSphere;
    vec4 brushColor;
    uvec4 params;       // x: log2 of the GPU vertex page size (used by mesh_paged.vert)
} frame;

layout(location = 0) out vec4 outColor;

const vec3 kClay = vec3(0.62, 0.36, 0.27);
const vec3 kSkyAmbient = vec3(0.22, 0.24, 0.28);
const vec3 kGroundAmbient = vec3(0.10, 0.08, 0.07);
const vec3 kKeyDirection = normalize(vec3(0.45, 0.80, 0.40));

void main()
{
    vec3 normal = normalize(inNormal);
    vec3 toEye = normalize(frame.eyePosition.xyz - inWorldPosition);

    // Basic diffuse shading: a fixed key light, a camera-attached fill light so
    // the surface relief stays readable from every angle, and hemisphere ambient.
    float key = max(dot(normal, kKeyDirection), 0.0);
    float fill = max(dot(normal, toEye), 0.0);
    vec3 ambient = mix(kGroundAmbient, kSkyAmbient, normal.y * 0.5 + 0.5);
    vec3 color = kClay * (ambient * 0.8 + 0.62 * key + 0.24 * fill);

    // Soft specular highlight from the key light.
    vec3 halfVector = normalize(kKeyDirection + toEye);
    color += vec3(0.16) * pow(max(dot(normal, halfVector), 0.0), 48.0);

    // Brush cursor: the intersection of the brush sphere with the surface,
    // drawn as an anti-aliased ring plus a faint tint of the influence area.
    float radius = frame.brushSphere.w;
    if (radius > 0.0) {
        float distanceToBrush = distance(inWorldPosition, frame.brushSphere.xyz);
        float pixel = max(fwidth(distanceToBrush), 1e-6);
        float ring = 1.0 - smoothstep(pixel * 0.75, pixel * 2.25, abs(distanceToBrush - radius));
        float inside = 1.0 - smoothstep(radius - pixel, radius, distanceToBrush);
        color = mix(color, frame.brushColor.rgb, frame.brushColor.a * max(ring, inside * 0.07));
    }

    // The swapchain is a UNORM target: encode to display gamma here.
    outColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
}
