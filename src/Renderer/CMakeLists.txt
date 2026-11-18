# --- Shaders: GLSL -> SPIR-V -> embedded C++ header --------------------------
set(SE_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/generated/Renderer/Shaders")
file(MAKE_DIRECTORY "${SE_SHADER_OUTPUT_DIR}")

function(se_compile_shader source symbol out_header_var)
    get_filename_component(_name "${source}" NAME)
    set(_spv "${SE_SHADER_OUTPUT_DIR}/${_name}.spv")
    set(_header "${SE_SHADER_OUTPUT_DIR}/${_name}.h")
    add_custom_command(
        OUTPUT "${_header}"
        COMMAND ${SE_GLSLANG_COMMAND} -V --quiet -o "${_spv}" "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
        COMMAND "${CMAKE_COMMAND}" -DINPUT=${_spv} -DOUTPUT=${_header} -DSYMBOL=${symbol}
                -P "${CMAKE_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        DEPENDS "${source}" "${CMAKE_SOURCE_DIR}/cmake/EmbedSpirv.cmake" ${SE_GLSLANG_DEPENDS}
        COMMENT "Compiling shader ${source}"
        VERBATIM)
    set(${out_header_var} "${_header}" PARENT_SCOPE)
endfunction()

se_compile_shader(Shaders/mesh.frag kMeshFragmentSpirv SE_MESH_FRAG_HEADER)
se_compile_shader(Shaders/mesh_paged.vert kMeshPagedVertexSpirv SE_MESH_PAGED_VERT_HEADER)
se_compile_shader(Shaders/mesh_coarse.vert kMeshCoarseVertexSpirv SE_MESH_COARSE_VERT_HEADER)

add_library(se_renderer STATIC
    ChunkPageMap.h
    Frustum.h
    PagePool.h
    OrbitCamera.cpp
    OrbitCamera.h
    RenderScene.h
    Vulkan/CoarseMeshBuffers.cpp
    Vulkan/CoarseMeshBuffers.h
    Vulkan/GpuBuffer.cpp
    Vulkan/GpuBuffer.h
    Vulkan/GpuTimer.cpp
    Vulkan/GpuTimer.h
    Vulkan/MeshPipeline.cpp
    Vulkan/MeshPipeline.h
    Vulkan/ResidentMeshBuffers.cpp
    Vulkan/ResidentMeshBuffers.h
    Vulkan/SceneRenderer.cpp
    Vulkan/SceneRenderer.h
    Vulkan/VulkanContext.cpp
    Vulkan/VulkanContext.h
    "${SE_MESH_FRAG_HEADER}"
    "${SE_MESH_PAGED_VERT_HEADER}"
    "${SE_MESH_COARSE_VERT_HEADER}")

target_include_directories(se_renderer PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(se_renderer
    PUBLIC  se_geometry Qt6::Gui se_vulkan_headers
    PRIVATE se_warnings)
