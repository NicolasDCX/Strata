#include "Geometry/Icosphere.h"
#include "Geometry/PagedArray.h"
#include "Geometry/PagedMesh.h"
#include "TestUtils.h"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

using namespace se;

// --- PagedArray ----------------------------------------------------------------------

TEST(PagedArray, AllocatesPagesOnDemand)
{
    geo::PagedArray<int> array(4); // 16 elements per page
    EXPECT_EQ(array.pageCapacity(), 16u);
    EXPECT_EQ(array.pageCount(), 0u);
    EXPECT_TRUE(array.empty());

    array.resize(1);
    EXPECT_EQ(array.pageCount(), 1u);
    array.resize(16);
    EXPECT_EQ(array.pageCount(), 1u) << "a full page needs no second page";
    array.resize(17);
    EXPECT_EQ(array.pageCount(), 2u);
    array.resize(1000);
    EXPECT_EQ(array.pageCount(), 63u);
    EXPECT_EQ(array.size(), 1000u);
    EXPECT_GE(array.memoryBytes(), 63u * 16u * sizeof(int));

    // The last page is partly filled.
    EXPECT_EQ(array.page(61).size(), 16u);
    EXPECT_EQ(array.page(62).size(), 1000u - 62u * 16u);

    array.resize(20);
    EXPECT_EQ(array.pageCount(), 2u) << "shrinking frees unused pages";
    array.resize(0);
    EXPECT_EQ(array.pageCount(), 0u);
}

TEST(PagedArray, IndicesAndAddressesAreStableWhenTheArrayGrows)
{
    geo::PagedArray<int> array(3); // 8 per page
    array.resize(20);
    for (std::size_t i = 0; i < array.size(); ++i) {
        array[i] = static_cast<int>(i * 7);
    }
    const int* const addressOfFive = &array[5];
    const int* const addressOfNineteen = &array[19];

    // Grow by a lot: a std::vector would reallocate and move every element.
    array.resize(50000);
    EXPECT_EQ(&array[5], addressOfFive);
    EXPECT_EQ(&array[19], addressOfNineteen);
    for (std::size_t i = 0; i < 20; ++i) {
        ASSERT_EQ(array[i], static_cast<int>(i * 7)) << "element " << i << " changed";
    }
}

TEST(PagedArray, HandleEncodesPageAndSlot)
{
    geo::PagedArray<u32> array(5); // 32 per page
    array.resize(1000);
    for (u32 i = 0; i < 1000; ++i) {
        array[i] = i;
    }
    for (u32 i = 0; i < 1000; ++i) {
        ASSERT_EQ(array.page(i >> 5)[i & 31u], i);
    }
}

TEST(PagedArray, ViewsBehaveLikeArrays)
{
    geo::PagedArray<int> array(2); // 4 per page: every few elements cross a page
    array.resize(23);
    std::iota(array.view().begin(), array.view().end(), 100);

    const geo::PagedArrayView<int> view = array.view();
    const geo::PagedArrayView<const int> constView = view; // mutable -> const
    EXPECT_EQ(view.size(), 23u);
    EXPECT_EQ(constView[22], 122);
    EXPECT_EQ(std::accumulate(constView.begin(), constView.end(), 0), 100 * 23 + 22 * 23 / 2);
    EXPECT_EQ(constView.end() - constView.begin(), 23);
    EXPECT_EQ(*(constView.begin() + 9), 109);
    EXPECT_TRUE(std::is_sorted(constView.begin(), constView.end()));

    view[7] = -1; // writes through to the array
    EXPECT_EQ(array[7], -1);

    const std::vector<int> copy(constView.begin(), constView.end());
    EXPECT_EQ(copy.size(), 23u);
    EXPECT_EQ(copy[7], -1);
}

TEST(PagedArray, CopiesRangesAcrossPageBoundaries)
{
    geo::PagedArray<int> array(3); // 8 per page
    std::vector<int> reference(100);
    std::iota(reference.begin(), reference.end(), 0);
    array.resize(reference.size());
    array.copyIn(0, reference.size(), reference.data());
    for (std::size_t i = 0; i < reference.size(); ++i) {
        ASSERT_EQ(array[i], reference[i]);
    }

    // Ranges that start mid-page, end mid-page and span several pages.
    for (const auto& [first, count] : {std::pair{0, 100}, std::pair{5, 1}, std::pair{6, 4}, std::pair{13, 50}, std::pair{95, 5}}) {
        std::vector<int> out(static_cast<std::size_t>(count), -1);
        array.copyOut(static_cast<std::size_t>(first), out.size(), out.data());
        ASSERT_TRUE(std::equal(out.begin(), out.end(), reference.begin() + first)) << "range " << first << "+" << count;
    }

    const std::vector<int> patch{-10, -11, -12, -13, -14, -15, -16, -17, -18, -19};
    array.copyIn(11, patch.size(), patch.data()); // crosses the page boundary at 16
    EXPECT_EQ(array[10], 10);
    EXPECT_EQ(array[11], -10);
    EXPECT_EQ(array[20], -19);
    EXPECT_EQ(array[21], 21);
}

TEST(PagedArray, MoveTransfersPagesWithoutCopying)
{
    geo::PagedArray<int> source(4);
    source.resize(100);
    source[42] = 4242;
    const int* const address = &source[42];

    geo::PagedArray<int> moved(std::move(source));
    EXPECT_EQ(&moved[42], address);
    EXPECT_EQ(moved[42], 4242);
    EXPECT_EQ(moved.size(), 100u);

    geo::PagedArray<int> assigned(2);
    assigned.resize(9);
    assigned = std::move(moved);
    EXPECT_EQ(&assigned[42], address);
    EXPECT_EQ(assigned.pageShift(), 4u);
}

// --- PagedMesh -----------------------------------------------------------------------

TEST(PagedMesh, LayoutDeterminesPageCounts)
{
    // Level 3: 642 vertices, 1,280 triangles.
    const geo::MeshLayout layout{.vertexPageShift = 6, .trianglePageShift = 7};
    const geo::Mesh mesh = geo::makeIcosphere(3, 1.0f, geo::VertexOrder::Spatial, layout);

    EXPECT_EQ(mesh.layout().vertexPageCapacity(), 64u);
    EXPECT_EQ(mesh.vertexPageCount(), 11u);    // ceil(642 / 64)
    EXPECT_EQ(mesh.trianglePageCount(), 10u);  // 1280 / 128
    EXPECT_EQ(mesh.positionPage(0).size(), 64u);
    EXPECT_EQ(mesh.positionPage(10).size(), 642u - 640u) << "last vertex page is partly filled";
    EXPECT_EQ(mesh.normalPage(10).size(), 2u);
    EXPECT_EQ(mesh.trianglePage(9).size(), 128u);

    std::string reason;
    EXPECT_TRUE(mesh.validate(&reason)) << reason;

    // Vertex and triangle page sizes are independent of each other.
    const geo::Mesh other = geo::makeIcosphere(3, 1.0f, geo::VertexOrder::Spatial, {.vertexPageShift = 9, .trianglePageShift = 4});
    EXPECT_EQ(other.vertexPageCount(), 2u);
    EXPECT_EQ(other.trianglePageCount(), 80u);
}

TEST(PagedMesh, IdsAddressPageAndSlot)
{
    const geo::Mesh mesh = geo::makeIcosphere(3, 1.0f, geo::VertexOrder::Spatial, {.vertexPageShift = 5, .trianglePageShift = 6});
    const auto positions = mesh.positions();
    const auto normals = mesh.normals();
    for (geo::VertexId v = 0; v < mesh.vertexCount(); ++v) {
        ASSERT_EQ(mesh.vertexPageOf(v), v >> 5);
        ASSERT_EQ(&positions[v], &mesh.positionPage(v >> 5)[v & 31u]);
        ASSERT_EQ(&normals[v], &mesh.normalPage(v >> 5)[v & 31u]);
    }
    for (geo::TriangleId t = 0; t < mesh.triangleCount(); ++t) {
        ASSERT_EQ(&mesh.triangle(t), &mesh.trianglePage(t >> 6)[t & 63u]);
        // The flat index view is the same data.
        for (u32 corner = 0; corner < 3; ++corner) {
            ASSERT_EQ(mesh.indices()[t * 3 + corner], mesh.triangle(t)[corner]);
        }
    }
    EXPECT_EQ(mesh.indices().size(), mesh.triangleCount() * 3u);
}

TEST(PagedMesh, GeometryDoesNotDependOnTheLayout)
{
    // The same mesh in tiny pages and in one page each: identical content.
    const geo::Mesh small = geo::makeIcosphere(4, 1.0f, geo::VertexOrder::Spatial, {.vertexPageShift = 4, .trianglePageShift = 5});
    const geo::Mesh large = geo::makeIcosphere(4, 1.0f, geo::VertexOrder::Spatial, {.vertexPageShift = 16, .trianglePageShift = 16});
    ASSERT_GT(small.vertexPageCount(), 100u);
    ASSERT_EQ(large.vertexPageCount(), 1u);

    EXPECT_TRUE(test::bitwiseEqual(small.positions(), large.positions()));
    EXPECT_TRUE(test::bitwiseEqual(small.normals(), large.normals()));
    EXPECT_TRUE(std::equal(small.indices().begin(), small.indices().end(), large.indices().begin()));
    for (geo::VertexId v = 0; v < small.vertexCount(); ++v) {
        const auto a = small.neighbors(v);
        const auto b = large.neighbors(v);
        ASSERT_TRUE(std::equal(a.begin(), a.end(), b.begin(), b.end())) << "neighbours of " << v;
        const auto ta = small.incidentTriangles(v);
        const auto tb = large.incidentTriangles(v);
        ASSERT_TRUE(std::equal(ta.begin(), ta.end(), tb.begin(), tb.end())) << "triangles of " << v;
    }
    EXPECT_TRUE(small.bounds() == large.bounds());
}

TEST(PagedMesh, AdjacencyCrossesPageBoundaries)
{
    // 16 vertices per page: almost every vertex has neighbours in other pages.
    const geo::Mesh mesh = geo::makeIcosphere(4, 1.0f, geo::VertexOrder::Spatial, {.vertexPageShift = 4, .trianglePageShift = 4});
    u32 crossPageEdges = 0;
    u32 trianglesSpanningPages = 0;
    for (geo::VertexId v = 0; v < mesh.vertexCount(); ++v) {
        const auto ring = mesh.neighbors(v);
        ASSERT_TRUE(ring.size() == 5 || ring.size() == 6);
        ASSERT_TRUE(std::is_sorted(ring.begin(), ring.end()));
        for (const geo::VertexId neighbor : ring) {
            ASSERT_LT(neighbor, mesh.vertexCount());
            crossPageEdges += mesh.vertexPageOf(neighbor) != mesh.vertexPageOf(v) ? 1u : 0u;
            // Symmetric, whichever pages the two ends are in.
            const auto back = mesh.neighbors(neighbor);
            ASSERT_TRUE(std::binary_search(back.begin(), back.end(), v));
        }
        for (const geo::TriangleId t : mesh.incidentTriangles(v)) {
            const geo::Mesh::Triangle& corners = mesh.triangle(t);
            ASSERT_TRUE(std::find(corners.begin(), corners.end(), v) != corners.end());
        }
    }
    for (geo::TriangleId t = 0; t < mesh.triangleCount(); ++t) {
        const geo::Mesh::Triangle& c = mesh.triangle(t);
        trianglesSpanningPages += (mesh.vertexPageOf(c[0]) != mesh.vertexPageOf(c[1]) ||
                                   mesh.vertexPageOf(c[0]) != mesh.vertexPageOf(c[2]))
                                      ? 1u
                                      : 0u;
    }
    EXPECT_GT(crossPageEdges, mesh.vertexCount()) << "the test must actually exercise page boundaries";
    EXPECT_GT(trianglesSpanningPages, mesh.triangleCount() / 4);

    // A vertex shared by triangles in different triangle pages is still one vertex.
    u32 sharedAcrossTrianglePages = 0;
    for (geo::VertexId v = 0; v < mesh.vertexCount(); ++v) {
        const auto triangles = mesh.incidentTriangles(v);
        const u32 firstPage = mesh.trianglePageOf(triangles.front());
        sharedAcrossTrianglePages +=
            std::any_of(triangles.begin(), triangles.end(), [&](geo::TriangleId t) { return mesh.trianglePageOf(t) != firstPage; }) ? 1u : 0u;
    }
    EXPECT_GT(sharedAcrossTrianglePages, 0u);
    EXPECT_EQ(mesh.vertexCount(), geo::icosphereVertexCount(4)) << "no vertex was duplicated";
}

TEST(PagedMesh, HandlesSurviveMovingTheMesh)
{
    geo::Mesh mesh = geo::makeIcosphere(3, 1.0f, geo::VertexOrder::Spatial, {.vertexPageShift = 5, .trianglePageShift = 5});
    const geo::VertexId vertex = 333;
    const glm::vec3* const address = &mesh.positions()[vertex];
    const glm::vec3 value = mesh.positions()[vertex];
    const std::vector<u32> ring(mesh.neighbors(vertex).begin(), mesh.neighbors(vertex).end());

    const geo::Mesh moved = std::move(mesh);
    EXPECT_EQ(&moved.positions()[vertex], address) << "page storage must not be reallocated by a move";
    EXPECT_EQ(moved.positions()[vertex], value);
    EXPECT_TRUE(std::equal(ring.begin(), ring.end(), moved.neighbors(vertex).begin(), moved.neighbors(vertex).end()));
}

TEST(PagedMesh, EditsThroughTheViewLandInTheRightPage)
{
    geo::Mesh mesh = geo::makeIcosphere(3, 1.0f, geo::VertexOrder::Spatial, {.vertexPageShift = 4, .trianglePageShift = 4});
    const geo::VertexId vertex = 16 * 7 + 3; // page 7, slot 3
    mesh.mutablePositions()[vertex] = glm::vec3(9.0f, 8.0f, 7.0f);
    EXPECT_EQ(mesh.positionPage(7)[3], glm::vec3(9.0f, 8.0f, 7.0f));
    EXPECT_NE(mesh.positionPage(7)[2], glm::vec3(9.0f, 8.0f, 7.0f));

    // Normals of a region that spans pages, recomputed partially, match a full recompute.
    std::vector<geo::VertexId> affected{vertex};
    affected.insert(affected.end(), mesh.neighbors(vertex).begin(), mesh.neighbors(vertex).end());
    mesh.recomputeNormals(affected);
    const std::vector<glm::vec3> partial(mesh.normals().begin(), mesh.normals().end());
    mesh.recomputeNormals();
    for (geo::VertexId v = 0; v < mesh.vertexCount(); ++v) {
        ASSERT_NEAR(glm::distance(partial[v], mesh.normals()[v]), 0.0f, 1e-5f) << "vertex " << v;
    }
}

TEST(PagedMesh, PageBoundsAreCachedUntilInvalidated)
{
    geo::Mesh mesh = geo::makeIcosphere(3, 1.0f, geo::VertexOrder::Spatial, {.vertexPageShift = 5, .trianglePageShift = 5});
    const auto bruteForce = [&mesh](u32 page) {
        geo::Aabb bounds;
        for (const glm::vec3& p : mesh.positionPage(page)) {
            bounds.expand(p);
        }
        return bounds;
    };
    geo::Aabb all;
    for (u32 page = 0; page < mesh.vertexPageCount(); ++page) {
        ASSERT_TRUE(mesh.vertexPageBounds(page) == bruteForce(page)) << "page " << page;
        all.expand(mesh.vertexPageBounds(page));
    }
    EXPECT_TRUE(all == mesh.bounds()) << "page bounds together cover exactly the mesh";

    // Spatial order makes pages compact: a page is much smaller than the mesh.
    const glm::vec3 pageExtent = mesh.vertexPageBounds(3).extent();
    EXPECT_LT(std::max({pageExtent.x, pageExtent.y, pageExtent.z}), 1.2f);

    const geo::VertexId vertex = 32 * 4 + 1; // page 4
    mesh.mutablePositions()[vertex] = glm::vec3(5.0f);
    geo::DirtyVertexSet modified = mesh.makeDirtySet();
    modified.mark(vertex);
    mesh.invalidatePageBounds(modified);
    EXPECT_TRUE(mesh.vertexPageBounds(4) == bruteForce(4));
    EXPECT_FLOAT_EQ(mesh.vertexPageBounds(4).max.x, 5.0f);
    EXPECT_TRUE(mesh.vertexPageBounds(3) == bruteForce(3));

    // The modified set may use a different page size than the mesh.
    mesh.mutablePositions()[32 * 9] = glm::vec3(-6.0f);
    geo::DirtyVertexSet coarse(mesh.vertexCount(), 8);
    coarse.mark(32 * 9);
    mesh.invalidatePageBounds(coarse);
    EXPECT_FLOAT_EQ(mesh.vertexPageBounds(9).min.x, -6.0f);
}

TEST(PagedMesh, CopiesIdRangesToContiguousMemory)
{
    const geo::Mesh mesh = geo::makeIcosphere(3, 1.0f, geo::VertexOrder::Spatial, {.vertexPageShift = 4, .trianglePageShift = 4});
    std::vector<glm::vec3> positions(200);
    std::vector<glm::vec3> normals(200);
    mesh.copyPositions(50, 200, positions.data()); // 12.5 pages
    mesh.copyNormals(50, 200, normals.data());
    for (u32 i = 0; i < 200; ++i) {
        ASSERT_EQ(positions[i], mesh.positions()[50 + i]);
        ASSERT_EQ(normals[i], mesh.normals()[50 + i]);
    }
    std::vector<u32> indices(3 * 100);
    mesh.copyIndices(7, 100, indices.data());
    for (u32 i = 0; i < 300; ++i) {
        ASSERT_EQ(indices[i], mesh.indices()[7 * 3 + i]);
    }
}

TEST(PagedMesh, MemoryIsAccountedByKind)
{
    const geo::Mesh mesh = geo::makeIcosphere(5); // 10,242 vertices, 20,480 triangles, default layout
    const geo::MeshMemory memory = mesh.memory();
    EXPECT_EQ(memory.total(), mesh.memoryBytes());
    EXPECT_GE(memory.positions, mesh.vertexCount() * sizeof(glm::vec3));
    EXPECT_GE(memory.normals, mesh.vertexCount() * sizeof(glm::vec3));
    EXPECT_GE(memory.triangles, mesh.triangleCount() * 3u * sizeof(u32));
    EXPECT_GT(memory.adjacency, mesh.triangleCount() * 3u * sizeof(u32)); // two tables
    // Paging overhead (rounding up to whole pages + page tables) stays small.
    const std::size_t payload = mesh.vertexCount() * 24u + mesh.triangleCount() * 12u;
    EXPECT_LT(memory.positions + memory.normals + memory.triangles, payload + payload / 8);

    // Oversized pages waste memory on a small mesh: that is the trade-off.
    const geo::Mesh wasteful = geo::makeIcosphere(2, 1.0f, geo::VertexOrder::Spatial, {.vertexPageShift = 16, .trianglePageShift = 16});
    EXPECT_GE(wasteful.memory().positions, 65536u * sizeof(glm::vec3));
}

TEST(PagedMesh, DirtySetUsesTheVertexPageSize)
{
    const geo::Mesh mesh = geo::makeIcosphere(3, 1.0f, geo::VertexOrder::Spatial, {.vertexPageShift = 6, .trianglePageShift = 6});
    geo::DirtyVertexSet dirty = mesh.makeDirtySet();
    EXPECT_EQ(dirty.pageSize(), 64u);
    EXPECT_EQ(dirty.pageCount(), mesh.vertexPageCount());
    dirty.mark(64 * 3 + 5);
    EXPECT_TRUE(dirty.pageDirty(3));
    EXPECT_EQ(dirty.dirtyVertexCount(), 64u);

    std::vector<u32> pages;
    dirty.mark(641);
    dirty.forEachPage([&pages](u32 page) { pages.push_back(page); });
    EXPECT_EQ(pages, (std::vector<u32>{3, 10}));
}

TEST(PagedMesh, EmptyMesh)
{
    const geo::Mesh mesh;
    EXPECT_EQ(mesh.vertexCount(), 0u);
    EXPECT_EQ(mesh.vertexPageCount(), 0u);
    EXPECT_EQ(mesh.trianglePageCount(), 0u);
    EXPECT_TRUE(mesh.positions().empty());
    EXPECT_EQ(mesh.indices().size(), 0u);
}
