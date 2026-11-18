cmake_minimum_required(VERSION 3.28)

project(SculptEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SE_BUILD_APP   "Build the Qt 6 / Vulkan editor application" ON)
option(SE_BUILD_TESTS "Build the unit tests (no GPU required)"     ON)
option(SE_BUILD_BENCH "Build the headless geometry/sculpt benchmark" ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(FetchContent)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Warnings shared by all first-party targets
# ---------------------------------------------------------------------------
add_library(se_warnings INTERFACE)
if(MSVC)
    target_compile_options(se_warnings INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(se_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
        # Designated initialisers that leave defaulted members out are intentional.
        -Wno-missing-field-initializers)
endif()

# ---------------------------------------------------------------------------
# GLM (header only). Uses an installed package when present, otherwise fetched.
# ---------------------------------------------------------------------------
find_package(glm CONFIG QUIET)
if(NOT glm_FOUND)
    message(STATUS "SculptEngine: GLM not found, fetching 1.0.1")
    FetchContent_Declare(glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG        1.0.1
        GIT_SHALLOW    TRUE
        EXCLUDE_FROM_ALL
        SYSTEM)
    FetchContent_MakeAvailable(glm)
endif()

add_subdirectory(src)

if(SE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SE_BUILD_BENCH)
    add_subdirectory(tools/Bench)
endif()
