#include "Editor/ViewportWindow.h"

#include "Renderer/Vulkan/SceneRenderer.h"

#include <QMouseEvent>
#include <QWheelEvent>

namespace se::editor {

ViewportWindow::ViewportWindow(app::AppController& controller, const render::RenderSettings& settings)
    : m_controller(controller)
{
    m_scene.settings = settings;
    m_scene.chunks = &m_controller.spatialIndex();
    m_scene.mesh = &m_controller.mesh();
    m_scene.camera = &m_camera;
    m_scene.meshLock = &m_controller.documentMutex();
    m_scene.pendingVertexUpdates = &m_controller.pendingVertexUpdates();
    m_camera.frame(m_controller.bounds());

    // Called on the sculpt thread: only hop to the GUI thread. The flag
    // collapses a burst of changes into one queued call.
    m_controller.setChangeListener([this] {
        if (!m_engineChangeQueued.exchange(true)) {
            QMetaObject::invokeMethod(this, [this] { onEngineChanged(); }, Qt::QueuedConnection);
        }
    });
}

ViewportWindow::~ViewportWindow()
{
    // After this returns the sculpt thread no longer calls into this object.
    m_controller.setChangeListener({});
}

void ViewportWindow::onEngineChanged()
{
    m_engineChangeQueued.store(false);
    if (m_controller.strokeActive()) {
        m_scene.cursor.center = m_controller.strokeCenter();
    }
    refresh();
}

QVulkanWindowRenderer* ViewportWindow::createRenderer()
{
    // Owned by QVulkanWindow.
    return new render::SceneRenderer(this, &m_scene, &m_renderStats);
}

void ViewportWindow::refresh()
{
    if (m_scene.cursor.visible && !m_controller.strokeActive()) {
        m_scene.cursor.radius = m_controller.activeSettings().radius;
    }
    requestUpdate();
    emit viewportChanged();
}

void ViewportWindow::setCullMode(render::CullMode mode)
{
    m_scene.settings.cullMode = mode;
    refresh();
}

void ViewportWindow::setFrameSampling(bool enabled)
{
    if (enabled) {
        m_renderStats.frameGpuSamplesMs.clear();
        m_renderStats.uploadKiBSamples.clear();
        m_renderStats.chunksDrawnSamples.clear();
        m_renderStats.trianglesSubmittedSamples.clear();
        m_renderStats.backFacingSamples.clear();
        m_renderStats.chunksCoarseSamples.clear();
        m_renderStats.chunksMissingSamples.clear();
        m_renderStats.evictionSamples.clear();
        m_renderStats.frameCpuSamplesMs.clear();
        m_renderStats.frameIntervalSamplesMs.clear();
        m_renderStats.lockWaitSamplesMs.clear();
        m_renderStats.uploadVertexSamples.clear();
    }
    m_renderStats.recordSamples = enabled;
}

void ViewportWindow::frameMesh()
{
    m_camera.frame(m_controller.bounds());
    refresh();
}

geo::Ray ViewportWindow::rayAt(const QPointF& position) const
{
    return m_camera.screenRay(static_cast<float>(position.x()), static_cast<float>(position.y()),
                              static_cast<float>(width()), static_cast<float>(height()));
}

void ViewportWindow::updateCursor(const QPointF& position)
{
    if (m_controller.strokeActive()) {
        // Follow the brush itself: for Move the edit centre is not under the pointer ray.
        m_scene.cursor.visible = true;
        m_scene.cursor.center = m_controller.strokeCenter();
        m_scene.cursor.radius = m_controller.strokeRadius();
        return;
    }
    if (m_interaction != Interaction::None) {
        m_scene.cursor.visible = false; // camera navigation
        return;
    }
    const std::optional<geo::RayHit> hit = m_controller.pick(rayAt(position));
    m_scene.cursor.visible = hit.has_value();
    if (hit) {
        m_scene.cursor.center = hit->position;
        m_scene.cursor.radius = m_controller.activeSettings().radius;
    }
}

void ViewportWindow::pointerPress(const QPointF& position, Qt::MouseButton button, Qt::KeyboardModifiers modifiers)
{
    if (m_interaction != Interaction::None) {
        return; // a second button while dragging is ignored
    }
    m_lastPosition = position;
    m_activeButton = button;

    const bool alt = modifiers.testFlag(Qt::AltModifier);
    const bool shift = modifiers.testFlag(Qt::ShiftModifier);
    const bool control = modifiers.testFlag(Qt::ControlModifier);

    if (button == Qt::LeftButton && !alt) {
        const app::StrokeModifiers strokeModifiers{.invert = control, .smooth = shift};
        if (m_controller.beginStroke(rayAt(position), strokeModifiers)) {
            m_interaction = Interaction::Sculpt;
        } else {
            m_interaction = Interaction::Orbit; // clicked beside the model
        }
    } else if (button == Qt::MiddleButton || (button == Qt::RightButton && shift)) {
        m_interaction = Interaction::Pan;
    } else if (button == Qt::LeftButton || button == Qt::RightButton) {
        m_interaction = Interaction::Orbit;
    } else {
        m_activeButton = Qt::NoButton;
        return;
    }
    updateCursor(position);
    refresh();
}

void ViewportWindow::pointerMove(const QPointF& position)
{
    const QPointF delta = position - m_lastPosition;
    m_lastPosition = position;

    switch (m_interaction) {
    case Interaction::Sculpt:
        m_controller.updateStroke(rayAt(position));
        break;
    case Interaction::Orbit:
        m_camera.orbit(static_cast<float>(delta.x()), static_cast<float>(delta.y()));
        break;
    case Interaction::Pan:
        m_camera.pan(static_cast<float>(delta.x()), static_cast<float>(delta.y()), static_cast<float>(height()));
        break;
    case Interaction::None:
        break;
    }
    updateCursor(position);
    refresh();
}

void ViewportWindow::pointerRelease()
{
    const bool wasSculpting = m_interaction == Interaction::Sculpt;
    if (wasSculpting) {
        m_controller.endStroke();
    }
    m_interaction = Interaction::None;
    m_activeButton = Qt::NoButton;
    updateCursor(m_lastPosition);
    refresh();
    if (wasSculpting) {
        emit strokeFinished();
    }
}

void ViewportWindow::wheelZoom(float steps)
{
    m_camera.zoom(steps);
    updateCursor(m_lastPosition);
    refresh();
}

void ViewportWindow::mousePressEvent(QMouseEvent* event)
{
    pointerPress(event->position(), event->button(), event->modifiers());
}

void ViewportWindow::mouseMoveEvent(QMouseEvent* event)
{
    pointerMove(event->position());
}

void ViewportWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == m_activeButton) {
        pointerRelease();
    }
}

void ViewportWindow::wheelEvent(QWheelEvent* event)
{
    // One wheel notch is 120 units of angleDelta.
    const float steps = static_cast<float>(event->angleDelta().y()) / 120.0f;
    if (steps != 0.0f && m_interaction != Interaction::Sculpt) {
        m_lastPosition = event->position();
        wheelZoom(steps);
    }
}

} // namespace se::editor
