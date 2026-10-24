// The engine's dynamic array (16 bytes: data, capacity, count, grow step), inlined everywhere in
// the original. Elements are plain data, copied element by element when the array grows.
#pragma once

#include <cstdint>
#include <new>
#include <type_traits>

namespace ls3d {

template <class T> class C_vector {
    static_assert(std::is_trivially_copyable<T>::value, "C_vector holds plain data");

public:
    explicit C_vector(int growBy = 0x20) : m_data(nullptr), m_capacity(0), m_count(0), m_growBy(growBy) {}
    ~C_vector() { Free(); }
    C_vector(const C_vector &) = delete;
    C_vector &operator=(const C_vector &) = delete;

    T *Data() { return m_data; }
    const T *Data() const { return m_data; }
    int Size() const { return m_count; }
    int Capacity() const { return m_capacity; }
    bool Empty() const { return m_count == 0; }
    T &operator[](int i) { return m_data[i]; }
    const T &operator[](int i) const { return m_data[i]; }
    T *begin() { return m_data; }
    T *end() { return m_data + m_count; }
    const T *begin() const { return m_data; }
    const T *end() const { return m_data + m_count; }

    // Releases the storage (the original's destructor: data, count, capacity cleared).
    void Free()
    {
        ::operator delete(m_data);
        m_data = nullptr;
        m_count = 0;
        m_capacity = 0;
    }
    void Clear() { m_count = 0; }
    // The first `n` elements kept (n not above the size).
    void SetCount(int n) { m_count = n; }
    void PopBack() { --m_count; }
    void SetGrowBy(int growBy) { m_growBy = growBy; }

    // Makes room for `n` elements (exactly `n`, never shrinks).
    void Reserve(int n)
    {
        if (n < 0)
            n = 0;
        if (m_capacity < n) {
            m_capacity = n;
            Reallocate();
        }
    }
    // Clear() + Reserve(n) + count = n: the common "resize, contents undefined" idiom.
    void ResizeDiscard(int n)
    {
        m_count = 0;
        Reserve(n);
        m_count = n;
    }

    // Appends, growing the capacity by the grow step when full.
    T &PushBack(const T &value)
    {
        if (m_capacity <= m_count) {
            m_capacity += m_growBy;
            Reallocate();
        }
        m_data[m_count] = value;
        return m_data[m_count++];
    }

    // Appends an element left as the storage held it (the renderer's queue items).
    T &PushBackUninit()
    {
        if (m_capacity <= m_count) {
            m_capacity += m_growBy;
            Reallocate();
        }
        return m_data[m_count++];
    }

    // Removes the first element equal to `value` by moving the last one into its place.
    bool RemoveSwap(const T &value)
    {
        for (int i = 0; i < m_count; ++i) {
            if (m_data[i] == value) {
                --m_count;
                m_data[i] = m_data[m_count];
                return true;
            }
        }
        return false;
    }
    void RemoveAtSwap(int i)
    {
        --m_count;
        m_data[i] = m_data[m_count];
    }

private:
    void Reallocate()
    {
        T *data = static_cast<T *>(::operator new(sizeof(T) * static_cast<size_t>(m_capacity)));
        for (int i = 0; i < m_count; ++i)
            data[i] = m_data[i];
        ::operator delete(m_data);
        m_data = data;
    }

    T *m_data;          // +0x00
    int m_capacity;     // +0x04
    int m_count;        // +0x08
    int m_growBy;       // +0x0C
};

static_assert(sizeof(C_vector<int>) == 16, "C_vector layout");

}  // namespace ls3d
