#include "SculptFixture.h"

using namespace se;
using sculpt::BrushId;
using sculpt::BrushSettings;

namespace {

class StrokeTest : public test::SculptFixture {};

const BrushSettings kSettings{.radius = 0.2f, .strength = 0.5f, .invert = false};

} // namespace

TEST_F(StrokeTest, MissingTheMeshDoesNotStartAStroke)
{
    EXPECT_FALSE(stroke.begin(brushes.brush(BrushId::Draw), kSettings, rayAt(3.0f, 3.0f)));
    EXPECT_FALSE(stroke.active());
    EXPECT_FALSE(stroke.end().has_value());
    EXPECT_EQ(movedVertexCount(), 0u);
    geo::DirtyVertexSet dirty(mesh.vertexCount());
    stroke.drainDirty(dirty);
    EXPECT_TRUE(dirty.empty());
}

TEST_F(StrokeTest, UpdateWithoutBeginIsANoOp)
{
    stroke.update(rayAt(0.0f, 0.0f));
    EXPECT_EQ(movedVertexCount(), 0u);
}

TEST_F(StrokeTest, DabCountDependsOnDistanceNotOnEventRate)
{
    // The same 0.4 unit path delivered as 4 coarse or 40 fine pointer events
    // must produce the same number of dabs (within one, from end-point rounding).
    const auto run = [this](int events) {
        geo::Mesh localMesh = geo::makeIcosphere(4);
        geo::ChunkedBvh localBvh;
        localBvh.build(localMesh);
        sculpt::StrokeProcessor local(localMesh, localBvh);
        EXPECT_TRUE(local.begin(brushes.brush(BrushId::Smooth), kSettings, rayAt(-0.2f, 0.0f)));
        for (int i = 1; i <= events; ++i) {
            local.update(rayAt(-0.2f + 0.4f * static_cast<float>(i) / static_cast<float>(events), 0.0f));
        }
        const u32 dabs = local.stats().dabCount;
        local.end();
        return dabs;
    };

    const u32 coarse = run(4);
    const u32 fine = run(40);
    const u32 expected = static_cast<u32>(0.4f / (kSettings.radius * sculpt::StrokeProcessor::kDabSpacing)) + 1;
    EXPECT_NEAR(static_cast<int>(coarse), static_cast<int>(expected), 1);
    EXPECT_NEAR(static_cast<int>(fine), static_cast<int>(coarse), 1);
}

TEST_F(StrokeTest, TinyPointerMovementsDoNotAddDabs)
{
    ASSERT_TRUE(stroke.begin(brushes.brush(BrushId::Draw), kSettings, rayAt(0.0f, 0.0f)));
    for (int i = 0; i < 50; ++i) {
        stroke.update(rayAt(0.0001f * static_cast<float>(i % 2), 0.0f)); // jitter far below the spacing
    }
    EXPECT_EQ(stroke.stats().dabCount, 1u);
    stroke.end();
}

TEST_F(StrokeTest, DirtySetCoversEveryChangedVertex)
{
    ASSERT_TRUE(stroke.begin(brushes.brush(BrushId::Draw), kSettings, rayAt(0.0f, 0.0f)));
    stroke.update(rayAt(0.2f, 0.1f));
    geo::DirtyVertexSet dirty(mesh.vertexCount());
    stroke.drainDirty(dirty);
    stroke.end();

    ASSERT_FALSE(dirty.empty());
    const geo::Mesh pristine = geo::makeIcosphere(4);
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        const bool changed = mesh.positions()[vertex] != pristine.positions()[vertex] ||
                             mesh.normals()[vertex] != pristine.normals()[vertex];
        if (changed) {
            ASSERT_TRUE(dirty.contains(vertex)) << "vertex " << vertex;
        }
    }
    geo::DirtyVertexSet again(mesh.vertexCount());
    stroke.drainDirty(again);
    EXPECT_TRUE(again.empty()) << "draining must clear the pending set";
}

TEST_F(StrokeTest, StrokeRecordsEachVertexOnceWithItsOriginalPosition)
{
    ASSERT_TRUE(stroke.begin(brushes.brush(BrushId::Draw), kSettings, rayAt(0.0f, 0.0f)));
    for (int i = 1; i <= 10; ++i) {
        stroke.update(rayAt(0.03f * static_cast<float>(i), 0.0f)); // overlapping dabs
    }
    const auto delta = stroke.end();
    ASSERT_TRUE(delta.has_value());
    ASSERT_EQ(delta->vertices.size(), delta->before.size());
    ASSERT_EQ(delta->vertices.size(), delta->after.size());

    std::vector<u32> sorted = delta->vertices;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_TRUE(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end()) << "vertex recorded twice";

    for (std::size_t i = 0; i < delta->vertices.size(); ++i) {
        ASSERT_EQ(delta->before[i], original[delta->vertices[i]]);
        ASSERT_EQ(delta->after[i], mesh.positions()[delta->vertices[i]]);
    }
    // Sparse: an undo record must not be a copy of the whole mesh.
    EXPECT_LT(delta->vertices.size(), mesh.vertexCount() / 4);
    EXPECT_EQ(delta->vertices.size(), movedVertexCount());
}

TEST_F(StrokeTest, LeavingAndReenteringTheSurfaceContinuesTheStroke)
{
    ASSERT_TRUE(stroke.begin(brushes.brush(BrushId::Draw), kSettings, rayAt(0.9f, 0.0f)));
    stroke.update(rayAt(2.0f, 0.0f)); // off the mesh
    EXPECT_TRUE(stroke.active());
    const u32 dabsBefore = stroke.stats().dabCount;
    stroke.update(rayAt(0.0f, 0.0f)); // back on: dabs immediately, no line across the gap
    EXPECT_EQ(stroke.stats().dabCount, dabsBefore + 1);
    stroke.end();
}

TEST(StrokeDirtySet, SmallBrushDirtiesAFractionOfALargeMesh)
{
    // 163,842 vertices = 161 pages. One small dab must not mark most of them.
    geo::Mesh mesh = geo::makeIcosphere(7);
    geo::ChunkedBvh bvh;
    bvh.build(mesh);
    sculpt::StrokeProcessor stroke(mesh, bvh);
    const sculpt::BrushLibrary brushes;

    const BrushSettings settings{.radius = 0.1f, .strength = 0.5f, .invert = false};
    ASSERT_TRUE(stroke.begin(brushes.brush(BrushId::Draw), settings, geo::Ray{{0.3f, 0.2f, 5.0f}, {0.0f, 0.0f, -1.0f}}));
    geo::DirtyVertexSet dirty(mesh.vertexCount());
    stroke.drainDirty(dirty);
    stroke.end();

    ASSERT_FALSE(dirty.empty());
    EXPECT_GE(dirty.dirtyVertexCount(), stroke.stats().lastDabVertexCount);
    EXPECT_LT(dirty.dirtyPageCount(), dirty.pageCount() / 4)
        << dirty.dirtyPageCount() << " of " << dirty.pageCount() << " pages dirty";
}