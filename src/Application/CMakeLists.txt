add_library(se_application STATIC
    AppController.cpp
    AppController.h)

target_link_libraries(se_application
    PUBLIC  se_sculpt
    PRIVATE se_warnings)
