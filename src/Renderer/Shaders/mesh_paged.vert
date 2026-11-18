#version 450

// The mesh vertex shader (page pools, see ResidentMeshBuffers). There are no vertex buffers:
// the index buffer holds global vertex ids, and this shader looks the vertex up
// itself, through a page table, in a pool of GPU vertex pages.
//
//   id -> page = id >> pageShift -> slot = pageTable[page] -> pool[slot][id & mask]
//
// That indirection is what lets a vertex page live in any slot (an edited page
// moves to a fresh slot; under a budget the pool is smaller than the mesh),
// without renumbering the vertices or duplicating the ones shared between chunks.

layout(push_constant) uniform FrameConstants {
    mat4 viewProjection;
    vec4 eyePosition;
    vec4 brushSphere;
    vec4 brushColor;
    uvec4 params;       // x: log2 of the GPU vertex page size
} frame;

// One entry per GPU vertex page: the pool slot holding it. Each frame slot has
// its own table, so a frame in flight keeps the mapping it was recorded with.
layout(set = 0, binding = 0) readonly buffer PageTable {
    uint slotOf[];
} pageTable;

// Pool slots. A slot is [pageSize positions | pageSize normals], three floats
// each. Declared as floats because a vec3 array would be padded to 16 bytes.
layout(set = 0, binding = 1) readonly buffer VertexPool {
    float data[];
} pool;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outNormal;

// Same transform, bit for bit, as mesh_coarse.vert (crack-free LOD seams).
invariant gl_Position;

void main()
{
    uint id = uint(gl_VertexIndex);
    uint pageShift = frame.params.x;
    uint pageSize = 1u << pageShift;
    uint slot = pageTable.slotOf[id >> pageShift];

    uint positionAt = (slot * pageSize * 2u + (id & (pageSize - 1u))) * 3u;
    uint normalAt = positionAt + pageSize * 3u;
    vec3 position = vec3(pool.data[positionAt], pool.data[positionAt + 1u], pool.data[positionAt + 2u]);
    vec3 normal = vec3(pool.data[normalAt], pool.data[normalAt + 1u], pool.data[normalAt + 2u]);

    outWorldPosition = position;
    outNormal = normal;
    gl_Position = frame.viewProjection * vec4(position, 1.0);
}
