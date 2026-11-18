#pragma once

#include "Sculpt/Brush.h"

#include <memory>
#include <vector>

namespace se::sculpt {

/// Identifies a brush inside a BrushLibrary.
enum class BrushId : u32 {
    Draw = 0,
    Smooth = 1,
    Move = 2,
};

/// Owns the brush instances and their per-brush settings (each brush remembers
/// its own radius and strength, as sculpting applications usually do).
///
/// Extension point: add() registers additional brushes at runtime and returns
/// their id; nothing else in the engine needs to know the concrete type.
class BrushLibrary {
public:
    /// Creates the library with the three built-in brushes.
    BrushLibrary();

    BrushId add(std::unique_ptr<Brush> brush, const BrushSettings& defaults);

    [[nodiscard]] std::size_t size() const noexcept { return m_entries.size(); }
    [[nodiscard]] bool contains(BrushId id) const noexcept { return static_cast<std::size_t>(id) < m_entries.size(); }

    [[nodiscard]] const Brush& brush(BrushId id) const;
    [[nodiscard]] BrushSettings& settings(BrushId id);
    [[nodiscard]] const BrushSettings& settings(BrushId id) const;

private:
    struct Entry {
        std::unique_ptr<Brush> brush;
        BrushSettings settings;
    };
    std::vector<Entry> m_entries;
};

} // namespace se::sculpt
