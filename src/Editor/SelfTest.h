#pragma once

#include "Application/AppController.h"

#include <QObject>
#include <QPointF>
#include <QString>
#include <QStringList>

#include <glm/vec3.hpp>

#include <functional>
#include <utility>
#include <vector>

class QImage;

namespace se::editor {

class MainWindow;

/// Scripted end-to-end smoke test (`--selftest <dir>`).
///
/// Drives the real window through the same pointer entry points as the mouse:
/// picks the surface, sculpts with each brush, undoes and redoes, moves the
/// camera, and after each step grabs the rendered frame from the swapchain.
/// Writes PNG screenshots and report.txt into the output directory and exits
/// with code 0 when every check passed.
///
/// This verifies that the Vulkan path actually renders and that edits reach
/// the screen; it complements, not replaces, the headless unit tests.
/// Fraction of pixels that differ noticeably between two images (1 if the sizes differ).
[[nodiscard]] double imageDifference(const QImage& a, const QImage& b);

class SelfTest final : public QObject {
    Q_OBJECT

public:
    enum class Scenario {
        /// Functional checks: sculpting, undo, camera, input events, culling
        /// equivalence, render measurements.
        Standard,
        /// A fixed sequence of views, camera motion and live sculpting meant to
        /// be run under a small GPU memory budget, and again with the default pools to
        /// produce the reference images.
        Pressure,
        /// Repeated undo and redo of typical and strong strokes while frames
        /// render back to back, so that frames overlap the commands' lock
        /// holds: the renderer's lock wait, command latency and scratch.
        History,
        /// Live Move strokes at 500 inputs per second (small to large radius,
        /// with a reversal), while frames render normally: queue growth, drain
        /// after release, dropped inputs (AppConfig::coalesceMoveUpdates),
        /// renderer lock wait; then undo all and redo all. Notes a hash of the
        /// mesh after every stroke: Move depends only on the last input, so it
        /// must be the same with --move-updates all and latest.
        Move,
        /// Smooth shading check: fixed close-up cameras captured before and
        /// after repeatable Smooth strokes on the plain sphere, then with every
        /// normal recomputed by the whole-mesh reference, then after undo and
        /// redo. Writes the positions before and after, the reference and
        /// incremental normals and the triangle indices (s_*.bin) for analysis.
        Smooth,
        /// Latency trace: one Draw and one Smooth stroke (radius 0.25, a
        /// circle over the front of the model) at 120 scripted inputs per
        /// second through the real viewport. Every dab-producing input is linked
        /// to its dab's commit and to the first frame that took that commit's
        /// updates; writes trace_<brush>.csv.
        Latency,
        /// Smooth on a sculpted surface: a broad Draw ridge with small bumps on
        /// its crest (world-space rays, fixed radius and strength, so the same
        /// stroke at every mesh level), then Smooth along one half of it.
        /// Captures fixed close-ups under grazing light before, after one Smooth
        /// pass and after five, and notes a crest height profile.
        SmoothFeel,
        /// Startup timings: process launch to main(), controller ready (sphere
        /// generated or loaded from --sphere-cache, mesh built, BVH built),
        /// first frame submitted; the sphere cache's stages and the startup
        /// mesh hash. Then exits.
        Startup,
    };

    SelfTest(MainWindow& window, app::AppController& controller, const QString& outputDirectory,
             Scenario scenario = Scenario::Standard, const QString& referenceDirectory = {});

    /// Schedules the script; returns immediately.
    void start();

private:
    void runNextStep();
    void finish();
    /// A step that continues asynchronously calls holdSteps() and, when it is
    /// done, resumeSteps().
    void holdSteps() { m_holding = true; }
    void resumeSteps();
    /// Draws a closed loop at interactive pace while frames render normally.
    void runLiveStroke(bool reframe, double radiusX, double radiusY, const QString& captureName);
    void buildStandardSteps();
    void buildPressureSteps();
    /// Compares the three culling modes in the current view. Returns the
    /// triangles submitted with culling off and on.
    std::pair<u32, u32> compareCullModes(const QString& label);
    /// Strong strokes (large Draw, deep inverted Draw, long Move pulls, also on
    /// the far side), then undo and redo them all. After each stage: the
    /// published normal cones must hold every triangle normal, and the model is
    /// captured from four views. With `compareModes` every view also checks that
    /// culling off / frustum / on give identical images. Captures are named
    /// `<prefix><stage>_<view>.png`; the edits are undone again at the end.
    void checkStrongDeformation(const QString& prefix, bool compareModes);
    /// The strong strokes of checkStrongDeformation (front, then back; the
    /// camera ends orbited by 180 degrees). Returns the number of strokes and
    /// adds their sculpt-thread costs to the arguments.
    int applyStrongStrokes(std::vector<float>& inputs, double& bvhMs, u32& dabs, u64& coneTriangles);
    void buildHistorySteps();
    void buildMoveSteps();
    void buildSmoothSteps();
    void buildSmoothFeelSteps();
    void buildStartupSteps();
    void buildLatencySteps();
    /// One traced stroke of the Latency scenario; undone afterwards.
    void runLatencyTrace(sculpt::BrushId brush, const QString& name);
    /// Writes `count` floats (or u32s) to <output>/<name>, prefixed by the count.
    void writeBinary(const QString& name, const void* data, std::size_t bytes) const;
    /// One live Move stroke: `samples` inputs every 2 ms from `from` to `to`,
    /// back to their midpoint for the last 40%. Appends the mesh hash after it.
    void runLiveMove(float radius, const QPointF& from, const QPointF& to, int samples, const QString& label,
                     std::shared_ptr<std::vector<u64>> hashes);
    /// FNV-1a hash of every position and normal (waits until idle).
    [[nodiscard]] u64 meshHash() const;
    /// Undoes `steps` strokes and redoes them, `cycles` times, each command
    /// queued as soon as the previous one is published, while a timer requests
    /// a frame every millisecond. Notes the renderer's lock waits, the
    /// commands' hold and total, and the scratch; the mesh must end where it began.
    void runOverlappedHistory(const QString& label, int steps, int cycles);
    /// Notes the sculpt-thread cost of the current/last stroke.
    void noteSculptCost(const QString& label, const std::vector<float>& inputSamplesMs, double bvhMs, u32 dabs, u64 coneTriangles);
    void runRenderMeasurements();
    void runCameraMotion();
    /// Draws the current view 64 times per frame for a batch of presented
    /// frames and notes GPU time per pass (comparable across runs) and CPU time.
    void runLoadedMeasurement(const QString& label);
    /// Renders (without presenting) until the renderer has no deferred uploads left.
    void settle();
    void noteGpuState(const QString& label);
    /// Every visible chunk must be drawn if the view fits the GPU pools;
    /// otherwise the nearest ones that fit are drawn.
    void checkVisibleChunksDrawn(const QString& label);
    /// With a reference directory (an earlier run, e.g. with the default pools), the
    /// image must equal the file of the same name there, provided every
    /// visible chunk was drawn.
    void compareWithReference(const QImage& image, const QString& fileName);
    /// PROTOTYPE coarse fallback: draws the current view all detailed, with odd
    /// chunks coarse and with every chunk coarse, and counts hole pixels.
    void checkCoarseSeams(const QString& label);
    /// PROTOTYPE: captures the view with every chunk coarse and checks that the
    /// proxies on the GPU equal the current CPU mesh. Returns a null image when
    /// the coarse fallback is off.
    QImage captureAllCoarse(const QString& fileName);

    void check(bool condition, const QString& description);
    void note(const QString& text);
    QImage capture(const QString& fileName);
    void stroke(const QPointF& from, const QPointF& to, int samples, Qt::KeyboardModifiers modifiers = {});
    [[nodiscard]] QPointF viewportPoint(double xFraction, double yFraction) const;
    [[nodiscard]] std::vector<glm::vec3> positions() const;

    MainWindow& m_window;
    app::AppController& m_controller;
    QString m_outputDirectory;
    QString m_referenceDirectory;

    std::vector<std::function<void()>> m_steps;
    std::size_t m_nextStep = 0;
    QStringList m_report;
    int m_failures = 0;
    int m_waitedForExposeMs = 0;
    bool m_holding = false;
    int m_liveSample = 0;
    Scenario m_scenario = Scenario::Standard;
    int m_measureIndex = -1;
    u64 m_measureTargetFrame = 0;
    int m_motionStep = 0;
};

} // namespace se::editor
