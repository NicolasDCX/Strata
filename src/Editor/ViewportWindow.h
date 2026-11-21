#pragma once

#include "Application/AppController.h"
#include "Renderer/OrbitCamera.h"
#include "Renderer/RenderScene.h"

#include <QPointF>
#include <QVulkanWindow>

#include <atomic>

namespace se::editor {

/// The 3D viewport: a Vulkan window that turns pointer input into camera moves
/// and sculpt strokes.
///
/// It talks to the engine only through app::AppController and to the renderer
/// only through render::RenderScene. Sculpting is asynchronous: pointer events
/// are queued to the engine's sculpt thread and return at once; when the engine
/// reports a change, the viewport redraws. Mouse handling is split into the three
/// pointer* functions so the scripted self-test can drive exactly the same code
/// path as real mouse events.
///
/// Controls:
///   LMB on the model            sculpt with the active brush
///   Ctrl + LMB                  sculpt with the brush direction inverted
///   Shift + LMB                 smooth, whatever brush is selected
///   LMB on empty space / RMB / Alt + LMB    orbit
///   MMB / Shift + RMB           pan
///   Wheel                       zoom
class ViewportWindow final : public QVulkanWindow {
    Q_OBJECT

public:
    explicit ViewportWindow(app::AppController& controller, const render::RenderSettings& settings = {});
    ~ViewportWindow() override;

    QVulkanWindowRenderer* createRenderer() override;

    /// Schedules a redraw and tells listeners the displayed state may have
    /// changed. Call after anything outside this window changed the brush.
    void refresh();

    /// Fits the camera to the current mesh bounds.
    void frameMesh();

    // Pointer input in window coordinates (device independent pixels).
    void pointerPress(const QPointF& position, Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
    void pointerMove(const QPointF& position);
    void pointerRelease();
    void wheelZoom(float steps);
    /// The pick ray through a window position (device independent pixels).
    [[nodiscard]] geo::Ray rayAt(const QPointF& position) const;

    [[nodiscard]] render::OrbitCamera& camera() noexcept { return m_camera; }
    [[nodiscard]] const render::RenderStats& renderStats() const noexcept { return m_renderStats; }
    /// Chunk culling can be switched while running; the GPU memory settings cannot.
    void setCullMode(render::CullMode mode);
    /// On: render a frame as soon as the GUI thread hears of an engine change
    /// (an UpdateRequest sent directly), instead of requestUpdate(), which Qt
    /// delivers from a timer. Off (default): requestUpdate().
    void setFrameOnEngineChange(bool enabled) noexcept { m_frameOnEngineChange = enabled; }
    [[nodiscard]] bool frameOnEngineChange() const noexcept { return m_frameOnEngineChange; }
    /// Measurement only: draws the scene this many times per frame (RenderSettings::drawRepeat).
    void setDrawRepeat(u32 repeat) { m_scene.settings.drawRepeat = repeat; }
    /// PROTOTYPE seam test: 0 off, 1 odd chunks coarse, 2 all chunks coarse (RenderSettings::coarseDebug).
    /// Leaves the brush cursor alone so captures before and after stay comparable.
    void setCoarseDebug(u32 mode)
    {
        m_scene.settings.coarseDebug = mode;
        requestUpdate();
    }
    /// Test support: a single world-space light for shading inspection, or
    /// zero for the editor's lighting (RenderSettings::inspectionLight).
    void setInspectionLight(const glm::vec3& direction)
    {
        m_scene.settings.inspectionLight = direction;
        requestUpdate();
    }
    /// Test support: hides the brush cursor without changing its position or radius.
    void setCursorHiddenForCapture(bool hidden)
    {
        m_scene.cursor.color.a = hidden ? 0.0f : m_cursorOpacity;
        requestUpdate();
    }
    [[nodiscard]] render::CullMode cullMode() const noexcept { return m_scene.settings.cullMode; }
    [[nodiscard]] const render::RenderSettings& renderSettings() const noexcept { return m_scene.settings; }

    /// Starts (clearing earlier samples) or stops per-frame sample recording.
    void setFrameSampling(bool enabled);
    /// True when the pointer is over the model (brush cursor visible).
    [[nodiscard]] bool hovering() const noexcept { return m_scene.cursor.visible; }

signals:
    /// Emitted when something the status bar shows may have changed.
    void viewportChanged();
    /// Emitted when a stroke finished (undo availability changed).
    void strokeFinished();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    /// Stamps update requests while frames are sampled (latency tracing).
    bool event(QEvent* event) override;

private:
    enum class Interaction { None, Sculpt, Orbit, Pan };

    void updateCursor(const QPointF& position);
    /// Runs on the GUI thread after the sculpt thread changed something.
    void onEngineChanged();

    app::AppController& m_controller;
    render::OrbitCamera m_camera;
    render::RenderScene m_scene;
    float m_cursorOpacity = render::BrushCursor{}.color.a;
    render::RenderStats m_renderStats;

    Interaction m_interaction = Interaction::None;
    Qt::MouseButton m_activeButton = Qt::NoButton;
    QPointF m_lastPosition;
    std::atomic<bool> m_engineChangeQueued{false};
    bool m_frameOnEngineChange = false;
};

} // namespace se::editor
