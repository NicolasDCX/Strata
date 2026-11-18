#pragma once

#include "Application/AppController.h"
#include "Renderer/RenderScene.h"

#include <QList>
#include <QMainWindow>

class QAction;
class QActionGroup;
class QLabel;
class QSlider;
class QVulkanInstance;

namespace se::editor {

class ViewportWindow;

/// Main editor window: toolbar (brush, radius, strength, undo/redo), the
/// embedded Vulkan viewport and a status bar. All engine access goes through
/// app::AppController.
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(app::AppController& controller, QVulkanInstance& vulkanInstance,
               const render::RenderSettings& renderSettings = {}, QWidget* parent = nullptr);

    [[nodiscard]] ViewportWindow& viewport() noexcept { return *m_viewport; }

    /// Selects a brush and syncs the toolbar (also used by the self-test).
    void selectBrush(sculpt::BrushId id);

    void undo();
    void redo();

protected:
    /// The viewport is a native child window with its own keyboard focus, so
    /// window shortcuts would not fire while it is focused. Key presses are
    /// intercepted here and matched against the registered actions.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildToolbar();
    void buildStatusBar();
    QAction* addShortcutAction(const QString& text, const QKeySequence& shortcut, const QString& toolTip);

    void syncControlsFromBrush();
    void updateHistoryActions();
    void updateStatus();
    void scaleRadius(float factor);

    app::AppController& m_controller;
    ViewportWindow* m_viewport = nullptr; // owned by its container widget

    QActionGroup* m_brushGroup = nullptr;
    QList<QAction*> m_brushActions;
    QList<QAction*> m_shortcutActions;
    QAction* m_invertAction = nullptr;
    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;
    QAction* m_cullAction = nullptr;

    QSlider* m_radiusSlider = nullptr;
    QSlider* m_strengthSlider = nullptr;
    QLabel* m_radiusValue = nullptr;
    QLabel* m_strengthValue = nullptr;

    QLabel* m_meshLabel = nullptr;
    QLabel* m_brushLabel = nullptr;
    QLabel* m_historyLabel = nullptr;
    QLabel* m_timingLabel = nullptr;
    QLabel* m_renderLabel = nullptr;
};

} // namespace se::editor
