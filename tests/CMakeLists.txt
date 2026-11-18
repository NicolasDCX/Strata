find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
    message(STATUS "SculptEngine: GoogleTest not found, fetching v1.15.2")
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
        EXCLUDE_FROM_ALL
        SYSTEM)
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

function(se_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE GTest::gtest GTest::gtest_main se_warnings)
    target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    if(MINGW)
        # Self-contained test binaries: no MinGW runtime DLLs needed on PATH.
        target_link_options(${name} PRIVATE -static)
    endif()
    gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60)
endfunction()

add_subdirectory(GeometryTests)
add_subdirectory(SculptTests)
