// The engine's reference-counted string: one pointer to a block {refs, length, text}. All of it
// is inlined in the original (assignment from text 0x1007d3e0, release 0x1007d3c0, the text of
// ISND_source's name 0x10056150).
#pragma once

#include <cstring>
#include <new>

namespace ls3d {

class C_str {
public:
    C_str() = default;
    C_str(const char *text) { *this = text; }
    C_str(const C_str &other) : m_rep(other.m_rep)
    {
        if (m_rep)
            ++m_rep->refs;
    }
    ~C_str() { Release(); }

    C_str &operator=(const C_str &other)
    {
        if (other.m_rep)
            ++other.m_rep->refs;
        Release();
        m_rep = other.m_rep;
        return *this;
    }
    // A new block holding a copy of `text` (null: no block). The copy is made before the old
    // block is released, so `text` may point into it.
    C_str &operator=(const char *text)
    {
        S_rep *rep = nullptr;
        if (text) {
            const size_t length = std::strlen(text);
            rep = static_cast<S_rep *>(::operator new(sizeof(int) * 2 + length + 1));
            rep->refs = 1;
            rep->length = static_cast<int>(length);
            std::memcpy(rep->text, text, length + 1);
        }
        Release();
        m_rep = rep;
        return *this;
    }

    // The text ("" without a block).
    const char *c_str() const { return m_rep ? m_rep->text : ""; }
    int Length() const { return m_rep ? m_rep->length : 0; }

private:
    struct S_rep {
        int refs;
        int length;
        char text[1];
    };

    void Release()
    {
        if (m_rep && --m_rep->refs == 0)
            ::operator delete(m_rep);
        m_rep = nullptr;
    }

    S_rep *m_rep = nullptr;
};

static_assert(sizeof(C_str) == 4, "C_str layout");

}  // namespace ls3d
