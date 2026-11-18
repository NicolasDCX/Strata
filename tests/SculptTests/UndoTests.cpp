#include "SculptFixture.h"

#include "Sculpt/NormalUpdater.h"
#include "Sculpt/UndoStack.h"

using namespace se;
using sculpt::BrushId;
using sculpt::BrushSettings;

namespace {

class UndoTest : public test::SculptFixture {
protected:
    /// Runs a short stroke and pushes it on the stack. Returns the mesh state after it.
    std::vector<glm::vec3> strokeAt(BrushId brush, float x, float y)
    {
        const BrushSettings settings{.radius = 0.3f, .strength = 0.8f, .invert = false};
        EXPECT_TRUE(stroke.begin(brushes.brush(brush), settings, rayAt(x, y)));
        stroke.update(rayAt(x + 0.15f, y + 0.05f));
        stroke.update(rayAt(x + 0.3f, y));
        auto delta = stroke.end();
        EXPECT_TRUE(delta.has_value());
        undo.push(std::move(*delta));
        return {mesh.positions().begin(), mesh.positions().end()};
    }

    void expectPositions(const std::vector<glm::vec3>& expected) const
    {
        for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
            ASSERT_EQ(mesh.positions()[vertex], expected[vertex]) << "vertex " << vertex;
        }
    }

    void applyAndRefresh(std::span<const u32> vertices)
    {
        geo::DirtyVertexSet dirty(mesh.vertexCount());
        normalUpdater.update(mesh, vertices, dirty);
        bvh.refit(mesh, vertices);
        bvh.optimize(mesh);
    }

    sculpt::UndoStack undo;
    sculpt::NormalUpdater normalUpdater;
};

} // namespace

TEST_F(UndoTest, EmptyStackDoesNothing)
{
    EXPECT_FALSE(undo.canUndo());
    EXPECT_FALSE(undo.canRedo());
    EXPECT_TRUE(undo.undo(mesh).empty());
    EXPECT_TRUE(undo.redo(mesh).empty());
    expectPositions(original);
}

TEST_F(UndoTest, UndoRestoresExactPositionsAndRedoReappliesThem)
{
    const std::vector<glm::vec3> afterStroke = strokeAt(BrushId::Draw, 0.0f, 0.0f);
    ASSERT_NE(afterStroke, original);
    EXPECT_EQ(undo.undoCount(), 1u);

    const auto undone = undo.undo(mesh);
    EXPECT_FALSE(undone.empty());
    expectPositions(original); // bit-exact, not approximately
    EXPECT_TRUE(undo.canRedo());
    EXPECT_FALSE(undo.canUndo());

    const auto redone = undo.redo(mesh);
    EXPECT_EQ(redone.size(), undone.size());
    expectPositions(afterStroke);
}

TEST_F(UndoTest, UndoRestoresNormals)
{
    const std::vector<glm::vec3> originalNormals(mesh.normals().begin(), mesh.normals().end());
    strokeAt(BrushId::Draw, 0.0f, 0.0f);
    applyAndRefresh(undo.undo(mesh));
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        ASSERT_NEAR(glm::distance(mesh.normals()[vertex], originalNormals[vertex]), 0.0f, 1e-6f) << "vertex " << vertex;
    }
}

TEST_F(UndoTest, OneStrokeIsOneUndoStepAcrossAllBrushes)
{
    const std::vector<glm::vec3> s1 = strokeAt(BrushId::Draw, -0.2f, 0.0f);
    const std::vector<glm::vec3> s2 = strokeAt(BrushId::Smooth, -0.1f, 0.1f);
    const std::vector<glm::vec3> s3 = strokeAt(BrushId::Move, 0.0f, -0.1f);
    EXPECT_EQ(undo.undoCount(), 3u);

    applyAndRefresh(undo.undo(mesh));
    expectPositions(s2);
    applyAndRefresh(undo.undo(mesh));
    expectPositions(s1);
    applyAndRefresh(undo.undo(mesh));
    expectPositions(original);
    EXPECT_FALSE(undo.canUndo());

    applyAndRefresh(undo.redo(mesh));
    expectPositions(s1);
    applyAndRefresh(undo.redo(mesh));
    expectPositions(s2);
    applyAndRefresh(undo.redo(mesh));
    expectPositions(s3);
    EXPECT_FALSE(undo.canRedo());
}

TEST_F(UndoTest, NewStrokeDiscardsRedoHistory)
{
    strokeAt(BrushId::Draw, -0.2f, 0.0f);
    strokeAt(BrushId::Draw, 0.1f, 0.2f);
    applyAndRefresh(undo.undo(mesh));
    EXPECT_EQ(undo.redoCount(), 1u);

    strokeAt(BrushId::Draw, 0.0f, -0.3f);
    EXPECT_EQ(undo.redoCount(), 0u);
    EXPECT_EQ(undo.undoCount(), 2u);
}

TEST_F(UndoTest, HistoryIsBoundedAndSparse)
{
    sculpt::UndoStack small(2);
    for (int i = 0; i < 4; ++i) {
        const BrushSettings settings{.radius = 0.2f, .strength = 0.5f, .invert = false};
        ASSERT_TRUE(stroke.begin(brushes.brush(BrushId::Draw), settings, rayAt(0.1f * static_cast<float>(i), 0.0f)));
        small.push(std::move(*stroke.end()));
    }
    EXPECT_EQ(small.undoCount(), 2u);
    // Two small strokes cost far less than one full copy of the positions.
    EXPECT_LT(small.memoryBytes(), mesh.vertexCount() * sizeof(glm::vec3));
    small.clear();
    EXPECT_FALSE(small.canUndo());
}

TEST_F(UndoTest, EmptyDeltaIsIgnored)
{
    undo.push(sculpt::StrokeDelta{});
    EXPECT_FALSE(undo.canUndo());
}
