#pragma once

#include "Core/Types.h"

#include <glm/vec3.hpp>

#include <vector>

namespace se::geo {

/// Reorders vertices, and then triangles, by recursive median splits (k-d tree
/// leaf order) so that elements close in space are close in memory and in
/// index, and so that every aligned power-of-two run of ids (a vertex page, a
/// spatial chunk, of any size) is the content of one compact box.
///
/// The surface is unchanged: the same triangles with the same winding, only
/// renumbered. What improves is locality: a brush touches vertices from a few
/// index ranges instead of from all over the arrays, which means fewer dirty
/// pages to upload and fewer cache misses in the brush loops.
///
/// Intended for mesh creation and import; O(n log n).
void sortSpatially(std::vector<glm::vec3>& positions, std::vector<u32>& indices);

} // namespace se::geo
