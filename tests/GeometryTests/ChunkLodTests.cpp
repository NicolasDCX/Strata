// Coarse chunk-group stand-ins (coarse fallback): crack-free borders, valid topology.

#include "Core/JobSystem.h"
#include "Geometry/ChunkLod.h"
#include "Geometry/Icosphere.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <utility>
#include <vector>

using namespace se;

namespace {

using Edge = std::pair<u32, u32>;
using Triangle = std::array<u32, 3>;

/// Triangles of a mesh where group g is drawn coarse when coarseGroup(g), else detailed.
template <typename Pick>
std::vector<Triangle> mixedTriangles(const geo::Mesh& mesh, const geo::CoarseChunks& coarse, Pick coarseGroup)
{
    const u32 groupTriangleShift = coarse.chunkShift + coarse.groupShift;
    std::vector<Triangle> out;
    for (u32 group = 0; group < coarse.groupCount(); ++group) {
        if (coarseGroup(group)) {
            const auto vertices = coarse.groupVertices(group);
            const auto indices = coarse.groupIndices(group);
            for (std::size_t i = 0; i < indices.size(); i += 3) {
                out.push_back({vertices[indices[i]], vertices[indices[i + 1]], vertices[indices[i + 2]]});
            }
        } else {
            const u32 first = group << groupTriangleShift;
            const u32 last = std::min(mesh.triangleCount(), first + (1u << groupTriangleShift));
            for (u32 t = first; t < last; ++t) {
                const auto& tri = mesh.triangle(t);
                out.push_back({tri[0], tri[1], tri[2]});
            }
        }
    }
    return out;
}

/// Closed, consistently oriented 2-manifold: every directed edge appears once and its reverse once.
::testing::AssertionResult watertight(const std::vector<Triangle>& triangles)
{
    std::map<Edge, int> directed;
    for (const Triangle& t : triangles) {
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) {
            return ::testing::AssertionFailure() << "degenerate triangle";
        }
        for (int c = 0; c < 3; ++c) {
            if (++directed[{t[static_cast<std::size_t>(c)], t[static_cast<std::size_t>((c + 1) % 3)]}] > 1) {
                return ::testing::AssertionFailure() << "directed edge used twice (non-manifold or flipped)";
            }
        }
    }
    for (const auto& [edge, count] : directed) {
        if (directed.find({edge.second, edge.first}) == directed.end()) {
            return ::testing::AssertionFailure() << "open edge " << edge.first << "-" << edge.second << " (crack)";
        }
    }
    return ::testing::AssertionSuccess();
}

geo::Mesh bumpySphere(int subdivisions, unsigned seed)
{
    geo::Mesh mesh = geo::makeIcosphere(subdivisions);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> noise(-0.02f, 0.02f);
    auto positions = mesh.mutablePositions();
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        positions[v] *= 1.0f + noise(rng);
    }
    return mesh;
}

} // namespace

TEST(ChunkLodTest, ReducesTrianglesAndKeepsOriginalVertexIds)
{
    const geo::Mesh mesh = geo::makeIcosphere(6);
    constexpr u32 kShift = 9;
    for (const u32 groupShift : {0u, 2u}) {
        const geo::CoarseChunks coarse = geo::buildCoarseChunks(mesh, kShift, groupShift, 1.0f / 16.0f);
        const u32 groupTriangleShift = kShift + groupShift;
        ASSERT_EQ(coarse.groupCount(), (mesh.triangleCount() + (1u << groupTriangleShift) - 1u) >> groupTriangleShift);
        EXPECT_EQ(coarse.groupOf(coarse.firstChunk(1) + (1u << groupShift) - 1u), 1u);
        u32 coarseTriangles = 0;
        for (u32 group = 0; group < coarse.groupCount(); ++group) {
            coarseTriangles += coarse.groupTriangles(group);
            EXPECT_LE(coarse.groupVertices(group).size(), coarse.maxGroupVertices);
            std::set<u32> original;
            for (u32 t = group << groupTriangleShift; t < std::min(mesh.triangleCount(), (group + 1) << groupTriangleShift); ++t) {
                for (const auto v : mesh.triangle(t)) {
                    original.insert(v);
                }
            }
            for (const auto v : coarse.groupVertices(group)) {
                EXPECT_TRUE(original.contains(v));
            }
        }
        EXPECT_LT(coarseTriangles, mesh.triangleCount() / 3);
    }
}

TEST(ChunkLodTest, GroupsNeedFewerVerticesThanSingleChunks)
{
    // Interior chunk borders of a group are not locked, so for the same
    // triangle ratio a group of 4 chunks keeps fewer vertices than 4 chunks
    // simplified on their own.
    const geo::Mesh mesh = geo::makeIcosphere(7);
    const geo::CoarseChunks single = geo::buildCoarseChunks(mesh, 10, 0, 1.0f / 16.0f);
    const geo::CoarseChunks grouped = geo::buildCoarseChunks(mesh, 10, 2, 1.0f / 16.0f);
    EXPECT_LT(grouped.vertices.size() * 10, single.vertices.size() * 9);
    EXPECT_LE(grouped.indices.size(), single.indices.size());
}

TEST(ChunkLodTest, AllCoarseAndMixedMeshesAreWatertight)
{
    for (const unsigned seed : {0u, 1u}) {
        const geo::Mesh mesh = seed == 0 ? geo::makeIcosphere(6) : bumpySphere(6, seed);
        for (const u32 shift : {8u, 10u}) {
            for (const u32 groupShift : {0u, 2u}) {
                const geo::CoarseChunks coarse = geo::buildCoarseChunks(mesh, shift, groupShift, 1.0f / 32.0f);
                EXPECT_TRUE(watertight(mixedTriangles(mesh, coarse, [](u32) { return true; })));
                EXPECT_TRUE(watertight(mixedTriangles(mesh, coarse, [](u32 g) { return g % 2 == 0; })));
                EXPECT_TRUE(watertight(mixedTriangles(mesh, coarse, [](u32 g) { return g % 3 == 1; })));
            }
        }
    }
}

TEST(ChunkLodTest, ResultDoesNotDependOnThreads)
{
    const geo::Mesh mesh = bumpySphere(6, 7);
    core::JobSystem jobs(3);
    for (const u32 groupShift : {0u, 2u}) {
        const geo::CoarseChunks a = geo::buildCoarseChunks(mesh, 9, groupShift, 1.0f / 16.0f);
        const geo::CoarseChunks b = geo::buildCoarseChunks(mesh, 9, groupShift, 1.0f / 16.0f, &jobs);
        EXPECT_EQ(a.vertices, b.vertices);
        EXPECT_EQ(a.indices, b.indices);
    }
}
