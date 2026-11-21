#include "SculptFixture.h"

#include "Sculpt/Brushes/DrawBrush.h"
#include "Sculpt/Brushes/SmoothBrush.h"
#include "Sculpt/Falloff.h"
#include "TestUtils.h"

#include <random>

using namespace se;
using sculpt::BrushId;
using sculpt::BrushSettings;

namespace {

class BrushTest : public test::SculptFixture {};

const glm::vec3 kPole(0.0f, 0.0f, 1.0f);

/// Sum of squared differences between each vertex's distance to the origin and the
/// average distance of its neighbours. Exactly 0 for a sphere, so it measures only
/// the bumps that were added to it.
float roughness(const geo::Mesh& mesh)
{
    float total = 0.0f;
    const auto positions = mesh.positions();
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        float average = 0.0f;
        const auto ring = mesh.neighbors(vertex);
        for (const u32 neighbor : ring) {
            average += glm::length(positions[neighbor]);
        }
        average /= static_cast<float>(ring.size());
        const float d = glm::length(positions[vertex]) - average;
        total += d * d;
    }
    return total;
}

} // namespace

TEST(Falloff, IsOneAtCentreZeroAtRimAndMonotonic)
{
    EXPECT_FLOAT_EQ(sculpt::smoothFalloff(0.0f), 1.0f);
    EXPECT_FLOAT_EQ(sculpt::smoothFalloff(1.0f), 0.0f);
    EXPECT_FLOAT_EQ(sculpt::smoothFalloff(1.5f), 0.0f);
    float previous = 1.0f;
    for (int i = 1; i <= 100; ++i) {
        const float value = sculpt::smoothFalloff(static_cast<float>(i) / 100.0f);
        ASSERT_LE(value, previous);
        previous = value;
    }
}

TEST_F(BrushTest, DrawRaisesSurfaceAlongNormalInsideRadiusOnly)
{
    const BrushSettings settings{.radius = 0.3f, .strength = 1.0f, .invert = false};
    ASSERT_TRUE(stroke.begin(brushes.brush(BrushId::Draw), settings, rayAt(0.0f, 0.0f)));
    const auto delta = stroke.end();
    ASSERT_TRUE(delta.has_value());

    const u32 centre = closestVertexTo(kPole);
    const float expectedPeak = settings.radius * settings.strength * sculpt::DrawBrush::kGainPerDab;
    const glm::vec3 displacement = mesh.positions()[centre] - original[centre];
    // The centre vertex moves outward, along the surface normal (+Z at the pole)...
    EXPECT_NEAR(displacement.z, expectedPeak, expectedPeak * 0.05f);
    EXPECT_NEAR(displacement.x, 0.0f, expectedPeak * 0.05f);
    EXPECT_NEAR(displacement.y, 0.0f, expectedPeak * 0.05f);

    u32 moved = 0;
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        const float distanceToCentre = glm::distance(original[vertex], kPole);
        const float amount = glm::length(mesh.positions()[vertex] - original[vertex]);
        if (distanceToCentre >= settings.radius + 1e-3f) {
            // ...and nothing outside the brush radius is touched at all.
            ASSERT_EQ(mesh.positions()[vertex], original[vertex]) << "vertex " << vertex;
        } else if (amount > 0.0f) {
            ++moved;
            // Smooth falloff: never more than the centre.
            ASSERT_LE(amount, expectedPeak * 1.001f);
        }
    }
    EXPECT_GT(moved, 10u);
    EXPECT_EQ(delta->vertices.size(), moved);
}

TEST_F(BrushTest, DrawInvertedLowersSurface)
{
    const BrushSettings settings{.radius = 0.3f, .strength = 1.0f, .invert = true};
    ASSERT_TRUE(stroke.begin(brushes.brush(BrushId::Draw), settings, rayAt(0.0f, 0.0f)));
    stroke.end();
    const u32 centre = closestVertexTo(kPole);
    EXPECT_LT(mesh.positions()[centre].z, original[centre].z - 0.01f);
}

TEST_F(BrushTest, DrawStrengthAndRadiusScaleTheEffect)
{
    const u32 centre = closestVertexTo(kPole);

    ASSERT_TRUE(stroke.begin(brushes.brush(BrushId::Draw), {.radius = 0.3f, .strength = 0.25f, .invert = false}, rayAt(0, 0)));
    stroke.end();
    const float weak = mesh.positions()[centre].z - original[centre].z;
    const u32 smallRadiusCount = movedVertexCount();

    mesh.mutablePositions()[centre] = original[centre]; // irrelevant for the comparison below
    geo::Mesh fresh = geo::makeIcosphere(4);
    geo::ChunkedBvh freshBvh;
    freshBvh.build(fresh);
    sculpt::StrokeProcessor freshStroke(fresh, freshBvh);
    ASSERT_TRUE(freshStroke.begin(brushes.brush(BrushId::Draw), {.radius = 0.6f, .strength = 1.0f, .invert = false}, rayAt(0, 0)));
    freshStroke.end();
    const float strong = fresh.positions()[centre].z - original[centre].z;

    u32 largeRadiusCount = 0;
    for (u32 vertex = 0; vertex < fresh.vertexCount(); ++vertex) {
        largeRadiusCount += fresh.positions()[vertex] != original[vertex] ? 1u : 0u;
    }

    EXPECT_GT(weak, 0.0f);
    EXPECT_NEAR(strong / weak, 8.0f, 0.5f); // 4x strength, 2x radius
    EXPECT_GT(largeRadiusCount, smallRadiusCount * 3);
}

TEST_F(BrushTest, DrawUpdatesNormalsAndKeepsBvhCorrect)
{
    const BrushSettings settings{.radius = 0.4f, .strength = 1.0f, .invert = false};
    ASSERT_TRUE(stroke.begin(brushes.brush(BrushId::Draw), settings, rayAt(0.1f, 0.0f)));
    for (int i = 1; i <= 20; ++i) {
        stroke.update(rayAt(0.1f + 0.02f * static_cast<float>(i), 0.0f));
    }
    stroke.end();

    // Incrementally maintained normals must equal a full recompute.
    const std::vector<glm::vec3> incremental(mesh.normals().begin(), mesh.normals().end());
    mesh.recomputeNormals();
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        ASSERT_NEAR(glm::distance(incremental[vertex], mesh.normals()[vertex]), 0.0f, 1e-5f) << "vertex " << vertex;
    }

    // The refitted BVH must still agree with a brute-force raycast.
    std::mt19937 rng(7);
    for (int i = 0; i < 300; ++i) {
        const geo::Ray ray = test::randomRayTowardsOrigin(rng);
        const auto expected = test::bruteForceRaycast(mesh, ray);
        const auto actual = bvh.raycast(mesh, ray);
        ASSERT_EQ(expected.has_value(), actual.has_value());
        if (expected) {
            ASSERT_NEAR(*expected, actual->distance, 1e-4f);
        }
    }
}

TEST_F(BrushTest, SmoothReducesRoughnessAndLeavesTheRestUntouched)
{
    // Roughen the cap around the pole with deterministic radial noise.
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> noise(-0.03f, 0.03f);
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        if (glm::distance(original[vertex], kPole) < 0.2f) {
            mesh.mutablePositions()[vertex] += glm::normalize(original[vertex]) * noise(rng);
        }
    }
    mesh.recomputeNormals();
    bvh.build(mesh);
    const std::vector<glm::vec3> roughened(mesh.positions().begin(), mesh.positions().end());
    const float before = roughness(mesh);

    const BrushSettings settings{.radius = 0.5f, .strength = 1.0f, .invert = false};
    ASSERT_TRUE(stroke.begin(brushes.brush(BrushId::Smooth), settings, rayAt(0.0f, 0.0f)));
    for (int i = 0; i < 12; ++i) {
        // Small circular motion so several dabs are emitted.
        const float a = static_cast<float>(i) * 0.7f;
        stroke.update(rayAt(0.15f * std::cos(a), 0.15f * std::sin(a)));
    }
    stroke.end();

    const float after = roughness(mesh);
    EXPECT_LT(after, before * 0.1f) << "before " << before << " after " << after;

    // Vertices on the far side were never inside any dab.
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        if (original[vertex].z < 0.0f) {
            ASSERT_EQ(mesh.positions()[vertex], roughened[vertex]);
        }
    }
}

TEST_F(BrushTest, SmoothResultDoesNotDependOnVertexOrder)
{
    // A single spike: with a stable source snapshot, the spike's neighbours must be
    // pulled by the *original* spike height regardless of which is processed first.
    const u32 spike = closestVertexTo(kPole);
    mesh.mutablePositions()[spike] += glm::vec3(0.0f, 0.0f, 0.2f);
    mesh.recomputeNormals();
    bvh.refit(mesh);
    const std::vector<glm::vec3> source(mesh.positions().begin(), mesh.positions().end());

    // Sparse mode (Laplacian passes only), whatever this mesh's density.
    struct SparseOnly {
        SparseOnly() { sculpt::SmoothBrush::setDenseThreshold(1e9f); }
        ~SparseOnly() { sculpt::SmoothBrush::setDenseThreshold(sculpt::SmoothBrush::kDenseVerticesPerCell); }
    } sparseOnly;
    // Strength 1 / kMaxPassesPerDab is exactly one relaxation pass.
    const BrushSettings settings{.radius = 0.6f,
                                 .strength = 1.0f / static_cast<float>(sculpt::SmoothBrush::kMaxPassesPerDab),
                                 .invert = false};
    // Aim beside the spike so the hit point is on the undisturbed sphere.
    ASSERT_TRUE(stroke.begin(brushes.brush(BrushId::Smooth), settings, rayAt(0.3f, 0.0f)));
    stroke.end();

    const glm::vec3 centre(0.3f, 0.0f, std::sqrt(1.0f - 0.09f));
    for (const u32 vertex : mesh.neighbors(spike)) {
        glm::vec3 average(0.0f);
        const auto ring = mesh.neighbors(vertex);
        for (const u32 neighbor : ring) {
            average += source[neighbor];
        }
        average /= static_cast<float>(ring.size());
        // Hit point is on a facet, so allow a small tolerance on the weight.
        const float d2 = glm::dot(source[vertex] - centre, source[vertex] - centre) / (settings.radius * settings.radius);
        const glm::vec3 expected = glm::mix(source[vertex], average, sculpt::smoothFalloff(d2));
        ASSERT_NEAR(glm::distance(mesh.positions()[vertex], expected), 0.0f, 2e-3f) << "vertex " << vertex;
    }
}

TEST_F(BrushTest, MoveDragsGrabbedRegionWithThePointer)
{
    const BrushSettings settings{.radius = 0.4f, .strength = 1.0f, .invert = false};
    const u32 centre = closestVertexTo(kPole);

    ASSERT_TRUE(stroke.begin(brushes.brush(BrushId::Move), settings, rayAt(0.0f, 0.0f)));
    EXPECT_EQ(movedVertexCount(), 0u) << "grabbing alone must not move anything";

    stroke.update(rayAt(0.1f, 0.0f));
    stroke.update(rayAt(0.25f, 0.1f)); // only the final pointer position matters
    const auto delta = stroke.end();
    ASSERT_TRUE(delta.has_value());

    // Rays are parallel to Z, so the drag plane is Z = const and the centre
    // follows the pointer exactly at strength 1.
    const glm::vec3 displacement = mesh.positions()[centre] - original[centre];
    EXPECT_NEAR(displacement.x, 0.25f, 0.01f);
    EXPECT_NEAR(displacement.y, 0.10f, 0.01f);
    EXPECT_NEAR(displacement.z, 0.0f, 1e-4f);

    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        const float amount = glm::length(mesh.positions()[vertex] - original[vertex]);
        if (glm::distance(original[vertex], kPole) >= settings.radius + 1e-3f) {
            ASSERT_EQ(amount, 0.0f);
        } else {
            ASSERT_LE(amount, std::hypot(0.25f, 0.10f) * 1.001f);
        }
    }
}

TEST_F(BrushTest, MoveReturningToStartRestoresTheSurface)
{
    const BrushSettings settings{.radius = 0.4f, .strength = 1.0f, .invert = false};
    ASSERT_TRUE(stroke.begin(brushes.brush(BrushId::Move), settings, rayAt(0.0f, 0.0f)));
    stroke.update(rayAt(0.3f, -0.2f));
    stroke.update(rayAt(0.0f, 0.0f));
    stroke.end();
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        ASSERT_NEAR(glm::distance(mesh.positions()[vertex], original[vertex]), 0.0f, 1e-6f);
    }
}

TEST_F(BrushTest, CustomBrushCanBeRegisteredWithoutTouchingGeometry)
{
    // A brush defined entirely outside the engine: flattens towards the brush plane.
    class FlattenBrush final : public sculpt::Brush {
    public:
        std::string_view name() const noexcept override { return "Flatten"; }
        sculpt::StrokeMode mode() const noexcept override { return sculpt::StrokeMode::Dabs; }
        void apply(const sculpt::DabContext& ctx) const override
        {
            for (std::size_t i = 0; i < ctx.vertices.size(); ++i) {
                glm::vec3& p = ctx.positions[ctx.vertices[i]];
                const float height = glm::dot(p - ctx.center, ctx.normal);
                p -= ctx.normal * (height * ctx.strength * ctx.weights[i]);
            }
        }
    };

    const BrushId id = brushes.add(std::make_unique<FlattenBrush>(), {.radius = 0.5f, .strength = 1.0f, .invert = false});
    EXPECT_EQ(brushes.brush(id).name(), "Flatten");
    ASSERT_TRUE(stroke.begin(brushes.brush(id), brushes.settings(id), rayAt(0.0f, 0.0f)));
    stroke.end();
    EXPECT_GT(movedVertexCount(), 0u);
    // Vertices near the rim were below the tangent plane and moved up towards it.
    const u32 centre = closestVertexTo(kPole);
    for (const u32 neighbor : mesh.neighbors(centre)) {
        EXPECT_GT(mesh.positions()[neighbor].z, original[neighbor].z);
    }
}

namespace {

/// Raises a broad bump at the +Z pole, scrubs it with Smooth, and returns the
/// fraction of the bump's height that is left (measured against the rim, so
/// overall shrinking of the sphere does not count).
float bumpRemainingAfterSmoothing(int level, float smoothRadius)
{
    geo::Mesh mesh = geo::makeIcosphere(level);
    geo::ChunkedBvh bvh;
    bvh.build(mesh);
    sculpt::StrokeProcessor stroke(mesh, bvh);
    const sculpt::BrushLibrary brushes;
    const auto ray = [](float x, float y) { return geo::Ray{{x, y, 5.0f}, {0.0f, 0.0f, -1.0f}}; };
    const auto bumpHeight = [&] {
        const auto top = bvh.raycast(mesh, ray(0.0f, 0.0f));
        const auto rim = bvh.raycast(mesh, ray(0.26f, 0.0f));
        return glm::length(top->position) - glm::length(rim->position);
    };

    // The bump: a few stationary dabs of a medium brush.
    const BrushSettings draw{.radius = 0.2f, .strength = 1.0f, .invert = false};
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(stroke.begin(brushes.brush(BrushId::Draw), draw, ray(0.0f, 0.0f)));
        stroke.end();
    }
    const float before = bumpHeight();
    EXPECT_GT(before, 0.05f);

    const BrushSettings smooth{.radius = smoothRadius, .strength = 0.5f, .invert = false};
    EXPECT_TRUE(stroke.begin(brushes.brush(BrushId::Smooth), smooth, ray(-0.1f, 0.0f)));
    for (int pass = 0; pass < 4; ++pass) {
        for (int i = 0; i <= 10; ++i) {
            const float t = (pass % 2 == 0) ? static_cast<float>(i) / 10.0f : 1.0f - static_cast<float>(i) / 10.0f;
            stroke.update(ray(-0.1f + 0.2f * t, 0.0f));
        }
    }
    stroke.end();
    return bumpHeight() / before;
}

} // namespace

TEST(AdaptiveSmooth, FlattensBroadShapesOnDenseMeshes)
{
    // 327,680 triangles: edge-neighbour averaging alone barely moves a bump this
    // wide (dozens of edges across). The density-adaptive filter must.
    const float remaining = bumpRemainingAfterSmoothing(7, 0.3f);
    EXPECT_LT(remaining, 0.6f) << "fraction of the bump left: " << remaining;
    EXPECT_GT(remaining, -0.2f) << "smoothing must not turn the bump into a pit";
}

TEST(AdaptiveSmooth, EffectDoesNotDependOnMeshDensity)
{
    // Same bump, same strokes, 4x the triangles: the result should be close.
    const float coarse = bumpRemainingAfterSmoothing(6, 0.3f);
    const float fine = bumpRemainingAfterSmoothing(7, 0.3f);
    EXPECT_NEAR(coarse, fine, 0.15f) << "82k triangles: " << coarse << ", 328k triangles: " << fine;
}