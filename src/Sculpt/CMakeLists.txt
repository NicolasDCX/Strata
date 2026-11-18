add_library(se_sculpt STATIC
    Brush.h
    BrushLibrary.cpp
    BrushLibrary.h
    Brushes/DrawBrush.cpp
    Brushes/DrawBrush.h
    Brushes/MoveBrush.cpp
    Brushes/MoveBrush.h
    Brushes/SmoothBrush.cpp
    Brushes/SmoothBrush.h
    Falloff.h
    NormalUpdater.cpp
    NormalUpdater.h
    PositionStaging.cpp
    PositionStaging.h
    StrokeProcessor.cpp
    StrokeProcessor.h
    UndoStack.cpp
    UndoStack.h)

target_link_libraries(se_sculpt
    PUBLIC  se_geometry
    PRIVATE se_warnings)
