#include "Sculpt/BrushLibrary.h"

#include "Core/Assert.h"
#include "Sculpt/Brushes/DrawBrush.h"
#include "Sculpt/Brushes/MoveBrush.h"
#include "Sculpt/Brushes/SmoothBrush.h"

#include <utility>

namespace se::sculpt {

BrushLibrary::BrushLibrary()
{
    // Order must match the built-in BrushId values.
    add(std::make_unique<DrawBrush>(), BrushSettings{.radius = 0.25f, .strength = 0.5f, .invert = false});
    add(std::make_unique<SmoothBrush>(), BrushSettings{.radius = 0.25f, .strength = 0.5f, .invert = false});
    add(std::make_unique<MoveBrush>(), BrushSettings{.radius = 0.40f, .strength = 1.0f, .invert = false});
}

BrushId BrushLibrary::add(std::unique_ptr<Brush> brush, const BrushSettings& defaults)
{
    SE_ASSERT(brush != nullptr);
    m_entries.push_back(Entry{std::move(brush), defaults});
    return static_cast<BrushId>(m_entries.size() - 1);
}

const Brush& BrushLibrary::brush(BrushId id) const
{
    SE_ASSERT(contains(id));
    return *m_entries[static_cast<std::size_t>(id)].brush;
}

BrushSettings& BrushLibrary::settings(BrushId id)
{
    SE_ASSERT(contains(id));
    return m_entries[static_cast<std::size_t>(id)].settings;
}

const BrushSettings& BrushLibrary::settings(BrushId id) const
{
    SE_ASSERT(contains(id));
    return m_entries[static_cast<std::size_t>(id)].settings;
}

} // namespace se::sculpt
