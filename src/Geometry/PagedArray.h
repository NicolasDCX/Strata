#pragma once

#include "Core/Assert.h"
#include "Core/Types.h"

#include <algorithm>
#include <compare>
#include <concepts>
#include <cstring>
#include <iterator>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace se::geo {

/// Random-access iterator over any small, copyable "view" type that offers
/// operator[](std::size_t). It stores a copy of the view, so iterators obtained
/// from a temporary view (`mesh.positions().begin()`) stay valid.
template <typename View>
class ViewIterator {
public:
    using iterator_category = std::random_access_iterator_tag;
    using reference = decltype(std::declval<const View&>()[std::size_t{0}]);
    using value_type = std::remove_cvref_t<reference>;
    using difference_type = std::ptrdiff_t;
    using pointer = std::remove_reference_t<reference>*;

    ViewIterator() = default;
    ViewIterator(const View& view, std::size_t index) : m_view(view), m_index(index) {}

    reference operator*() const { return m_view[m_index]; }
    pointer operator->() const { return &m_view[m_index]; }
    reference operator[](difference_type offset) const
    {
        return m_view[static_cast<std::size_t>(static_cast<difference_type>(m_index) + offset)];
    }

    ViewIterator& operator++() { ++m_index; return *this; }
    ViewIterator operator++(int) { ViewIterator copy = *this; ++m_index; return copy; }
    ViewIterator& operator--() { --m_index; return *this; }
    ViewIterator operator--(int) { ViewIterator copy = *this; --m_index; return copy; }
    ViewIterator& operator+=(difference_type offset)
    {
        m_index = static_cast<std::size_t>(static_cast<difference_type>(m_index) + offset);
        return *this;
    }
    ViewIterator& operator-=(difference_type offset) { return *this += -offset; }
    friend ViewIterator operator+(ViewIterator it, difference_type offset) { return it += offset; }
    friend ViewIterator operator+(difference_type offset, ViewIterator it) { return it += offset; }
    friend ViewIterator operator-(ViewIterator it, difference_type offset) { return it -= offset; }
    friend difference_type operator-(const ViewIterator& a, const ViewIterator& b)
    {
        return static_cast<difference_type>(a.m_index) - static_cast<difference_type>(b.m_index);
    }
    friend bool operator==(const ViewIterator& a, const ViewIterator& b) { return a.m_index == b.m_index; }
    friend std::strong_ordering operator<=>(const ViewIterator& a, const ViewIterator& b)
    {
        return a.m_index <=> b.m_index;
    }

private:
    View m_view{};
    std::size_t m_index = 0;
};

/// Non-owning, array-like access to the elements of a PagedArray. `T` may be
/// const. Indexing costs one extra load compared with a flat array (the page
/// pointer), which stays cached because the page table is small.
///
/// A view is invalidated when its PagedArray grows or is destroyed. Element
/// addresses and indices are not: see PagedArray.
template <typename T>
class PagedArrayView {
public:
    using value_type = std::remove_const_t<T>;
    using iterator = ViewIterator<PagedArrayView>;

    PagedArrayView() = default;
    PagedArrayView(T* const* table, u32 pageShift, std::size_t size) noexcept
        : m_table(table), m_shift(pageShift), m_mask((std::size_t{1} << pageShift) - 1), m_size(size)
    {
    }

    /// View of mutable elements -> view of const elements.
    template <typename U>
        requires(std::is_const_v<T> && std::same_as<U, std::remove_const_t<T>>)
    PagedArrayView(const PagedArrayView<U>& other) noexcept // NOLINT(google-explicit-constructor)
        : m_table(other.table()), m_shift(other.pageShift()), m_mask((std::size_t{1} << other.pageShift()) - 1),
          m_size(other.size())
    {
    }

    [[nodiscard]] T& operator[](std::size_t index) const noexcept { return m_table[index >> m_shift][index & m_mask]; }

    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
    [[nodiscard]] u32 pageShift() const noexcept { return m_shift; }
    [[nodiscard]] T* const* table() const noexcept { return m_table; }

    [[nodiscard]] iterator begin() const { return iterator(*this, 0); }
    [[nodiscard]] iterator end() const { return iterator(*this, m_size); }

private:
    T* const* m_table = nullptr;
    u32 m_shift = 0;
    std::size_t m_mask = 0;
    std::size_t m_size = 0;
};

/// Array stored as fixed-capacity pages, each its own allocation.
///
/// Why pages instead of one std::vector:
/// - Growing allocates new pages and never moves existing elements, so an
///   element's index is a stable handle and its address never changes.
/// - Memory is acquired in page-sized steps; nothing ever needs one contiguous
///   block the size of the whole mesh.
/// - A page is a natural unit for dirty tracking, bounds, GPU upload and, later,
///   for loading and evicting parts of a mesh.
///
/// The page capacity is 2^pageShift elements, chosen per array. Elements are
/// not initialised when pages are allocated (T must be trivially copyable).
template <typename T>
class PagedArray {
    static_assert(std::is_trivially_copyable_v<T>, "pages are copied with memcpy and left uninitialised");

public:
    PagedArray() = default;
    explicit PagedArray(u32 pageShift) : m_shift(pageShift) { SE_ASSERT(pageShift < 31); }
    ~PagedArray() { release(); }

    PagedArray(PagedArray&& other) noexcept
        : m_table(std::move(other.m_table)), m_shift(other.m_shift), m_size(std::exchange(other.m_size, 0))
    {
        other.m_table.clear();
    }
    PagedArray& operator=(PagedArray&& other) noexcept
    {
        if (this != &other) {
            release();
            m_table = std::move(other.m_table);
            other.m_table.clear();
            m_shift = other.m_shift;
            m_size = std::exchange(other.m_size, 0);
        }
        return *this;
    }
    PagedArray(const PagedArray&) = delete;
    PagedArray& operator=(const PagedArray&) = delete;

    /// Frees everything and sets a new page capacity.
    void reset(u32 pageShift)
    {
        SE_ASSERT(pageShift < 31);
        release();
        m_shift = pageShift;
    }

    /// Sets the element count. Growing allocates pages; shrinking frees the pages
    /// that are no longer needed. Existing elements keep their index and address.
    void resize(std::size_t count)
    {
        const std::size_t needed = (count + pageCapacity() - 1) >> m_shift;
        while (m_table.size() < needed) {
            m_table.push_back(new T[pageCapacity()]);
        }
        while (m_table.size() > needed) {
            delete[] m_table.back();
            m_table.pop_back();
        }
        m_size = count;
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
    [[nodiscard]] u32 pageShift() const noexcept { return m_shift; }
    [[nodiscard]] std::size_t pageCapacity() const noexcept { return std::size_t{1} << m_shift; }
    [[nodiscard]] std::size_t pageCount() const noexcept { return m_table.size(); }

    [[nodiscard]] T& operator[](std::size_t index) noexcept { return m_table[index >> m_shift][index & (pageCapacity() - 1)]; }
    [[nodiscard]] const T& operator[](std::size_t index) const noexcept
    {
        return m_table[index >> m_shift][index & (pageCapacity() - 1)];
    }

    /// The valid elements of one page (the last page may be partly filled).
    [[nodiscard]] std::span<T> page(std::size_t pageIndex) noexcept { return {m_table[pageIndex], pageSize(pageIndex)}; }
    [[nodiscard]] std::span<const T> page(std::size_t pageIndex) const noexcept
    {
        return {m_table[pageIndex], pageSize(pageIndex)};
    }

    [[nodiscard]] PagedArrayView<T> view() noexcept { return {m_table.data(), m_shift, m_size}; }
    [[nodiscard]] PagedArrayView<const T> view() const noexcept { return {m_table.data(), m_shift, m_size}; }

    /// Copies elements [first, first + count) to contiguous memory, page by page.
    void copyOut(std::size_t first, std::size_t count, T* destination) const noexcept
    {
        while (count > 0) {
            const std::size_t slot = first & (pageCapacity() - 1);
            const std::size_t chunk = std::min(count, pageCapacity() - slot);
            std::memcpy(static_cast<void*>(destination), m_table[first >> m_shift] + slot, chunk * sizeof(T));
            destination += chunk;
            first += chunk;
            count -= chunk;
        }
    }

    /// Copies contiguous memory into elements [first, first + count).
    void copyIn(std::size_t first, std::size_t count, const T* source) noexcept
    {
        while (count > 0) {
            const std::size_t slot = first & (pageCapacity() - 1);
            const std::size_t chunk = std::min(count, pageCapacity() - slot);
            std::memcpy(static_cast<void*>(m_table[first >> m_shift] + slot), source, chunk * sizeof(T));
            source += chunk;
            first += chunk;
            count -= chunk;
        }
    }

    /// Bytes allocated: whole pages plus the page table.
    [[nodiscard]] std::size_t memoryBytes() const noexcept
    {
        return m_table.size() * pageCapacity() * sizeof(T) + m_table.capacity() * sizeof(T*);
    }

private:
    [[nodiscard]] std::size_t pageSize(std::size_t pageIndex) const noexcept
    {
        const std::size_t begin = pageIndex << m_shift;
        return std::min(pageCapacity(), m_size - begin);
    }

    void release() noexcept
    {
        for (T* page : m_table) {
            delete[] page;
        }
        m_table.clear();
        m_size = 0;
    }

    std::vector<T*> m_table; // page table: owning pointers to the pages
    u32 m_shift = 12;
    std::size_t m_size = 0;
};

} // namespace se::geo
