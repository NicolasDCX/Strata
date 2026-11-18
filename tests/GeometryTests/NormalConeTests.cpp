#include "Core/JobSystem.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/Icosphere.h"
#include "Geometry/NormalCone.h"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <vector>

using namespace se;

namespace {

u32 chunkFirst(const geo::ChunkedBvh& bvh, u32 chunk) { return chunk << bvh.chunkTriangleShift(); }
u32 chunkSize(const geo::Mesh& mesh, const geo::ChunkedBvh& bvh, u32 chunk)
{
    return std::min(1u << bvh.chunkTriangleShift(), mesh.triangleCount() - chunkFirst(bvh, chunk));
}

/// Moves the vertices within `radius` of `center` by a noisy offset strong
/// enough to fold the surface. Returns the moved vertices.
std::vector<u32> crumple(geo::Mesh& mesh, const glm::vec3& center, float radius, float amount, u32 seed)
{
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    std::vector<u32> moved;
    auto positions = mesh.mutablePositions();
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        const float distance = glm::length(positions[vertex] - center);
        if (distance < radius) {
            const float falloff = 1.0f - distance / radius;
            positions[vertex] += amount * falloff * glm::vec3(noise(random), noise(random), noise(random));
            moved.push_back(vertex);
        }
    }
    return moved;
}

/// Every cone is conservative and equals a fresh computation from the mesh.
void expectConesCurrent(const geo::Mesh& mesh, const geo::ChunkedBvh& bvh)
{
    for (u32 chunk = 0; chunk < bvh.chunkCount(); ++chunk) {
        const geo::NormalCone& cone = bvh.chunkCone(chunk);
        ASSERT_TRUE(geo::coneContainsNormals(cone, mesh, chunkFirst(bvh, chunk), chunkSize(mesh, bvh, chunk))) << "chunk " << chunk;
        ASSERT_TRUE(cone == geo::computeNormalCone(mesh, chunkFirst(bvh, chunk), chunkSize(mesh, bvh, chunk))) << "chunk " << chunk;
    }
}

/// Brute force: when the test rejects a chunk, every triangle of the chunk
/// faces away from the eye at each of its corners.
u32 expectRejectionsExact(const geo::Mesh& mesh, const geo::ChunkedBvh& bvh, const std::vector<glm::vec3>& eyes)
{
    const auto positions = mesh.positions();
    u32 rejected = 0;
    for (const glm::vec3& eye : eyes) {
        for (u32 chunk = 0; chunk < bvh.chunkCount(); ++chunk) {
            if (!geo::coneFacesAway(bvh.chunkCone(chunk), bvh.chunk(chunk).bounds(), eye)) {
                continue;
            }
            ++rejected;
            const u32 first = chunkFirst(bvh, chunk);
            for (u32 t = first; t < first + chunkSize(mesh, bvh, chunk); ++t) {
                const auto& tri = mesh.triangle(t);
                const glm::vec3 n = glm::cross(positions[tri[1]] - positions[tri[0]], positions[tri[2]] - positions[tri[0]]);
                for (const u32 vertex : tri) {
                    if (glm::dot(n, positions[vertex] - eye) < 0.0f) {
                        ADD_FAILURE() << "rejected chunk " << chunk << " has front-facing triangle " << t;
                        return rejected;
                    }
                }
            }
        }
    }
    return rejected;
}

std::vector<glm::vec3> eyesAround(float distance)
{
    std::vector<glm::vec3> eyes;
    std::mt19937 random(7);
    std::normal_distribution<float> gauss;
    for (int i = 0; i < 24; ++i) {
        eyes.push_back(distance * glm::normalize(glm::vec3(gauss(random), gauss(random), gauss(random))));
    }
    return eyes;
}

} // namespace

TEST(NormalCone, OffByDefaultAndUnusable)
{
    const geo::Mesh mesh = geo::makeIcosphere(4);
    geo::ChunkedBvh bvh;
    bvh.build(mesh, 8);
    EXPECT_FALSE(bvh.hasNormalCones());
    for (u32 chunk = 0; chunk < bvh.chunkCount(); ++chunk) {
        EXPECT_FALSE(bvh.chunkCone(chunk).usable());
    }
    EXPECT_EQ(bvh.lastConeTriangles(), 0u);
}

TEST(NormalCone, WholeSphereChunkCannotReject)
{
    const geo::Mesh mesh = geo::makeIcosphere(3);
    geo::ChunkedBvh bvh;
    bvh.build(mesh, 20, nullptr, true); // a single chunk holding the closed sphere
    ASSERT_EQ(bvh.chunkCount(), 1u);
    EXPECT_FALSE(bvh.chunkCone(0).usable());
    EXPECT_FALSE(geo::coneFacesAway(bvh.chunkCone(0), bvh.chunk(0).bounds(), {0, 0, 5}));
}

TEST(NormalCone, BuiltConesHoldAndRejectOnlyBackFacingChunks)
{
    const geo::Mesh mesh = geo::makeIcosphere(5);
    geo::ChunkedBvh bvh;
    bvh.build(mesh, 8, nullptr, true);
    expectConesCurrent(mesh, bvh);
    u32 usable = 0;
    for (u32 chunk = 0; chunk < bvh.chunkCount(); ++chunk) {
        usable += bvh.chunkCone(chunk).usable() ? 1u : 0u;
    }
    EXPECT_EQ(usable, bvh.chunkCount()) << "small patches of a sphere have narrow cones";
    const u32 rejected = expectRejectionsExact(mesh, bvh, eyesAround(3.0f));
    EXPECT_GT(rejected, 24u * bvh.chunkCount() / 4) << "roughly the far side should be rejected";
}

TEST(NormalCone, IncrementalRefitKeepsConesCurrentThroughFolds)
{
    geo::Mesh mesh = geo::makeIcosphere(6);
    geo::ChunkedBvh bvh;
    bvh.build(mesh, 9, nullptr, true);
    const glm::vec3 centers[] = {{0, 0, 1}, {0.6f, 0.6f, 0.5f}, {0, 0, 1}, {-1, 0, 0}};
    for (int step = 0; step < 4; ++step) {
        const std::vector<u32> moved = crumple(mesh, centers[step], 0.5f, 0.15f, 100u + static_cast<u32>(step));
        bvh.refit(mesh, moved);
        EXPECT_GT(bvh.lastConeTriangles(), 0u);
        expectConesCurrent(mesh, bvh);
        expectRejectionsExact(mesh, bvh, eyesAround(2.5f));
    }
    // Background re-partitioning does not change a chunk's triangles.
    bvh.optimize(mesh);
    expectConesCurrent(mesh, bvh);
    // A full refit recomputes every cone.
    for (glm::vec3& position : mesh.mutablePositions()) {
        position.y *= -1.0f; // mirror: every winding normal flips
    }
    bvh.refit(mesh);
    expectConesCurrent(mesh, bvh);
}

TEST(NormalCone, ParallelRefitEqualsSerial)
{
    core::JobSystem jobs(4);
    geo::Mesh mesh = geo::makeIcosphere(7);
    geo::ChunkedBvh serial;
    serial.build(mesh, 9, nullptr, true);
    geo::ChunkedBvh parallel;
    parallel.build(mesh, 9, &jobs, true);
    ASSERT_TRUE(serial.sameBounds(parallel));
    for (int step = 0; step < 3; ++step) {
        const std::vector<u32> moved = crumple(mesh, {0.2f * static_cast<float>(step), 0, 1}, 0.7f, 0.1f, 7u + static_cast<u32>(step));
        serial.refit(mesh, moved);
        parallel.refit(mesh, moved, &jobs);
        ASSERT_TRUE(serial.sameBounds(parallel)) << "after step " << step;
        ASSERT_EQ(serial.lastConeTriangles(), parallel.lastConeTriangles());
    }
}

namespace {

bool bitwiseSame(const geo::NormalCone& a, const geo::NormalCone& b)
{
    return std::memcmp(&a, &b, sizeof(geo::NormalCone)) == 0;
}

} // namespace

TEST(NormalCone, ReusedFaceNormalsGiveTheSameConeBitForBit)
{
    // Ranges on a smooth sphere (narrow cones, the second pass runs to the
    // end), on crumpled and folded patches (the second pass stops early or the
    // cone is unusable), with degenerate triangles, and of many sizes.
    geo::Mesh mesh = geo::makeIcosphere(7);
    std::vector<glm::vec4> faces(1u << 13);
    u32 compared = 0, stoppedEarly = 0, unusable = 0;
    for (int stage = 0; stage < 4; ++stage) {
        if (stage == 1) {
            crumple(mesh, {0, 0, 1}, 0.4f, 0.03f, 3u);
        } else if (stage == 2) {
            crumple(mesh, {0.7f, 0.7f, 0}, 0.6f, 0.25f, 4u);
        } else if (stage == 3) {
            // Collapse a few triangles to zero area (they are ignored).
            auto positions = mesh.mutablePositions();
            for (u32 t = 100; t < 4000; t += 37) {
                const auto& tri = mesh.triangle(t);
                positions[tri[1]] = positions[tri[0]];
            }
        }
        for (const u32 count : {1u, 7u, 64u, 1000u, 4096u, 8192u}) {
            for (u32 first = 0; first + count <= mesh.triangleCount(); first += std::max(count, mesh.triangleCount() / 23)) {
                const geo::NormalCone reference = geo::computeNormalCone(mesh, first, count);
                geo::NormalConeTimes recomputed, reused;
                const geo::NormalCone a = geo::computeNormalConeTimed(mesh, first, count, nullptr, recomputed);
                const geo::NormalCone b = geo::computeNormalConeTimed(mesh, first, count, faces.data(), reused);
                ASSERT_TRUE(bitwiseSame(reference, a)) << "stage " << stage << ", first " << first << ", count " << count;
                ASSERT_TRUE(bitwiseSame(reference, b)) << "stage " << stage << ", first " << first << ", count " << count;
                ASSERT_EQ(recomputed.secondPassTriangles, reused.secondPassTriangles);
                ++compared;
                stoppedEarly += reused.secondPassTriangles < count ? 1u : 0u;
                unusable += reference.usable() ? 0u : 1u;
            }
        }
    }
    EXPECT_GT(compared, 200u);
    EXPECT_GT(stoppedEarly, 0u) << "no range stopped its second pass early";
    EXPECT_GT(unusable, 0u);
}

TEST(NormalCone, ReusedFaceNormalsKeepChunkConesExactThroughFolds)
{
    core::JobSystem jobs(3);
    for (const u32 shift : {6u, 9u, 12u}) {
        for (core::JobSystem* pool : {static_cast<core::JobSystem*>(nullptr), &jobs}) {
            SCOPED_TRACE(testing::Message() << "shift " << shift << (pool ? ", 3 helpers" : ", serial"));
            geo::Mesh mesh = geo::makeIcosphere(7);
            geo::ChunkedBvh reused;
            reused.setReuseConeNormals(true);
            reused.build(mesh, shift, pool, true);
            geo::ChunkedBvh recomputed;
            recomputed.build(mesh, shift, pool, true);
            ASSERT_TRUE(reused.sameBounds(recomputed));
            expectConesCurrent(mesh, reused);
            const glm::vec3 centers[] = {{0, 0, 1}, {0.6f, 0.6f, 0.5f}, {0, 0, 1}, {-1, 0, 0}, {0, -1, 0}};
            for (int step = 0; step < 5; ++step) {
                const std::vector<u32> moved =
                    crumple(mesh, centers[step], 0.3f + 0.15f * static_cast<float>(step), 0.12f, 50u + static_cast<u32>(step));
                reused.refit(mesh, moved, pool);
                recomputed.refit(mesh, moved, pool);
                ASSERT_TRUE(reused.sameBounds(recomputed)) << "step " << step;
                expectConesCurrent(mesh, reused);
                expectRejectionsExact(mesh, reused, eyesAround(2.5f));
            }
            reused.refit(mesh);
            recomputed.refit(mesh);
            EXPECT_TRUE(reused.sameBounds(recomputed));
        }
    }
    EXPECT_GT(geo::ChunkedBvh::coneScratchBytes(), 0u);
}
