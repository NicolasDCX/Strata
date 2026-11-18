#pragma once

#include "Geometry/ChunkedBvh.h"
#include "Geometry/Icosphere.h"
#include "Geometry/Mesh.h"
#include "Sculpt/BrushLibrary.h"
#include "Sculpt/StrokeProcessor.h"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <vector>

namespace se::test {

/// Unit sphere + BVH + stroke processor, with a ray helper aimed at the +Z pole.
class SculptFixture : public ::testing::Test {
protected:
    explicit SculptFixture(int subdivisions = 4)
        : mesh(geo::makeIcosphere(subdivisions)), stroke(mesh, bvh)
    {
        bvh.build(mesh);
        original.assign(mesh.positions().begin(), mesh.positions().end());
    }

    /// Ray parallel to -Z through (x, y): hits the sphere near the +Z pole.
    static geo::Ray rayAt(float x, float y) { return geo::Ray{{x, y, 5.0f}, {0.0f, 0.0f, -1.0f}}; }

    [[nodiscard]] u32 closestVertexTo(const glm::vec3& point) const
    {
        u32 best = 0;
        float bestDistance = 1e30f;
        for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
            const float d = glm::distance(original[vertex], point);
            if (d < bestDistance) {
                bestDistance = d;
                best = vertex;
            }
        }
        return best;
    }

    [[nodiscard]] u32 movedVertexCount() const
    {
        u32 count = 0;
        for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
            count += mesh.positions()[vertex] != original[vertex] ? 1u : 0u;
        }
        return count;
    }

    geo::Mesh mesh;
    geo::ChunkedBvh bvh;
    sculpt::StrokeProcessor stroke;
    sculpt::BrushLibrary brushes;
    std::vector<glm::vec3> original;
};

} // namespace se::test
