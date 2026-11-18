#version 450

// PROTOTYPE coarse fallback (CoarseMeshBuffers): draws the simplified stand-in
// of a chunk whose detailed pages are not on the GPU. Each pool slot holds one
// chunk's coarse vertices, 4 uints each: position xyz (float bits) and the
// normal packed as snorm8x4. The index buffer is local to the slot; the draw's
// vertexOffset selects the slot. Positions are bit-identical copies of the
// mesh positions and transformed by the same expression as mesh_paged.vert
// (both declare gl_Position invariant), so shared border edges rasterize
// exactly as in a detailed neighbour: no cracks.

layout(push_constant) uniform FrameConstants {
    mat4 viewProjection;
    vec4 eyePosition;
    vec4 brushSphere;
    vec4 brushColor;
    uvec4 params;
} frame;

layout(set = 0, binding = 1) readonly buffer CoarsePool {
    uint data[];
} pool;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outNormal;

invariant gl_Position;

void main()
{
    uint at = uint(gl_VertexIndex) * 4u;
    vec3 position = vec3(uintBitsToFloat(pool.data[at]), uintBitsToFloat(pool.data[at + 1u]),
                         uintBitsToFloat(pool.data[at + 2u]));
    vec3 normal = unpackSnorm4x8(pool.data[at + 3u]).xyz;

    outWorldPosition = position;
    outNormal = normal;
    gl_Position = frame.viewProjection * vec4(position, 1.0);
}
