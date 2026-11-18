# Module dependency graph (arrows point at dependencies):
#
#   Core <- Geometry <- Sculpt <- Application <- Editor -> Renderer -> Geometry
#
# Geometry, Sculpt and Application never see Qt or Vulkan.

add_subdirectory(Core)
add_subdirectory(Geometry)
add_subdirectory(Sculpt)
add_subdirectory(Application)

if(SE_BUILD_APP)
    # --- Vulkan headers ----------------------------------------------------
    # QVulkanWindow resolves all Vulkan entry points at runtime, so only the
    # headers are needed at build time. Use a Vulkan SDK when present.
    find_path(SE_VULKAN_INCLUDE_DIR vulkan/vulkan.h
        HINTS ENV VULKAN_SDK
        PATH_SUFFIXES include Include)
    if(NOT SE_VULKAN_INCLUDE_DIR)
        message(STATUS "SculptEngine: Vulkan headers not found, fetching Vulkan-Headers")
        FetchContent_Declare(vulkan_headers
            GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
            GIT_TAG        v1.3.296
            GIT_SHALLOW    TRUE
            EXCLUDE_FROM_ALL
            SYSTEM)
        FetchContent_MakeAvailable(vulkan_headers)
        set(SE_VULKAN_INCLUDE_DIR "${vulkan_headers_SOURCE_DIR}/include" CACHE PATH "" FORCE)
    endif()
    # Lets Qt's own Vulkan header lookup succeed without an SDK.
    set(Vulkan_INCLUDE_DIR "${SE_VULKAN_INCLUDE_DIR}" CACHE PATH "" FORCE)

    add_library(se_vulkan_headers INTERFACE)
    target_include_directories(se_vulkan_headers SYSTEM INTERFACE "${SE_VULKAN_INCLUDE_DIR}")

    # --- GLSL -> SPIR-V compiler -------------------------------------------
    find_program(SE_GLSLANG NAMES glslangValidator glslang
        HINTS ENV VULKAN_SDK
        PATH_SUFFIXES bin Bin)
    if(SE_GLSLANG)
        set(SE_GLSLANG_COMMAND "${SE_GLSLANG}")
        set(SE_GLSLANG_DEPENDS "")
    else()
        message(STATUS "SculptEngine: glslang not found, building it from source as a host tool")
        set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
        set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
        set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
        set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
        set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(glslang
            GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
            GIT_TAG        15.1.0
            GIT_SHALLOW    TRUE
            EXCLUDE_FROM_ALL
            SYSTEM)
        FetchContent_MakeAvailable(glslang)
        set(SE_GLSLANG_COMMAND "$<TARGET_FILE:glslang-standalone>")
        set(SE_GLSLANG_DEPENDS glslang-standalone)
    endif()

    find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)

    add_subdirectory(Renderer)
    add_subdirectory(Editor)

    qt_add_executable(SculptEngine main.cpp)
    target_link_libraries(SculptEngine PRIVATE se_editor se_warnings)
    set_target_properties(SculptEngine PROPERTIES WIN32_EXECUTABLE TRUE)

    if(WIN32)
        # Copy the Qt runtime next to the executable so it starts from Explorer.
        get_target_property(_se_qmake Qt6::qmake IMPORTED_LOCATION)
        get_filename_component(_se_qt_bin "${_se_qmake}" DIRECTORY)
        find_program(SE_WINDEPLOYQT windeployqt HINTS "${_se_qt_bin}")
        if(SE_WINDEPLOYQT)
            add_custom_command(TARGET SculptEngine POST_BUILD
                COMMAND "${SE_WINDEPLOYQT}" --no-translations --no-system-d3d-compiler
                        --no-opengl-sw --compiler-runtime "$<TARGET_FILE:SculptEngine>"
                COMMENT "Deploying Qt runtime next to SculptEngine"
                VERBATIM)
        endif()
    endif()
endif()
