set(CMAKE_AUTOMOC ON)

add_library(se_editor STATIC
    EditorApp.cpp
    EditorApp.h
    MainWindow.cpp
    MainWindow.h
    SelfTest.cpp
    SelfTest.h
    ViewportWindow.cpp
    ViewportWindow.h)

target_link_libraries(se_editor
    PUBLIC  se_application se_renderer Qt6::Widgets
    PRIVATE se_warnings)
