// std::multiset as MSVC 6's library (Dinkumware <xtree>) builds it: a red-black tree with a head
// node (its parent is the root, its left / right the smallest / largest node) and one nil node
// shared by every tree of a type.
//
// The buffer managers (vb_manager.h) keep the original's tree instead of the C++ library's
// multiset: their buffer trees are ordered by a buffer's free space, which changes while the
// buffer is in the tree, so where a new buffer lands - and the order the allocator tries the
// buffers in - depends on the exact shape of the tree. The insert, erase and rebalancing steps
// below are the original's, one for one, and so is the node layout.
#pragma once

#include <cstdint>

namespace ls3d {

template <class T, class Less> class Multiset {
public:
    enum : int { kRed = 0, kBlack = 1 };
    struct Node {
        Node *left;                         // +0x00
        Node *parent;                       // +0x04
        Node *right;                        // +0x08
        T value;                            // +0x0C
        int color;                          // +0x10 kRed / kBlack
    };

    class iterator {
    public:
        explicit iterator(Node *node = nullptr) : m_node(node) {}
        T &operator*() const { return m_node->value; }
        Node *node() const { return m_node; }
        bool operator==(const iterator &o) const { return m_node == o.m_node; }
        bool operator!=(const iterator &o) const { return m_node != o.m_node; }
        iterator &operator++()
        {
            if (m_node->right != Nil()) {
                m_node = Min(m_node->right);
            } else {
                Node *p;
                while (m_node == (p = m_node->parent)->right)
                    m_node = p;
                if (m_node->right != p)     // (the head: past the root)
                    m_node = p;
            }
            return *this;
        }
        iterator operator++(int)
        {
            iterator old = *this;
            ++*this;
            return old;
        }
        iterator &operator--()
        {
            if (m_node->color == kRed && m_node->parent->parent == m_node) {
                m_node = m_node->right;     // the head: the largest node
            } else if (m_node->left != Nil()) {
                m_node = Max(m_node->left);
            } else {
                Node *p;
                while (m_node == (p = m_node->parent)->left)
                    m_node = p;
                m_node = p;
            }
            return *this;
        }

    private:
        Node *m_node;
    };

    Multiset() : m_head(NewNode(Nil(), kRed))
    {
        m_head->left = m_head;
        m_head->right = m_head;
    }
    ~Multiset()
    {
        Erase(begin(), end());
        delete m_head;
    }
    Multiset(const Multiset &) = delete;
    Multiset &operator=(const Multiset &) = delete;

    iterator begin() const { return iterator(m_head->left); }
    iterator end() const { return iterator(m_head); }
    int Size() const { return m_size; }
    bool Empty() const { return m_size == 0; }
    // The tree as it is (tests compare shapes).
    Node *Head() const { return m_head; }
    static Node *Nil()
    {
        static Node nil = {nullptr, nullptr, nullptr, T(), kBlack};
        return &nil;
    }

    // After the equal values already there.
    iterator Insert(const T &value)
    {
        Node *x = Root();
        Node *y = m_head;
        while (x != Nil()) {
            y = x;
            x = Less()(value, x->value) ? x->left : x->right;
        }
        return InsertAt(x, y, value);
    }

    // Returns the next node.
    iterator Erase(iterator where)
    {
        Node *z = where.node();
        ++where;
        Node *y = z;
        Node *x;
        if (y->left == Nil()) {
            x = y->right;
        } else if (y->right == Nil()) {
            x = y->left;
        } else {
            y = Min(y->right);
            x = y->right;
        }
        if (y != z) {
            // y (z's successor) takes z's place.
            z->left->parent = y;
            y->left = z->left;
            if (y == z->right) {
                x->parent = y;
            } else {
                x->parent = y->parent;
                y->parent->left = x;
                y->right = z->right;
                z->right->parent = y;
            }
            if (Root() == z)
                Root() = y;
            else if (z->parent->left == z)
                z->parent->left = y;
            else
                z->parent->right = y;
            y->parent = z->parent;
            const int c = y->color;
            y->color = z->color;
            z->color = c;
            y = z;
        } else {
            x->parent = y->parent;
            if (Root() == z)
                Root() = x;
            else if (z->parent->left == z)
                z->parent->left = x;
            else
                z->parent->right = x;
            if (Lmost() == z)
                Lmost() = z->right == Nil() ? z->parent : Min(x);
            if (Rmost() == z)
                Rmost() = z->left == Nil() ? z->parent : Max(x);
        }
        if (y->color == kBlack) {
            for (; x != Root() && x->color == kBlack; x = x->parent) {
                if (x == x->parent->left) {
                    Node *w = x->parent->right;
                    if (w->color == kRed) {
                        w->color = kBlack;
                        x->parent->color = kRed;
                        RotateLeft(x->parent);
                        w = x->parent->right;
                    }
                    if (w->left->color == kBlack && w->right->color == kBlack) {
                        w->color = kRed;
                    } else {
                        if (w->right->color == kBlack) {
                            w->left->color = kBlack;
                            w->color = kRed;
                            RotateRight(w);
                            w = x->parent->right;
                        }
                        w->color = x->parent->color;
                        x->parent->color = kBlack;
                        w->right->color = kBlack;
                        RotateLeft(x->parent);
                        break;
                    }
                } else {
                    Node *w = x->parent->left;
                    if (w->color == kRed) {
                        w->color = kBlack;
                        x->parent->color = kRed;
                        RotateRight(x->parent);
                        w = x->parent->left;
                    }
                    if (w->right->color == kBlack && w->left->color == kBlack) {
                        w->color = kRed;
                    } else {
                        if (w->left->color == kBlack) {
                            w->right->color = kBlack;
                            w->color = kRed;
                            RotateLeft(w);
                            w = x->parent->left;
                        }
                        w->color = x->parent->color;
                        x->parent->color = kBlack;
                        w->left->color = kBlack;
                        RotateRight(x->parent);
                        break;
                    }
                }
            }
            x->color = kBlack;
        }
        delete y;
        --m_size;
        return where;
    }

    // All of them at once, or one by one.
    iterator Erase(iterator first, iterator last)
    {
        if (m_size == 0 || first != begin() || last != end()) {
            while (first != last)
                Erase(first++);
            return first;
        }
        EraseSubtree(Root());
        Root() = Nil();
        m_size = 0;
        Lmost() = m_head;
        Rmost() = m_head;
        return begin();
    }

private:
    static Node *NewNode(Node *parent, int color)
    {
        Node *n = new Node;
        n->parent = parent;
        n->color = color;
        return n;
    }
    static Node *Min(Node *n)
    {
        while (n->left != Nil())
            n = n->left;
        return n;
    }
    static Node *Max(Node *n)
    {
        while (n->right != Nil())
            n = n->right;
        return n;
    }
    static void EraseSubtree(Node *x)
    {
        for (Node *y = x; y != Nil(); x = y) {
            EraseSubtree(y->right);
            y = y->left;
            delete x;
        }
    }
    Node *&Root() { return m_head->parent; }
    Node *&Lmost() { return m_head->left; }
    Node *&Rmost() { return m_head->right; }

    iterator InsertAt(Node *x, Node *y, const T &value)
    {
        Node *z = NewNode(y, kRed);
        z->left = Nil();
        z->right = Nil();
        z->value = value;
        ++m_size;
        if (y == m_head || x != Nil() || Less()(value, y->value)) {
            y->left = z;
            if (y == m_head) {
                Root() = z;
                Rmost() = z;
            } else if (y == Lmost()) {
                Lmost() = z;
            }
        } else {
            y->right = z;
            if (y == Rmost())
                Rmost() = z;
        }
        for (x = z; x != Root() && x->parent->color == kRed;) {
            if (x->parent == x->parent->parent->left) {
                y = x->parent->parent->right;
                if (y->color == kRed) {
                    x->parent->color = kBlack;
                    y->color = kBlack;
                    x->parent->parent->color = kRed;
                    x = x->parent->parent;
                } else {
                    if (x == x->parent->right) {
                        x = x->parent;
                        RotateLeft(x);
                    }
                    x->parent->color = kBlack;
                    x->parent->parent->color = kRed;
                    RotateRight(x->parent->parent);
                }
            } else {
                y = x->parent->parent->left;
                if (y->color == kRed) {
                    x->parent->color = kBlack;
                    y->color = kBlack;
                    x->parent->parent->color = kRed;
                    x = x->parent->parent;
                } else {
                    if (x == x->parent->left) {
                        x = x->parent;
                        RotateRight(x);
                    }
                    x->parent->color = kBlack;
                    x->parent->parent->color = kRed;
                    RotateLeft(x->parent->parent);
                }
            }
        }
        Root()->color = kBlack;
        return iterator(z);
    }

    void RotateLeft(Node *x)
    {
        Node *y = x->right;
        x->right = y->left;
        if (y->left != Nil())
            y->left->parent = x;
        y->parent = x->parent;
        if (x == Root())
            Root() = y;
        else if (x == x->parent->left)
            x->parent->left = y;
        else
            x->parent->right = y;
        y->left = x;
        x->parent = y;
    }
    void RotateRight(Node *x)
    {
        Node *y = x->left;
        x->left = y->right;
        if (y->right != Nil())
            y->right->parent = x;
        y->parent = x->parent;
        if (x == Root())
            Root() = y;
        else if (x == x->parent->right)
            x->parent->right = y;
        else
            x->parent->left = y;
        y->right = x;
        x->parent = y;
    }

    uint8_t m_unused[4] = {};               // +0x00 (the comparator and allocator)
    Node *m_head;                           // +0x04
    bool m_multi = true;                    // +0x08 (MSVC 6's _Multi: a multiset)
    int m_size = 0;                         // +0x0C
};

}  // namespace ls3d
