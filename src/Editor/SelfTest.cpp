#include "Editor/SelfTest.h"

#include "Core/Log.h"
#include "Core/Timer.h"
#include "Editor/MainWindow.h"
#include "Editor/ViewportWindow.h"
#include "Sculpt/Brushes/SmoothBrush.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QRegularExpression>
#include <QScreen>
#include <QTextStream>
#include <QWheelEvent>
#include <QTimer>

#include <glm/geometric.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <numeric>
#include <vector>

namespace se::editor {

namespace {

constexpr int kStepDelayMs = 120;
constexpr int kExposeTimeoutMs = 10000;

} // namespace

double imageDifference(const QImage& a, const QImage& b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size()) {
        return 1.0;
    }
    qint64 different = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb pa = a.pixel(x, y);
            const QRgb pb = b.pixel(x, y);
            const int delta = std::abs(qRed(pa) - qRed(pb)) + std::abs(qGreen(pa) - qGreen(pb)) +
                              std::abs(qBlue(pa) - qBlue(pb));
            different += delta > 6 ? 1 : 0;
        }
    }
    return static_cast<double>(different) / (static_cast<double>(a.width()) * a.height());
}

/// The renderer's clear colour (SceneRenderer kBackground) as it appears in captures.
bool isBackgroundPixel(QRgb p)
{
    return std::abs(qRed(p) - 29) + std::abs(qGreen(p) - 31) + std::abs(qBlue(p) - 34) <= 6;
}

/// Background pixels of `image` that are not 4-connected to background on the
/// image border: background seen through the surface (cracks, pinholes,
/// holes enclosed by drawn chunks).
qint64 enclosedBackgroundPixels(const QImage& image)
{
    const int w = image.width();
    const int h = image.height();
    std::vector<u8> state(static_cast<std::size_t>(w) * h, 0); // 0 surface, 1 background, 2 reached from border
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            state[static_cast<std::size_t>(y) * w + x] = isBackgroundPixel(image.pixel(x, y)) ? 1 : 0;
        }
    }
    std::vector<int> stack;
    const auto seed = [&](int x, int y) {
        u8& v = state[static_cast<std::size_t>(y) * w + x];
        if (v == 1) {
            v = 2;
            stack.push_back(y * w + x);
        }
    };
    for (int x = 0; x < w; ++x) {
        seed(x, 0);
        seed(x, h - 1);
    }
    for (int y = 0; y < h; ++y) {
        seed(0, y);
        seed(w - 1, y);
    }
    while (!stack.empty()) {
        const int at = stack.back();
        stack.pop_back();
        const int x = at % w;
        const int y = at / w;
        if (x > 0) seed(x - 1, y);
        if (x + 1 < w) seed(x + 1, y);
        if (y > 0) seed(x, y - 1);
        if (y + 1 < h) seed(x, y + 1);
    }
    return std::count(state.begin(), state.end(), u8{1});
}

/// Pixels that are surface in `reference`, at least `margin` pixels inside its
/// silhouette, but background in `image`: missing chunks, cracks, or a
/// silhouette that moved inward (coarse geometry at the limb).
qint64 holePixels(const QImage& image, const QImage& reference, int margin = 4)
{
    if (image.isNull() || reference.isNull() || image.size() != reference.size()) {
        return -1;
    }
    const auto isBackground = [](QRgb p) { return isBackgroundPixel(p); };
    const int w = reference.width();
    const int h = reference.height();
    // Distance (in pixels, chessboard) from the reference background, capped.
    std::vector<int> inside(static_cast<std::size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            inside[static_cast<std::size_t>(y) * w + x] = isBackground(reference.pixel(x, y)) ? 0 : margin + 1;
        }
    }
    for (int pass = 0; pass < margin; ++pass) {
        std::vector<int> next = inside;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int& v = next[static_cast<std::size_t>(y) * w + x];
                for (int dy = -1; dy <= 1 && v > 0; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h || inside[static_cast<std::size_t>(ny) * w + nx] == 0) {
                            v = 0;
                            break;
                        }
                    }
                }
            }
        }
        inside.swap(next);
    }
    qint64 holes = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (inside[static_cast<std::size_t>(y) * w + x] > 0 && isBackground(image.pixel(x, y))) {
                ++holes;
            }
        }
    }
    return holes;
}

namespace {

double differingFraction(const QImage& a, const QImage& b)
{
    return imageDifference(a, b);
}

} // namespace

SelfTest::SelfTest(MainWindow& window, app::AppController& controller, const QString& outputDirectory, Scenario scenario,
                   const QString& referenceDirectory)
    : m_window(window), m_controller(controller), m_outputDirectory(outputDirectory),
      m_referenceDirectory(referenceDirectory), m_scenario(scenario)
{
}

void SelfTest::settle()
{
    // Uploads beyond the per-frame limit are spread over frames; keep rendering
    // until none is waiting, so that what is captured next is the final image.
    m_controller.waitUntilIdle();
    ViewportWindow& viewport = m_window.viewport();
    for (int i = 0; i < 400; ++i) {
        viewport.grab();
        if (!viewport.renderStats().pendingWork) {
            return;
        }
    }
    check(false, QStringLiteral("renderer settled within 400 frames"));
}

QPointF SelfTest::viewportPoint(double xFraction, double yFraction) const
{
    const ViewportWindow& viewport = m_window.viewport();
    return {viewport.width() * xFraction, viewport.height() * yFraction};
}

std::vector<glm::vec3> SelfTest::positions() const
{
    m_controller.waitUntilIdle(); // edits are applied on the sculpt thread
    const auto span = m_controller.mesh().positions();
    return {span.begin(), span.end()};
}

void SelfTest::check(bool condition, const QString& description)
{
    m_report.append(QStringLiteral("[%1] %2").arg(condition ? QStringLiteral("PASS") : QStringLiteral("FAIL"), description));
    if (!condition) {
        ++m_failures;
        core::logError("selftest FAILED: {}", description.toStdString());
    }
}

void SelfTest::note(const QString& text)
{
    m_report.append(QStringLiteral("       %1").arg(text));
}

QImage SelfTest::capture(const QString& fileName)
{
    // Park the pointer in a corner so the brush cursor does not appear in comparisons.
    m_controller.waitUntilIdle();
    m_window.viewport().pointerMove(viewportPoint(0.005, 0.005));
    settle();
    const QImage image = m_window.viewport().grab();
    const QString path = QDir(m_outputDirectory).filePath(fileName);
    check(!image.isNull() && image.save(path), QStringLiteral("frame grabbed and saved: %1").arg(fileName));
    // Pools sized for the whole mesh: every visible chunk is drawn, with current data.
    const render::RenderStats& stats = m_window.viewport().renderStats();
    if (!stats.limitedBudget && stats.chunksCoarse == 0) {
        check(stats.chunksDrawn == stats.chunksVisible && stats.lastStalePages == 0 && stats.chunksSkipped == 0,
              QStringLiteral("%1: full coverage, %2 of %3 visible chunks drawn, %4 stale pages")
                  .arg(fileName).arg(stats.chunksDrawn).arg(stats.chunksVisible).arg(stats.lastStalePages));
    }
    // Standard scenario: captures 01 to 11 follow scripted, timing-independent
    // input, so with a reference run they must match it. (The pressure
    // scenario chooses its comparisons itself.)
    static const QRegularExpression deterministic(QStringLiteral("^(0\\d|1[01]|17)_"));
    if (m_scenario == Scenario::Standard && deterministic.match(fileName).hasMatch()) {
        compareWithReference(image, fileName);
    }
    return image;
}

void SelfTest::stroke(const QPointF& from, const QPointF& to, int samples, Qt::KeyboardModifiers modifiers)
{
    ViewportWindow& viewport = m_window.viewport();
    viewport.pointerPress(from, Qt::LeftButton, modifiers);
    for (int i = 1; i <= samples; ++i) {
        const double t = static_cast<double>(i) / samples;
        viewport.pointerMove(from + (to - from) * t);
    }
    viewport.pointerRelease();
    m_controller.waitUntilIdle();
}

void SelfTest::start()
{
    QDir().mkpath(m_outputDirectory);
    if (m_scenario == Scenario::Pressure) {
        buildPressureSteps();
    } else if (m_scenario == Scenario::History) {
        buildHistorySteps();
    } else if (m_scenario == Scenario::Move) {
        buildMoveSteps();
    } else if (m_scenario == Scenario::Smooth) {
        buildSmoothSteps();
    } else if (m_scenario == Scenario::Latency) {
        buildLatencySteps();
    } else {
        buildStandardSteps();
    }
    QTimer::singleShot(kStepDelayMs, this, &SelfTest::runNextStep);
}

void SelfTest::buildStandardSteps()
{
    // State shared between steps.
    auto initialPositions = std::make_shared<std::vector<glm::vec3>>();
    auto sculptedPositions = std::make_shared<std::vector<glm::vec3>>();
    auto initialImage = std::make_shared<QImage>();
    auto previousImage = std::make_shared<QImage>();
    auto sculptedImage = std::make_shared<QImage>();

    m_steps.push_back([=, this] {
        *initialPositions = positions();
        *initialImage = capture(QStringLiteral("01_initial.png"));
        *previousImage = *initialImage;

        // The sphere must be visible: centre pixel is lit clay, corner is background.
        if (!initialImage->isNull()) {
            const QRgb centre = initialImage->pixel(initialImage->width() / 2, initialImage->height() / 2);
            const QRgb corner = initialImage->pixel(2, 2);
            check(qRed(centre) > qRed(corner) + 40, QStringLiteral("sphere is rendered and lit (centre brighter than background)"));
            note(QStringLiteral("frame size %1x%2").arg(initialImage->width()).arg(initialImage->height()));
        }

        // Picking: the centre of the viewport looks straight at the sphere.
        ViewportWindow& viewport = m_window.viewport();
        const QPointF centrePoint = viewportPoint(0.5, 0.5);
        const auto ray = viewport.camera().screenRay(static_cast<float>(centrePoint.x()), static_cast<float>(centrePoint.y()),
                                                     static_cast<float>(viewport.width()), static_cast<float>(viewport.height()));
        const auto hit = m_controller.pick(ray);
        check(hit.has_value(), QStringLiteral("ray through the viewport centre hits the mesh"));
        if (hit) {
            const float expected = viewport.camera().distance() - 1.0f;
            check(std::abs(hit->distance - expected) < 0.01f,
                  QStringLiteral("hit distance %1 matches camera distance - radius %2").arg(hit->distance).arg(expected));
            check(glm::dot(hit->normal, -ray.direction) > 0.99f, QStringLiteral("hit normal faces the camera"));
        }
        const auto miss = m_controller.pick(viewport.camera().screenRay(1.0f, 1.0f, static_cast<float>(viewport.width()),
                                                                        static_cast<float>(viewport.height())));
        check(!miss.has_value(), QStringLiteral("ray through the viewport corner misses the mesh"));

        // Hover shows the brush cursor.
        viewport.pointerMove(centrePoint);
        check(viewport.hovering(), QStringLiteral("brush cursor is shown while hovering the model"));
        const QImage withCursor = viewport.grab();
        withCursor.save(QDir(m_outputDirectory).filePath(QStringLiteral("02_brush_cursor.png")));
        check(differingFraction(withCursor, *initialImage) > 0.0005, QStringLiteral("brush cursor is visible in the frame"));
    });

    m_steps.push_back([=, this] {
        m_window.selectBrush(sculpt::BrushId::Draw);
        const auto before = positions();
        stroke(viewportPoint(0.38, 0.42), viewportPoint(0.62, 0.42), 40);
        check(positions() != before, QStringLiteral("Draw stroke modified the mesh"));
        check(m_controller.undoCount() == 1, QStringLiteral("Draw stroke produced exactly one undo step"));
        const QImage image = capture(QStringLiteral("03_draw.png"));
        check(differingFraction(image, *previousImage) > 0.002, QStringLiteral("Draw stroke is visible in the rendered frame"));
        note(QStringLiteral("Draw: %1 dabs, worst input %2 ms, %3 verts in last dab")
                 .arg(m_controller.stats().strokeDabCount)
                 .arg(m_controller.stats().worstUpdateMs, 0, 'f', 3)
                 .arg(m_controller.stats().lastDabVertexCount));
        *previousImage = image;
    });

    m_steps.push_back([=, this] {
        const auto before = positions();
        stroke(viewportPoint(0.38, 0.60), viewportPoint(0.62, 0.60), 40, Qt::ControlModifier);
        check(positions() != before, QStringLiteral("Ctrl+Draw (inverted) stroke modified the mesh"));
        const QImage image = capture(QStringLiteral("04_draw_inverted.png"));
        check(differingFraction(image, *previousImage) > 0.002, QStringLiteral("inverted Draw stroke is visible"));
        *previousImage = image;
    });

    m_steps.push_back([=, this] {
        m_window.selectBrush(sculpt::BrushId::Smooth);
        const auto before = positions();
        // Scrub back and forth over the right half of the raised stroke.
        ViewportWindow& viewport = m_window.viewport();
        viewport.pointerPress(viewportPoint(0.50, 0.42), Qt::LeftButton, {});
        for (int pass = 0; pass < 6; ++pass) {
            for (int i = 0; i <= 20; ++i) {
                const double t = (pass % 2 == 0) ? i / 20.0 : 1.0 - i / 20.0;
                viewport.pointerMove(viewportPoint(0.50 + 0.12 * t, 0.42));
            }
        }
        viewport.pointerRelease();
        m_controller.waitUntilIdle();
        check(positions() != before, QStringLiteral("Smooth stroke modified the mesh"));
        const QImage image = capture(QStringLiteral("05_smooth.png"));
        check(differingFraction(image, *previousImage) > 0.0005, QStringLiteral("Smooth stroke is visible"));
        note(QStringLiteral("Smooth: %1 dabs, worst input %2 ms")
                 .arg(m_controller.stats().strokeDabCount)
                 .arg(m_controller.stats().worstUpdateMs, 0, 'f', 3));
        *previousImage = image;
    });

    m_steps.push_back([=, this] {
        m_window.selectBrush(sculpt::BrushId::Move);
        const auto before = positions();
        stroke(viewportPoint(0.50, 0.27), viewportPoint(0.50, 0.12), 30);
        check(positions() != before, QStringLiteral("Move stroke modified the mesh"));
        check(m_controller.undoCount() == 4, QStringLiteral("four strokes -> four undo steps"));
        *sculptedPositions = positions();
        *sculptedImage = capture(QStringLiteral("06_move.png"));
        check(differingFraction(*sculptedImage, *previousImage) > 0.002, QStringLiteral("Move stroke is visible"));
        *previousImage = *sculptedImage;
    });

    m_steps.push_back([=, this] {
        for (int i = 0; i < 4; ++i) {
            m_window.undo();
        }
        m_controller.waitUntilIdle();
        check(positions() == *initialPositions, QStringLiteral("undo x4 restored every vertex exactly"));
        check(!m_controller.canUndo() && m_controller.redoCount() == 4, QStringLiteral("history is fully undone"));
        const QImage image = capture(QStringLiteral("07_undo_all.png"));
        check(differingFraction(image, *initialImage) < 0.0005,
              QStringLiteral("frame after undo matches the initial frame (GPU buffers were updated)"));
        *previousImage = image;
    });

    m_steps.push_back([=, this] {
        for (int i = 0; i < 4; ++i) {
            m_window.redo();
        }
        m_controller.waitUntilIdle();
        check(positions() == *sculptedPositions, QStringLiteral("redo x4 restored the sculpted mesh exactly"));
        const QImage image = capture(QStringLiteral("08_redo_all.png"));
        check(differingFraction(image, *sculptedImage) < 0.0005, QStringLiteral("frame after redo matches the sculpted frame"));
        *previousImage = image;
    });

    m_steps.push_back([=, this] {
        ViewportWindow& viewport = m_window.viewport();
        // Orbit with the right button, pan with the middle button, zoom with the wheel.
        viewport.pointerPress(viewportPoint(0.5, 0.5), Qt::RightButton, {});
        viewport.pointerMove(viewportPoint(0.5, 0.5) + QPointF(140.0, -60.0));
        viewport.pointerRelease();
        const QImage orbited = capture(QStringLiteral("09_orbit.png"));
        check(differingFraction(orbited, *previousImage) > 0.01, QStringLiteral("orbit changed the view"));

        viewport.pointerPress(viewportPoint(0.5, 0.5), Qt::MiddleButton, {});
        viewport.pointerMove(viewportPoint(0.5, 0.5) + QPointF(-80.0, 40.0));
        viewport.pointerRelease();
        viewport.wheelZoom(3.0f);
        const QImage zoomed = capture(QStringLiteral("10_pan_zoom.png"));
        check(differingFraction(zoomed, orbited) > 0.01, QStringLiteral("pan + zoom changed the view"));

        // Sculpting still works from the new viewpoint.
        m_window.selectBrush(sculpt::BrushId::Draw);
        const auto before = positions();
        stroke(viewportPoint(0.45, 0.55), viewportPoint(0.60, 0.50), 20);
        check(positions() != before, QStringLiteral("sculpting works after moving the camera"));
        capture(QStringLiteral("11_sculpt_after_camera.png"));

        const render::RenderStats& renderStats = viewport.renderStats();
        note(QStringLiteral("%1 frames rendered; last frame CPU %2 ms; last upload %3 verts in %4 ms; worst wait for the mesh lock %5 ms")
                 .arg(renderStats.frameCount)
                 .arg(renderStats.lastFrameCpuMs, 0, 'f', 3)
                 .arg(renderStats.lastUploadVertexCount)
                 .arg(renderStats.lastUploadMs, 0, 'f', 3)
                 .arg(renderStats.worstMeshLockWaitMs, 0, 'f', 3));
        note(QStringLiteral("sculpt thread helpers: %1").arg(m_controller.jobThreadCount()));
    });

    // Strong deformations, undo and redo: culling off / frustum / on must give
    // identical images, and the normal cones published with the chunk bounds
    // must stay conservative. Scripted input, so the 17_ captures are compared
    // with a reference run too. The edits are undone again afterwards.
    m_steps.push_back([this] { checkStrongDeformation(QStringLiteral("17_"), true); });

    // A stroke at interactive pace, with frames rendered by the normal event loop
    // while the sculpt thread works: this is the contended, real-time path.
    m_steps.push_back([this] { runLiveStroke(true, 0.11, 0.16, QStringLiteral("14_live_stroke.png")); });

    // Chunk culling must not change the picture, from any view.
    m_steps.push_back([this] {
        ViewportWindow& viewport = m_window.viewport();
        viewport.frameMesh();
        compareCullModes(QStringLiteral("full"));

        viewport.wheelZoom(7.0f); // close enough that most of the model is off screen
        const auto [trianglesOff, trianglesOn] = compareCullModes(QStringLiteral("zoom"));
        if (viewport.renderStats().chunkCount >= 16) {
            check(trianglesOn < trianglesOff,
                  QStringLiteral("culling submits fewer triangles in the zoomed view (%1 instead of %2)").arg(trianglesOn).arg(trianglesOff));
        }

        viewport.camera().orbit(160.0f, -90.0f);
        viewport.camera().pan(60.0f, -40.0f, static_cast<float>(viewport.height()));
        viewport.refresh();
        compareCullModes(QStringLiteral("zoom_orbit"));

        // Most of the model off screen: only its limb is in view.
        viewport.frameMesh();
        viewport.wheelZoom(7.0f);
        viewport.camera().pan(static_cast<float>(viewport.height()) * 0.8f, 0.0f, static_cast<float>(viewport.height()));
        viewport.refresh();
        compareCullModes(QStringLiteral("edge"));
        viewport.frameMesh();
    });

    // PROTOTYPE coarse fallback (--lod): where coarse and detailed chunks meet
    // there must be no cracks, from any view, after the strokes above.
    m_steps.push_back([this] {
        ViewportWindow& viewport = m_window.viewport();
        if (viewport.renderStats().coarseBytes == 0) {
            note(QStringLiteral("coarse fallback not enabled (--lod): seam checks skipped"));
            return;
        }
        viewport.frameMesh();
        checkCoarseSeams(QStringLiteral("full"));
        viewport.wheelZoom(7.0f);
        checkCoarseSeams(QStringLiteral("zoom"));
        viewport.camera().orbit(160.0f, -90.0f);
        viewport.refresh();
        checkCoarseSeams(QStringLiteral("zoom_orbit"));
        viewport.frameMesh();
        viewport.wheelZoom(7.0f);
        viewport.camera().pan(static_cast<float>(viewport.height()) * 0.8f, 0.0f, static_cast<float>(viewport.height()));
        viewport.refresh();
        checkCoarseSeams(QStringLiteral("edge"));
        viewport.frameMesh();
    });

    // Frame times with culling off / on / one draw per chunk, in a fully
    // visible and in a partially visible view.
    m_steps.push_back([this] { runRenderMeasurements(); });

    // Real Qt input events: mouse events delivered to the viewport window and a
    // keyboard shortcut routed through MainWindow's event filter.
    m_steps.push_back([=, this] {
        ViewportWindow& viewport = m_window.viewport();
        const auto sendMouse = [&viewport](QEvent::Type type, const QPointF& position, Qt::MouseButton button,
                                           Qt::MouseButtons buttons) {
            QMouseEvent event(type, position, viewport.mapToGlobal(position), button, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(&viewport, &event);
        };
        const std::size_t undoBefore = m_controller.undoCount();
        const auto before = positions();
        const QPointF from = viewportPoint(0.40, 0.40);
        const QPointF to = viewportPoint(0.55, 0.45);
        sendMouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i <= 15; ++i) {
            sendMouse(QEvent::MouseMove, from + (to - from) * (i / 15.0), Qt::NoButton, Qt::LeftButton);
        }
        sendMouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
        m_controller.waitUntilIdle();
        check(positions() != before && m_controller.undoCount() == undoBefore + 1,
              QStringLiteral("QMouseEvent press/move/release performs one sculpt stroke"));

        QKeyEvent undoKey(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
        QCoreApplication::sendEvent(&viewport, &undoKey);
        m_controller.waitUntilIdle();
        check(positions() == before && m_controller.undoCount() == undoBefore,
              QStringLiteral("Ctrl+Z pressed in the viewport undoes the stroke"));

        QKeyEvent smoothKey(QEvent::KeyPress, Qt::Key_S, Qt::NoModifier);
        QCoreApplication::sendEvent(&viewport, &smoothKey);
        check(m_controller.activeBrush() == sculpt::BrushId::Smooth, QStringLiteral("S key selects the Smooth brush"));

        QWheelEvent wheel(viewportPoint(0.5, 0.5), viewport.mapToGlobal(viewportPoint(0.5, 0.5)), QPoint(), QPoint(0, -240),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        const float distanceBefore = viewport.camera().distance();
        QCoreApplication::sendEvent(&viewport, &wheel);
        check(viewport.camera().distance() > distanceBefore, QStringLiteral("QWheelEvent zooms the camera out"));

        // Next step checks that the swapchain follows a window resize.
        m_window.resize(m_window.width() - 260, m_window.height() - 140);
    });

    m_steps.push_back([=, this] {
        ViewportWindow& viewport = m_window.viewport();
        viewport.frameMesh();
        const QImage image = capture(QStringLiteral("12_after_resize.png"));
        check(!image.isNull() && image.size() != initialImage->size(),
              QStringLiteral("swapchain was recreated at the new window size (%1x%2)").arg(image.width()).arg(image.height()));
        if (!image.isNull()) {
            const QRgb centre = image.pixel(image.width() / 2, image.height() / 2);
            const QRgb corner = image.pixel(2, 2);
            check(qRed(centre) > qRed(corner) + 40, QStringLiteral("model is still rendered after the resize"));
        }
        // Whole window as it appears on screen (toolbar, viewport, status bar).
        viewport.pointerMove(viewportPoint(0.5, 0.5));
        QCoreApplication::processEvents();
        // Grabbed from the desktop rather than from the window: a window grab does
        // not include the content a Vulkan swapchain presents.
        if (QScreen* screen = m_window.screen()) {
            const QRect area = m_window.geometry().translated(-screen->geometry().topLeft());
            const QImage shot = screen->grabWindow(0, area.x(), area.y(), area.width(), area.height()).toImage();
            shot.save(QDir(m_outputDirectory).filePath(QStringLiteral("13_window.png")));
            // Informational only: another window on top of ours would change the result.
            if (!shot.isNull()) {
                const QRgb centre = shot.pixel(shot.width() / 2, shot.height() / 2);
                const QRgb edge = shot.pixel(shot.width() / 20, shot.height() / 2);
                note(QStringLiteral("on-screen presentation: centre pixel (%1,%2,%3), viewport edge (%4,%5,%6) -> %7")
                         .arg(qRed(centre)).arg(qGreen(centre)).arg(qBlue(centre))
                         .arg(qRed(edge)).arg(qGreen(edge)).arg(qBlue(edge))
                         .arg(qRed(centre) > qRed(edge) + 40 ? QStringLiteral("model visible on screen")
                                                             : QStringLiteral("model NOT detected on screen (window covered?)")));
            }
        }
    });

}

void SelfTest::runNextStep()
{
    // Wait until the window is on screen and the swapchain exists.
    if (m_nextStep == 0 && !m_window.viewport().isExposed()) {
        m_waitedForExposeMs += kStepDelayMs;
        if (m_waitedForExposeMs > kExposeTimeoutMs) {
            check(false, QStringLiteral("viewport was exposed within %1 ms").arg(kExposeTimeoutMs));
            finish();
            return;
        }
        QTimer::singleShot(kStepDelayMs, this, &SelfTest::runNextStep);
        return;
    }

    if (m_nextStep < m_steps.size()) {
        m_steps[m_nextStep++]();
        if (!m_holding) {
            QTimer::singleShot(kStepDelayMs, this, &SelfTest::runNextStep);
        }
        return;
    }
    finish();
}

void SelfTest::resumeSteps()
{
    m_holding = false;
    QTimer::singleShot(kStepDelayMs, this, &SelfTest::runNextStep);
}

namespace {

/// Value at fraction `q` of the sorted samples (0 when there are none).
template <typename T>
double percentile(std::vector<T> samples, double q)
{
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t index = std::min(samples.size() - 1, static_cast<std::size_t>(q * static_cast<double>(samples.size())));
    return static_cast<double>(samples[index]);
}

} // namespace

void SelfTest::runLiveStroke(bool reframe, double radiusX, double radiusY, const QString& captureName)
{
    constexpr int kLiveSamples = 500;
    constexpr int kSampleIntervalMs = 2;

    holdSteps();
    m_window.selectBrush(sculpt::BrushId::Draw);
    ViewportWindow& viewport = m_window.viewport();
    if (reframe) {
        viewport.frameMesh();
    }
    auto before = std::make_shared<std::vector<glm::vec3>>(positions());
    const std::size_t undoBefore = m_controller.undoCount();

    // A closed loop on the model, so the stroke keeps hitting the surface.
    const auto pointAt = [this, radiusX, radiusY](int sample) {
        const double angle = 6.283185307179586 * 2.0 * sample / kLiveSamples;
        return viewportPoint(0.5 + radiusX * std::cos(angle), 0.5 + radiusY * std::sin(angle));
    };

    viewport.setFrameSampling(true);
    m_liveSample = 0;
    viewport.pointerPress(pointAt(0), Qt::LeftButton, {});

    auto* timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);
    connect(timer, &QTimer::timeout, this, [this, timer, pointAt, before, undoBefore, captureName] {
        ViewportWindow& view = m_window.viewport();
        if (++m_liveSample <= kLiveSamples) {
            view.pointerMove(pointAt(m_liveSample));
            return;
        }
        timer->stop();
        timer->deleteLater();
        view.pointerRelease();
        const u32 queuedAtRelease = m_controller.stats().queuedInputs;
        m_controller.waitUntilIdle();
        view.setFrameSampling(false);

        const render::RenderStats& stats = view.renderStats();
        // The first interval is the idle time before the stroke began, not a frame time.
        std::vector<float> intervals = stats.frameIntervalSamplesMs;
        if (!intervals.empty()) {
            intervals.erase(intervals.begin());
        }
        check(positions() != *before && m_controller.undoCount() == undoBefore + 1,
              QStringLiteral("live stroke modified the mesh and produced one undo step"));
        check(stats.frameCpuSamplesMs.size() >= 10,
              QStringLiteral("frames were rendered while the stroke was running (%1)").arg(stats.frameCpuSamplesMs.size()));
        const double worstWait = percentile(stats.lockWaitSamplesMs, 1.0);
        check(worstWait < 50.0, QStringLiteral("renderer never waited long for the mesh (worst %1 ms)").arg(worstWait, 0, 'f', 3));

        note(QStringLiteral("live stroke: %1 inputs at %2 ms intervals, %3 dabs, %4 frames, %5 inputs still queued at release")
                 .arg(kLiveSamples).arg(kSampleIntervalMs).arg(m_controller.stats().strokeDabCount)
                 .arg(stats.frameCpuSamplesMs.size()).arg(queuedAtRelease));
        note(QStringLiteral("  frame interval ms: median %1, p95 %2, max %3")
                 .arg(percentile(intervals, 0.5), 0, 'f', 2)
                 .arg(percentile(intervals, 0.95), 0, 'f', 2)
                 .arg(percentile(intervals, 1.0), 0, 'f', 2));
        note(QStringLiteral("  frame CPU ms: median %1, p95 %2, max %3")
                 .arg(percentile(stats.frameCpuSamplesMs, 0.5), 0, 'f', 3)
                 .arg(percentile(stats.frameCpuSamplesMs, 0.95), 0, 'f', 3)
                 .arg(percentile(stats.frameCpuSamplesMs, 1.0), 0, 'f', 3));
        note(QStringLiteral("  wait for mesh lock ms: median %1, p95 %2, max %3")
                 .arg(percentile(stats.lockWaitSamplesMs, 0.5), 0, 'f', 3)
                 .arg(percentile(stats.lockWaitSamplesMs, 0.95), 0, 'f', 3)
                 .arg(worstWait, 0, 'f', 3));
        note(QStringLiteral("  vertices uploaded per frame: median %1, p95 %2, max %3 (GPU pages of %4)")
                 .arg(percentile(stats.uploadVertexSamples, 0.5), 0, 'f', 0)
                 .arg(percentile(stats.uploadVertexSamples, 0.95), 0, 'f', 0)
                 .arg(percentile(stats.uploadVertexSamples, 1.0), 0, 'f', 0)
                 .arg(stats.gpuPageVertices));
        note(QStringLiteral("  frame GPU ms: median %1, p95 %2, max %3")
                 .arg(percentile(stats.frameGpuSamplesMs, 0.5), 0, 'f', 3)
                 .arg(percentile(stats.frameGpuSamplesMs, 0.95), 0, 'f', 3)
                 .arg(percentile(stats.frameGpuSamplesMs, 1.0), 0, 'f', 3));
        note(QStringLiteral("  uploaded per frame KiB: median %1, p95 %2, max %3; evictions during the stroke: %4")
                 .arg(percentile(stats.uploadKiBSamples, 0.5), 0, 'f', 0)
                 .arg(percentile(stats.uploadKiBSamples, 0.95), 0, 'f', 0)
                 .arg(percentile(stats.uploadKiBSamples, 1.0), 0, 'f', 0)
                 .arg(std::accumulate(stats.evictionSamples.begin(), stats.evictionSamples.end(), 0u)));
        {
            const app::InteractionStats sculptStats = m_controller.stats();
            noteSculptCost(QStringLiteral("  sculpt thread"), sculptStats.updateSamplesMs, sculptStats.strokeBvhMs,
                           sculptStats.strokeDabCount, sculptStats.strokeConeTriangles);
            note(QStringLiteral("  sculpt thread lock hold per dab ms: median %1, p95 %2, max %3 | dab latency ms: median %4, p95 %5, max %6 | edit scratch %7 MiB")
                     .arg(percentile(sculptStats.lockHoldSamplesMs, 0.5), 0, 'f', 3)
                     .arg(percentile(sculptStats.lockHoldSamplesMs, 0.95), 0, 'f', 3)
                     .arg(percentile(sculptStats.lockHoldSamplesMs, 1.0), 0, 'f', 3)
                     .arg(percentile(sculptStats.dabSamplesMs, 0.5), 0, 'f', 3)
                     .arg(percentile(sculptStats.dabSamplesMs, 0.95), 0, 'f', 3)
                     .arg(percentile(sculptStats.dabSamplesMs, 1.0), 0, 'f', 3)
                     .arg(static_cast<double>(sculptStats.editScratchBytes) / (1024.0 * 1024.0), 0, 'f', 1));
        }
        note(QStringLiteral("  chunks missing per frame: median %1, max %2 | coarse per frame: median %3, max %4")
                 .arg(percentile(stats.chunksMissingSamples, 0.5), 0, 'f', 0).arg(percentile(stats.chunksMissingSamples, 1.0), 0, 'f', 0)
                 .arg(percentile(stats.chunksCoarseSamples, 0.5), 0, 'f', 0).arg(percentile(stats.chunksCoarseSamples, 1.0), 0, 'f', 0));
        capture(captureName);
        checkVisibleChunksDrawn(QStringLiteral("after the stroke"));
        resumeSteps();
    });
    timer->start(kSampleIntervalMs);
}

void SelfTest::noteGpuState(const QString& label)
{
    constexpr double kMiB = 1024.0 * 1024.0;
    const render::RenderStats& stats = m_window.viewport().renderStats();
    note(QStringLiteral("%1: chunks drawn %2 of %3 visible (%4 total), %5 triangles, %6 draw calls")
             .arg(label).arg(stats.chunksDrawn).arg(stats.chunksVisible).arg(stats.chunkCount)
             .arg(stats.trianglesSubmitted).arg(stats.drawCalls));
    note(QStringLiteral("  GPU: %1 MiB allocated (%2); vertex pages resident %3 of %4 (pool holds %5); chunk indices resident %6 of %7 (pool holds %8)")
             .arg(static_cast<double>(stats.gpuBytesAllocated) / kMiB, 0, 'f', 1)
             .arg(stats.limitedBudget ? QStringLiteral("budget %1 MiB").arg(static_cast<double>(stats.gpuBudgetBytes) / kMiB, 0, 'f', 1)
                                      : QStringLiteral("sized for the whole mesh"))
             .arg(stats.vertexPagesResident).arg(stats.gpuPageCount).arg(stats.vertexSlots)
             .arg(stats.indexChunksResident).arg(stats.chunkCount).arg(stats.indexSlots));
    if (stats.coarseBytes > 0) {
        note(QStringLiteral("  coarse fallback: %1 MiB on the GPU (in the total above); %2 chunks coarse, %3 missing, %4 coarse triangles; %5 groups, %6 chunks replaced although detailed was ready; %7")
                 .arg(static_cast<double>(stats.coarseBytes) / kMiB, 0, 'f', 2).arg(stats.chunksCoarse)
                 .arg(stats.chunksMissing).arg(stats.coarseTriangles).arg(stats.coarseGroups).arg(stats.coarseReplacedDetailed)
                 .arg(stats.coarseIndirectDraws ? QStringLiteral("one indirect draw per pass") : QStringLiteral("one draw per group")));
    } else {
        note(QStringLiteral("  no coarse fallback: %1 visible chunks missing").arg(stats.chunksMissing));
    }
    note(QStringLiteral("  so far: %1 MiB uploaded by frames (not counting the initial fill), %2 evictions")
             .arg(static_cast<double>(stats.totalUploadBytes) / kMiB, 0, 'f', 1).arg(stats.totalEvictions));
}

void SelfTest::compareWithReference(const QImage& image, const QString& fileName)
{
    if (m_referenceDirectory.isEmpty()) {
        return;
    }
    const render::RenderStats& stats = m_window.viewport().renderStats();
    const QImage reference(QDir(m_referenceDirectory).filePath(fileName));
    if (stats.chunksDrawn != stats.chunksVisible) {
        // Not every chunk is detailed: count what is missing from the picture.
        const qint64 holes = holePixels(image, reference);
        const qint64 enclosed = enclosedBackgroundPixels(image);
        note(QStringLiteral("%1: not compared exactly (%2 of %3 visible chunks detailed, %4 coarse, %5 missing); vs reference: %6 surface pixels lost, %7 of pixels differ; %8 enclosed background pixels")
                 .arg(fileName).arg(stats.chunksDrawn).arg(stats.chunksVisible).arg(stats.chunksCoarse)
                 .arg(stats.chunksMissing).arg(holes).arg(imageDifference(image, reference), 0, 'g', 3).arg(enclosed));
        if (stats.coarseBytes > 0 && stats.chunksMissing == 0) {
            check(enclosed == 0, QStringLiteral("%1: coarse fallback shows no background through the surface (%2 pixels)").arg(fileName).arg(enclosed));
        }
        return;
    }
    if (reference.isNull()) {
        check(false, QStringLiteral("%1: reference image exists").arg(fileName));
        return;
    }
    const double difference = imageDifference(image, reference);
    check(difference == 0.0, QStringLiteral("%1: identical to the reference run (%2 of pixels differ)")
                                 .arg(fileName).arg(difference, 0, 'g', 3));
}

void SelfTest::checkVisibleChunksDrawn(const QString& label)
{
    const render::RenderStats& stats = m_window.viewport().renderStats();
    const bool fits = stats.visibleVertexPages + stats.vertexReserveSlots <= stats.vertexSlots &&
                      stats.chunksVisible <= stats.indexSlots;
    if (!stats.limitedBudget) {
        check(fits, QStringLiteral("%1: pools sized for the whole mesh fit the view").arg(label));
    }
    if (fits) {
        check(stats.chunksDrawn == stats.chunksVisible,
              QStringLiteral("%1: the view fits the GPU pools and every visible chunk is drawn (%2 of %3)")
                  .arg(label).arg(stats.chunksDrawn).arg(stats.chunksVisible));
        return;
    }
    note(QStringLiteral("%1: the view needs %2 vertex pages and %3 chunks of indices; the pools hold %4 pages (%5 kept for copy on write) and %6 chunks")
             .arg(label).arg(stats.visibleVertexPages).arg(stats.chunksVisible)
             .arg(stats.vertexSlots).arg(stats.vertexReserveSlots).arg(stats.indexSlots));
    // The view does not fit: the pool must still be used, nearest chunks first.
    const u32 usableVertexSlots = stats.vertexSlots - stats.vertexReserveSlots;
    check(stats.chunksDrawn > 0 && (stats.chunksDrawn == stats.indexSlots || stats.vertexPagesResident + 8 >= usableVertexSlots),
          QStringLiteral("%1: the view does not fit; %2 nearest chunks are drawn and a pool is full (pages %3 of %4 usable, chunks %5 of %6)")
              .arg(label).arg(stats.chunksDrawn).arg(stats.vertexPagesResident).arg(usableVertexSlots)
              .arg(stats.indexChunksResident).arg(stats.indexSlots));
}

QImage SelfTest::captureAllCoarse(const QString& fileName)
{
    ViewportWindow& viewport = m_window.viewport();
    const render::RenderStats& stats = viewport.renderStats();
    if (stats.coarseBytes == 0) {
        return {};
    }
    viewport.setCoarseDebug(2);
    viewport.setCursorHiddenForCapture(true);
    const QImage image = capture(fileName);
    viewport.setCursorHiddenForCapture(false);
    check(stats.chunksCoarse > 0 && stats.coarseMismatchedVertices == 0,
          QStringLiteral("%1: %2 proxies drawn, GPU copies equal the full-resolution mesh (%3 vertices differ)")
              .arg(fileName).arg(stats.chunksCoarse).arg(stats.coarseMismatchedVertices));
    viewport.setCoarseDebug(0);
    settle();
    return image;
}

void SelfTest::checkCoarseSeams(const QString& label)
{
    ViewportWindow& viewport = m_window.viewport();
    const render::RenderStats& stats = viewport.renderStats();
    viewport.setCoarseDebug(0);
    const QImage detailed = capture(QStringLiteral("16_%1_detailed.png").arg(label));
    viewport.setCoarseDebug(1);
    const QImage mixed = capture(QStringLiteral("16_%1_mixed.png").arg(label));
    const u32 mixedCoarse = stats.chunksCoarse;
    viewport.setCoarseDebug(2);
    const QImage coarse = capture(QStringLiteral("16_%1_coarse.png").arg(label));
    const u32 allCoarse = stats.chunksCoarse;
    const u32 coarseTriangles = stats.coarseTriangles;
    viewport.setCoarseDebug(0);
    const qint64 detailedEnclosed = enclosedBackgroundPixels(detailed);
    const qint64 mixedCracks = enclosedBackgroundPixels(mixed) - detailedEnclosed;
    const qint64 coarseCracks = enclosedBackgroundPixels(coarse) - detailedEnclosed;
    check(mixedCoarse > 0 && mixedCracks == 0,
          QStringLiteral("view '%1': odd chunks coarse, no cracks at coarse/detailed seams (%2 chunks coarse, %3 enclosed background pixels more than detailed)")
              .arg(label).arg(mixedCoarse).arg(mixedCracks));
    check(allCoarse > 0 && coarseCracks == 0,
          QStringLiteral("view '%1': every chunk coarse, no cracks (%2 chunks, %3 triangles, %4 enclosed background pixels more than detailed)")
              .arg(label).arg(allCoarse).arg(coarseTriangles).arg(coarseCracks));
    note(QStringLiteral("view '%1': pixels differing from the detailed image: mixed %2, all coarse %3; silhouette pixels lost (4 px inside): mixed %4, all coarse %5")
             .arg(label).arg(imageDifference(mixed, detailed), 0, 'g', 3).arg(imageDifference(coarse, detailed), 0, 'g', 3)
             .arg(holePixels(mixed, detailed)).arg(holePixels(coarse, detailed)));
}

std::pair<u32, u32> SelfTest::compareCullModes(const QString& label)
{
    ViewportWindow& viewport = m_window.viewport();
    const render::RenderStats& stats = viewport.renderStats();

    viewport.setCullMode(render::CullMode::Off);
    const QImage off = capture(QStringLiteral("15_%1_cull_off.png").arg(label));
    const u32 offTriangles = stats.trianglesSubmitted;
    const u32 offChunks = stats.chunksDrawn;
    const u32 offDraws = stats.drawCalls;

    viewport.setCullMode(render::CullMode::Frustum);
    const QImage frustum = capture(QStringLiteral("15_%1_cull_frustum.png").arg(label));
    const u32 frustumTriangles = stats.trianglesSubmitted;
    const u32 frustumChunks = stats.chunksDrawn;

    viewport.setCullMode(render::CullMode::OnUnmerged);
    settle();
    const QImage unmerged = viewport.grab();
    const u32 unmergedDraws = stats.drawCalls;

    viewport.setCullMode(render::CullMode::On);
    const QImage on = capture(QStringLiteral("15_%1_cull_on.png").arg(label));
    const u32 onTriangles = stats.trianglesSubmitted;
    const u32 onChunks = stats.chunksDrawn;
    const u32 onDraws = stats.drawCalls;
    const u32 backFacing = stats.backFacingChunks;
    const u32 keptStale = stats.backFacingKeptStale;

    check(imageDifference(off, frustum) == 0.0, QStringLiteral("view '%1': same image with frustum culling and culling off").arg(label));
    check(imageDifference(off, on) == 0.0,
          QStringLiteral("view '%1': same image with culling on (frustum + back-facing) and off").arg(label));
    check(imageDifference(off, unmerged) == 0.0, QStringLiteral("view '%1': same image with one draw call per chunk").arg(label));
    note(QStringLiteral("view '%1': culling off %2 chunks / %3 triangles / %4 draws; frustum %5 chunks / %6 triangles; on %7 chunks / %8 triangles / %9 draws (%10 unmerged); %11 chunks back-facing, %12 kept for stale GPU copies")
             .arg(label).arg(offChunks).arg(offTriangles).arg(offDraws).arg(frustumChunks).arg(frustumTriangles)
             .arg(onChunks).arg(onTriangles).arg(onDraws).arg(unmergedDraws).arg(backFacing).arg(keptStale));
    return {offTriangles, onTriangles};
}

void SelfTest::noteSculptCost(const QString& label, const std::vector<float>& inputSamplesMs, double bvhMs, u32 dabs,
                              u64 coneTriangles)
{
    note(QStringLiteral("%1: %2 inputs, ms per input median %3, p95 %4, max %5 | %6 dabs, BVH refit%7 %8 ms per dab, %9 cone triangles per dab")
             .arg(label).arg(inputSamplesMs.size())
             .arg(percentile(inputSamplesMs, 0.5), 0, 'f', 3).arg(percentile(inputSamplesMs, 0.95), 0, 'f', 3)
             .arg(percentile(inputSamplesMs, 1.0), 0, 'f', 3).arg(dabs)
             .arg(m_controller.hasNormalCones() ? QStringLiteral(" + normal cones") : QString())
             .arg(dabs > 0 ? bvhMs / dabs : 0.0, 0, 'f', 4)
             .arg(dabs > 0 ? static_cast<double>(coneTriangles) / dabs : 0.0, 0, 'f', 0));
}

void SelfTest::checkStrongDeformation(const QString& prefix, bool compareModes)
{
    ViewportWindow& viewport = m_window.viewport();
    const auto checkCones = [this](const QString& stage) {
        if (!m_controller.hasNormalCones()) {
            return;
        }
        const auto [violations, usable] = m_controller.checkNormalCones();
        check(violations == 0, QStringLiteral("%1: every published normal cone holds its chunk's triangle normals (%2 violations, %3 of %4 chunks have a usable cone)")
                                   .arg(stage).arg(violations).arg(usable).arg(m_controller.chunkCount()));
    };
    const auto views = [&](const QString& stage) {
        const auto one = [&](const QString& view) {
            const QString name = QStringLiteral("%1_%2").arg(stage, view);
            if (compareModes) {
                compareCullModes(name);
            }
            const QString fileName = QStringLiteral("%1%2.png").arg(prefix, name);
            const QImage image = capture(fileName);
            if (m_scenario == Scenario::Pressure) {
                compareWithReference(image, fileName);
            }
        };
        viewport.frameMesh();
        one(QStringLiteral("full"));
        viewport.camera().orbit(90.0f, 0.0f);
        viewport.refresh();
        one(QStringLiteral("side"));
        viewport.camera().orbit(90.0f, 0.0f);
        viewport.refresh();
        one(QStringLiteral("back"));
        viewport.frameMesh();
        viewport.wheelZoom(4.0f);
        one(QStringLiteral("zoom"));
        viewport.frameMesh();
    };

    // Strong strokes from the front, then from the back.
    viewport.frameMesh();
    const std::size_t undoBefore = m_controller.undoCount();
    const auto before = positions();
    std::vector<float> inputs;
    double bvhMs = 0.0;
    u32 dabs = 0;
    u64 coneTriangles = 0;
    const int strokes = applyStrongStrokes(inputs, bvhMs, dabs, coneTriangles);
    const auto deformed = positions();
    check(deformed != before && m_controller.undoCount() == undoBefore + static_cast<std::size_t>(strokes),
          QStringLiteral("%1 strong strokes deformed the mesh (one undo step each)").arg(strokes));
    noteSculptCost(QStringLiteral("strong strokes, sculpt thread"), inputs, bvhMs, dabs, coneTriangles);
    checkCones(QStringLiteral("after strong strokes"));
    views(QStringLiteral("deformed"));

    std::vector<float> undoMs;
    for (int i = 0; i < strokes; ++i) {
        m_window.undo();
        m_controller.waitUntilIdle();
        undoMs.push_back(static_cast<float>(m_controller.stats().lastUndoRedoMs));
    }
    check(positions() == before, QStringLiteral("undo of the strong strokes restored every vertex exactly"));
    checkCones(QStringLiteral("after undo"));
    views(QStringLiteral("undone"));

    std::vector<float> redoMs;
    for (int i = 0; i < strokes; ++i) {
        m_window.redo();
        m_controller.waitUntilIdle();
        redoMs.push_back(static_cast<float>(m_controller.stats().lastUndoRedoMs));
    }
    check(positions() == deformed, QStringLiteral("redo of the strong strokes restored the deformed mesh exactly"));
    checkCones(QStringLiteral("after redo"));
    views(QStringLiteral("redone"));
    note(QStringLiteral("strong strokes: undo ms median %1, max %2 | redo ms median %3, max %4 (sculpt thread, %5 steps each)")
             .arg(percentile(undoMs, 0.5), 0, 'f', 3).arg(percentile(undoMs, 1.0), 0, 'f', 3)
             .arg(percentile(redoMs, 0.5), 0, 'f', 3).arg(percentile(redoMs, 1.0), 0, 'f', 3).arg(strokes));

    // Leave the mesh as it was for the steps that follow.
    for (int i = 0; i < strokes; ++i) {
        m_window.undo();
    }
    m_controller.waitUntilIdle();
    check(positions() == before, QStringLiteral("strong strokes undone again"));
    viewport.frameMesh();
}

int SelfTest::applyStrongStrokes(std::vector<float>& inputs, double& bvhMs, u32& dabs, u64& coneTriangles)
{
    ViewportWindow& viewport = m_window.viewport();
    int strokes = 0;
    const auto brushStroke = [&](sculpt::BrushId brush, float radius, float strength, QPointF from, QPointF to, int samples,
                                 Qt::KeyboardModifiers modifiers = {}) {
        m_window.selectBrush(brush);
        const sculpt::BrushSettings saved = m_controller.activeSettings();
        m_controller.setRadius(radius);
        m_controller.setStrength(strength);
        stroke(from, to, samples, modifiers);
        m_controller.setRadius(saved.radius);
        m_controller.setStrength(saved.strength);
        const app::InteractionStats sculptStats = m_controller.stats();
        inputs.insert(inputs.end(), sculptStats.updateSamplesMs.begin(), sculptStats.updateSamplesMs.end());
        bvhMs += sculptStats.strokeBvhMs;
        dabs += sculptStats.strokeDabCount;
        coneTriangles += sculptStats.strokeConeTriangles;
        ++strokes;
    };
    using sculpt::BrushId;
    for (int pass = 0; pass < 3; ++pass) {
        const bool forward = pass % 2 == 0;
        brushStroke(BrushId::Draw, 0.5f, 1.0f, viewportPoint(forward ? 0.35 : 0.65, 0.5), viewportPoint(forward ? 0.65 : 0.35, 0.5), 40);
    }
    brushStroke(BrushId::Draw, 0.3f, 1.0f, viewportPoint(0.5, 0.32), viewportPoint(0.5, 0.68), 40, Qt::ControlModifier);
    brushStroke(BrushId::Draw, 0.3f, 1.0f, viewportPoint(0.5, 0.68), viewportPoint(0.5, 0.32), 40, Qt::ControlModifier);
    brushStroke(BrushId::Move, 0.5f, 1.0f, viewportPoint(0.5, 0.45), viewportPoint(0.88, 0.15), 30);
    brushStroke(BrushId::Move, 0.35f, 1.0f, viewportPoint(0.38, 0.68), viewportPoint(0.10, 0.92), 30);
    viewport.camera().orbit(180.0f, 0.0f);
    viewport.refresh();
    brushStroke(BrushId::Draw, 0.5f, 1.0f, viewportPoint(0.3, 0.45), viewportPoint(0.7, 0.55), 40);
    brushStroke(BrushId::Move, 0.45f, 1.0f, viewportPoint(0.5, 0.5), viewportPoint(0.5, 0.05), 30);
    m_window.selectBrush(BrushId::Draw);
    return strokes;
}

void SelfTest::buildHistorySteps()
{
    // Cycles of undo all + redo all per stroke set; at 21M a step restores
    // hundreds of thousands of vertices, so frames overlap every command.
    constexpr int kCycles = 4;
    auto before = std::make_shared<std::vector<glm::vec3>>();
    auto camera = std::make_shared<render::OrbitCamera>();
    m_steps.push_back([this, before, camera] {
        m_window.viewport().frameMesh();
        *camera = m_window.viewport().camera();
        capture(QStringLiteral("h01_initial.png"));
        *before = positions();
        // Typical strokes: Draw, Smooth and Move at the editor's default strength.
        m_controller.setRadius(0.25f);
        m_window.selectBrush(sculpt::BrushId::Draw);
        stroke(viewportPoint(0.30, 0.45), viewportPoint(0.70, 0.50), 120);
        m_window.selectBrush(sculpt::BrushId::Smooth);
        stroke(viewportPoint(0.30, 0.55), viewportPoint(0.70, 0.45), 120);
        m_window.selectBrush(sculpt::BrushId::Move);
        stroke(viewportPoint(0.50, 0.50), viewportPoint(0.60, 0.35), 60);
        m_window.selectBrush(sculpt::BrushId::Draw);
        check(positions() != *before && m_controller.undoCount() == 3, QStringLiteral("3 typical strokes, one undo step each"));
    });
    m_steps.push_back([this] { runOverlappedHistory(QStringLiteral("typical strokes (radius 0.25)"), 3, kCycles); });
    m_steps.push_back([this] {
        std::vector<float> inputs;
        double bvhMs = 0.0;
        u32 dabs = 0;
        u64 coneTriangles = 0;
        const int strokes = applyStrongStrokes(inputs, bvhMs, dabs, coneTriangles);
        check(m_controller.undoCount() == 3u + static_cast<std::size_t>(strokes),
              QStringLiteral("%1 strong strokes, one undo step each").arg(strokes));
        m_window.viewport().frameMesh();
        runOverlappedHistory(QStringLiteral("strong strokes (radius 0.3 to 0.5, strength 1)"), strokes, kCycles);
    });
    m_steps.push_back([this, before, camera] {
        const std::size_t steps = m_controller.undoCount();
        for (std::size_t i = 0; i < steps; ++i) {
            m_window.undo();
        }
        check(positions() == *before, QStringLiteral("undo of every stroke restored every vertex exactly"));
        if (m_controller.hasNormalCones()) {
            const auto [violations, usable] = m_controller.checkNormalCones();
            check(violations == 0, QStringLiteral("after undo: every published normal cone holds its chunk's triangle normals (%1 violations, %2 usable)")
                                       .arg(violations).arg(usable));
        }
        m_window.viewport().camera() = *camera; // the strong strokes orbit it
        m_window.viewport().refresh();
        capture(QStringLiteral("h02_undone.png"));
        const QImage initial(QDir(m_outputDirectory).filePath(QStringLiteral("h01_initial.png")));
        const QImage undone(QDir(m_outputDirectory).filePath(QStringLiteral("h02_undone.png")));
        check(imageDifference(initial, undone) == 0.0, QStringLiteral("the undone mesh renders exactly as the initial one"));
    });
}

u64 SelfTest::meshHash() const
{
    m_controller.waitUntilIdle();
    const std::scoped_lock lock(m_controller.documentMutex());
    u64 hash = 1469598103934665603ull;
    const auto mix = [&hash](const glm::vec3& v) {
        u32 words[3];
        std::memcpy(words, &v, sizeof(words));
        for (const u32 word : words) {
            hash = (hash ^ word) * 1099511628211ull;
        }
    };
    for (const glm::vec3& p : m_controller.mesh().positions()) {
        mix(p);
    }
    for (const glm::vec3& n : m_controller.mesh().normals()) {
        mix(n);
    }
    return hash;
}

void SelfTest::writeBinary(const QString& name, const void* data, std::size_t bytes) const
{
    QFile file(QDir(m_outputDirectory).filePath(name));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(static_cast<const char*>(data), static_cast<qint64>(bytes));
    }
}

void SelfTest::buildSmoothSteps()
{
    // Fixed cameras, set once from the plain sphere so that sculpting does not
    // move them: the whole sphere (where the strokes are drawn), a close-up of
    // the upper-left silhouette, and a closer one.
    auto cameras = std::make_shared<std::vector<render::OrbitCamera>>();
    auto before = std::make_shared<std::vector<glm::vec3>>();
    auto afterHash = std::make_shared<u64>(0);
    static const char* const kViews[] = {"full", "close", "closer"};
    const auto view = [this, cameras](std::size_t index) {
        m_window.viewport().camera() = (*cameras)[index];
        m_window.viewport().refresh();
    };
    const auto captureAll = [this, cameras, view](const QString& stage) {
        std::vector<QImage> images;
        for (std::size_t i = 0; i < cameras->size(); ++i) {
            view(i);
            images.push_back(capture(QStringLiteral("s_%1_%2.png").arg(stage, QString::fromLatin1(kViews[i]))));
        }
        view(0);
        return images;
    };
    auto beforeImages = std::make_shared<std::vector<QImage>>();
    auto afterImages = std::make_shared<std::vector<QImage>>();
    auto referenceImages = std::make_shared<std::vector<QImage>>();

    m_steps.push_back([this, cameras, before, captureAll, beforeImages] {
        ViewportWindow& viewport = m_window.viewport();
        const float height = static_cast<float>(viewport.height());
        render::OrbitCamera full;
        full.frame(m_controller.bounds());
        render::OrbitCamera close = full;
        close.zoom(6.0f); // distance x 0.88^6
        close.pan(0.44f * height, 0.44f * height, height);
        render::OrbitCamera closer = full;
        closer.zoom(10.0f);
        closer.pan(0.95f * height, 0.95f * height, height);
        *cameras = {full, close, closer};
        *before = positions();
        *beforeImages = captureAll(QStringLiteral("1_before"));
    });
    auto after = std::make_shared<std::vector<glm::vec3>>();
    auto region = std::make_shared<std::vector<u32>>(); // dumped vertices: a cap around the strokes
    m_steps.push_back([this, view, captureAll, before, after, region, afterHash, afterImages] {
        // Repeatable strokes in the full view, over the upper-left of the
        // sphere: the editor's Smooth defaults (radius 0.25, strength 0.5),
        // three passes back and forth along a diagonal.
        view(0);
        m_window.selectBrush(sculpt::BrushId::Smooth);
        const sculpt::BrushSettings& settings = m_controller.activeSettings();
        note(QStringLiteral("Smooth radius %1, strength %2").arg(static_cast<double>(settings.radius)).arg(static_cast<double>(settings.strength)));
        // (Every point is on the sphere in the full view: a press off it would orbit.)
        stroke(viewportPoint(0.36, 0.32), viewportPoint(0.56, 0.48), 60);
        stroke(viewportPoint(0.56, 0.48), viewportPoint(0.36, 0.32), 60);
        stroke(viewportPoint(0.36, 0.32), viewportPoint(0.56, 0.48), 60);
        const app::InteractionStats stats = m_controller.stats();
        check(m_controller.undoCount() == 3, QStringLiteral("3 Smooth strokes, one undo step each (%1)").arg(m_controller.undoCount()));
        note(QStringLiteral("last stroke: %1 dabs, %2 vertices in the last dab").arg(stats.strokeDabCount).arg(stats.lastDabVertexCount));
        *after = positions();
        check(*after != *before, QStringLiteral("the strokes moved vertices"));
        // Dump a cap around the moved vertices (with a margin), and the
        // triangles inside it with indices into the dump.
        glm::dvec3 centre(0.0);
        for (std::size_t v = 0; v < after->size(); ++v) {
            if ((*after)[v] != (*before)[v]) {
                centre += glm::dvec3(glm::normalize((*before)[v]));
            }
        }
        const glm::vec3 axis = glm::normalize(glm::vec3(centre));
        std::vector<u32> local(after->size(), kInvalidIndex);
        region->clear();
        for (std::size_t v = 0; v < after->size(); ++v) {
            if (glm::dot(glm::normalize((*before)[v]), axis) > 0.9f) { // within about 26 degrees
                local[v] = static_cast<u32>(region->size());
                region->push_back(static_cast<u32>(v));
            }
        }
        std::vector<glm::vec3> values;
        values.reserve(region->size());
        for (const u32 v : *region) {
            values.push_back((*before)[v]);
        }
        writeBinary(QStringLiteral("s_positions_before.bin"), values.data(), values.size() * sizeof(glm::vec3));
        values.clear();
        for (const u32 v : *region) {
            values.push_back((*after)[v]);
        }
        writeBinary(QStringLiteral("s_positions_after.bin"), values.data(), values.size() * sizeof(glm::vec3));
        writeBinary(QStringLiteral("s_vertex_ids.bin"), region->data(), region->size() * sizeof(u32));
        std::vector<u32> triangles;
        const geo::Mesh& mesh = m_controller.mesh();
        for (u32 t = 0; t < mesh.triangleCount(); ++t) {
            const auto& corners = mesh.triangle(t);
            if (local[corners[0]] != kInvalidIndex && local[corners[1]] != kInvalidIndex && local[corners[2]] != kInvalidIndex) {
                triangles.insert(triangles.end(), {local[corners[0]], local[corners[1]], local[corners[2]]});
            }
        }
        writeBinary(QStringLiteral("s_indices.bin"), triangles.data(), triangles.size() * sizeof(u32));
        {
            const std::scoped_lock lock(m_controller.documentMutex());
            const auto normals = m_controller.mesh().normals();
            values.clear();
            for (const u32 v : *region) {
                values.push_back(normals[v]);
            }
            writeBinary(QStringLiteral("s_normals_incremental.bin"), values.data(), values.size() * sizeof(glm::vec3));
        }
        note(QStringLiteral("dumped %1 vertices and %2 triangles around the strokes").arg(region->size()).arg(triangles.size() / 3));
        *afterHash = meshHash();
        *afterImages = captureAll(QStringLiteral("2_after"));
    });
    m_steps.push_back([this, captureAll, afterImages, referenceImages, region] {
        // Same positions, every normal recomputed by the whole-mesh reference.
        m_controller.recomputeAllNormalsForReference();
        {
            const std::scoped_lock lock(m_controller.documentMutex());
            const auto normals = m_controller.mesh().normals();
            std::vector<glm::vec3> values;
            for (const u32 v : *region) {
                values.push_back(normals[v]);
            }
            writeBinary(QStringLiteral("s_normals_reference.bin"), values.data(), values.size() * sizeof(glm::vec3));
        }
        const std::vector<QImage> reference = captureAll(QStringLiteral("3_reference_normals"));
        *referenceImages = reference;
        for (std::size_t i = 0; i < reference.size(); ++i) {
            note(QStringLiteral("%1 view: after vs reference normals, %2 of pixels differ")
                     .arg(QString::fromLatin1(kViews[i])).arg(imageDifference((*afterImages)[i], reference[i]), 0, 'g', 4));
        }
    });
    m_steps.push_back([this, captureAll, before, after, afterHash, beforeImages, referenceImages] {
        for (int i = 0; i < 3; ++i) {
            m_window.undo();
        }
        check(positions() == *before, QStringLiteral("undo of the 3 Smooth strokes restored every vertex exactly"));
        // Undo recomputes only the normals around the restored vertices; the
        // reference pass above changed the others by rounding, so recompute
        // them all again before comparing images with the initial ones.
        m_controller.recomputeAllNormalsForReference();
        const std::vector<QImage> undone = captureAll(QStringLiteral("4_undone"));
        for (std::size_t i = 0; i < undone.size(); ++i) {
            note(QStringLiteral("%1 view: undone vs before, %2 of pixels differ")
                     .arg(QString::fromLatin1(kViews[i])).arg(imageDifference((*beforeImages)[i], undone[i]), 0, 'g', 4));
        }
        for (int i = 0; i < 3; ++i) {
            m_window.redo();
        }
        check(positions() == *after, QStringLiteral("redo of the 3 Smooth strokes restored every sculpted vertex exactly"));
        m_controller.recomputeAllNormalsForReference();
        const std::vector<QImage> redoneImages = captureAll(QStringLiteral("5_redone"));
        for (std::size_t i = 0; i < redoneImages.size(); ++i) {
            note(QStringLiteral("%1 view: redone vs after, both with reference normals, %2 of pixels differ")
                     .arg(QString::fromLatin1(kViews[i])).arg(imageDifference((*referenceImages)[i], redoneImages[i]), 0, 'g', 4));
        }
        (void)afterHash;
    });
}

void SelfTest::buildMoveSteps()
{
    auto before = std::make_shared<std::vector<glm::vec3>>();
    auto hashes = std::make_shared<std::vector<u64>>();
    m_steps.push_back([this, before] {
        m_window.viewport().frameMesh();
        capture(QStringLiteral("m01_initial.png"));
        *before = positions();
        note(QStringLiteral("Move updates: %1").arg(m_controller.coalesceMoveUpdates() ? QStringLiteral("latest-wins")
                                                                                       : QStringLiteral("every one applied")));
    });
    struct Plan {
        float radius;
        double fromX, fromY, toX, toY;
        int samples;
    };
    const Plan plans[] = {{0.125f, 0.45, 0.45, 0.60, 0.30, 300},
                          {0.25f, 0.55, 0.55, 0.35, 0.70, 300},
                          {0.5f, 0.50, 0.50, 0.75, 0.40, 500}};
    for (std::size_t i = 0; i < std::size(plans); ++i) {
        const Plan plan = plans[i];
        m_steps.push_back([this, plan, i, hashes] {
            runLiveMove(plan.radius, viewportPoint(plan.fromX, plan.fromY), viewportPoint(plan.toX, plan.toY), plan.samples,
                        QStringLiteral("Move %1, radius %2").arg(i + 1).arg(static_cast<double>(plan.radius)), hashes);
        });
    }
    m_steps.push_back([this, before, hashes] {
        capture(QStringLiteral("m02_moved.png"));
        const std::size_t steps = m_controller.undoCount();
        check(steps == 3, QStringLiteral("3 Move strokes, one undo step each (%1)").arg(steps));
        for (std::size_t i = 0; i < steps; ++i) {
            m_window.undo();
        }
        check(positions() == *before, QStringLiteral("undo of every Move stroke restored every vertex exactly"));
        if (m_controller.hasNormalCones()) {
            const auto [violations, usable] = m_controller.checkNormalCones();
            check(violations == 0, QStringLiteral("after undo: every normal cone holds its chunk's triangle normals (%1 violations, %2 usable)")
                                       .arg(violations).arg(usable));
        }
        capture(QStringLiteral("m03_undone.png"));
        const QImage initial(QDir(m_outputDirectory).filePath(QStringLiteral("m01_initial.png")));
        const QImage undone(QDir(m_outputDirectory).filePath(QStringLiteral("m03_undone.png")));
        check(imageDifference(initial, undone) == 0.0, QStringLiteral("the undone mesh renders exactly as the initial one"));
        for (std::size_t i = 0; i < steps; ++i) {
            m_window.redo();
        }
        const u64 redone = meshHash();
        check(!hashes->empty() && redone == hashes->back(), QStringLiteral("redo of every Move stroke restored the moved mesh exactly"));
        capture(QStringLiteral("m04_redone.png"));
        const QImage moved(QDir(m_outputDirectory).filePath(QStringLiteral("m02_moved.png")));
        const QImage redoneImage(QDir(m_outputDirectory).filePath(QStringLiteral("m04_redone.png")));
        check(imageDifference(moved, redoneImage) == 0.0, QStringLiteral("the redone mesh renders exactly as after the strokes"));
        QString list;
        for (const u64 hash : *hashes) {
            list += QStringLiteral(" %1").arg(hash, 16, 16, QLatin1Char('0'));
        }
        note(QStringLiteral("mesh hash after each Move stroke:%1 (must not depend on --move-updates)").arg(list));
        for (std::size_t i = 0; i < steps; ++i) {
            m_window.undo();
        }
    });
}

void SelfTest::runLiveMove(float radius, const QPointF& from, const QPointF& to, int samples, const QString& label,
                           std::shared_ptr<std::vector<u64>> hashes)
{
    constexpr int kSampleIntervalMs = 2;
    holdSteps();
    m_window.selectBrush(sculpt::BrushId::Move);
    m_controller.setRadius(radius);
    ViewportWindow& viewport = m_window.viewport();
    const u64 hashBefore = meshHash();
    const std::size_t undoBefore = m_controller.undoCount();
    // Out to `to` for 60% of the inputs, then back towards the midpoint.
    const auto pointAt = [from, to, samples](int sample) {
        const double t = static_cast<double>(sample) / samples;
        const QPointF mid = (from + to) * 0.5;
        return t <= 0.6 ? from + (to - from) * (t / 0.6) : to + (mid - to) * ((t - 0.6) / 0.4);
    };
    viewport.setFrameSampling(true);
    m_liveSample = 0;
    viewport.pointerPress(pointAt(0), Qt::LeftButton, {});
    auto* timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);
    connect(timer, &QTimer::timeout, this, [this, timer, pointAt, samples, label, hashes, hashBefore, undoBefore] {
        ViewportWindow& view = m_window.viewport();
        if (++m_liveSample <= samples) {
            view.pointerMove(pointAt(m_liveSample));
            return;
        }
        timer->stop();
        timer->deleteLater();
        const u32 queuedAtRelease = m_controller.stats().queuedInputs;
        const core::Stopwatch drain;
        view.pointerRelease();
        // Until the stroke's end is applied (not the background BVH work after it).
        while (m_controller.undoCount() == undoBefore || m_controller.stats().queuedInputs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const double drainMs = drain.elapsedMs();
        m_controller.waitUntilIdle();
        view.setFrameSampling(false);

        const app::InteractionStats stats = m_controller.stats();
        const render::RenderStats& render = view.renderStats();
        std::vector<float> wait;
        std::vector<float> process;
        u32 applied = 0;
        u32 dropped = 0;
        for (const auto& s : stats.inputSamples) {
            if (s.coalesced) {
                ++dropped;
                continue;
            }
            ++applied;
            wait.push_back(static_cast<float>(s.startedAtMs - s.queuedAtMs));
            process.push_back(static_cast<float>(s.doneAtMs - s.startedAtMs));
        }
        const u64 hash = meshHash();
        hashes->push_back(hash);
        check(hash != hashBefore && m_controller.undoCount() == undoBefore + 1,
              QStringLiteral("%1: modified the mesh, one undo step").arg(label));
        check(applied + dropped == static_cast<u32>(samples) + 1u,
              QStringLiteral("%1: every input accounted for (%2 applied + %3 dropped)").arg(label).arg(applied).arg(dropped));
        check(m_controller.coalesceMoveUpdates() || dropped == 0, QStringLiteral("%1: no input dropped unless latest-wins is on").arg(label));
        const double worstWait = percentile(render.lockWaitSamplesMs, 1.0);
        check(worstWait < 50.0, QStringLiteral("%1: renderer never waited long for the mesh (worst %2 ms)").arg(label).arg(worstWait, 0, 'f', 3));
        if (m_controller.hasNormalCones()) {
            const auto [violations, usable] = m_controller.checkNormalCones();
            check(violations == 0, QStringLiteral("%1: every normal cone holds its chunk's triangle normals (%2 violations, %3 usable)")
                                       .arg(label).arg(violations).arg(usable));
        }
        note(QStringLiteral("%1: %2 inputs at %3 ms, %4 applied, %5 dropped, %6 still queued at release, end applied %7 ms after release, %8 frames, %9 vertices per grab")
                 .arg(label).arg(samples).arg(kSampleIntervalMs).arg(applied).arg(dropped).arg(queuedAtRelease)
                 .arg(drainMs, 0, 'f', 0).arg(render.frameCpuSamplesMs.size()).arg(stats.lastDabVertexCount));
        note(QStringLiteral("  queue wait ms: median %1, p95 %2, max %3 | per applied input ms: median %4, p95 %5, max %6")
                 .arg(percentile(wait, 0.5), 0, 'f', 1).arg(percentile(wait, 0.95), 0, 'f', 1).arg(percentile(wait, 1.0), 0, 'f', 1)
                 .arg(percentile(process, 0.5), 0, 'f', 2).arg(percentile(process, 0.95), 0, 'f', 2).arg(percentile(process, 1.0), 0, 'f', 2));
        note(QStringLiteral("  renderer lock wait ms: median %1, p95 %2, max %3 | frame CPU ms: median %4, p95 %5")
                 .arg(percentile(render.lockWaitSamplesMs, 0.5), 0, 'f', 3).arg(percentile(render.lockWaitSamplesMs, 0.95), 0, 'f', 3)
                 .arg(worstWait, 0, 'f', 3).arg(percentile(render.frameCpuSamplesMs, 0.5), 0, 'f', 3)
                 .arg(percentile(render.frameCpuSamplesMs, 0.95), 0, 'f', 3));
        note(QStringLiteral("  mesh hash %1").arg(hash, 16, 16, QLatin1Char('0')));
        resumeSteps();
    });
    timer->start(kSampleIntervalMs);
}

void SelfTest::buildLatencySteps()
{
    m_steps.push_back([this] {
        m_window.viewport().frameMesh();
        capture(QStringLiteral("l01_initial.png"));
        note(QStringLiteral("%1 triangles; clocks: steady_clock ms; the last observable event of a frame is frameReady() returning "
                            "(QVulkanWindow has issued the queue submit and the present request; no presentation timing)")
                 .arg(m_controller.mesh().triangleCount()));
    });
    m_steps.push_back([this] { runLatencyTrace(sculpt::BrushId::Draw, QStringLiteral("draw")); });
    m_steps.push_back([this] { runLatencyTrace(sculpt::BrushId::Smooth, QStringLiteral("smooth")); });
    m_steps.push_back([this] {
        check(m_controller.undoCount() == 0, QStringLiteral("both traced strokes undone (%1 undo steps left)").arg(m_controller.undoCount()));
        capture(QStringLiteral("l02_end.png"));
    });
}

void SelfTest::runLatencyTrace(sculpt::BrushId brush, const QString& name)
{
    constexpr double kIntervalMs = 1000.0 / 120.0;
    constexpr int kMoves = 240; // after the press: 2 s at 120 Hz
    constexpr float kRadius = 0.25f;
    constexpr double kTailMs = 300.0; // frames kept rendering after the stroke's end is applied
    using Clock = std::chrono::steady_clock;
    const auto nowMs = [] { return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count(); };

    holdSteps();
    m_controller.waitUntilIdle();
    m_window.selectBrush(brush);
    m_controller.setRadius(kRadius);
    sculpt::SmoothBrush::resetSteps(); // the brush's step timers are process-wide; this stroke only
    ViewportWindow& viewport = m_window.viewport();

    // A circle over the front of the model, at 0.45 of its screen radius
    // (about 2.8 units of surface on the unit sphere: the rates stroke's 1.5
    // per second for 2 s).
    const QPointF centre = viewportPoint(0.5, 0.5);
    double inside = 0.0;
    double outside = 0.5 * viewport.width();
    for (int i = 0; i < 30; ++i) {
        const double mid = 0.5 * (inside + outside);
        (m_controller.pick(viewport.rayAt(centre + QPointF(mid, 0.0))).has_value() ? inside : outside) = mid;
    }
    const double circle = 0.45 * inside;
    const auto pointAt = [centre, circle](int move) {
        const double angle = 2.0 * 3.14159265358979323846 * move / kMoves;
        return centre + QPointF(circle * std::cos(angle), circle * std::sin(angle));
    };

    struct State {
        std::vector<double> intendedMs; // per input (0: the press)
        std::vector<double> sentMs;
        double startMs = 0.0;
        double releaseMs = 0.0;
        double endAppliedMs = 0.0;
        int nextMove = 1;
        std::size_t undoBefore = 0;
    };
    auto state = std::make_shared<State>();
    state->undoBefore = m_controller.undoCount();
    viewport.setFrameSampling(true);
    state->startMs = nowMs();
    state->intendedMs.push_back(state->startMs);
    state->sentMs.push_back(state->startMs);
    viewport.pointerPress(pointAt(0), Qt::LeftButton, {});

    auto* timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);
    connect(timer, &QTimer::timeout, this, [this, timer, state, pointAt, nowMs, name, circle] {
        ViewportWindow& view = m_window.viewport();
        // Inputs due by now are sent in order (a late GUI thread gets them as a burst, as from the OS queue).
        while (state->nextMove <= kMoves) {
            const double due = state->startMs + kIntervalMs * state->nextMove;
            if (nowMs() < due) {
                return;
            }
            state->intendedMs.push_back(due);
            state->sentMs.push_back(nowMs());
            view.pointerMove(pointAt(state->nextMove));
            ++state->nextMove;
        }
        if (state->releaseMs == 0.0) {
            state->releaseMs = nowMs();
            view.pointerRelease();
            return;
        }
        if (state->endAppliedMs == 0.0) {
            if (m_controller.undoCount() == state->undoBefore || m_controller.stats().queuedInputs > 0) {
                return;
            }
            state->endAppliedMs = nowMs();
        }
        if (nowMs() < state->endAppliedMs + kTailMs) {
            return;
        }
        timer->stop();
        timer->deleteLater();
        m_controller.waitUntilIdle();
        view.setFrameSampling(false);

        const app::InteractionStats stats = m_controller.stats();
        const render::RenderStats& render = view.renderStats();
        const std::vector<render::FrameTrace>& frames = render.frameTraces;
        const double t0 = state->startMs;
        check(stats.inputSamples.size() == state->sentMs.size(),
              QStringLiteral("%1 trace: one input sample per sent input (%2 of %3)").arg(name).arg(stats.inputSamples.size()).arg(state->sentMs.size()));

        // For each input with dabs: the first frame that took its last dab's
        // commit and drew every visible page current.
        std::vector<double> toCommit;
        std::vector<double> toConsume;
        std::vector<double> toSubmit;
        std::vector<double> total;
        u32 measured = 0;
        u32 dabInputs = 0;
        u32 olderFramesAfterCommit = 0; // frames submitted after the commit that still had older geometry
        const QString path = QDir(m_outputDirectory).filePath(QStringLiteral("trace_%1.csv").arg(name));
        QFile file(path);
        const bool opened = file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
        QTextStream out(&file);
        out << "kind,index,intended_ms,sent_ms,queued_ms,started_ms,done_ms,dabs,commit_seq,committed_ms,frame\n";
        std::size_t frameCursor = 0;
        for (std::size_t i = 0; i < stats.inputSamples.size() && i < state->sentMs.size(); ++i) {
            const auto& s = stats.inputSamples[i];
            long long frameIndex = -1;
            if (s.dabs > 0) {
                ++dabInputs;
                while (frameCursor < frames.size() &&
                       (frames[frameCursor].commitSeq < s.commitSeq || frames[frameCursor].stalePages > 0 || frames[frameCursor].pendingWork)) {
                    ++frameCursor;
                }
                if (frameCursor < frames.size()) {
                    frameIndex = static_cast<long long>(frameCursor);
                    const render::FrameTrace& f = frames[frameCursor];
                    ++measured;
                    toCommit.push_back(s.committedAtMs - s.queuedAtMs);
                    toConsume.push_back(f.consumedMs - s.committedAtMs);
                    toSubmit.push_back(f.submittedMs - s.committedAtMs);
                    total.push_back(f.submittedMs - s.queuedAtMs);
                    for (std::size_t k = 0; k < frameCursor; ++k) {
                        if (frames[k].submittedMs > s.committedAtMs && frames[k].commitSeq < s.commitSeq) {
                            ++olderFramesAfterCommit;
                        }
                    }
                }
            }
            out << "input," << i << ',' << QString::number(state->intendedMs[i] - t0, 'f', 4) << ','
                << QString::number(state->sentMs[i] - t0, 'f', 4) << ',' << QString::number(s.queuedAtMs - t0, 'f', 4) << ','
                << QString::number(s.startedAtMs - t0, 'f', 4) << ',' << QString::number(s.doneAtMs - t0, 'f', 4) << ',' << s.dabs << ','
                << s.commitSeq << ',' << (s.dabs > 0 ? QString::number(s.committedAtMs - t0, 'f', 4) : QString()) << ',' << frameIndex << '\n';
        }
        out << "kind,index,start_ms,consumed_ms,submitted_ms,commit_seq,stale_pages,pending_work,upload_vertices\n";
        for (std::size_t k = 0; k < frames.size(); ++k) {
            const render::FrameTrace& f = frames[k];
            out << "frame," << k << ',' << QString::number(f.startMs - t0, 'f', 4) << ',' << QString::number(f.consumedMs - t0, 'f', 4) << ','
                << QString::number(f.submittedMs - t0, 'f', 4) << ',' << f.commitSeq << ',' << f.stalePages << ','
                << (f.pendingWork ? 1 : 0) << ',' << f.uploadVertices << '\n';
        }
        for (std::size_t k = 0; k < render.changeNoticedMs.size(); ++k) {
            out << "notice," << k << ',' << QString::number(render.changeNoticedMs[k] - t0, 'f', 4) << '\n';
        }
        for (std::size_t k = 0; k < render.updateRequestMs.size(); ++k) {
            out << "update_request," << k << ',' << QString::number(render.updateRequestMs[k] - t0, 'f', 4) << '\n';
        }
        out << "release,0," << QString::number(state->releaseMs - t0, 'f', 4) << '\n';
        out << "end_applied,0," << QString::number(state->endAppliedMs - t0, 'f', 4) << '\n';
        out.flush();
        check(opened && file.error() == QFileDevice::NoError, QStringLiteral("%1 trace written: %2").arg(name, path));
        check(measured == dabInputs && measured > 0,
              QStringLiteral("%1 trace: every dab-producing input reached a frame (%2 of %3)").arg(name).arg(measured).arg(dabInputs));
        const auto row = [](const char* label, const std::vector<double>& v) {
            return QStringLiteral("%1 p50 %2 p95 %3 max %4").arg(QString::fromLatin1(label))
                .arg(percentile(v, 0.5), 0, 'f', 2).arg(percentile(v, 0.95), 0, 'f', 2).arg(percentile(v, 1.0), 0, 'f', 2);
        };
        note(QStringLiteral("%1 trace: radius %2, circle of %3 px, %4 inputs, %5 with dabs, %6 measured, %7 frames, "
                            "%8 frames submitted after a measured commit still had older geometry")
                 .arg(name).arg(static_cast<double>(kRadius)).arg(circle, 0, 'f', 0).arg(stats.inputSamples.size())
                 .arg(dabInputs).arg(measured).arg(frames.size()).arg(olderFramesAfterCommit));
        note(QStringLiteral("  ms: %1 | %2 | %3 | %4").arg(row("queued->commit", toCommit), row("commit->frame took it", toConsume),
                                                          row("commit->submit+present request", toSubmit), row("queued->submit", total)));
        {
            const sculpt::StrokeStats& st = stats.strokeStages;
            const double n = std::max<u32>(st.dabCount, 1u);
            note(QStringLiteral("  stage timers, ms per dab over %1 dabs: gather %2 (query %3, select %4, prepare wall %5), stage %6, brush %7, "
                                "normals %8 (region %9, compute %10, dirty %11), publish %12, BVH %13, commit lock wait %14")
                     .arg(st.dabCount).arg(st.gatherMs / n, 0, 'f', 2).arg(st.gather.queryMs / n, 0, 'f', 2)
                     .arg(st.gather.selectMs / n, 0, 'f', 2).arg(st.gather.prepareWallMs / n, 0, 'f', 2).arg(st.stageMs / n, 0, 'f', 2)
                     .arg(st.brushMs / n, 0, 'f', 2).arg(st.normalsMs / n, 0, 'f', 2).arg(st.normals.regionMs / n, 0, 'f', 2)
                     .arg(st.normals.computeMs / n, 0, 'f', 2).arg(st.normals.dirtyMs / n, 0, 'f', 2).arg(st.publishMs / n, 0, 'f', 2)
                     .arg(st.bvhMs / n, 0, 'f', 2).arg(st.commitWaitMs / n, 0, 'f', 2));
            note(QStringLiteral("  per dab: %1 moved vertices, %2 affected (normals), %3 triangles; largest dab %4 vertices")
                     .arg(static_cast<double>(st.normals.moved) / n, 0, 'f', 0).arg(static_cast<double>(st.normals.affected) / n, 0, 'f', 0)
                     .arg(static_cast<double>(st.normals.triangles) / n, 0, 'f', 0).arg(st.maxDabVertexCount));
        }
        {
            const sculpt::SmoothBrush::Steps sm = sculpt::SmoothBrush::steps();
            if (sm.calls > 0) {
                const double c = static_cast<double>(sm.calls);
                note(QStringLiteral("  smooth brush per dab, ms: grid clear %1, bin %2, occupied %3, kernel %4, kernel commit %5, Laplacian %6, "
                                    "Laplacian commit %7; %8 of %9 dabs dense, %10 binned in parallel, %11 Laplacian passes per dab")
                         .arg(sm.ms[0] / c, 0, 'f', 3).arg(sm.ms[1] / c, 0, 'f', 3).arg(sm.ms[2] / c, 0, 'f', 3).arg(sm.ms[3] / c, 0, 'f', 3)
                         .arg(sm.ms[4] / c, 0, 'f', 3).arg(sm.ms[5] / c, 0, 'f', 3).arg(sm.ms[6] / c, 0, 'f', 3).arg(sm.denseCalls)
                         .arg(sm.calls).arg(sm.parallelBins).arg(static_cast<double>(sm.laplacianPasses) / c, 0, 'f', 2));
            }
        }

        m_window.undo();
        m_controller.waitUntilIdle();
        resumeSteps();
    });
    timer->start(1);
}

void SelfTest::runOverlappedHistory(const QString& label, int steps, int cycles)
{
    holdSteps();
    ViewportWindow& viewport = m_window.viewport();
    viewport.pointerMove(viewportPoint(0.005, 0.005)); // no brush cursor
    settle();
    struct State {
        std::vector<glm::vec3> start;
        std::size_t startUndo = 0;
        int issued = 0;
        std::size_t expectedUndo = 0;
        std::vector<float> undoHold, undoTotal, redoHold, redoTotal;
        bool lastWasUndo = true;
    };
    auto state = std::make_shared<State>();
    state->start = positions();
    state->startUndo = m_controller.undoCount();
    const app::InteractionStats before = m_controller.stats();
    const int commands = 2 * steps * cycles;
    // Command k: undo while k mod 2*steps < steps, else redo.
    const auto issue = [this, state, steps] {
        const bool undo = (state->issued % (2 * steps)) < steps;
        state->lastWasUndo = undo;
        state->expectedUndo = undo ? m_controller.undoCount() - 1 : m_controller.undoCount() + 1;
        ++state->issued;
        if (undo) {
            m_window.undo();
        } else {
            m_window.redo();
        }
    };

    viewport.setFrameSampling(true);
    issue();
    auto* timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);
    connect(timer, &QTimer::timeout, this, [this, timer, state, issue, commands, label, steps, cycles, before] {
        ViewportWindow& view = m_window.viewport();
        view.refresh(); // frames back to back, whether or not the mesh changed
        if (m_controller.undoCount() != state->expectedUndo) {
            return; // still running
        }
        const app::InteractionStats stats = m_controller.stats();
        (state->lastWasUndo ? state->undoHold : state->redoHold).push_back(static_cast<float>(stats.lastUndoRedoHoldMs));
        (state->lastWasUndo ? state->undoTotal : state->redoTotal).push_back(static_cast<float>(stats.lastUndoRedoMs));
        if (state->issued < commands) {
            issue();
            return;
        }
        timer->stop();
        timer->deleteLater();
        m_controller.waitUntilIdle();
        const render::RenderStats& renderStats = view.renderStats();
        const std::vector<float> waits = renderStats.lockWaitSamplesMs;
        const std::vector<float> gpuMs = renderStats.frameGpuSamplesMs;
        std::vector<float> triangles(renderStats.trianglesSubmittedSamples.begin(), renderStats.trianglesSubmittedSamples.end());
        std::vector<float> backFacing(renderStats.backFacingSamples.begin(), renderStats.backFacingSamples.end());
        view.setFrameSampling(false);
        const app::InteractionStats after = m_controller.stats();
        std::vector<float> waited;
        for (const float wait : waits) {
            if (wait >= 0.05f) {
                waited.push_back(wait);
            }
        }
        check(positions() == state->start && m_controller.undoCount() == state->startUndo,
              QStringLiteral("%1: %2 cycles of undo all + redo all end exactly where they began").arg(label).arg(cycles));
        if (m_controller.hasNormalCones()) {
            const auto [violations, usable] = m_controller.checkNormalCones();
            check(violations == 0, QStringLiteral("%1: every published normal cone holds its chunk's triangle normals (%2 violations, %3 usable)")
                                       .arg(label).arg(violations).arg(usable));
        }
        check(waits.size() >= 10,
              QStringLiteral("%1: frames rendered while undo and redo ran (%2 frames, %3 commands)").arg(label).arg(waits.size()).arg(commands));
        check(!waited.empty(), QStringLiteral("%1: frames overlapped the commands' lock holds (%2 frames waited 0.05 ms or more)")
                                   .arg(label).arg(waited.size()));
        const auto spread = [](const std::vector<float>& samples) {
            return QStringLiteral("p50 %1, p95 %2, max %3")
                .arg(percentile(samples, 0.5), 0, 'f', 3)
                .arg(percentile(samples, 0.95), 0, 'f', 3)
                .arg(percentile(samples, 1.0), 0, 'f', 3);
        };
        note(QStringLiteral("overlapped history, %1: %2 steps x %3 cycles, %4 frames, edit scratch %5 MiB, normals %6 the lock")
                 .arg(label).arg(steps).arg(cycles).arg(waits.size())
                 .arg(static_cast<double>(stats.editScratchBytes) / (1024.0 * 1024.0), 0, 'f', 1)
                 .arg(m_controller.historyNormalsBeforeLock() ? QStringLiteral("before") : QStringLiteral("under")));
        note(QStringLiteral("  renderer wait for the mesh lock ms (every frame): %1").arg(spread(waits)));
        note(QStringLiteral("  renderer wait ms (the %1 frames that waited): %2").arg(waited.size()).arg(spread(waited)));
        note(QStringLiteral("  undo lock hold ms: %1 | total (sculpt thread): %2").arg(spread(state->undoHold), spread(state->undoTotal)));
        note(QStringLiteral("  redo lock hold ms: %1 | total (sculpt thread): %2").arg(spread(state->redoHold), spread(state->redoTotal)));
        note(QStringLiteral("  frame CPU ms: %1").arg(spread(renderStats.frameCpuSamplesMs)));
        note(QStringLiteral("  frame GPU ms: %1 (%2 frames)").arg(spread(gpuMs)).arg(gpuMs.size()));
        double triangleSum = 0.0;
        for (const float t : triangles) {
            triangleSum += t;
        }
        note(QStringLiteral("  triangles submitted per frame: mean %1, p50 %2, p95 %3, max %4 | back-facing chunks skipped p50 %5, min %6")
                 .arg(triangles.empty() ? 0.0 : triangleSum / static_cast<double>(triangles.size()), 0, 'f', 0)
                 .arg(percentile(triangles, 0.5), 0, 'f', 0).arg(percentile(triangles, 0.95), 0, 'f', 0)
                 .arg(percentile(triangles, 1.0), 0, 'f', 0).arg(percentile(backFacing, 0.5), 0, 'f', 0)
                 .arg(percentile(backFacing, 0.0), 0, 'f', 0));
        note(QStringLiteral("  cones %1: rebuild %2 steps, %3 chunks, %4 ms compute (worst step %5 ms), worst publish hold %6 ms")
                 .arg(m_controller.incrementalCones() ? QStringLiteral("incremental") : QStringLiteral("exact"))
                 .arg(after.coneRebuildSteps - before.coneRebuildSteps).arg(after.coneRebuildChunks - before.coneRebuildChunks)
                 .arg(after.coneRebuildMs - before.coneRebuildMs, 0, 'f', 2).arg(after.worstConeRebuildMs, 0, 'f', 3)
                 .arg(after.worstConeRebuildHoldMs, 0, 'f', 4));
        resumeSteps();
    });
    timer->start(1);
}

void SelfTest::runRenderMeasurements()
{
    // Nine batches of presented frames: {fully visible, zoomed, only the limb visible} x
    // {culling off, on, on with one draw per chunk}. Then the same nine with the
    // scene drawn kLoadRepeat times per frame: a light frame leaves the GPU at
    // power-saving clocks, so its GPU time says little about rendering cost.
    // Loaded, the GPU runs at full clock and the time per pass is comparable.
    constexpr int kModeCount = 4;
    constexpr int kPerLoad = 3 * kModeCount;
    constexpr int kBatches = 2 * kPerLoad;
    constexpr u32 kLoadRepeat = 64;
    constexpr u64 kFramesPerBatch = 90;
    static const char* const kModeNames[] = {"culling off", "frustum culling", "culling on (frustum + back-facing)",
                                             "culling on, one draw per chunk"};
    static constexpr render::CullMode kModes[] = {render::CullMode::Off, render::CullMode::Frustum, render::CullMode::On,
                                                  render::CullMode::OnUnmerged};

    holdSteps();
    m_measureIndex = -1;
    auto* timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);
    connect(timer, &QTimer::timeout, this, [this, timer] {
        ViewportWindow& viewport = m_window.viewport();
        const render::RenderStats& stats = viewport.renderStats();
        if (m_measureIndex >= 0 && stats.frameCount < m_measureTargetFrame) {
            viewport.requestUpdate(); // keep presenting frames
            return;
        }
        if (m_measureIndex >= 0) {
            // The first samples of a batch still belong to the previous settings.
            const auto tail = [](std::vector<float> samples) {
                samples.erase(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(samples.size(), 4)));
                return samples;
            };
            const bool loaded = m_measureIndex >= kPerLoad;
            const int view = (m_measureIndex % kPerLoad) / kModeCount;
            std::vector<float> gpu = tail(stats.frameGpuSamplesMs);
            if (loaded) {
                for (float& sample : gpu) {
                    sample /= static_cast<float>(kLoadRepeat);
                }
            }
            const std::vector<float> cpu = tail(stats.frameCpuSamplesMs);
            const std::vector<float> interval = tail(stats.frameIntervalSamplesMs);
            // Draw calls and triangles are per frame, so loaded batches count kLoadRepeat passes.
            note(QStringLiteral("render [%1 view, %2%3]: %4 of %5 chunks, %6 triangles, %7 draw calls")
                     .arg(view == 0 ? QStringLiteral("full") : view == 1 ? QStringLiteral("zoomed") : QStringLiteral("edge"))
                     .arg(QString::fromLatin1(kModeNames[m_measureIndex % kModeCount]))
                     .arg(loaded ? QStringLiteral(", %1 passes per frame").arg(kLoadRepeat) : QString())
                     .arg(stats.chunksDrawn).arg(stats.chunkCount).arg(stats.trianglesSubmitted).arg(stats.drawCalls));
            note(QStringLiteral("    %1: median %2, p95 %3, max %4 | CPU ms: median %5, p95 %6, max %7 | interval ms: median %8, p95 %9")
                     .arg(loaded ? QStringLiteral("GPU ms per pass, GPU fully loaded") : QStringLiteral("GPU ms (light load, clocks may be low)"))
                     .arg(percentile(gpu, 0.5), 0, 'f', 3).arg(percentile(gpu, 0.95), 0, 'f', 3).arg(percentile(gpu, 1.0), 0, 'f', 3)
                     .arg(percentile(cpu, 0.5), 0, 'f', 3).arg(percentile(cpu, 0.95), 0, 'f', 3).arg(percentile(cpu, 1.0), 0, 'f', 3)
                     .arg(percentile(interval, 0.5), 0, 'f', 2).arg(percentile(interval, 0.95), 0, 'f', 2));
            if (kModes[m_measureIndex % kModeCount] == render::CullMode::On) {
                note(QStringLiteral("    back-facing: %1 chunks skipped; last chunk selection %2 ms")
                         .arg(stats.backFacingChunks).arg(stats.selectMs, 0, 'f', 4));
            }
        }
        if (++m_measureIndex >= kBatches) {
            timer->stop();
            timer->deleteLater();
            viewport.setFrameSampling(false);
            viewport.setDrawRepeat(1);
            viewport.setCullMode(render::CullMode::On);
            viewport.frameMesh();
            check(!stats.gpuTimingAvailable || stats.lastGpuMs > 0.0, QStringLiteral("GPU frame times were measured"));
            resumeSteps();
            return;
        }
        const int viewStep = m_measureIndex % kPerLoad;
        if (viewStep == 0) {
            viewport.frameMesh();
        } else if (viewStep == kModeCount) {
            viewport.frameMesh();
            viewport.wheelZoom(7.0f);
        } else if (viewStep == 2 * kModeCount) {
            viewport.camera().pan(static_cast<float>(viewport.height()) * 0.8f, 0.0f, static_cast<float>(viewport.height()));
            viewport.refresh();
        }
        viewport.setDrawRepeat(m_measureIndex >= kPerLoad ? kLoadRepeat : 1);
        viewport.setCullMode(kModes[m_measureIndex % kModeCount]);
        settle();
        viewport.setFrameSampling(true);
        m_measureTargetFrame = stats.frameCount + kFramesPerBatch;
        viewport.requestUpdate();
    });
    timer->start(1);
}

void SelfTest::runLoadedMeasurement(const QString& label)
{
    constexpr u32 kLoadRepeat = 64;
    constexpr u64 kFrames = 60;
    holdSteps();
    ViewportWindow& viewport = m_window.viewport();
    viewport.setDrawRepeat(kLoadRepeat);
    settle();
    viewport.setFrameSampling(true);
    m_measureTargetFrame = viewport.renderStats().frameCount + kFrames;
    viewport.requestUpdate();
    auto* timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);
    connect(timer, &QTimer::timeout, this, [this, timer, label] {
        ViewportWindow& view = m_window.viewport();
        const render::RenderStats& stats = view.renderStats();
        if (stats.frameCount < m_measureTargetFrame) {
            view.requestUpdate();
            return;
        }
        timer->stop();
        timer->deleteLater();
        std::vector<float> gpu = stats.frameGpuSamplesMs;
        gpu.erase(gpu.begin(), gpu.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(gpu.size(), 4)));
        for (float& sample : gpu) {
            sample /= static_cast<float>(kLoadRepeat);
        }
        note(QStringLiteral("loaded [%1, %2 passes per frame]: %3 detailed + %4 coarse chunks, %5 missing, %6 triangles, %7 draw calls")
                 .arg(label).arg(kLoadRepeat).arg(stats.chunksDrawn).arg(stats.chunksCoarse).arg(stats.chunksMissing)
                 .arg(stats.trianglesSubmitted).arg(stats.drawCalls));
        note(QStringLiteral("    GPU ms per pass: median %1, p95 %2 | frame CPU ms: median %3, p95 %4")
                 .arg(percentile(gpu, 0.5), 0, 'f', 3).arg(percentile(gpu, 0.95), 0, 'f', 3)
                 .arg(percentile(stats.frameCpuSamplesMs, 0.5), 0, 'f', 3).arg(percentile(stats.frameCpuSamplesMs, 0.95), 0, 'f', 3));
        if (stats.coarseBytes > 0) {
            note(QStringLiteral("    coarse: %1 groups, %2 chunks replaced although detailed was ready")
                     .arg(stats.coarseGroups).arg(stats.coarseReplacedDetailed));
        }
        view.setFrameSampling(false);
        view.setDrawRepeat(1);
        resumeSteps();
    });
    timer->start(1);
}

void SelfTest::runCameraMotion()
{
    // Orbit and pan in small steps with normal rendering, so pages have to be
    // brought in and thrown out continuously.
    constexpr int kMotionSteps = 240;
    holdSteps();
    ViewportWindow& viewport = m_window.viewport();
    viewport.setFrameSampling(true);
    m_motionStep = 0;
    auto* timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);
    connect(timer, &QTimer::timeout, this, [this, timer] {
        ViewportWindow& view = m_window.viewport();
        if (++m_motionStep <= kMotionSteps) {
            // A full turn around the model plus a slow vertical sweep.
            view.camera().orbit(785.4f / kMotionSteps, m_motionStep <= kMotionSteps / 2 ? 0.6f : -0.6f);
            view.refresh();
            return;
        }
        timer->stop();
        timer->deleteLater();
        view.setFrameSampling(false);
        const render::RenderStats& stats = view.renderStats();
        std::vector<float> intervals = stats.frameIntervalSamplesMs;
        if (!intervals.empty()) {
            intervals.erase(intervals.begin());
        }
        check(stats.frameCpuSamplesMs.size() >= 20,
              QStringLiteral("frames were rendered during the camera motion (%1)").arg(stats.frameCpuSamplesMs.size()));
        note(QStringLiteral("camera motion: %1 steps, %2 frames").arg(kMotionSteps).arg(stats.frameCpuSamplesMs.size()));
        note(QStringLiteral("  frame interval ms: median %1, p95 %2, max %3")
                 .arg(percentile(intervals, 0.5), 0, 'f', 2).arg(percentile(intervals, 0.95), 0, 'f', 2)
                 .arg(percentile(intervals, 1.0), 0, 'f', 2));
        note(QStringLiteral("  frame CPU ms: median %1, p95 %2, max %3")
                 .arg(percentile(stats.frameCpuSamplesMs, 0.5), 0, 'f', 3).arg(percentile(stats.frameCpuSamplesMs, 0.95), 0, 'f', 3)
                 .arg(percentile(stats.frameCpuSamplesMs, 1.0), 0, 'f', 3));
        note(QStringLiteral("  frame GPU ms: median %1, p95 %2, max %3")
                 .arg(percentile(stats.frameGpuSamplesMs, 0.5), 0, 'f', 3).arg(percentile(stats.frameGpuSamplesMs, 0.95), 0, 'f', 3)
                 .arg(percentile(stats.frameGpuSamplesMs, 1.0), 0, 'f', 3));
        note(QStringLiteral("  uploaded per frame KiB: median %1, p95 %2, max %3; total %4 MiB; evictions %5")
                 .arg(percentile(stats.uploadKiBSamples, 0.5), 0, 'f', 0).arg(percentile(stats.uploadKiBSamples, 0.95), 0, 'f', 0)
                 .arg(percentile(stats.uploadKiBSamples, 1.0), 0, 'f', 0)
                 .arg(std::accumulate(stats.uploadKiBSamples.begin(), stats.uploadKiBSamples.end(), 0.0) / 1024.0, 0, 'f', 1)
                 .arg(std::accumulate(stats.evictionSamples.begin(), stats.evictionSamples.end(), 0u)));
        note(QStringLiteral("  chunks drawn per frame: min %1, median %2, max %3")
                 .arg(percentile(stats.chunksDrawnSamples, 0.0), 0, 'f', 0).arg(percentile(stats.chunksDrawnSamples, 0.5), 0, 'f', 0)
                 .arg(percentile(stats.chunksDrawnSamples, 1.0), 0, 'f', 0));
        note(QStringLiteral("  chunks missing per frame: median %1, p95 %2, max %3, frames with any missing %4 | coarse per frame: median %5, max %6")
                 .arg(percentile(stats.chunksMissingSamples, 0.5), 0, 'f', 0).arg(percentile(stats.chunksMissingSamples, 0.95), 0, 'f', 0)
                 .arg(percentile(stats.chunksMissingSamples, 1.0), 0, 'f', 0)
                 .arg(std::count_if(stats.chunksMissingSamples.begin(), stats.chunksMissingSamples.end(), [](u32 m) { return m > 0; }))
                 .arg(percentile(stats.chunksCoarseSamples, 0.5), 0, 'f', 0).arg(percentile(stats.chunksCoarseSamples, 1.0), 0, 'f', 0));
        resumeSteps();
    });
    timer->start(4);
}

void SelfTest::buildPressureSteps()
{
    // The same script is run with the default pools (reference) and with a
    // small GPU budget; with --selftest-reference the second run compares its
    // captures with the first wherever every visible chunk is drawn.
    auto afterMotion = std::make_shared<QImage>();
    auto coarseBefore = std::make_shared<QImage>();

    m_steps.push_back([this] {
        ViewportWindow& viewport = m_window.viewport();
        viewport.frameMesh();
        const QImage full = capture(QStringLiteral("p01_full.png"));
        compareWithReference(full, QStringLiteral("p01_full.png"));
        noteGpuState(QStringLiteral("full view"));
        const render::RenderStats& stats = viewport.renderStats();
        check(stats.chunksDrawn > 0, QStringLiteral("something is drawn in the full view"));
        check(stats.gpuBudgetBytes == 0 || stats.gpuBytesAllocated <= stats.gpuBudgetBytes + stats.gpuBudgetBytes / 20 + (1u << 20),
              QStringLiteral("GPU allocation stays within the budget"));
        checkVisibleChunksDrawn(QStringLiteral("full view"));
    });
    m_steps.push_back([this] { runLoadedMeasurement(QStringLiteral("full view")); });

    m_steps.push_back([this] {
        ViewportWindow& viewport = m_window.viewport();
        viewport.wheelZoom(7.0f);
        compareWithReference(capture(QStringLiteral("p02_zoom.png")), QStringLiteral("p02_zoom.png"));
        noteGpuState(QStringLiteral("zoomed view"));
        checkVisibleChunksDrawn(QStringLiteral("zoomed view"));
        const auto hit = m_controller.pick(viewport.camera().screenRay(static_cast<float>(viewport.width()) * 0.5f,
                                                                       static_cast<float>(viewport.height()) * 0.5f,
                                                                       static_cast<float>(viewport.width()),
                                                                       static_cast<float>(viewport.height())));
        check(hit.has_value(), QStringLiteral("picking works whatever is resident on the GPU"));
    });
    m_steps.push_back([this] { runLoadedMeasurement(QStringLiteral("zoomed view")); });

    m_steps.push_back([this] { runCameraMotion(); });

    m_steps.push_back([this, afterMotion, coarseBefore] {
        ViewportWindow& viewport = m_window.viewport();
        // The stroke that follows uses a small brush; set it now so the cursor
        // in this capture matches the one in p05 (after undo).
        m_controller.setRadius(0.04f);
        viewport.refresh();
        *afterMotion = capture(QStringLiteral("p03_after_motion.png"));
        compareWithReference(*afterMotion, QStringLiteral("p03_after_motion.png"));
        noteGpuState(QStringLiteral("after camera motion"));
        *coarseBefore = captureAllCoarse(QStringLiteral("p03c_coarse_before_stroke.png"));
        const render::RenderStats& stats = viewport.renderStats();
        checkVisibleChunksDrawn(QStringLiteral("after the motion"));
        // With back-facing chunks skipped, the front-facing set can fit, and then nothing needs evicting.
        if (stats.limitedBudget && stats.vertexSlots < stats.gpuPageCount && stats.backFacingChunks == 0) {
            check(stats.totalEvictions > 0, QStringLiteral("pages were evicted while the camera moved (%1)").arg(stats.totalEvictions));
        }
    });

    // Live sculpting in the zoomed view, then undo.
    m_steps.push_back([this] {
        // The number of dabs depends on timing (the Draw brush raises the
        // surface toward the camera, under the pointer), so the sculpted mesh
        // can differ between runs; p04/p06 are compared within the run only.
        runLiveStroke(false, 0.16, 0.20, QStringLiteral("p04_sculpt.png"));
    });

    m_steps.push_back([this, afterMotion, coarseBefore] {
        noteGpuState(QStringLiteral("after live sculpting"));
        check(m_window.viewport().renderStats().lastStalePages == 0,
              QStringLiteral("every edit reached the GPU copy once the renderer settled (stale pages: %1)")
                  .arg(m_window.viewport().renderStats().lastStalePages));
        const QImage coarseStroke = captureAllCoarse(QStringLiteral("p04c_coarse_after_stroke.png"));
        m_window.undo();
        m_controller.waitUntilIdle();
        const QImage coarseUndo = captureAllCoarse(QStringLiteral("p05c_coarse_after_undo.png"));
        const QImage undone = capture(QStringLiteral("p05_undo.png"));
        check(imageDifference(undone, *afterMotion) < 0.0005, QStringLiteral("undo restores the picture from before the stroke"));
        compareWithReference(undone, QStringLiteral("p05_undo.png"));
        m_window.redo();
        m_controller.waitUntilIdle();
        const QImage redone = capture(QStringLiteral("p06_redo.png"));
        const QImage sculpted(QDir(m_outputDirectory).filePath(QStringLiteral("p04_sculpt.png")));
        check(imageDifference(redone, sculpted) < 0.0005, QStringLiteral("redo restores the sculpted picture"));
        const QImage coarseRedo = captureAllCoarse(QStringLiteral("p06c_coarse_after_redo.png"));
        if (!coarseRedo.isNull() && !coarseBefore->isNull()) {
            // The brush cursor moves between captures, hence the small tolerance
            // (as for the detailed undo/redo checks above); the readback check
            // in captureAllCoarse() is the exact one.
            const double strokeChange = imageDifference(coarseStroke, *coarseBefore);
            const double undoResidual = imageDifference(coarseUndo, *coarseBefore);
            const double redoResidual = imageDifference(coarseRedo, coarseStroke);
            check(strokeChange > 0.0005 && strokeChange > 4.0 * std::max(undoResidual, redoResidual),
                  QStringLiteral("proxies show the stroke (%1 of pixels changed)").arg(strokeChange, 0, 'g', 3));
            check(undoResidual < 0.0005, QStringLiteral("proxies after undo match before the stroke (%1 differ)").arg(undoResidual, 0, 'g', 3));
            check(redoResidual < 0.0005, QStringLiteral("proxies after redo match after the stroke (%1 differ)").arg(redoResidual, 0, 'g', 3));
        }
        checkVisibleChunksDrawn(QStringLiteral("after redo"));
    });

    // Strong scripted deformations, undo and redo, compared with the reference
    // run wherever every visible chunk is drawn (missing chunks are reported
    // separately from image differences).
    // The live stroke above depends on timing: undo it first, so the mesh is
    // the same as in the reference run.
    m_steps.push_back([this] {
        m_window.undo();
        m_controller.waitUntilIdle();
        checkStrongDeformation(QStringLiteral("p08_"), false);
    });

    m_steps.push_back([this] {
        m_window.viewport().frameMesh();
        capture(QStringLiteral("p07_full_again.png"));
        noteGpuState(QStringLiteral("full view again"));
    });
}
void SelfTest::finish()
{
    noteGpuState(QStringLiteral("end"));
    m_report.append(QString());
    m_report.append(QStringLiteral("mesh: %1 triangles, %2 vertices, %3 vertex pages of %4, %5 chunks")
                        .arg(m_controller.mesh().triangleCount())
                        .arg(m_controller.mesh().vertexCount())
                        .arg(m_controller.mesh().vertexPageCount())
                        .arg(m_controller.mesh().layout().vertexPageCapacity())
                        .arg(m_controller.chunkCount()));
    m_report.append(m_failures == 0 ? QStringLiteral("RESULT: PASS") : QStringLiteral("RESULT: FAIL (%1 check(s))").arg(m_failures));

    QFile file(QDir(m_outputDirectory).filePath(QStringLiteral("report.txt")));
    if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QTextStream stream(&file);
        for (const QString& line : std::as_const(m_report)) {
            stream << line << '\n';
        }
    }
    core::logInfo("selftest finished: {}", m_failures == 0 ? "PASS" : "FAIL");
    QCoreApplication::exit(m_failures == 0 ? 0 : 1);
}

} // namespace se::editor
