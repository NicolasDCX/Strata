#include "Sculpt/Brushes/SmoothBrush.h"

#include "Core/Timer.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace se::sculpt {

namespace {

SmoothBrush::Steps g_steps;
std::atomic<bool> g_parallelBinning{true};
std::atomic<bool> g_quadraticKernel{true};

/// Uniform grid over the brush sphere used by the dense-mode filter.
/// Storage lives in DabScratch: vectors[cell] = sum of positions,
/// vectors[cellCount + cell] = sum of normals, scalars[cell] = vertex count.
struct KernelGrid {
    glm::vec3 origin{0.0f};
    float inverseCellSize = 0.0f;
    int size = 0; // cells per axis

    [[nodiscard]] int cellCount() const noexcept { return size * size * size; }
    [[nodiscard]] int index(int x, int y, int z) const noexcept { return (z * size + y) * size + x; }
    [[nodiscard]] int clampCoordinate(int value) const noexcept { return std::clamp(value, 0, size - 1); }
    [[nodiscard]] u32 cellOf(const glm::vec3& position) const noexcept
    {
        const glm::vec3 local = (position - origin) * inverseCellSize;
        return static_cast<u32>(index(clampCoordinate(static_cast<int>(local.x)), clampCoordinate(static_cast<int>(local.y)),
                                      clampCoordinate(static_cast<int>(local.z))));
    }
};

/// Parallel binning with the serial loop's sums. Pass 1, per block of
/// vertices: each vertex's cell and position, and the block's cell histogram.
/// Pass 2: the cells are cut into contiguous ranges of about equal vertex
/// count, one task each; a task walks the blocks in order, skips those with no
/// vertex in its range, and adds its cells' vertices in list order. Every cell
/// therefore sums the same values in the same order as the serial loop.
void binParallel(const DabContext& ctx, const KernelGrid& grid, std::size_t cells)
{
    const std::size_t count = ctx.vertices.size();
    const std::size_t blocks = (count + SmoothBrush::kBinBlock - 1) / SmoothBrush::kBinBlock;
    std::vector<glm::vec3>& points = ctx.scratch.points;
    std::vector<u32>& cellOf = ctx.scratch.indices;
    std::vector<u32>& histogram = ctx.scratch.counts; // blocks x cells
    points.resize(count);
    cellOf.resize(count);
    histogram.assign(blocks * cells, 0u);
    core::parallelFor(ctx.jobs, blocks, 1, [&](std::size_t blockBegin, std::size_t blockEnd) {
        for (std::size_t b = blockBegin; b < blockEnd; ++b) {
            u32* blockCounts = histogram.data() + b * cells;
            const std::size_t end = std::min(count, (b + 1) * SmoothBrush::kBinBlock);
            for (std::size_t i = b * SmoothBrush::kBinBlock; i < end; ++i) {
                const glm::vec3 position = ctx.positions[ctx.vertices[i]];
                const u32 cell = grid.cellOf(position);
                points[i] = position;
                cellOf[i] = cell;
                ++blockCounts[cell];
            }
        }
    });

    // Cut the cells into ranges of about count / tasks vertices each.
    constexpr std::size_t kMaxTasks = 64;
    const std::size_t tasks =
        std::min({cells, kMaxTasks, static_cast<std::size_t>(ctx.jobs->workerCount()) + 1});
    u32 bounds[kMaxTasks + 1];
    std::size_t boundCount = 1;
    bounds[0] = 0;
    std::size_t accumulated = 0;
    for (std::size_t cell = 0; cell < cells && boundCount < tasks; ++cell) {
        for (std::size_t b = 0; b < blocks; ++b) {
            accumulated += histogram[b * cells + cell];
        }
        if (accumulated * tasks >= count * boundCount) {
            bounds[boundCount++] = static_cast<u32>(cell + 1);
        }
    }
    while (boundCount < tasks + 1) {
        bounds[boundCount++] = static_cast<u32>(cells);
    }
    bounds[tasks] = static_cast<u32>(cells);

    const auto normals = ctx.mesh.normals();
    glm::vec3* positionSums = ctx.scratch.vectors.data();
    glm::vec3* normalSums = ctx.scratch.vectors.data() + cells;
    float* counts = ctx.scratch.scalars.data();
    core::parallelFor(ctx.jobs, tasks, 1, [&](std::size_t taskBegin, std::size_t taskEnd) {
        for (std::size_t task = taskBegin; task < taskEnd; ++task) {
            const u32 first = bounds[task];
            const u32 last = bounds[task + 1];
            if (first == last) {
                continue;
            }
            for (std::size_t b = 0; b < blocks; ++b) {
                const u32* blockCounts = histogram.data() + b * cells;
                bool any = false;
                for (u32 cell = first; cell < last && !any; ++cell) {
                    any = blockCounts[cell] != 0;
                }
                if (!any) {
                    continue;
                }
                const std::size_t end = std::min(count, (b + 1) * SmoothBrush::kBinBlock);
                for (std::size_t i = b * SmoothBrush::kBinBlock; i < end; ++i) {
                    const u32 cell = cellOf[i];
                    if (cell < first || cell >= last) {
                        continue;
                    }
                    positionSums[cell] += points[i];
                    normalSums[cell] += normals[ctx.vertices[i]];
                    counts[cell] += 1.0f;
                }
            }
        }
    });
}

/// Bins the affected vertices. Returns the average number of vertices per occupied cell.
float buildKernelGrid(const DabContext& ctx, KernelGrid& grid)
{
    const float cellSize = ctx.radius * SmoothBrush::kKernelFraction;
    // One cell of margin on each side so that the 3x3x3 neighbourhood of every
    // vertex inside the brush sphere exists.
    grid.origin = ctx.center - glm::vec3(ctx.radius + cellSize);
    grid.inverseCellSize = 1.0f / cellSize;
    grid.size = static_cast<int>(std::ceil(2.0f * (ctx.radius + cellSize) * grid.inverseCellSize)) + 1;

    const std::size_t cells = static_cast<std::size_t>(grid.cellCount());
    core::Stopwatch step;
    ctx.scratch.vectors.assign(cells * 2, glm::vec3(0.0f));
    ctx.scratch.scalars.assign(cells, 0.0f);
    g_steps.ms[0] += step.elapsedMs();
    step.reset();

    if (g_parallelBinning.load(std::memory_order_relaxed) && ctx.jobs != nullptr && ctx.jobs->workerCount() > 0 &&
        ctx.vertices.size() >= SmoothBrush::kParallelBinMinVertices) {
        binParallel(ctx, grid, cells);
        ++g_steps.parallelBins;
    } else {
        const auto normals = ctx.mesh.normals();
        for (const u32 vertex : ctx.vertices) {
            const std::size_t cell = grid.cellOf(ctx.positions[vertex]);
            ctx.scratch.vectors[cell] += ctx.positions[vertex];
            ctx.scratch.vectors[cells + cell] += normals[vertex];
            ctx.scratch.scalars[cell] += 1.0f;
        }
    }

    g_steps.ms[1] += step.elapsedMs();
    step.reset();
    std::size_t occupied = 0;
    for (const float count : ctx.scratch.scalars) {
        occupied += count > 0.0f ? 1u : 0u;
    }
    g_steps.ms[2] += step.elapsedMs();
    return occupied > 0 ? static_cast<float>(ctx.vertices.size()) / static_cast<float>(occupied) : 0.0f;
}

/// Dense mode: move each vertex along the locally averaged normal onto the
/// locally averaged surface. "Locally averaged" is a blend of the nearest cell
/// averages, weighted by how many vertices each cell holds and by how well it
/// faces the same way as the vertex. The blend weights are quadratic B-splines
/// over the 3x3x3 nearest cells (see SmoothBrush::setQuadraticKernel), or
/// trilinear over the 2x2x2 nearest when that switch is off.
void kernelPass(const DabContext& ctx, const KernelGrid& grid, float amount)
{
    const bool quadratic = g_quadraticKernel.load(std::memory_order_relaxed);
    const std::size_t count = ctx.vertices.size();
    const std::size_t cells = static_cast<std::size_t>(grid.cellCount());
    const auto normals = ctx.mesh.normals();
    const glm::vec3* positionSums = ctx.scratch.vectors.data();
    const glm::vec3* normalSums = ctx.scratch.vectors.data() + cells;
    const float* counts = ctx.scratch.scalars.data();
    ctx.scratch.points.resize(count);

    core::Stopwatch step;
    core::parallelFor(ctx.jobs, count, kBrushParallelGrain, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const u32 vertex = ctx.vertices[i];
            const glm::vec3 position = ctx.positions[vertex];
            const glm::vec3 normal = normals[vertex];

            // Per axis: the first cell of the neighbourhood and the weight of
            // each of its `taps` cells. Cell averages are samples located at
            // cell centres.
            int first[3];
            float axisWeights[3][3];
            int taps = 0;
            if (quadratic) {
                // Quadratic B-spline centred on each cell centre: the cell
                // holding the vertex and its two neighbours. The weights and
                // their slopes are continuous where the vertex changes cell.
                taps = 3;
                const glm::vec3 local = (position - grid.origin) * grid.inverseCellSize;
                for (int axis = 0; axis < 3; ++axis) {
                    const float cellCoordinate = std::floor(local[axis]);
                    const float f = local[axis] - cellCoordinate;
                    first[axis] = static_cast<int>(cellCoordinate) - 1;
                    axisWeights[axis][0] = 0.5f * (1.0f - f) * (1.0f - f);
                    axisWeights[axis][1] = 0.75f - (f - 0.5f) * (f - 0.5f);
                    axisWeights[axis][2] = 0.5f * f * f;
                }
            } else {
                taps = 2;
                const glm::vec3 local = (position - grid.origin) * grid.inverseCellSize - 0.5f;
                for (int axis = 0; axis < 3; ++axis) {
                    const float cellCoordinate = std::floor(local[axis]);
                    const float t = local[axis] - cellCoordinate;
                    first[axis] = static_cast<int>(cellCoordinate);
                    axisWeights[axis][0] = 1.0f - t;
                    axisWeights[axis][1] = t;
                }
            }

            glm::vec3 positionSum(0.0f);
            glm::vec3 normalSum(0.0f);
            float weightSum = 0.0f;
            for (int dz = 0; dz < taps; ++dz) {
                const int z = grid.clampCoordinate(first[2] + dz);
                for (int dy = 0; dy < taps; ++dy) {
                    const int y = grid.clampCoordinate(first[1] + dy);
                    const float weightZY = axisWeights[2][dz] * axisWeights[1][dy];
                    for (int dx = 0; dx < taps; ++dx) {
                        const std::size_t cell =
                            static_cast<std::size_t>(grid.index(grid.clampCoordinate(first[0] + dx), y, z));
                        const float cellCount = counts[cell];
                        if (cellCount <= 0.0f) {
                            continue;
                        }
                        // dot(normal, average normal of the cell): near 1 for the same
                        // surface, 0 or negative for a surface facing the other way.
                        const float facing = glm::dot(normal, normalSums[cell]) / cellCount;
                        if (facing <= 0.0f) {
                            continue;
                        }
                        // times cellCount, folded into the sums below
                        const float weight = weightZY * axisWeights[0][dx] * facing;
                        positionSum += positionSums[cell] * weight;
                        normalSum += normalSums[cell] * weight;
                        weightSum += cellCount * weight;
                    }
                }
            }

            glm::vec3 result = position;
            const float normalLength = glm::length(normalSum);
            if (weightSum > 1e-6f && normalLength > 1e-6f) {
                const glm::vec3 surfacePoint = positionSum / weightSum;
                const glm::vec3 surfaceNormal = normalSum / normalLength;
                // Along the normal only: removes relief without sliding vertices
                // sideways, so the triangle distribution is preserved.
                const float height = glm::dot(surfacePoint - position, surfaceNormal);
                result = position + surfaceNormal * (height * ctx.weights[i] * amount);
            }
            ctx.scratch.points[i] = result;
        }
    });
    g_steps.ms[3] += step.elapsedMs();
    step.reset();

    core::parallelFor(ctx.jobs, count, kBrushParallelGrain, [&ctx](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            ctx.positions[ctx.vertices[i]] = ctx.scratch.points[i];
        }
    });
    g_steps.ms[4] += step.elapsedMs();
}

/// Laplacian relaxation. `passes` may be fractional: the remainder is applied
/// as a partial last pass, so the response to the strength slider is continuous.
void laplacianPasses(const DabContext& ctx, float passes)
{
    const std::size_t count = ctx.vertices.size();
    ctx.scratch.points.resize(count);
    const int passCount = static_cast<int>(std::ceil(passes));

    for (int pass = 0; pass < passCount; ++pass) {
        const float passAmount = std::min(1.0f, passes - static_cast<float>(pass));
        core::Stopwatch step;
        ++g_steps.laplacianPasses;

        // Step 1: compute every target from the positions as they are at the
        // start of the pass. Writing in place here would make the result depend
        // on vertex order (and, in parallel, on thread timing), because later
        // vertices would average neighbours that were already moved.
        core::parallelFor(ctx.jobs, count, kBrushParallelGrain, [&ctx, passAmount](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                const u32 vertex = ctx.vertices[i];
                const glm::vec3 current = ctx.positions[vertex];
                // Ascending neighbour order, stored or derived: the float sum
                // is the same bit for bit either way.
                glm::vec3 sum(0.0f);
                const u32 ringSize = ctx.mesh.forEachNeighbor(vertex, [&](u32 neighbor) { sum += ctx.positions[neighbor]; });
                if (ringSize == 0) {
                    ctx.scratch.points[i] = current;
                    continue;
                }
                const glm::vec3 average = sum / static_cast<float>(ringSize);
                ctx.scratch.points[i] = glm::mix(current, average, ctx.weights[i] * passAmount);
            }
        });

        g_steps.ms[5] += step.elapsedMs();
        step.reset();

        // Step 2: commit.
        core::parallelFor(ctx.jobs, count, kBrushParallelGrain, [&ctx](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                ctx.positions[ctx.vertices[i]] = ctx.scratch.points[i];
            }
        });
        g_steps.ms[6] += step.elapsedMs();
    }
}

} // namespace

SmoothBrush::Steps SmoothBrush::steps() noexcept
{
    return g_steps;
}

void SmoothBrush::resetSteps() noexcept
{
    g_steps = {};
}

void SmoothBrush::setParallelBinning(bool enabled) noexcept
{
    g_parallelBinning.store(enabled, std::memory_order_relaxed);
}

bool SmoothBrush::parallelBinning() noexcept
{
    return g_parallelBinning.load(std::memory_order_relaxed);
}

void SmoothBrush::setQuadraticKernel(bool enabled) noexcept
{
    g_quadraticKernel.store(enabled, std::memory_order_relaxed);
}

bool SmoothBrush::quadraticKernel() noexcept
{
    return g_quadraticKernel.load(std::memory_order_relaxed);
}

void SmoothBrush::apply(const DabContext& ctx) const
{
    if (ctx.vertices.empty() || ctx.radius <= 0.0f) {
        return;
    }
    const float strength = std::clamp(ctx.strength, 0.0f, 1.0f);

    ++g_steps.calls;
    KernelGrid grid;
    if (buildKernelGrid(ctx, grid) >= kDenseVerticesPerCell) {
        ++g_steps.denseCalls;
        kernelPass(ctx, grid, std::min(1.0f, strength * 2.0f));
        laplacianPasses(ctx, strength);
    } else {
        laplacianPasses(ctx, strength * static_cast<float>(kMaxPassesPerDab));
    }
}

} // namespace se::sculpt
