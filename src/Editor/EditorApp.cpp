#include "Editor/EditorApp.h"

#include "Application/AppController.h"
#include "Core/Log.h"
#include "Editor/MainWindow.h"
#include "Editor/ViewportWindow.h"
#include "Editor/SelfTest.h"
#include "Core/ProcessClock.h"
#include "Geometry/Icosphere.h"
#include "Sculpt/Brushes/SmoothBrush.h"

#include <filesystem>
#include <QApplication>
#include <QColor>
#include <QCommandLineParser>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QPalette>
#include <QStyleFactory>
#include <QVulkanInstance>

#include <algorithm>
#include <bit>
#include <memory>

namespace se::editor {

namespace {

void applyDarkTheme(QApplication& application)
{
    application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(45, 46, 50));
    palette.setColor(QPalette::WindowText, QColor(222, 222, 222));
    palette.setColor(QPalette::Base, QColor(32, 33, 36));
    palette.setColor(QPalette::AlternateBase, QColor(45, 46, 50));
    palette.setColor(QPalette::ToolTipBase, QColor(32, 33, 36));
    palette.setColor(QPalette::ToolTipText, QColor(222, 222, 222));
    palette.setColor(QPalette::Text, QColor(222, 222, 222));
    palette.setColor(QPalette::Button, QColor(58, 59, 64));
    palette.setColor(QPalette::ButtonText, QColor(222, 222, 222));
    palette.setColor(QPalette::Highlight, QColor(214, 138, 60));
    palette.setColor(QPalette::HighlightedText, QColor(20, 20, 20));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 120, 120));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(120, 120, 120));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(120, 120, 120));
    application.setPalette(palette);
}

} // namespace

int runEditor(int argc, char** argv)
{
    const double mainEntryMs = core::processUptimeMs();
    QApplication application(argc, argv);
    application.setProperty("se.mainEntryMs", mainEntryMs);
    QApplication::setApplicationName(QStringLiteral("SculptEngine"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    applyDarkTheme(application);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("SculptEngine - 3D sculpting prototype"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption subdivisionsOption(
        QStringLiteral("subdivisions"),
        QStringLiteral("Icosphere subdivision level: 20 * 4^n triangles (6 = 81,920, 7 = 327,680, 8 = 1,310,720, 10 = 20,971,520, 11 = 83,886,080, the maximum)."),
        QStringLiteral("n"), QStringLiteral("6"));
    const QCommandLineOption validationOption(QStringLiteral("validation"),
                                              QStringLiteral("Enable the Vulkan validation layer if it is installed."));
    const QCommandLineOption selfTestOption(
        QStringLiteral("selftest"),
        QStringLiteral("Run the scripted smoke test, write screenshots and report.txt into <dir>, then exit."),
        QStringLiteral("dir"));
    const QCommandLineOption threadsOption(
        QStringLiteral("threads"),
        QStringLiteral("Helper threads for the sculpt thread (0 = none; default: chosen from the hardware)."),
        QStringLiteral("n"));
    const QCommandLineOption vertexPageOption(
        QStringLiteral("vertex-page"),
        QStringLiteral("Vertices per geometry page; rounded down to a power of two (default 1024)."), QStringLiteral("n"));
    const QCommandLineOption chunkOption(
        QStringLiteral("chunk"),
        QStringLiteral("Triangles per spatial chunk; rounded down to a power of two (default 4096)."), QStringLiteral("n"));
    const QCommandLineOption gpuBudgetOption(
        QStringLiteral("gpu-budget"),
        QStringLiteral("GPU memory for mesh data in MiB; pages are then uploaded on demand and evicted (default: room for the whole mesh)."),
        QStringLiteral("MiB"));
    const QCommandLineOption gpuBudgetFractionOption(
        QStringLiteral("gpu-budget-fraction"),
        QStringLiteral("GPU memory for mesh data as a fraction of one complete copy (e.g. 0.25); see --gpu-budget."),
        QStringLiteral("f"));
    const QCommandLineOption maxUploadOption(
        QStringLiteral("max-upload"),
        QStringLiteral("Most MiB of mesh data uploaded per frame (default 32)."), QStringLiteral("MiB"));
    const QCommandLineOption cullOption(
        QStringLiteral("cull"), QStringLiteral("Chunk culling: on (default: view frustum + back-facing chunks), frustum (view frustum only), off, unmerged (on, one draw per chunk)."), QStringLiteral("mode"));
    const QCommandLineOption normalConesOption(
        QStringLiteral("normal-cones"),
        QStringLiteral("Keep per-chunk normal cones in the sculpt thread's BVH refit (on, default) or not (off); without them no chunk is rejected as back-facing."),
        QStringLiteral("on|off"));
    const QCommandLineOption gatherOption(
        QStringLiteral("gather"),
        QStringLiteral("Gather each dab's vertices on the helper threads (parallel, default) or on the sculpt thread (serial). Same results bit for bit."),
        QStringLiteral("parallel|serial"));
    const QCommandLineOption regionOption(
        QStringLiteral("region"),
        QStringLiteral("Edit region walk of each dab, undo and redo: on the helper threads for 16K moved vertices or more (parallel, default), or on the sculpt thread (serial). Same results bit for bit."),
        QStringLiteral("parallel|serial"));
    const QCommandLineOption smoothBinOption(
        QStringLiteral("smooth-binning"),
        QStringLiteral("Smooth's kernel grid for dabs of 32K vertices or more: binned on the helper threads, each cell summed in list order (parallel, default), or on the sculpt thread (serial). Same results bit for bit."),
        QStringLiteral("parallel|serial"));
    const QCommandLineOption smoothKernelOption(
        QStringLiteral("smooth-kernel"),
        QStringLiteral("Blend of Smooth's dense-mode kernel cells: quadratic B-spline over 3x3x3 cells (quadratic, default) or the former trilinear blend over 2x2x2 cells (trilinear), which leaves creases along the cell planes."),
        QStringLiteral("quadratic|trilinear"));
    const QCommandLineOption smoothDenseOption(
        QStringLiteral("smooth-dense"),
        QStringLiteral("Average vertices per kernel cell from which a Smooth dab uses the dense-mode filter instead of Laplacian passes (default 4)."),
        QStringLiteral("n"));
    const QCommandLineOption sphereCacheOption(
        QStringLiteral("sphere-cache"),
        QStringLiteral("Directory of the pristine-sphere cache: load the sphere for --subdivisions from it if a valid file exists, else generate it and write the file there. Off by default. Same mesh bit for bit."),
        QStringLiteral("dir"));
    const QCommandLineOption moveUpdatesOption(
        QStringLiteral("move-updates"),
        QStringLiteral("Move updates queued behind the one being applied: apply only the newest (latest, default) or every one (all). Same final mesh, undo record and BVH; only intermediate frames are skipped."),
        QStringLiteral("all|latest"));
    const QCommandLineOption prepareOption(
        QStringLiteral("dab-prepare"),
        QStringLiteral("Undo record, average normal and page staging of each dab of 16K vertices or more: at the same time on the helper threads (overlap, default), or one after the other (serial). Same results bit for bit."),
        QStringLiteral("overlap|serial"));
    const QCommandLineOption regionOverlapOption(
        QStringLiteral("region-overlap"),
        QStringLiteral("With --dab-prepare overlap: also walk each dab's edit region and queue its BVH leaves during the preparation (on), or after the brush (off, default). Same results bit for bit."),
        QStringLiteral("on|off"));
    const QCommandLineOption frameOnCommitOption(
        QStringLiteral("frame-on-commit"),
        QStringLiteral("Render a frame as soon as an edit is published (on), or request one and let Qt deliver the request from its timer (off, default)."),
        QStringLiteral("on|off"));
    const QCommandLineOption markingOption(
        QStringLiteral("marking"),
        QStringLiteral("Queue each dab's BVH leaves on the helper threads, one chunk per task (parallel, default), or on the sculpt thread (serial). Same results bit for bit."),
        QStringLiteral("parallel|serial"));
    const QCommandLineOption historyNormalsOption(
        QStringLiteral("history-normals"),
        QStringLiteral("Undo and redo compute the new normals before taking the document lock (before) or under it (under). Same results bit for bit."),
        QStringLiteral("before|under"));
    const QCommandLineOption boxRefitOption(
        QStringLiteral("box-refit"),
        QStringLiteral("BVH box refit: a chunk whose refit touches at least half of its leaves sweeps its whole tree in node order (dense, default), or always only the touched nodes level by level (incremental). Same boxes bit for bit."),
        QStringLiteral("dense|incremental"));
    const QCommandLineOption conesOption(
        QStringLiteral("cones"),
        QStringLiteral("Normal cones after an edit: widen every touched chunk's cone by the changed normals when at most half of the chunk changed and rebuild it exactly in the background (incremental, default), or recompute it (exact). Culling stays conservative either way."),
        QStringLiteral("exact|incremental"));
    const QCommandLineOption coneNormalsOption(
        QStringLiteral("cone-normals"),
        QStringLiteral("Exact normal cones: reuse the first pass's face normals from a per-thread scratch in the second pass (reuse, default), or recompute every face normal (recompute). Same cones bit for bit."),
        QStringLiteral("recompute|reuse"));
    const QCommandLineOption scenarioOption(
        QStringLiteral("selftest-scenario"),
        QStringLiteral("Self-test script: standard (default), pressure, history (undo/redo overlapping rendering) move (live Move strokes at 500 inputs per second) smooth (Smooth shading close-ups, data dumps) latency (120 Hz Draw and Smooth strokes traced from input to commit to the frame that took the edit) smoothfeel (Smooth along half of a bumpy Draw ridge, grazing-light close-ups) or startup (launch to ready-to-sculpt timings and the startup mesh hash, then exit)."), QStringLiteral("name"));
    const QCommandLineOption adjacencyOption(
        QStringLiteral("adjacency"),
        QStringLiteral("Storage of the mesh adjacency (neighbours, incident triangles): compact (16-bit offsets, about 58% of the wide size, default) or wide (u32 ids)."),
        QStringLiteral("wide|compact"));
    parser.addOption(adjacencyOption);
    const QCommandLineOption neighborsOption(
        QStringLiteral("neighbors"),
        QStringLiteral("Vertex neighbour lists: stored (a table built at startup, default) or derived (from the incident triangles when read; saves the table, same results)."),
        QStringLiteral("stored|derived"));
    parser.addOption(neighborsOption);
    parser.addOption(gpuBudgetOption);
    parser.addOption(gpuBudgetFractionOption);
    parser.addOption(maxUploadOption);
    parser.addOption(cullOption);
    parser.addOption(normalConesOption);
    parser.addOption(gatherOption);
    parser.addOption(markingOption);
    parser.addOption(regionOption);
    parser.addOption(moveUpdatesOption);
    parser.addOption(prepareOption);
    parser.addOption(regionOverlapOption);
    parser.addOption(frameOnCommitOption);
    parser.addOption(smoothBinOption);
    parser.addOption(smoothKernelOption);
    parser.addOption(smoothDenseOption);
    parser.addOption(sphereCacheOption);
    parser.addOption(historyNormalsOption);
    parser.addOption(boxRefitOption);
    parser.addOption(conesOption);
    parser.addOption(coneNormalsOption);
    const QCommandLineOption lodOption(
        QStringLiteral("lod"),
        QStringLiteral("Draw a coarse, crack-free stand-in for visible chunks whose detailed pages are not on the GPU (opt-in; allocated only with a budget too small for the whole mesh, and its memory comes out of that budget)."));
    parser.addOption(lodOption);
    const QCommandLineOption lodAlwaysOption(
        QStringLiteral("lod-always"),
        QStringLiteral("Testing: with --lod, allocate the coarse fallback even when the pools hold the whole mesh (seam checks)."));
    parser.addOption(lodAlwaysOption);
    const QCommandLineOption lodGroupOption(
        QStringLiteral("lod-group-chunks"),
        QStringLiteral("With --lod: chunks simplified and drawn coarse together, 1 (default), 2, 4 or 8."), QStringLiteral("n"));
    parser.addOption(lodGroupOption);
    const QCommandLineOption lodRatioOption(
        QStringLiteral("lod-ratio"),
        QStringLiteral("With --lod: fraction of the triangles the coarse stand-ins keep (default 1/16 = 0.0625)."), QStringLiteral("r"));
    parser.addOption(lodRatioOption);
    const QCommandLineOption lodDrawsOption(
        QStringLiteral("lod-draws"),
        QStringLiteral("With --lod: 'indirect' (default; one multi-draw-indirect call per pass when supported) or 'direct' (one draw per group)."),
        QStringLiteral("mode"));
    parser.addOption(lodDrawsOption);
    const QCommandLineOption referenceOption(
        QStringLiteral("selftest-reference"),
        QStringLiteral("Self-test: compare deterministic captures with those of an earlier run in <dir>."), QStringLiteral("dir"));
    parser.addOption(scenarioOption);
    parser.addOption(referenceOption);
    parser.addOption(vertexPageOption);
    parser.addOption(chunkOption);
    parser.addOption(threadsOption);
    parser.addOption(subdivisionsOption);
    parser.addOption(validationOption);
    parser.addOption(selfTestOption);
    parser.process(application);

    app::AppConfig config;
    bool subdivisionsValid = false;
    const int requested = parser.value(subdivisionsOption).toInt(&subdivisionsValid);
    if (subdivisionsValid) {
        config.sphereSubdivisions = std::clamp(requested, 0, geo::kMaxIcosphereSubdivisions);
    } else {
        core::logWarning("Ignoring invalid --subdivisions value; using {}", config.sphereSubdivisions);
    }

    if (parser.isSet(sphereCacheOption)) {
        config.sphereCacheDirectory = std::filesystem::path(parser.value(sphereCacheOption).toStdWString());
    }
    if (parser.isSet(threadsOption)) {
        bool threadsValid = false;
        const int threads = parser.value(threadsOption).toInt(&threadsValid);
        if (threadsValid && threads >= 0) {
            config.jobThreads = threads;
        }
    }

    // Sizes are given as counts and stored as powers of two.
    const auto shiftFromOption = [&parser](const QCommandLineOption& option, u32 minimum, u32 maximum, u32 fallback) {
        bool valid = false;
        const uint value = parser.value(option).toUInt(&valid);
        if (!parser.isSet(option) || !valid || value == 0) {
            return fallback;
        }
        return std::clamp(static_cast<u32>(std::bit_width(value) - 1), minimum, maximum);
    };
    config.meshLayout.vertexPageShift = shiftFromOption(vertexPageOption, 6, 20, config.meshLayout.vertexPageShift);
    config.chunkTriangleShift = shiftFromOption(chunkOption, 6, 24, config.chunkTriangleShift);
    if (parser.isSet(adjacencyOption)) {
        const QString value = parser.value(adjacencyOption).toLower();
        if (value == QStringLiteral("compact")) {
            config.meshLayout.adjacency = geo::AdjacencyLayout::Compact;
        } else if (value == QStringLiteral("wide")) {
            config.meshLayout.adjacency = geo::AdjacencyLayout::Wide;
        } else {
            core::logWarning("Ignoring invalid --adjacency value");
        }
    }
    if (parser.isSet(neighborsOption)) {
        const QString value = parser.value(neighborsOption).toLower();
        if (value == QStringLiteral("derived")) {
            config.meshLayout.neighbors = geo::NeighborStorage::Derived;
        } else if (value == QStringLiteral("stored")) {
            config.meshLayout.neighbors = geo::NeighborStorage::Stored;
        } else {
            core::logWarning("Ignoring invalid --neighbors value");
        }
    }
    if (parser.isSet(gatherOption)) {
        const QString value = parser.value(gatherOption).toLower();
        if (value == QStringLiteral("parallel")) {
            config.parallelGather = true;
        } else if (value == QStringLiteral("serial")) {
            config.parallelGather = false;
        } else {
            core::logWarning("Ignoring invalid --gather value");
        }
    }
    if (parser.isSet(regionOption)) {
        const QString value = parser.value(regionOption).toLower();
        if (value == QStringLiteral("parallel")) {
            config.parallelRegion = true;
        } else if (value == QStringLiteral("serial")) {
            config.parallelRegion = false;
        } else {
            core::logWarning("Ignoring invalid --region value");
        }
    }
    if (parser.isSet(smoothBinOption)) {
        const QString value = parser.value(smoothBinOption).toLower();
        if (value == QStringLiteral("parallel")) {
            sculpt::SmoothBrush::setParallelBinning(true);
        } else if (value == QStringLiteral("serial")) {
            sculpt::SmoothBrush::setParallelBinning(false);
        } else {
            core::logWarning("Ignoring invalid --smooth-binning value");
        }
    }
    if (parser.isSet(smoothKernelOption)) {
        const QString value = parser.value(smoothKernelOption).toLower();
        if (value == QStringLiteral("quadratic")) {
            sculpt::SmoothBrush::setQuadraticKernel(true);
        } else if (value == QStringLiteral("trilinear")) {
            sculpt::SmoothBrush::setQuadraticKernel(false);
        } else {
            core::logWarning("Ignoring invalid --smooth-kernel value");
        }
    }
    if (parser.isSet(smoothDenseOption)) {
        bool valid = false;
        const float value = parser.value(smoothDenseOption).toFloat(&valid);
        if (valid && value > 0.0f) {
            sculpt::SmoothBrush::setDenseThreshold(value);
        } else {
            core::logWarning("Ignoring invalid --smooth-dense value");
        }
    }
    if (parser.isSet(moveUpdatesOption)) {
        const QString value = parser.value(moveUpdatesOption).toLower();
        if (value == QStringLiteral("latest")) {
            config.coalesceMoveUpdates = true;
        } else if (value == QStringLiteral("all")) {
            config.coalesceMoveUpdates = false;
        } else {
            core::logWarning("Ignoring invalid --move-updates value");
        }
    }
    if (parser.isSet(prepareOption)) {
        const QString value = parser.value(prepareOption).toLower();
        if (value == QStringLiteral("overlap")) {
            config.overlapPrepare = true;
        } else if (value == QStringLiteral("serial")) {
            config.overlapPrepare = false;
        } else {
            core::logWarning("Ignoring invalid --dab-prepare value");
        }
    }
    if (parser.isSet(regionOverlapOption)) {
        const QString value = parser.value(regionOverlapOption).toLower();
        if (value == QStringLiteral("on")) {
            config.overlapRegion = true;
        } else if (value == QStringLiteral("off")) {
            config.overlapRegion = false;
        } else {
            core::logWarning("Ignoring invalid --region-overlap value");
        }
    }
    if (parser.isSet(markingOption)) {
        const QString value = parser.value(markingOption).toLower();
        if (value == QStringLiteral("parallel")) {
            config.parallelMarking = true;
        } else if (value == QStringLiteral("serial")) {
            config.parallelMarking = false;
        } else {
            core::logWarning("Ignoring invalid --marking value");
        }
    }
    if (parser.isSet(historyNormalsOption)) {
        const QString value = parser.value(historyNormalsOption).toLower();
        if (value == QStringLiteral("before")) {
            config.historyNormalsBeforeLock = true;
        } else if (value == QStringLiteral("under")) {
            config.historyNormalsBeforeLock = false;
        } else {
            core::logWarning("Ignoring invalid --history-normals value");
        }
    }
    if (parser.isSet(boxRefitOption)) {
        const QString value = parser.value(boxRefitOption).toLower();
        if (value == QStringLiteral("dense")) {
            config.denseBoxRefit = true;
        } else if (value == QStringLiteral("incremental")) {
            config.denseBoxRefit = false;
        } else {
            core::logWarning("Ignoring invalid --box-refit value");
        }
    }
    if (parser.isSet(coneNormalsOption)) {
        const QString value = parser.value(coneNormalsOption).toLower();
        if (value == QStringLiteral("reuse")) {
            config.reuseConeNormals = true;
        } else if (value == QStringLiteral("recompute")) {
            config.reuseConeNormals = false;
        } else {
            core::logWarning("Ignoring invalid --cone-normals value");
        }
    }
    if (parser.isSet(conesOption)) {
        const QString value = parser.value(conesOption).toLower();
        if (value == QStringLiteral("incremental")) {
            config.incrementalCones = true;
        } else if (value == QStringLiteral("exact")) {
            config.incrementalCones = false;
        } else {
            core::logWarning("Ignoring invalid --cones value");
        }
    }
    if (parser.isSet(normalConesOption)) {
        const QString value = parser.value(normalConesOption).toLower();
        if (value == QStringLiteral("off")) {
            config.normalCones = false;
        } else if (value != QStringLiteral("on")) {
            core::logWarning("Ignoring invalid --normal-cones value");
        }
    }

    render::RenderSettings renderSettings;
    if (parser.isSet(gpuBudgetOption)) {
        bool valid = false;
        const double mib = parser.value(gpuBudgetOption).toDouble(&valid);
        if (valid && mib > 0.0) {
            renderSettings.gpuBudgetBytes = static_cast<u64>(mib * 1024.0 * 1024.0);
        } else {
            core::logWarning("Ignoring invalid --gpu-budget value");
        }
    }
    if (parser.isSet(gpuBudgetFractionOption)) {
        bool valid = false;
        const double fraction = parser.value(gpuBudgetFractionOption).toDouble(&valid);
        if (valid && fraction > 0.0) {
            renderSettings.gpuBudgetFraction = fraction;
        } else {
            core::logWarning("Ignoring invalid --gpu-budget-fraction value");
        }
    }
    if (parser.isSet(maxUploadOption)) {
        bool valid = false;
        const double mib = parser.value(maxUploadOption).toDouble(&valid);
        if (valid && mib > 0.0) {
            renderSettings.maxUploadBytesPerFrame = static_cast<u64>(mib * 1024.0 * 1024.0);
        }
    }
    renderSettings.coarseFallback = parser.isSet(lodOption);
    renderSettings.coarseAlways = parser.isSet(lodAlwaysOption);
    if (parser.isSet(lodGroupOption)) {
        const int chunks = parser.value(lodGroupOption).toInt();
        if (chunks == 1 || chunks == 2 || chunks == 4 || chunks == 8) {
            renderSettings.coarseGroupShift = static_cast<u32>(std::countr_zero(static_cast<unsigned>(chunks)));
        } else {
            core::logWarning("ignoring --lod-group-chunks {} (expected 1, 2, 4 or 8)", chunks);
        }
    }
    if (parser.isSet(lodRatioOption)) {
        bool valid = false;
        const double ratio = parser.value(lodRatioOption).toDouble(&valid);
        if (valid && ratio > 0.0 && ratio <= 1.0) {
            renderSettings.coarseRatio = static_cast<float>(ratio);
        } else {
            core::logWarning("ignoring --lod-ratio {}", parser.value(lodRatioOption).toStdString());
        }
    }
    if (parser.isSet(lodDrawsOption)) {
        renderSettings.coarseIndirectDraws = parser.value(lodDrawsOption).toLower() != QStringLiteral("direct");
    }
    if (parser.isSet(cullOption)) {
        const QString mode = parser.value(cullOption).toLower();
        if (mode == QStringLiteral("off")) {
            renderSettings.cullMode = render::CullMode::Off;
        } else if (mode == QStringLiteral("unmerged")) {
            renderSettings.cullMode = render::CullMode::OnUnmerged;
        } else if (mode == QStringLiteral("frustum")) {
            renderSettings.cullMode = render::CullMode::Frustum;
        } else if (mode != QStringLiteral("on")) {
            core::logWarning("Ignoring invalid --cull value");
        }
    }
    SelfTest::Scenario scenario = SelfTest::Scenario::Standard;
    if (parser.value(scenarioOption).toLower() == QStringLiteral("pressure")) {
        scenario = SelfTest::Scenario::Pressure;
    } else if (parser.value(scenarioOption).toLower() == QStringLiteral("history")) {
        scenario = SelfTest::Scenario::History;
    } else if (parser.value(scenarioOption).toLower() == QStringLiteral("move")) {
        scenario = SelfTest::Scenario::Move;
    } else if (parser.value(scenarioOption).toLower() == QStringLiteral("smooth")) {
        scenario = SelfTest::Scenario::Smooth;
    } else if (parser.value(scenarioOption).toLower() == QStringLiteral("latency")) {
        scenario = SelfTest::Scenario::Latency;
    } else if (parser.value(scenarioOption).toLower() == QStringLiteral("smoothfeel")) {
        scenario = SelfTest::Scenario::SmoothFeel;
    } else if (parser.value(scenarioOption).toLower() == QStringLiteral("startup")) {
        scenario = SelfTest::Scenario::Startup;
    }

    QVulkanInstance vulkanInstance;
    if (parser.isSet(validationOption)) {
        QLoggingCategory::setFilterRules(QStringLiteral("qt.vulkan=true"));
        vulkanInstance.setLayers({"VK_LAYER_KHRONOS_validation"});
    }
    if (!vulkanInstance.create()) {
        const QString message = QStringLiteral("Could not create a Vulkan instance (VkResult %1).\n"
                                               "SculptEngine needs a GPU driver with Vulkan support.")
                                    .arg(static_cast<int>(vulkanInstance.errorCode()));
        core::logError("{}", message.toStdString());
        QMessageBox::critical(nullptr, QStringLiteral("SculptEngine"), message);
        return 1;
    }
    if (parser.isSet(validationOption) && !vulkanInstance.layers().contains("VK_LAYER_KHRONOS_validation")) {
        core::logWarning("VK_LAYER_KHRONOS_validation is not installed; running without validation");
    }

    // Declaration order is the lifetime contract: the controller (mesh) and the
    // Vulkan instance outlive the window, whose renderer references both.
    app::AppController controller(config);
    MainWindow window(controller, vulkanInstance, renderSettings);
    if (parser.isSet(frameOnCommitOption)) {
        const QString value = parser.value(frameOnCommitOption).toLower();
        if (value == QStringLiteral("on") || value == QStringLiteral("off")) {
            window.viewport().setFrameOnEngineChange(value == QStringLiteral("on"));
        } else {
            core::logWarning("Ignoring invalid --frame-on-commit value");
        }
    }
    window.resize(1280, 800);
    window.show();

    std::unique_ptr<SelfTest> selfTest;
    if (parser.isSet(selfTestOption)) {
        selfTest = std::make_unique<SelfTest>(window, controller, parser.value(selfTestOption), scenario,
                                              parser.value(referenceOption));
        selfTest->start();
    }

    return QApplication::exec();
}

} // namespace se::editor
