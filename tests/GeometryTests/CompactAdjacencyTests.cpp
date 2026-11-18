// The compact adjacency layout must give back exactly what the wide layout
// stores: the same ids, in the same order, for every vertex, whatever the page
// size, the vertex order or the valence.

#include "Geometry/Icosphere.h"
#include "Geometry/PagedMesh.h"
#include "TestUtils.h"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using namespace se;

namespace {

geo::MeshLayout wideLayout(u32 vertexPageShift = 10, u32 trianglePageShift = 12)
{
    return {.vertexPageShift = vertexPageShift, .trianglePageShift = trianglePageShift, .adjacency = geo::AdjacencyLayout::Wide};
}

geo::MeshLayout compactLayout(u32 vertexPageShift = 10, u32 trianglePageShift = 12)
{
    return {.vertexPageShift = vertexPageShift, .trianglePageShift = trianglePageShift, .adjacency = geo::AdjacencyLayout::Compact};
}

template <typename Row>
std::vector<u32> viaRangeFor(const Row& row)
{
    std::vector<u32> ids;
    for (const u32 id : row) {
        ids.push_back(id);
    }
    return ids;
}

template <typename Row>
std::vector<u32> viaForEach(const Row& row)
{
    std::vector<u32> ids;
    row.forEach([&ids](u32 id) { ids.push_back(id); });
    return ids;
}

/// Every row of both tables, read in both ways, matches the wide mesh.
void expectSameAdjacency(const geo::Mesh& wide, const geo::Mesh& compact)
{
    ASSERT_EQ(wide.vertexCount(), compact.vertexCount());
    for (geo::VertexId v = 0; v < wide.vertexCount(); ++v) {
        const std::vector<u32> neighbors = viaRangeFor(wide.neighbors(v));
        ASSERT_EQ(compact.neighbors(v).size(), neighbors.size()) << "vertex " << v;
        ASSERT_EQ(viaRangeFor(compact.neighbors(v)), neighbors) << "neighbours of " << v;
        ASSERT_EQ(viaForEach(compact.neighbors(v)), neighbors) << "neighbours of " << v;
        ASSERT_EQ(viaForEach(wide.neighbors(v)), neighbors) << "neighbours of " << v;

        const std::vector<u32> triangles = viaRangeFor(wide.incidentTriangles(v));
        ASSERT_EQ(compact.incidentTriangles(v).size(), triangles.size()) << "vertex " << v;
        ASSERT_EQ(viaRangeFor(compact.incidentTriangles(v)), triangles) << "triangles of " << v;
        ASSERT_EQ(viaForEach(compact.incidentTriangles(v)), triangles) << "triangles of " << v;
        if (!triangles.empty()) {
            ASSERT_EQ(compact.incidentTriangles(v).front(), triangles.front());
        }
    }
}

/// The icosphere with its vertices and triangles renumbered at random: ids of
/// neighbouring elements are then far apart, the worst case for 16-bit offsets.
geo::Mesh shuffledIcosphere(int level, const geo::MeshLayout& layout, u32 seed)
{
    const geo::Mesh source = geo::makeIcosphere(level, 1.0f, geo::VertexOrder::Spatial, wideLayout());
    std::vector<glm::vec3> positions(source.positions().begin(), source.positions().end());
    std::vector<u32> indices(source.indices().begin(), source.indices().end());

    std::mt19937 rng(seed);
    std::vector<u32> newIndexOf(positions.size());
    std::iota(newIndexOf.begin(), newIndexOf.end(), 0u);
    std::shuffle(newIndexOf.begin(), newIndexOf.end(), rng);
    std::vector<glm::vec3> shuffledPositions(positions.size());
    for (u32 v = 0; v < positions.size(); ++v) {
        shuffledPositions[newIndexOf[v]] = positions[v];
    }
    std::vector<u32> triangleOrder(indices.size() / 3);
    std::iota(triangleOrder.begin(), triangleOrder.end(), 0u);
    std::shuffle(triangleOrder.begin(), triangleOrder.end(), rng);
    std::vector<u32> shuffledIndices;
    shuffledIndices.reserve(indices.size());
    for (const u32 t : triangleOrder) {
        for (u32 corner = 0; corner < 3; ++corner) {
            shuffledIndices.push_back(newIndexOf[indices[t * 3 + corner]]);
        }
    }
    return geo::Mesh(std::move(shuffledPositions), std::move(shuffledIndices), layout);
}

} // namespace

TEST(CompactAdjacency, MatchesWideLayoutForEveryPageSize)
{
    for (const u32 shift : {4u, 6u, 10u, 16u}) {
        SCOPED_TRACE("vertex page shift " + std::to_string(shift));
        const geo::Mesh wide = geo::makeIcosphere(5, 1.0f, geo::VertexOrder::Spatial, wideLayout(shift));
        const geo::Mesh compact = geo::makeIcosphere(5, 1.0f, geo::VertexOrder::Spatial, compactLayout(shift));
        EXPECT_EQ(compact.layout().adjacency, geo::AdjacencyLayout::Compact);
        expectSameAdjacency(wide, compact);
        EXPECT_TRUE(test::bitwiseEqual(wide.normals(), compact.normals()));
        std::string reason;
        EXPECT_TRUE(compact.validate(&reason)) << reason;
    }
}

TEST(CompactAdjacency, IsAboutHalfTheSizeOnASpatiallyOrderedMesh)
{
    const geo::Mesh wide = geo::makeIcosphere(7, 1.0f, geo::VertexOrder::Spatial, wideLayout());
    const geo::Mesh compact = geo::makeIcosphere(7, 1.0f, geo::VertexOrder::Spatial, compactLayout());
    const geo::MeshMemory w = wide.memory();
    const geo::MeshMemory c = compact.memory();
    EXPECT_EQ(c.adjacency, c.adjacencyNeighbors + c.adjacencyIncidentTriangles);
    EXPECT_LT(c.adjacency, w.adjacency * 6 / 10) << "wide " << w.adjacency << " compact " << c.adjacency;
    EXPECT_LT(compact.wideAdjacencyRows(), compact.vertexCount() / 5) << "most rows fit in 16 bits";
    // Everything else is unchanged.
    EXPECT_EQ(c.positions, w.positions);
    EXPECT_EQ(c.normals, w.normals);
    EXPECT_EQ(c.triangles, w.triangles);
    EXPECT_EQ(wide.wideAdjacencyRows(), 0u);
}

TEST(CompactAdjacency, ArbitraryVertexOrderIsExactAndNotMuchLarger)
{
    // Random ids over 163,842 vertices (far more than 16 bits reach): rows
    // mostly fall back to u32 ids, and must still be exact.
    const geo::Mesh wide = shuffledIcosphere(7, wideLayout(), 3);
    const geo::Mesh compact = shuffledIcosphere(7, compactLayout(), 3);
    expectSameAdjacency(wide, compact);
    EXPECT_GT(compact.wideAdjacencyRows(), compact.vertexCount()) << "the test must exercise u32 rows";
    // At most a 16-bit alignment word per u32 row, plus one anchor per page.
    EXPECT_LT(compact.memory().adjacency, wide.memory().adjacency * 11 / 10);

    // Partial normal updates read the incident triangles: same bits either way.
    geo::Mesh wideEdit = shuffledIcosphere(7, wideLayout(), 3);
    geo::Mesh compactEdit = shuffledIcosphere(7, compactLayout(), 3);
    std::vector<geo::VertexId> some(wideEdit.vertexCount() / 3);
    std::iota(some.begin(), some.end(), 0u);
    for (geo::Mesh* mesh : {&wideEdit, &compactEdit}) {
        for (const geo::VertexId v : some) {
            mesh->mutablePositions()[v] *= 1.01f;
        }
        mesh->recomputeNormals(some);
    }
    EXPECT_TRUE(test::bitwiseEqual(wideEdit.normals(), compactEdit.normals()));
}

TEST(CompactAdjacency, HighValenceRowsNearAndFar)
{
    // Two fans of 300 triangles: the hub of the first has neighbours with close
    // ids (16-bit row), the hub of the second neighbours 1,000 ids apart (u32
    // row). Both rows hold more entries than fit in the header's count field.
    // Unused vertices in between have empty rows.
    constexpr u32 kRim = 300;
    constexpr u32 kFarSpacing = 1000;
    std::vector<glm::vec3> positions;
    std::vector<u32> indices;
    const auto addFan = [&](u32 hub, u32 firstRim, u32 spacing, float z) {
        for (u32 i = 0; i < kRim; ++i) {
            const float angle = 6.2831853f * static_cast<float>(i) / kRim;
            const u32 id = firstRim + i * spacing;
            if (positions.size() <= id) {
                positions.resize(id + 1, glm::vec3(0.0f));
            }
            positions[id] = glm::vec3(std::cos(angle), std::sin(angle), z);
        }
        if (positions.size() <= hub) {
            positions.resize(hub + 1, glm::vec3(0.0f));
        }
        positions[hub] = glm::vec3(0.0f, 0.0f, z + 0.5f);
        for (u32 i = 0; i < kRim; ++i) {
            indices.insert(indices.end(), {hub, firstRim + i * spacing, firstRim + ((i + 1) % kRim) * spacing});
        }
    };
    addFan(0, 1, 1, 0.0f);
    const u32 farHub = kRim + 1;
    addFan(farHub, farHub + 1, kFarSpacing, 2.0f);

    std::vector<glm::vec3> positionsCopy = positions;
    std::vector<u32> indicesCopy = indices;
    const geo::Mesh wide(std::move(positionsCopy), std::move(indicesCopy), wideLayout(6));
    const geo::Mesh compact(std::move(positions), std::move(indices), compactLayout(6, 6));
    expectSameAdjacency(wide, compact);

    EXPECT_EQ(compact.neighbors(0).size(), kRim);
    EXPECT_EQ(compact.incidentTriangles(0).size(), kRim);
    EXPECT_EQ(compact.neighbors(farHub).size(), kRim);
    EXPECT_GT(compact.wideAdjacencyRows(), 0u) << "the far fan needs u32 rows";
    const geo::VertexId unused = farHub + 2; // between two rim vertices of the far fan
    EXPECT_TRUE(compact.neighbors(unused).empty());
    EXPECT_TRUE(compact.incidentTriangles(unused).empty());
}

TEST(CompactAdjacency, RowsBehaveLikeSortedRanges)
{
    const geo::Mesh mesh = geo::makeIcosphere(4, 1.0f, geo::VertexOrder::Spatial, compactLayout(4, 4));
    for (geo::VertexId v = 0; v < mesh.vertexCount(); ++v) {
        const geo::NeighborRing ring = mesh.neighbors(v);
        ASSERT_FALSE(ring.empty());
        ASSERT_NE(ring.begin(), ring.end());
        ASSERT_TRUE(std::is_sorted(ring.begin(), ring.end()));
        ASSERT_EQ(static_cast<u32>(std::distance(ring.begin(), ring.end())), ring.size());
        for (const u32 n : ring) {
            ASSERT_TRUE(std::binary_search(mesh.neighbors(n).begin(), mesh.neighbors(n).end(), v));
        }
        const std::vector<u32> copy(ring.begin(), ring.end());
        ASSERT_EQ(copy.size(), ring.size());
    }
    const geo::AdjacencyRow empty;
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(empty.begin(), empty.end());
    u32 calls = 0;
    empty.forEach([&calls](u32) { ++calls; });
    EXPECT_EQ(calls, 0u);
}
