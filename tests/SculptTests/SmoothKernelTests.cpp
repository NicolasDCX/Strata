// Smooth's dense-mode kernel blend (SmoothBrush::setQuadraticKernel).
//
// On a curved surface each kernel cell's average lies below the surface by an
// amount that depends on how the surface cuts the cell. A trilinear blend of
// those averages has a slope jump on every plane through cell centres, so one
// dab leaves a lattice of creases (shallow pillows, one per cell), and a stroke,
// whose grid moves with each dab, stacks them into angular bands. The quadratic
// B-spline blend is continuous in slope, so the creases go away.

#include "Geometry/Icosphere.h"
#include "Sculpt/Brushes/SmoothBrush.h"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace se;
using sculpt::SmoothBrush;

namespace {

/// Restores the default (quadratic kernel) when a test ends.
struct KernelGuard {
    ~KernelGuard() { SmoothBrush::setQuadraticKernel(true); }
};

struct DabResult {
    float creaseP99 = 0.0f; ///< 99th percentile of |umbrella Laplacian of the radial displacement|
    float meanDepth = 0.0f; ///< mean inward radial displacement under the brush core
    u64 denseCalls = 0;
};

/// One Smooth dab on a plain sphere, off-axis so the grid is not symmetric.
DabResult applyDab(bool quadratic)
{
    geo::Mesh mesh = geo::makeIcosphere(8, 1.0f, geo::VertexOrder::Spatial);
    const auto before = std::vector<glm::vec3>(mesh.positions().begin(), mesh.positions().end());
    const glm::vec3 center = glm::normalize(glm::vec3(0.31f, 0.37f, 0.87f));
    const float radius = 0.25f;
    std::vector<u32> vertices;
    std::vector<float> weights;
    for (u32 v = 0; v < before.size(); ++v) {
        const float distance = glm::length(before[v] - center);
        if (distance < radius) {
            vertices.push_back(v);
            const float t = 1.0f - distance / radius;
            weights.push_back(t * t * (3.0f - 2.0f * t));
        }
    }
    sculpt::DabScratch scratch;
    const sculpt::DabContext ctx{
        .mesh = mesh,
        .positions = mesh.mutablePositions(),
        .vertices = vertices,
        .weights = weights,
        .center = center,
        .normal = center,
        .radius = radius,
        .strength = 0.5f,
        .scratch = scratch,
        .jobs = nullptr,
    };
    SmoothBrush::setQuadraticKernel(quadratic);
    SmoothBrush::resetSteps();
    const SmoothBrush brush;
    brush.apply(ctx);

    DabResult result;
    result.denseCalls = SmoothBrush::steps().denseCalls;
    const auto after = mesh.positions();
    std::vector<float> radial(before.size());
    for (std::size_t v = 0; v < before.size(); ++v) {
        radial[v] = glm::length(after[v]) - glm::length(before[v]);
    }
    std::vector<float> crease;
    double depth = 0.0;
    int depthCount = 0;
    for (const u32 v : vertices) {
        const float distance = glm::length(before[v] - center);
        if (distance > 0.7f * radius) {
            continue;
        }
        float sum = 0.0f;
        const u32 ring = mesh.forEachNeighbor(v, [&](u32 n) { sum += radial[n]; });
        crease.push_back(std::abs(sum / static_cast<float>(ring) - radial[v]));
        depth -= radial[v];
        ++depthCount;
    }
    std::sort(crease.begin(), crease.end());
    result.creaseP99 = crease[crease.size() * 99 / 100];
    result.meanDepth = static_cast<float>(depth / depthCount);
    return result;
}

} // namespace

TEST(SmoothKernel, QuadraticBlendLeavesNoCellCreases)
{
    const KernelGuard guard;
    const DabResult trilinear = applyDab(false);
    const DabResult quadratic = applyDab(true);
    ASSERT_EQ(trilinear.denseCalls, 1u) << "the dab must use the dense kernel";
    ASSERT_EQ(quadratic.denseCalls, 1u);
    RecordProperty("crease_p99_trilinear", testing::PrintToString(trilinear.creaseP99));
    RecordProperty("crease_p99_quadratic", testing::PrintToString(quadratic.creaseP99));
    RecordProperty("depth_trilinear", testing::PrintToString(trilinear.meanDepth));
    RecordProperty("depth_quadratic", testing::PrintToString(quadratic.meanDepth));
    // Both smooth by a comparable amount...
    EXPECT_GT(quadratic.meanDepth, 0.5f * trilinear.meanDepth);
    EXPECT_LT(quadratic.meanDepth, 2.0f * trilinear.meanDepth);
    // ...but the trilinear blend's slope jumps on the cell planes show up as
    // spikes of the discrete curvature, which the quadratic blend removes.
    EXPECT_LT(quadratic.creaseP99, 0.5f * trilinear.creaseP99)
        << "trilinear " << trilinear.creaseP99 << ", quadratic " << quadratic.creaseP99;
}

TEST(SmoothKernel, QuadraticIsTheDefault)
{
    EXPECT_TRUE(SmoothBrush::quadraticKernel());
}
