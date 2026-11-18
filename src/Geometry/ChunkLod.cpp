#include "Geometry/ChunkLod.h"

#include "Core/Assert.h"

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <queue>
#include <utility>

namespace se::geo {

namespace {

/// Plane quadric (Garland-Heckbert), symmetric 4x4 stored as 10 values.
struct Quadric {
    std::array<double, 10> q{};

    static Quadric plane(const glm::dvec3& n, double d, double weight)
    {
        Quadric r;
        r.q = {n.x * n.x, n.x * n.y, n.x * n.z, n.x * d, n.y * n.y, n.y * n.z, n.y * d, n.z * n.z, n.z * d, d * d};
        for (double& v : r.q) {
            v *= weight;
        }
        return r;
    }
    void add(const Quadric& o) noexcept
    {
        for (std::size_t i = 0; i < q.size(); ++i) {
            q[i] += o.q[i];
        }
    }
    [[nodiscard]] double error(const glm::dvec3& p) const noexcept
    {
        return q[0] * p.x * p.x + 2.0 * q[1] * p.x * p.y + 2.0 * q[2] * p.x * p.z + 2.0 * q[3] * p.x +
               q[4] * p.y * p.y + 2.0 * q[5] * p.y * p.z + 2.0 * q[6] * p.y + q[7] * p.z * p.z + 2.0 * q[8] * p.z + q[9];
    }
};

struct GroupResult {
    std::vector<VertexId> vertices;
    std::vector<u16> indices;
};

class GroupSimplifier {
public:
    GroupSimplifier(const Mesh& mesh, u32 first, u32 count, std::span<const u8> sharedVertex)
    {
        const auto positions = mesh.positions();
        // Local vertex numbering.
        m_global.reserve(static_cast<std::size_t>(count) * 3);
        for (u32 t = first; t < first + count; ++t) {
            for (const VertexId v : mesh.triangle(t)) {
                m_global.push_back(v);
            }
        }
        std::sort(m_global.begin(), m_global.end());
        m_global.erase(std::unique(m_global.begin(), m_global.end()), m_global.end());
        const auto local = [this](VertexId v) {
            return static_cast<u32>(std::lower_bound(m_global.begin(), m_global.end(), v) - m_global.begin());
        };
        const std::size_t n = m_global.size();
        m_position.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            m_position[i] = glm::dvec3(positions[m_global[i]]);
        }
        m_tris.resize(count);
        m_alive.assign(count, 1u);
        m_vt.resize(n);
        for (u32 t = 0; t < count; ++t) {
            const auto& tri = mesh.triangle(first + t);
            for (int c = 0; c < 3; ++c) {
                m_tris[t][static_cast<std::size_t>(c)] = local(tri[static_cast<std::size_t>(c)]);
                m_vt[m_tris[t][static_cast<std::size_t>(c)]].push_back(t);
            }
        }
        m_locked.assign(n, 0u);
        for (std::size_t i = 0; i < n; ++i) {
            m_locked[i] = sharedVertex[m_global[i]];
        }
        // Vertices on an edge with a single triangle in the group (open borders) are locked too.
        for (u32 t = 0; t < count; ++t) {
            for (int c = 0; c < 3; ++c) {
                const u32 a = m_tris[t][static_cast<std::size_t>(c)];
                const u32 b = m_tris[t][static_cast<std::size_t>((c + 1) % 3)];
                if (edgeTriangles(a, b) == 1) {
                    m_locked[a] = 1u;
                    m_locked[b] = 1u;
                }
            }
        }
        // Quadrics from the original faces, area weighted.
        m_quadric.resize(n);
        for (u32 t = 0; t < count; ++t) {
            const glm::dvec3 p0 = m_position[m_tris[t][0]];
            const glm::dvec3 cross = glm::cross(m_position[m_tris[t][1]] - p0, m_position[m_tris[t][2]] - p0);
            const double length = glm::length(cross);
            if (length <= 0.0) {
                continue;
            }
            const glm::dvec3 normal = cross / length;
            const Quadric q = Quadric::plane(normal, -glm::dot(normal, p0), 0.5 * length);
            for (const u32 v : m_tris[t]) {
                m_quadric[v].add(q);
            }
        }
        m_version.assign(n, 0u);
        m_aliveCount = count;
    }

    GroupResult run(u32 targetTriangles)
    {
        for (u32 v = 0; v < m_vt.size(); ++v) {
            pushBest(v);
        }
        while (m_aliveCount > targetTriangles && !m_heap.empty()) {
            const Candidate top = m_heap.top();
            m_heap.pop();
            if (top.version != m_version[top.from] || m_vt[top.from].empty()) {
                continue;
            }
            if (!valid(top.from, top.to)) {
                // Neighbourhood changed since it was queued: look again.
                pushBest(top.from);
                continue;
            }
            collapse(top.from, top.to);
        }
        return output();
    }

private:
    struct Candidate {
        double cost;
        u32 from;
        u32 to;
        u32 version;
        bool operator<(const Candidate& o) const noexcept { return cost > o.cost; } // min-heap
    };

    [[nodiscard]] bool contains(u32 t, u32 v) const noexcept
    {
        return m_tris[t][0] == v || m_tris[t][1] == v || m_tris[t][2] == v;
    }

    [[nodiscard]] u32 edgeTriangles(u32 a, u32 b) const noexcept
    {
        u32 n = 0;
        for (const u32 t : m_vt[a]) {
            n += contains(t, b) ? 1u : 0u;
        }
        return n;
    }

    void neighbours(u32 v, std::vector<u32>& out) const
    {
        out.clear();
        for (const u32 t : m_vt[v]) {
            for (const u32 w : m_tris[t]) {
                if (w != v) {
                    out.push_back(w);
                }
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    [[nodiscard]] bool valid(u32 from, u32 to)
    {
        if (m_locked[from] != 0u || m_vt[from].empty() || m_vt[to].empty()) {
            return false;
        }
        // Triangles that would be deleted: exactly the two around edge (from, to).
        u32 removed = 0;
        for (const u32 t : m_vt[from]) {
            if (!contains(t, to)) {
                continue;
            }
            ++removed;
            for (const u32 w : m_tris[t]) {
                // The edge (to, w) of a deleted triangle must not be a border edge.
                if (w != from && w != to && edgeTriangles(to, w) < 2) {
                    return false;
                }
            }
        }
        if (removed != 2) {
            return false;
        }
        // Link condition: the two endpoints share exactly the two opposite vertices.
        neighbours(from, m_scratchA);
        neighbours(to, m_scratchB);
        u32 common = 0;
        std::size_t i = 0;
        std::size_t j = 0;
        while (i < m_scratchA.size() && j < m_scratchB.size()) {
            if (m_scratchA[i] < m_scratchB[j]) {
                ++i;
            } else if (m_scratchB[j] < m_scratchA[i]) {
                ++j;
            } else {
                ++common;
                ++i;
                ++j;
            }
        }
        if (common != 2) {
            return false;
        }
        // No flipped or degenerate triangle.
        const glm::dvec3 target = m_position[to];
        for (const u32 t : m_vt[from]) {
            if (contains(t, to)) {
                continue;
            }
            std::array<glm::dvec3, 3> p{};
            std::array<glm::dvec3, 3> q{};
            for (std::size_t c = 0; c < 3; ++c) {
                p[c] = m_position[m_tris[t][c]];
                q[c] = m_tris[t][c] == from ? target : p[c];
            }
            const glm::dvec3 before = glm::cross(p[1] - p[0], p[2] - p[0]);
            const glm::dvec3 after = glm::cross(q[1] - q[0], q[2] - q[0]);
            const double lb = glm::length(before);
            const double la = glm::length(after);
            if (la <= 1e-14 || lb <= 0.0 || glm::dot(before, after) < 0.3 * la * lb) {
                return false;
            }
        }
        return true;
    }

    /// Queues the cheapest valid collapse of `v`; older entries for `v` become stale.
    void pushBest(u32 v)
    {
        ++m_version[v];
        if (m_locked[v] != 0u || m_vt[v].empty()) {
            return;
        }
        neighbours(v, m_scratchC);
        m_costs.clear();
        for (const u32 u : m_scratchC) {
            Quadric merged = m_quadric[v];
            merged.add(m_quadric[u]);
            // Small length term keeps the result well shaped on flat areas.
            const glm::dvec3 d = m_position[u] - m_position[v];
            m_costs.emplace_back(merged.error(m_position[u]) + 1e-3 * glm::dot(d, d), u);
        }
        std::sort(m_costs.begin(), m_costs.end());
        // Validity only for the cheapest candidates, in order.
        for (const auto& [cost, u] : m_costs) {
            if (valid(v, u)) {
                m_heap.push({cost, v, u, m_version[v]});
                return;
            }
        }
    }

    void collapse(u32 from, u32 to)
    {
        for (const u32 t : m_vt[from]) {
            if (contains(t, to)) {
                m_alive[t] = 0u;
                --m_aliveCount;
                for (const u32 w : m_tris[t]) {
                    if (w != from) {
                        auto& list = m_vt[w];
                        list.erase(std::find(list.begin(), list.end(), t));
                    }
                }
            } else {
                for (u32& w : m_tris[t]) {
                    if (w == from) {
                        w = to;
                    }
                }
                m_vt[to].push_back(t);
            }
        }
        m_vt[from].clear();
        ++m_version[from];
        m_quadric[to].add(m_quadric[from]);
        // Costs and validity around the merged vertex changed.
        neighbours(to, m_scratchC);
        const std::vector<u32> touched = m_scratchC;
        pushBest(to);
        for (const u32 w : touched) {
            pushBest(w);
        }
    }

    GroupResult output() const
    {
        GroupResult result;
        std::vector<u32> remap(m_vt.size(), ~0u);
        for (u32 v = 0; v < m_vt.size(); ++v) {
            if (!m_vt[v].empty()) {
                remap[v] = static_cast<u32>(result.vertices.size());
                result.vertices.push_back(m_global[v]);
            }
        }
        SE_ASSERT(result.vertices.size() <= 0xFFFFu);
        for (std::size_t t = 0; t < m_tris.size(); ++t) {
            if (m_alive[t] != 0u) {
                for (const u32 v : m_tris[t]) {
                    result.indices.push_back(static_cast<u16>(remap[v]));
                }
            }
        }
        return result;
    }

    std::vector<VertexId> m_global;
    std::vector<glm::dvec3> m_position;
    std::vector<std::array<u32, 3>> m_tris;
    std::vector<u8> m_alive;
    std::vector<std::vector<u32>> m_vt;
    std::vector<u8> m_locked;
    std::vector<Quadric> m_quadric;
    std::vector<u32> m_version;
    std::priority_queue<Candidate> m_heap;
    std::vector<u32> m_scratchA;
    std::vector<u32> m_scratchB;
    std::vector<u32> m_scratchC;
    std::vector<std::pair<double, u32>> m_costs;
    u32 m_aliveCount = 0;
};

} // namespace

CoarseChunks buildCoarseChunks(const Mesh& mesh, u32 chunkTriangleShift, u32 groupShift, float targetRatio,
                               core::JobSystem* jobs)
{
    const u32 triangleCount = mesh.triangleCount();
    const u32 unitShift = chunkTriangleShift + groupShift; // triangles per group, log2
    SE_ASSERT(unitShift < 32u);
    const u32 capacity = 1u << unitShift;
    const u32 groupTotal = static_cast<u32>((static_cast<u64>(triangleCount) + capacity - 1u) >> unitShift);

    // Vertices referenced by more than one group.
    std::vector<u32> owner(mesh.vertexCount(), ~0u);
    std::vector<u8> shared(mesh.vertexCount(), 0u);
    for (u32 t = 0; t < triangleCount; ++t) {
        const u32 group = t >> unitShift;
        for (const VertexId v : mesh.triangle(t)) {
            if (owner[v] == ~0u) {
                owner[v] = group;
            } else if (owner[v] != group) {
                shared[v] = 1u;
            }
        }
    }

    std::vector<GroupResult> results(groupTotal);
    core::parallelFor(jobs, groupTotal, 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t group = begin; group < end; ++group) {
            const u32 first = static_cast<u32>(group) << unitShift;
            const u32 count = std::min(capacity, triangleCount - first);
            const u32 target = std::max(1u, static_cast<u32>(static_cast<float>(count) * targetRatio));
            GroupSimplifier simplifier(mesh, first, count, shared);
            results[group] = simplifier.run(target);
        }
    });

    CoarseChunks coarse;
    coarse.chunkShift = chunkTriangleShift;
    coarse.groupShift = groupShift;
    coarse.vertexOffsets.assign(static_cast<std::size_t>(groupTotal) + 1, 0u);
    coarse.indexOffsets.assign(static_cast<std::size_t>(groupTotal) + 1, 0u);
    for (u32 group = 0; group < groupTotal; ++group) {
        const GroupResult& r = results[group];
        coarse.vertices.insert(coarse.vertices.end(), r.vertices.begin(), r.vertices.end());
        coarse.indices.insert(coarse.indices.end(), r.indices.begin(), r.indices.end());
        coarse.vertexOffsets[group + 1] = static_cast<u32>(coarse.vertices.size());
        coarse.indexOffsets[group + 1] = static_cast<u32>(coarse.indices.size());
        coarse.maxGroupVertices = std::max(coarse.maxGroupVertices, static_cast<u32>(r.vertices.size()));
    }
    return coarse;
}

} // namespace se::geo
