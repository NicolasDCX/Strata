#include "Editor/MainWindow.h"

#include "Core/Log.h"
#include "Editor/ViewportWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QToolBar>
#include <QVulkanInstance>

#include <algorithm>
#include <cmath>

namespace se::editor {

namespace {

// Slider positions are integers: radius in 1/100 world units, strength in percent.
constexpr int kRadiusSliderMin = 1;
constexpr int kRadiusSliderMax = 150;
constexpr float kRadiusSliderScale = 100.0f;

QLabel* makeToolbarLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setContentsMargins(8, 0, 4, 0);
    return label;
}

QSlider* makeToolbarSlider(int minimum, int maximum, QWidget* parent)
{
    auto* slider = new QSlider(Qt::Horizontal, parent);
    slider->setRange(minimum, maximum);
    slider->setFixedWidth(150);
    slider->setFocusPolicy(Qt::NoFocus); // keep keyboard shortcuts on the window
    return slider;
}

} // namespace

MainWindow::MainWindow(app::AppController& controller, QVulkanInstance& vulkanInstance,
                       const render::RenderSettings& renderSettings, QWidget* parent)
    : QMainWindow(parent), m_controller(controller)
{
    setWindowTitle(QStringLiteral("SculptEngine"));

    m_viewport = new ViewportWindow(m_controller, renderSettings);
    m_viewport->setVulkanInstance(&vulkanInstance);

    // Prefer a discrete GPU when the machine has several adapters.
    const QList<VkPhysicalDeviceProperties> devices = m_viewport->availablePhysicalDevices();
    for (qsizetype i = 0; i < devices.size(); ++i) {
        if (devices[i].deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            m_viewport->setPhysicalDeviceIndex(static_cast<int>(i));
            break;
        }
    }
    if (m_viewport->supportedSampleCounts().contains(4)) {
        m_viewport->setSampleCount(4);
    }

    QWidget* container = QWidget::createWindowContainer(m_viewport, this);
    container->setMinimumSize(320, 240);
    container->setFocusPolicy(Qt::StrongFocus);
    setCentralWidget(container);
    m_viewport->installEventFilter(this);

    buildToolbar();
    buildStatusBar();

    // Edits are applied asynchronously, so history state can change on any
    // engine notification, not only when a stroke is released.
    connect(m_viewport, &ViewportWindow::viewportChanged, this, &MainWindow::updateHistoryActions);

    syncControlsFromBrush();
    updateHistoryActions();
    updateStatus();
}

QAction* MainWindow::addShortcutAction(const QString& text, const QKeySequence& shortcut, const QString& toolTip)
{
    auto* action = new QAction(text, this);
    action->setShortcut(shortcut);
    action->setToolTip(QStringLiteral("%1 (%2)").arg(toolTip, shortcut.toString(QKeySequence::NativeText)));
    addAction(action);
    m_shortcutActions.append(action);
    return action;
}

void MainWindow::buildToolbar()
{
    QToolBar* toolbar = addToolBar(QStringLiteral("Sculpt"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);

    // --- Brush selection ---------------------------------------------------
    struct BrushEntry {
        sculpt::BrushId id;
        Qt::Key key;
        const char* toolTip;
    };
    const BrushEntry entries[] = {
        {sculpt::BrushId::Draw, Qt::Key_D, "Raise the surface; hold Ctrl to carve"},
        {sculpt::BrushId::Smooth, Qt::Key_S, "Smooth the surface"},
        {sculpt::BrushId::Move, Qt::Key_M, "Grab the surface and drag it"},
    };
    m_brushGroup = new QActionGroup(this);
    m_brushGroup->setExclusive(true);
    for (const BrushEntry& entry : entries) {
        const std::string_view name = m_controller.brushes().brush(entry.id).name();
        QAction* action = addShortcutAction(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())),
                                            QKeySequence(entry.key), QString::fromLatin1(entry.toolTip));
        action->setCheckable(true);
        action->setData(static_cast<uint>(entry.id));
        m_brushGroup->addAction(action);
        m_brushActions.append(action);
        toolbar->addAction(action);
        connect(action, &QAction::triggered, this, [this, id = entry.id] { selectBrush(id); });
    }

    toolbar->addSeparator();

    // --- Brush parameters --------------------------------------------------
    toolbar->addWidget(makeToolbarLabel(QStringLiteral("Radius"), toolbar));
    m_radiusSlider = makeToolbarSlider(kRadiusSliderMin, kRadiusSliderMax, toolbar);
    m_radiusSlider->setToolTip(QStringLiteral("Brush radius in world units ([ and ] to change)"));
    toolbar->addWidget(m_radiusSlider);
    m_radiusValue = makeToolbarLabel(QString(), toolbar);
    m_radiusValue->setMinimumWidth(36);
    toolbar->addWidget(m_radiusValue);
    connect(m_radiusSlider, &QSlider::valueChanged, this, [this](int value) {
        m_controller.setRadius(static_cast<float>(value) / kRadiusSliderScale);
        syncControlsFromBrush();
        m_viewport->refresh();
    });

    toolbar->addWidget(makeToolbarLabel(QStringLiteral("Strength"), toolbar));
    m_strengthSlider = makeToolbarSlider(0, 100, toolbar);
    m_strengthSlider->setToolTip(QStringLiteral("Brush strength"));
    toolbar->addWidget(m_strengthSlider);
    m_strengthValue = makeToolbarLabel(QString(), toolbar);
    m_strengthValue->setMinimumWidth(36);
    toolbar->addWidget(m_strengthValue);
    connect(m_strengthSlider, &QSlider::valueChanged, this, [this](int value) {
        m_controller.setStrength(static_cast<float>(value) / 100.0f);
        syncControlsFromBrush();
        m_viewport->refresh();
    });

    m_invertAction = addShortcutAction(QStringLiteral("Invert"), QKeySequence(Qt::Key_X),
                                       QStringLiteral("Invert the brush direction (Draw carves instead of raising)"));
    m_invertAction->setCheckable(true);
    toolbar->addAction(m_invertAction);
    connect(m_invertAction, &QAction::toggled, this, [this](bool checked) {
        m_controller.setInvert(checked);
        m_viewport->refresh();
    });

    toolbar->addSeparator();

    // --- History -----------------------------------------------------------
    m_undoAction = addShortcutAction(QStringLiteral("Undo"), QKeySequence::Undo, QStringLiteral("Undo the last stroke"));
    m_redoAction = addShortcutAction(QStringLiteral("Redo"), QKeySequence::Redo, QStringLiteral("Redo the undone stroke"));
    // QKeySequence::Redo is Ctrl+Y on Windows and Ctrl+Shift+Z elsewhere; accept both everywhere.
    m_redoAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Y), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)});
    toolbar->addAction(m_undoAction);
    toolbar->addAction(m_redoAction);
    connect(m_undoAction, &QAction::triggered, this, &MainWindow::undo);
    connect(m_redoAction, &QAction::triggered, this, &MainWindow::redo);

    toolbar->addSeparator();

    QAction* frameAction =
        addShortcutAction(QStringLiteral("Frame"), QKeySequence(Qt::Key_F), QStringLiteral("Fit the model in the view"));
    toolbar->addAction(frameAction);
    connect(frameAction, &QAction::triggered, this, [this] { m_viewport->frameMesh(); });

    m_cullAction = addShortcutAction(QStringLiteral("Cull"), QKeySequence(Qt::Key_C),
                                     QStringLiteral("Skip chunks outside the view (turn off to compare)"));
    m_cullAction->setCheckable(true);
    m_cullAction->setChecked(m_viewport->cullMode() != render::CullMode::Off);
    toolbar->addAction(m_cullAction);
    connect(m_cullAction, &QAction::toggled, this, [this](bool checked) {
        m_viewport->setCullMode(checked ? render::CullMode::On : render::CullMode::Off);
    });

    // Keyboard only: brush size.
    QAction* shrink = addShortcutAction(QStringLiteral("Smaller brush"), QKeySequence(Qt::Key_BracketLeft),
                                        QStringLiteral("Decrease the brush radius"));
    QAction* grow = addShortcutAction(QStringLiteral("Larger brush"), QKeySequence(Qt::Key_BracketRight),
                                      QStringLiteral("Increase the brush radius"));
    connect(shrink, &QAction::triggered, this, [this] { scaleRadius(1.0f / 1.15f); });
    connect(grow, &QAction::triggered, this, [this] { scaleRadius(1.15f); });
}

void MainWindow::buildStatusBar()
{
    m_meshLabel = new QLabel(this);
    m_brushLabel = new QLabel(this);
    m_historyLabel = new QLabel(this);
    m_timingLabel = new QLabel(this);
    m_renderLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_renderLabel);
    for (QLabel* label : {m_meshLabel, m_brushLabel, m_historyLabel, m_timingLabel, m_renderLabel}) {
        label->setContentsMargins(6, 0, 6, 0);
    }
    statusBar()->addWidget(m_brushLabel);
    statusBar()->addWidget(m_timingLabel, 1);
    statusBar()->addPermanentWidget(m_historyLabel);
    statusBar()->addPermanentWidget(m_meshLabel);

    const QLocale locale;
    m_meshLabel->setText(QStringLiteral("%1 triangles  |  %2 vertices")
                             .arg(locale.toString(m_controller.mesh().triangleCount()),
                                  locale.toString(m_controller.mesh().vertexCount())));
}

void MainWindow::selectBrush(sculpt::BrushId id)
{
    m_controller.setActiveBrush(id);
    syncControlsFromBrush();
    m_viewport->refresh();
}

void MainWindow::undo()
{
    m_controller.undo(); // queued; the viewport refreshes when the engine reports the change
}

void MainWindow::redo()
{
    m_controller.redo();
}

void MainWindow::scaleRadius(float factor)
{
    // Step by at least one slider tick so small radii do not get stuck.
    const float current = m_controller.activeSettings().radius;
    const float minimumStep = 1.0f / kRadiusSliderScale;
    const float scaled = current * factor;
    const float next = factor > 1.0f ? std::max(scaled, current + minimumStep) : std::min(scaled, current - minimumStep);
    m_controller.setRadius(std::clamp(next, static_cast<float>(kRadiusSliderMin) / kRadiusSliderScale,
                                      static_cast<float>(kRadiusSliderMax) / kRadiusSliderScale));
    syncControlsFromBrush();
    m_viewport->refresh();
}

void MainWindow::syncControlsFromBrush()
{
    const sculpt::BrushSettings& settings = m_controller.activeSettings();

    // Blockers: these setters must not echo back into the controller.
    {
        const QSignalBlocker blockRadius(m_radiusSlider);
        const QSignalBlocker blockStrength(m_strengthSlider);
        const QSignalBlocker blockInvert(m_invertAction);
        m_radiusSlider->setValue(static_cast<int>(std::lround(settings.radius * kRadiusSliderScale)));
        m_strengthSlider->setValue(static_cast<int>(std::lround(settings.strength * 100.0f)));
        m_invertAction->setChecked(settings.invert);
    }
    m_radiusValue->setText(QString::number(static_cast<double>(settings.radius), 'f', 2));
    m_strengthValue->setText(QStringLiteral("%1%").arg(std::lround(settings.strength * 100.0f)));

    for (QAction* action : m_brushActions) {
        action->setChecked(action->data().toUInt() == static_cast<uint>(m_controller.activeBrush()));
    }
    updateStatus();
}

void MainWindow::updateHistoryActions()
{
    m_undoAction->setEnabled(m_controller.canUndo());
    m_redoAction->setEnabled(m_controller.canRedo());
    m_historyLabel->setText(QStringLiteral("Undo %1  |  Redo %2  |  History %3 KiB")
                                .arg(m_controller.undoCount())
                                .arg(m_controller.redoCount())
                                .arg(m_controller.undoMemoryBytes() / 1024));
    updateStatus();
}

void MainWindow::updateStatus()
{
    const std::string_view name = m_controller.activeBrushName();
    const sculpt::BrushSettings& settings = m_controller.activeSettings();
    m_brushLabel->setText(QStringLiteral("%1  |  radius %2  |  strength %3%%4")
                              .arg(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())))
                              .arg(static_cast<double>(settings.radius), 0, 'f', 2)
                              .arg(std::lround(settings.strength * 100.0f))
                              .arg(settings.invert ? QStringLiteral("  |  inverted") : QString()));

    {
        const render::RenderStats& render = m_viewport->renderStats();
        constexpr double kMiB = 1024.0 * 1024.0;
        QString text = QStringLiteral("Chunks %1/%2  |  %3 tris  |  %4 draws")
                           .arg(render.chunksDrawn)
                           .arg(render.chunkCount)
                           .arg(QLocale().toString(render.trianglesSubmitted))
                           .arg(render.drawCalls);
        if (render.gpuTimingAvailable) {
            text += QStringLiteral("  |  GPU %1 ms").arg(render.lastGpuMs, 0, 'f', 2);
        }
        {
            text += QStringLiteral("  |  GPU mem %1 MiB, pages %2/%3")
                        .arg(static_cast<double>(render.gpuBytesAllocated) / kMiB, 0, 'f', 0)
                        .arg(render.vertexPagesResident)
                        .arg(render.gpuPageCount);
        }
        m_renderLabel->setText(text);
    }
    const app::InteractionStats stats = m_controller.stats();
    const render::RenderStats& renderStats = m_viewport->renderStats();
    if (stats.strokeDabCount > 0) {
        m_timingLabel->setText(
            QStringLiteral("Edit %1 ms (worst %2), %3 verts/dab, %4 queued  |  Upload %5 verts  |  UI wait %6 ms (worst %7)")
                .arg(stats.lastUpdateMs, 0, 'f', 2)
                .arg(stats.worstUpdateMs, 0, 'f', 2)
                .arg(stats.lastDabVertexCount)
                .arg(stats.queuedInputs)
                .arg(renderStats.lastUploadVertexCount)
                .arg(renderStats.lastMeshLockWaitMs, 0, 'f', 2)
                .arg(renderStats.worstMeshLockWaitMs, 0, 'f', 2));
    } else {
        m_timingLabel->setText(QStringLiteral(
            "LMB sculpt  |  Ctrl invert  |  Shift smooth  |  RMB or LMB on background: orbit  |  MMB pan  |  Wheel zoom"));
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_viewport && event->type() == QEvent::KeyPress) {
        const auto* keyEvent = static_cast<QKeyEvent*>(event);
        const QKeySequence pressed(keyEvent->keyCombination());
        for (QAction* action : std::as_const(m_shortcutActions)) {
            if (action->isEnabled() && action->shortcuts().contains(pressed)) {
                action->trigger();
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

} // namespace se::editor
