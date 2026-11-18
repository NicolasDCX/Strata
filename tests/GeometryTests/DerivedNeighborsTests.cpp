// Derived neighbours (NeighborStorage::Derived) must give back exactly the
// stored rows: the same ids in the same ascending order, for every vertex,
// whatever the adjacency layout, page size, vertex order or valence.

#include "Geometry/Icosphere.h"
#include "Geometry/PagedMesh.h"
#include "TestUtils.h"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <vector>

using namespace se;

namespace {

geo::MeshLayout layoutOf(geo::AdjacencyLayout adjacency, geo::NeighborStorage neighbors, u32 vertexPageShift = 10,
                         u32 trianglePageShift = 12)
{
    return {.vertexPageShift = vertexPageShift, .trianglePageShift = trianglePageShift, .adjacency = adjacency, .neighbors = neighbors};
}

std::vector<u32> viaRangeFor(const geo::NeighborRing& ring)
{
    std::vector<u32> ids;
    for (const u32 id : ring) {
        ids.push_back(id);
    }
    return ids;
}

std::vector<u32> viaForEachNeighbor(const geo::Mesh& mesh, geo::VertexId vertex)
{
    std::vector<u32> ids;
    const u32 count = mesh.forEachNeighbor(vertex, [&ids](u32 id) { ids.push_back(id); });
    EXPECT_EQ(count, ids.size());
    return ids;
}

void expectSameNeighbors(const geo::Mesh& stored, const geo::Mesh& derived)
{
    ASSERT_TRUE(stored.storesNeighbors());
    ASSERT_FALSE(derived.storesNeighbors());
    ASSERT_EQ(stored.vertexCount(), derived.vertexCount());
    for (geo::VertexId v = 0; v < stored.vertexCount(); ++v) {
        const std::vector<u32> expected = viaRangeFor(stored.neighbors(v));
        ASSERT_EQ(viaForEachNeighbor(stored, v), expected) << "vertex " << v;
        ASSERT_EQ(viaForEachNeighbor(derived, v), expected) << "vertex " << v;
        const geo::NeighborRing ring = derived.neighbors(v);
        ASSERT_EQ(ring.size(), expected.size()) << "vertex " << v;
        ASSERT_EQ(viaRangeFor(ring), expected) << "vertex " << v;
        // The ring is a value: a copy reads the same after the mesh is read again.
        const geo::NeighborRing copy = ring;
        (void)derived.neighbors((v + 1) % derived.vertexCount());
        ASSERT_EQ(viaRangeFor(copy), expected) << "vertex " << v;
    }
    EXPECT_TRUE(test::bitwiseEqual(stored.normals(), derived.normals())) << "normals do not read neighbours";
    // validate() does not read neighbours: both must agree (true for every
    // mesh here except the deliberately broken one).
    EXPECT_EQ(stored.validate(), derived.validate());
}

/// The icosphere with its vertices renumbered at random.
geo::Mesh shuffledIcosphere(int level, const geo::MeshLayout& layout, u32 seed)
{
    const geo::Mesh source = geo::makeIcosphere(level, 1.0f, geo::VertexOrder::Spatial, layout);
    std::vector<u32> order(source.vertexCount());
    std::iota(order.begin(), order.end(), 0u);
    std::mt19937 rng(seed);
    std::shuffle(order.begin(), order.end(), rng);
    std::vector<glm::vec3> positions(source.vertexCount());
    for (u32 v = 0; v < source.vertexCount(); ++v) {
        positions[order[v]] = source.positions()[v];
    }
    std::vector<u32> indices;
    for (const u32 index : source.indices()) {
        indices.push_back(order[index]);
    }
    return geo::Mesh(std::move(positions), std::move(indices), layout);
}

} // namespace

TEST(DerivedNeighbors, MatchStoredRowsForEveryLayoutAndPageSize)
{
    for (const geo::AdjacencyLayout adjacency : {geo::AdjacencyLayout::Compact, geo::AdjacencyLayout::Wide}) {
        for (const u32 shift : {4u, 6u, 10u, 16u}) {
            const geo::Mesh stored = geo::makeIcosphere(5, 1.0f, geo::VertexOrder::Spatial,
                                                        layoutOf(adjacency, geo::NeighborStorage::Stored, shift, shift + 1));
            const geo::Mesh derived = geo::makeIcosphere(5, 1.0f, geo::VertexOrder::Spatial,
                                                         layoutOf(adjacency, geo::NeighborStorage::Derived, shift, shift + 1));
            SCOPED_TRACE(testing::Message() << "page shift " << shift);
            expectSameNeighbors(stored, derived);
            EXPECT_TRUE(derived.validate());
        }
    }
}

TEST(DerivedNeighbors, MatchStoredRowsInArbitraryVertexOrder)
{
    const geo::Mesh stored = shuffledIcosphere(6, layoutOf(geo::AdjacencyLayout::Compact, geo::NeighborStorage::Stored), 7);
    const geo::Mesh derived = shuffledIcosphere(6, layoutOf(geo::AdjacencyLayout::Compact, geo::NeighborStorage::Derived), 7);
    ASSERT_GT(derived.wideAdjacencyRows(), 0u) << "the test must exercise u32 rows";
    expectSameNeighbors(stored, derived);
}

TEST(DerivedNeighbors, HighValenceAndIsolatedVertices)
{
    // A closed fan of 300 triangles around vertex 0 (600 candidate corners,
    // more than the stack buffer), one isolated vertex, and a lone triangle.
    constexpr u32 kRim = 300;
    std::vector<glm::vec3> positions{{0, 0, 0}};
    std::vector<u32> indices;
    for (u32 i = 0; i < kRim; ++i) {
        const float angle = 6.2831853f * static_cast<float>(i) / kRim;
        positions.push_back({std::cos(angle), std::sin(angle), 0.0f});
    }
    for (u32 i = 0; i < kRim; ++i) {
        indices.insert(indices.end(), {0u, 1 + i, 1 + (i + 1) % kRim});
    }
    const u32 isolated = static_cast<u32>(positions.size());
    positions.push_back({5, 5, 5});
    const u32 lone = static_cast<u32>(positions.size());
    positions.insert(positions.end(), {{3, 0, 0}, {4, 0, 0}, {3, 1, 0}});
    indices.insert(indices.end(), {lone, lone + 1, lone + 2});

    const geo::Mesh stored(positions, indices, layoutOf(geo::AdjacencyLayout::Compact, geo::NeighborStorage::Stored, 6, 6));
    const geo::Mesh derived(positions, indices, layoutOf(geo::AdjacencyLayout::Compact, geo::NeighborStorage::Derived, 6, 6));
    expectSameNeighbors(stored, derived);
    EXPECT_EQ(derived.neighbors(0).size(), kRim);
    EXPECT_TRUE(derived.neighbors(isolated).empty());
    EXPECT_EQ(derived.forEachNeighbor(isolated, [](u32) {}), 0u);
    EXPECT_EQ(viaRangeFor(derived.neighbors(lone)), (std::vector<u32>{lone + 1, lone + 2}));
}

TEST(DerivedNeighbors, StoreNoNeighbourTable)
{
    for (const geo::AdjacencyLayout adjacency : {geo::AdjacencyLayout::Compact, geo::AdjacencyLayout::Wide}) {
        const geo::Mesh stored = geo::makeIcosphere(6, 1.0f, geo::VertexOrder::Spatial, layoutOf(adjacency, geo::NeighborStorage::Stored));
        const geo::Mesh derived = geo::makeIcosphere(6, 1.0f, geo::VertexOrder::Spatial, layoutOf(adjacency, geo::NeighborStorage::Derived));
        const geo::MeshMemory s = stored.memory();
        const geo::MeshMemory d = derived.memory();
        // Left of the neighbour table: one bit per vertex (regular fans).
        EXPECT_GT(s.adjacencyNeighbors, derived.vertexCount() * 4u);
        EXPECT_LE(d.adjacencyNeighbors, (derived.vertexCount() + 63u) / 64u * 8u);
        EXPECT_EQ(d.adjacencyIncidentTriangles, s.adjacencyIncidentTriangles);
        EXPECT_EQ(d.total(), s.total() - s.adjacencyNeighbors + d.adjacencyNeighbors);
    }
}

TEST(DerivedNeighbors, IrregularFansTakeTheGeneralPath)
{
    // A closed mesh with some triangles wound the wrong way, one degenerate
    // triangle (a corner repeated) and two octahedra sharing one vertex: the
    // fast derivation must not be used where it would be wrong.
    const geo::Mesh source = geo::makeIcosphere(3);
    std::vector<glm::vec3> positions(source.positions().begin(), source.positions().end());
    std::vector<u32> indices(source.indices().begin(), source.indices().end());
    for (std::size_t t = 0; t < indices.size() / 3; t += 17) {
        std::swap(indices[t * 3 + 1], indices[t * 3 + 2]);
    }
    indices.insert(indices.end(), {5u, 5u, 9u});
    const u32 base = static_cast<u32>(positions.size());
    const glm::vec3 octahedron[] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const u32 faces[8][3] = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4}, {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
    // Second octahedron shares the first one's vertex 0 (as its vertex 1).
    for (const glm::vec3& p : octahedron) {
        positions.push_back(p + glm::vec3(4, 0, 0));
    }
    for (const glm::vec3& p : octahedron) {
        positions.push_back(p + glm::vec3(6, 0, 0));
    }
    const auto second = [&](u32 corner) { return corner == 1 ? base : base + 6 + corner; };
    for (const auto& f : faces) {
        indices.insert(indices.end(), {base + f[0], base + f[1], base + f[2]});
        indices.insert(indices.end(), {second(f[0]), second(f[1]), second(f[2])});
    }

    for (const geo::AdjacencyLayout adjacency : {geo::AdjacencyLayout::Compact, geo::AdjacencyLayout::Wide}) {
        const geo::Mesh stored(positions, indices, layoutOf(adjacency, geo::NeighborStorage::Stored, 5, 6));
        const geo::Mesh derived(positions, indices, layoutOf(adjacency, geo::NeighborStorage::Derived, 5, 6));
        expectSameNeighbors(stored, derived);
        EXPECT_FALSE(derived.validate()) << "the degenerate triangle should be reported";
        EXPECT_EQ(derived.neighbors(base).size(), 8u) << "the shared vertex sees both octahedra";
    }
}
