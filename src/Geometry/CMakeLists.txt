add_library(se_geometry STATIC
    Aabb.h
    Bvh.h
    BvhBuilder.cpp
    BvhRefit.cpp
    BvhTraversal.cpp
    ChunkLod.cpp
    ChunkLod.h
    ChunkedBvh.cpp
    ChunkedBvh.h
    DirtyVertexSet.h
    EditRegion.cpp
    EditRegion.h
    Icosphere.cpp
    Icosphere.h
    IcosphereCache.cpp
    IcosphereCache.h
    Intersect.cpp
    Intersect.h
    Mesh.h
    NormalCone.h
    PagedArray.h
    PagedMesh.cpp
    PagedMesh.h
    Ray.h
    SpatialOrder.cpp
    SpatialOrder.h)

target_link_libraries(se_geometry
    PUBLIC  se_core glm::glm
    PRIVATE se_warnings)

# Vulkan clip space depth is [0, 1]; every module must agree on GLM's conventions.
target_compile_definitions(se_geometry PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS)
