find_package(Threads REQUIRED)

add_library(se_core STATIC
    Assert.h
    Error.h
    FairMutex.h
    JobSystem.cpp
    JobSystem.h
    Log.cpp
    Log.h
    ProcessClock.cpp
    ProcessClock.h
    Timer.h
    Types.h)

target_include_directories(se_core PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(se_core
    PUBLIC  Threads::Threads
    PRIVATE se_warnings)
