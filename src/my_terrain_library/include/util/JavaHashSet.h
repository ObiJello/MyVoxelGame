#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace minecraft {
namespace util {

/**
 * JavaHashSet - HashSet implementation with Java-faithful iteration order.
 *
 * This mirrors java.util.HashSet backed by HashMap (ported from OpenJDK 25
 * java/util/HashMap.java) closely enough for parity-sensitive worldgen code:
 * - default capacity 16, load factor 0.75
 * - hash spreading via h ^ (h >>> 16)
 * - bucket chains append at tail; resize splits buckets into lo/hi lists
 *   preserving relative order
 * - TREE BINS: a bin reaching 8 nodes treeifies (red-black) once the table is
 *   >= 64 (resizing instead below that), untreeifies at <= 6 on split/remove.
 *   Treeified bins keep a next/prev chain whose order is changed by
 *   treeify()'s moveRootToFront and putTreeVal()'s insert-after-parent, so
 *   iteration order matches Java exactly.
 * - remove() ports HashMap.removeNode: unlinks without shrinking the table.
 *
 * Element type T needs: int32_t hashCode() const, operator==.
 *
 * NOTE on tie-breaking: Java orders tree nodes by spread hash; keys whose
 * classes do not directly declare Comparable<Self> (BlockPos does not - only
 * its base Vec3i does, so HashMap.comparableClassFor returns null) fall back
 * to tieBreakOrder = System.identityHashCode, which is nondeterministic even
 * in Java. That path only triggers for distinct keys with identical 32-bit
 * spread hashes; we use a fixed dir=-1 there.
 */
template <typename T>
class JavaHashSet {
private:
    struct Node {
        T value;
        int32_t hash;
        Node* next = nullptr;
        // Tree-bin fields (meaningful only while isTree)
        bool isTree = false;
        bool red = false;
        Node* parent = nullptr;
        Node* left = nullptr;
        Node* right = nullptr;
        Node* prev = nullptr;

        Node(const T& v, int32_t h) : value(v), hash(h) {}
    };

    static constexpr size_t DEFAULT_INITIAL_CAPACITY = 16;
    static constexpr float LOAD_FACTOR = 0.75f;
    static constexpr int TREEIFY_THRESHOLD = 8;
    static constexpr int UNTREEIFY_THRESHOLD = 6;
    static constexpr size_t MIN_TREEIFY_CAPACITY = 64;

    std::vector<Node*> m_table;
    std::vector<std::unique_ptr<Node>> m_nodes;
    size_t m_size = 0;
    size_t m_threshold = 0;

    static int32_t spread(int32_t hash) {
        uint32_t h = static_cast<uint32_t>(hash);
        return static_cast<int32_t>(h ^ (h >> 16));
    }

    static size_t bucketIndex(int32_t hash, size_t capacity) {
        return static_cast<size_t>(static_cast<uint32_t>(hash)) & (capacity - 1);
    }

    void initialize(size_t capacity = DEFAULT_INITIAL_CAPACITY) {
        size_t powerOfTwo = DEFAULT_INITIAL_CAPACITY;
        while (powerOfTwo < capacity) {
            powerOfTwo <<= 1;
        }
        m_table.assign(powerOfTwo, nullptr);
        m_threshold = std::max<size_t>(1, static_cast<size_t>(static_cast<float>(powerOfTwo) * LOAD_FACTOR));
    }

    void ensureInitialized() {
        if (m_table.empty()) {
            initialize();
        }
    }

    Node* newNode(const T& value, int32_t hash) {
        auto node = std::make_unique<Node>(value, hash);
        Node* raw = node.get();
        m_nodes.push_back(std::move(node));
        return raw;
    }

    // Java: HashMap.TreeNode.tieBreakOrder surrogate (see class comment).
    static int tieBreakOrder(const T& /*a*/, const T& /*b*/) {
        return -1;
    }

    //=========================================================================
    // Red-black tree machinery, ported 1:1 from OpenJDK HashMap.TreeNode
    //=========================================================================

    static Node* rootOf(Node* n) {
        for (Node* r = n, *p;;) {
            if ((p = r->parent) == nullptr) {
                return r;
            }
            r = p;
        }
    }

    // Java: TreeNode.moveRootToFront
    void moveRootToFront(Node* root) {
        size_t n;
        if (root != nullptr && (n = m_table.size()) > 0) {
            size_t index = bucketIndex(root->hash, n);
            Node* first = m_table[index];
            if (root != first) {
                m_table[index] = root;
                Node* rp = root->prev;
                Node* rn = root->next;
                if (rn != nullptr) {
                    rn->prev = rp;
                }
                if (rp != nullptr) {
                    rp->next = rn;
                }
                if (first != nullptr) {
                    first->prev = root;
                }
                root->next = first;
                root->prev = nullptr;
            }
        }
    }

    // Java: TreeNode.find (comparableClassFor(BlockPos)==null, so the
    // compareComparables branch is dead and omitted).
    static Node* treeFind(Node* from, int32_t h, const T& k) {
        Node* p = from;
        do {
            int32_t ph;
            Node* pl = p->left;
            Node* pr = p->right;
            Node* q;
            if ((ph = p->hash) > h) {
                p = pl;
            } else if (ph < h) {
                p = pr;
            } else if (p->value == k) {
                return p;
            } else if (pl == nullptr) {
                p = pr;
            } else if (pr == nullptr) {
                p = pl;
            } else if ((q = treeFind(pr, h, k)) != nullptr) {
                return q;
            } else {
                p = pl;
            }
        } while (p != nullptr);
        return nullptr;
    }

    static Node* getTreeNode(Node* binHead, int32_t h, const T& k) {
        return treeFind(binHead->parent != nullptr ? rootOf(binHead) : binHead, h, k);
    }

    // Java: TreeNode.rotateLeft
    static Node* rotateLeft(Node* root, Node* p) {
        Node *r, *pp, *rl;
        if (p != nullptr && (r = p->right) != nullptr) {
            if ((rl = p->right = r->left) != nullptr) {
                rl->parent = p;
            }
            if ((pp = r->parent = p->parent) == nullptr) {
                (root = r)->red = false;
            } else if (pp->left == p) {
                pp->left = r;
            } else {
                pp->right = r;
            }
            r->left = p;
            p->parent = r;
        }
        return root;
    }

    // Java: TreeNode.rotateRight
    static Node* rotateRight(Node* root, Node* p) {
        Node *l, *pp, *lr;
        if (p != nullptr && (l = p->left) != nullptr) {
            if ((lr = p->left = l->right) != nullptr) {
                lr->parent = p;
            }
            if ((pp = l->parent = p->parent) == nullptr) {
                (root = l)->red = false;
            } else if (pp->right == p) {
                pp->right = l;
            } else {
                pp->left = l;
            }
            l->right = p;
            p->parent = l;
        }
        return root;
    }

    // Java: TreeNode.balanceInsertion
    static Node* balanceInsertion(Node* root, Node* x) {
        x->red = true;
        for (Node *xp, *xpp, *xppl, *xppr;;) {
            if ((xp = x->parent) == nullptr) {
                x->red = false;
                return x;
            } else if (!xp->red || (xpp = xp->parent) == nullptr) {
                return root;
            }
            if (xp == (xppl = xpp->left)) {
                if ((xppr = xpp->right) != nullptr && xppr->red) {
                    xppr->red = false;
                    xp->red = false;
                    xpp->red = true;
                    x = xpp;
                } else {
                    if (x == xp->right) {
                        root = rotateLeft(root, x = xp);
                        xpp = (xp = x->parent) == nullptr ? nullptr : xp->parent;
                    }
                    if (xp != nullptr) {
                        xp->red = false;
                        if (xpp != nullptr) {
                            xpp->red = true;
                            root = rotateRight(root, xpp);
                        }
                    }
                }
            } else {
                if (xppl != nullptr && xppl->red) {
                    xppl->red = false;
                    xp->red = false;
                    xpp->red = true;
                    x = xpp;
                } else {
                    if (x == xp->left) {
                        root = rotateRight(root, x = xp);
                        xpp = (xp = x->parent) == nullptr ? nullptr : xp->parent;
                    }
                    if (xp != nullptr) {
                        xp->red = false;
                        if (xpp != nullptr) {
                            xpp->red = true;
                            root = rotateLeft(root, xpp);
                        }
                    }
                }
            }
        }
    }

    // Java: TreeNode.balanceDeletion
    static Node* balanceDeletion(Node* root, Node* x) {
        for (Node *xp, *xpl, *xpr;;) {
            if (x == nullptr || x == root) {
                return root;
            } else if ((xp = x->parent) == nullptr) {
                x->red = false;
                return x;
            } else if (x->red) {
                x->red = false;
                return root;
            } else if ((xpl = xp->left) == x) {
                if ((xpr = xp->right) != nullptr && xpr->red) {
                    xpr->red = false;
                    xp->red = true;
                    root = rotateLeft(root, xp);
                    xpr = (xp = x->parent) == nullptr ? nullptr : xp->right;
                }
                if (xpr == nullptr) {
                    x = xp;
                } else {
                    Node* sl = xpr->left;
                    Node* sr = xpr->right;
                    if ((sr == nullptr || !sr->red) &&
                        (sl == nullptr || !sl->red)) {
                        xpr->red = true;
                        x = xp;
                    } else {
                        if (sr == nullptr || !sr->red) {
                            if (sl != nullptr) {
                                sl->red = false;
                            }
                            xpr->red = true;
                            root = rotateRight(root, xpr);
                            xpr = (xp = x->parent) == nullptr ? nullptr : xp->right;
                        }
                        if (xpr != nullptr) {
                            xpr->red = (xp == nullptr) ? false : xp->red;
                            if ((sr = xpr->right) != nullptr) {
                                sr->red = false;
                            }
                        }
                        if (xp != nullptr) {
                            xp->red = false;
                            root = rotateLeft(root, xp);
                        }
                        x = root;
                    }
                }
            } else { // symmetric
                if (xpl != nullptr && xpl->red) {
                    xpl->red = false;
                    xp->red = true;
                    root = rotateRight(root, xp);
                    xpl = (xp = x->parent) == nullptr ? nullptr : xp->left;
                }
                if (xpl == nullptr) {
                    x = xp;
                } else {
                    Node* sl = xpl->left;
                    Node* sr = xpl->right;
                    if ((sl == nullptr || !sl->red) &&
                        (sr == nullptr || !sr->red)) {
                        xpl->red = true;
                        x = xp;
                    } else {
                        if (sl == nullptr || !sl->red) {
                            if (sr != nullptr) {
                                sr->red = false;
                            }
                            xpl->red = true;
                            root = rotateLeft(root, xpl);
                            xpl = (xp = x->parent) == nullptr ? nullptr : xp->left;
                        }
                        if (xpl != nullptr) {
                            xpl->red = (xp == nullptr) ? false : xp->red;
                            if ((sl = xpl->left) != nullptr) {
                                sl->red = false;
                            }
                        }
                        if (xp != nullptr) {
                            xp->red = false;
                            root = rotateRight(root, xp);
                        }
                        x = root;
                    }
                }
            }
        }
    }

    // Java: TreeNode.treeify (forms tree of nodes linked from head; the
    // comparable branch is dead for our key types - see class comment).
    void treeify(Node* head) {
        Node* root = nullptr;
        Node* next = nullptr;
        for (Node* x = head; x != nullptr; x = next) {
            next = x->next;
            x->left = x->right = nullptr;
            if (root == nullptr) {
                x->parent = nullptr;
                x->red = false;
                root = x;
            } else {
                int32_t h = x->hash;
                for (Node* p = root;;) {
                    int dir;
                    int32_t ph = p->hash;
                    if (ph > h) {
                        dir = -1;
                    } else if (ph < h) {
                        dir = 1;
                    } else {
                        dir = tieBreakOrder(x->value, p->value);
                    }

                    Node* xp = p;
                    if ((p = (dir <= 0) ? p->left : p->right) == nullptr) {
                        x->parent = xp;
                        if (dir <= 0) {
                            xp->left = x;
                        } else {
                            xp->right = x;
                        }
                        root = balanceInsertion(root, x);
                        break;
                    }
                }
            }
        }
        moveRootToFront(root);
    }

    // Java: TreeNode.untreeify (in-place: strip tree links in next order).
    static Node* untreeify(Node* head) {
        for (Node* q = head; q != nullptr; q = q->next) {
            q->isTree = false;
            q->red = false;
            q->parent = q->left = q->right = q->prev = nullptr;
        }
        return head;
    }

    // Java: HashMap.treeifyBin
    void treeifyBin(int32_t hash) {
        size_t n = m_table.size();
        if (n < MIN_TREEIFY_CAPACITY) {
            resize();
            return;
        }
        size_t index = bucketIndex(hash, n);
        Node* e = m_table[index];
        if (e != nullptr) {
            Node* hd = nullptr;
            Node* tl = nullptr;
            do {
                Node* p = e;
                p->isTree = true;
                p->red = false;
                p->parent = p->left = p->right = nullptr;
                p->prev = tl;
                if (tl == nullptr) {
                    hd = p;
                } else {
                    tl->next = p;
                }
                tl = p;
                e = e->next;
            } while (e != nullptr);
            m_table[index] = hd;
            if (hd != nullptr) {
                treeify(hd);
            }
        }
    }

    // Java: TreeNode.putTreeVal. Returns the existing node if the value is
    // already present, nullptr after inserting a new node.
    Node* putTreeVal(Node* binHead, int32_t h, const T& k) {
        bool searched = false;
        Node* root = (binHead->parent != nullptr) ? rootOf(binHead) : binHead;
        for (Node* p = root;;) {
            int dir;
            int32_t ph = p->hash;
            if (ph > h) {
                dir = -1;
            } else if (ph < h) {
                dir = 1;
            } else if (p->value == k) {
                return p;
            } else {
                if (!searched) {
                    Node *q, *ch;
                    searched = true;
                    if (((ch = p->left) != nullptr &&
                         (q = treeFind(ch, h, k)) != nullptr) ||
                        ((ch = p->right) != nullptr &&
                         (q = treeFind(ch, h, k)) != nullptr)) {
                        return q;
                    }
                }
                dir = tieBreakOrder(k, p->value);
            }

            Node* xp = p;
            if ((p = (dir <= 0) ? p->left : p->right) == nullptr) {
                Node* xpn = xp->next;
                Node* x = newNode(k, h);
                x->isTree = true;
                x->next = xpn;
                if (dir <= 0) {
                    xp->left = x;
                } else {
                    xp->right = x;
                }
                xp->next = x;
                x->parent = x->prev = xp;
                if (xpn != nullptr) {
                    xpn->prev = x;
                }
                moveRootToFront(balanceInsertion(root, x));
                return nullptr;
            }
        }
    }

    // Java: TreeNode.removeTreeNode (movable=true).
    void removeTreeNode(Node* self) {
        size_t n = m_table.size();
        if (n == 0) {
            return;
        }
        size_t index = bucketIndex(self->hash, n);
        Node* first = m_table[index];
        Node* root = first;
        Node* rl;
        Node* succ = self->next;
        Node* pred = self->prev;
        if (pred == nullptr) {
            m_table[index] = first = succ;
        } else {
            pred->next = succ;
        }
        if (succ != nullptr) {
            succ->prev = pred;
        }
        if (first == nullptr) {
            return;
        }
        if (root->parent != nullptr) {
            root = rootOf(root);
        }
        if (root == nullptr ||
            root->right == nullptr ||
            (rl = root->left) == nullptr ||
            rl->left == nullptr) {
            m_table[index] = untreeify(first); // too small
            return;
        }
        Node* p = self;
        Node* pl = self->left;
        Node* pr = self->right;
        Node* replacement;
        if (pl != nullptr && pr != nullptr) {
            Node* s = pr;
            Node* sl;
            while ((sl = s->left) != nullptr) { // find successor
                s = sl;
            }
            bool c = s->red; s->red = p->red; p->red = c; // swap colors
            Node* sr = s->right;
            Node* pp = p->parent;
            if (s == pr) { // p was s's direct parent
                p->parent = s;
                s->right = p;
            } else {
                Node* sp = s->parent;
                if ((p->parent = sp) != nullptr) {
                    if (s == sp->left) {
                        sp->left = p;
                    } else {
                        sp->right = p;
                    }
                }
                if ((s->right = pr) != nullptr) {
                    pr->parent = s;
                }
            }
            p->left = nullptr;
            if ((p->right = sr) != nullptr) {
                sr->parent = p;
            }
            if ((s->left = pl) != nullptr) {
                pl->parent = s;
            }
            if ((s->parent = pp) == nullptr) {
                root = s;
            } else if (p == pp->left) {
                pp->left = s;
            } else {
                pp->right = s;
            }
            if (sr != nullptr) {
                replacement = sr;
            } else {
                replacement = p;
            }
        } else if (pl != nullptr) {
            replacement = pl;
        } else if (pr != nullptr) {
            replacement = pr;
        } else {
            replacement = p;
        }
        if (replacement != p) {
            Node* pp = replacement->parent = p->parent;
            if (pp == nullptr) {
                (root = replacement)->red = false;
            } else if (p == pp->left) {
                pp->left = replacement;
            } else {
                pp->right = replacement;
            }
            p->left = p->right = p->parent = nullptr;
        }

        Node* r = p->red ? root : balanceDeletion(root, replacement);

        if (replacement == p) { // detach
            Node* pp = p->parent;
            p->parent = nullptr;
            if (pp != nullptr) {
                if (p == pp->left) {
                    pp->left = nullptr;
                } else if (p == pp->right) {
                    pp->right = nullptr;
                }
            }
        }
        moveRootToFront(r);
    }

    // Java: TreeNode.split (called only from resize).
    void split(Node* b, std::vector<Node*>& newTab, size_t index, size_t bit) {
        // Relink into lo and hi lists, preserving order
        Node* loHead = nullptr;
        Node* loTail = nullptr;
        Node* hiHead = nullptr;
        Node* hiTail = nullptr;
        int lc = 0, hc = 0;
        Node* next = nullptr;
        for (Node* e = b; e != nullptr; e = next) {
            next = e->next;
            e->next = nullptr;
            if ((static_cast<size_t>(static_cast<uint32_t>(e->hash)) & bit) == 0) {
                if ((e->prev = loTail) == nullptr) {
                    loHead = e;
                } else {
                    loTail->next = e;
                }
                loTail = e;
                ++lc;
            } else {
                if ((e->prev = hiTail) == nullptr) {
                    hiHead = e;
                } else {
                    hiTail->next = e;
                }
                hiTail = e;
                ++hc;
            }
        }

        if (loHead != nullptr) {
            if (lc <= UNTREEIFY_THRESHOLD) {
                newTab[index] = untreeify(loHead);
            } else {
                newTab[index] = loHead;
                if (hiHead != nullptr) { // (else is already treeified)
                    treeifyWithTable(loHead, newTab);
                }
            }
        }
        if (hiHead != nullptr) {
            if (hc <= UNTREEIFY_THRESHOLD) {
                newTab[index + bit] = untreeify(hiHead);
            } else {
                newTab[index + bit] = hiHead;
                if (loHead != nullptr) {
                    treeifyWithTable(hiHead, newTab);
                }
            }
        }
    }

    // treeify() variant used mid-resize, when m_table has already been
    // swapped for newTab by the caller (resize assigns m_table first, matching
    // Java where table=newTab happens before the transfer loop).
    void treeifyWithTable(Node* head, std::vector<Node*>& /*tab*/) {
        treeify(head);
    }

    // Java: HashMap.resize (growth-only paths that apply here).
    void resize() {
        size_t oldCap = m_table.size();
        std::vector<Node*> oldTab = std::move(m_table);
        size_t newCap = oldCap << 1;
        m_table.assign(newCap, nullptr);
        m_threshold = m_threshold << 1; // double threshold (oldCap >= 16)

        for (size_t j = 0; j < oldCap; ++j) {
            Node* e = oldTab[j];
            if (e == nullptr) {
                continue;
            }
            oldTab[j] = nullptr;
            if (e->next == nullptr) {
                m_table[bucketIndex(e->hash, newCap)] = e;
            } else if (e->isTree) {
                split(e, m_table, j, oldCap);
            } else { // preserve order
                Node* loHead = nullptr;
                Node* loTail = nullptr;
                Node* hiHead = nullptr;
                Node* hiTail = nullptr;
                Node* next;
                do {
                    next = e->next;
                    if ((static_cast<size_t>(static_cast<uint32_t>(e->hash)) & oldCap) == 0) {
                        if (loTail == nullptr) {
                            loHead = e;
                        } else {
                            loTail->next = e;
                        }
                        loTail = e;
                    } else {
                        if (hiTail == nullptr) {
                            hiHead = e;
                        } else {
                            hiTail->next = e;
                        }
                        hiTail = e;
                    }
                } while ((e = next) != nullptr);
                if (loTail != nullptr) {
                    loTail->next = nullptr;
                    m_table[j] = loHead;
                }
                if (hiTail != nullptr) {
                    hiTail->next = nullptr;
                    m_table[j + oldCap] = hiHead;
                }
            }
        }
    }

public:
    JavaHashSet() {
        initialize();
    }

    JavaHashSet(JavaHashSet&&) noexcept = default;
    JavaHashSet& operator=(JavaHashSet&&) noexcept = default;

    JavaHashSet(const JavaHashSet&) = delete;
    JavaHashSet& operator=(const JavaHashSet&) = delete;

    class const_iterator {
    private:
        const JavaHashSet* m_owner = nullptr;
        size_t m_bucketIndex = 0;
        Node* m_node = nullptr;

        void advanceToNextBucket() {
            while (!m_node && m_owner && m_bucketIndex < m_owner->m_table.size()) {
                m_node = m_owner->m_table[m_bucketIndex];
                if (m_node) {
                    break;
                }
                ++m_bucketIndex;
            }

            if (!m_node && m_owner) {
                m_bucketIndex = m_owner->m_table.size();
            }
        }

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        const_iterator() = default;

        const_iterator(const JavaHashSet* owner, size_t bucketIndex, Node* node)
            : m_owner(owner)
            , m_bucketIndex(bucketIndex)
            , m_node(node) {
            advanceToNextBucket();
        }

        reference operator*() const { return m_node->value; }
        pointer operator->() const { return &m_node->value; }

        const_iterator& operator++() {
            if (!m_owner || !m_node) {
                return *this;
            }

            if (m_node->next) {
                m_node = m_node->next;
                return *this;
            }

            ++m_bucketIndex;
            m_node = nullptr;
            advanceToNextBucket();
            return *this;
        }

        bool operator==(const const_iterator& other) const {
            return m_owner == other.m_owner &&
                   m_bucketIndex == other.m_bucketIndex &&
                   m_node == other.m_node;
        }

        bool operator!=(const const_iterator& other) const {
            return !(*this == other);
        }
    };

    // Java: HashMap.putVal
    bool add(const T& value) {
        ensureInitialized();

        int32_t hash = spread(value.hashCode());
        size_t n = m_table.size();
        size_t idx = bucketIndex(hash, n);
        Node* p = m_table[idx];
        Node* e = nullptr;

        if (p == nullptr) {
            m_table[idx] = newNode(value, hash);
        } else {
            if (p->hash == hash && p->value == value) {
                e = p;
            } else if (p->isTree) {
                e = putTreeVal(p, hash, value);
            } else {
                for (int binCount = 0;; ++binCount) {
                    if ((e = p->next) == nullptr) {
                        p->next = newNode(value, hash);
                        if (binCount >= TREEIFY_THRESHOLD - 1) { // -1 for 1st
                            treeifyBin(hash);
                        }
                        break;
                    }
                    if (e->hash == hash && e->value == value) {
                        break;
                    }
                    p = e;
                }
            }
            if (e != nullptr) { // existing mapping
                return false;
            }
        }

        if (++m_size > m_threshold) {
            resize();
        }
        return true;
    }

    /**
     * Java: HashMap.removeNode (movable=true). The table NEVER shrinks; tree
     * bins untreeify when they become too small, exactly like Java.
     */
    bool remove(const T& value) {
        if (m_table.empty() || m_size == 0) {
            return false;
        }

        int32_t hash = spread(value.hashCode());
        size_t n = m_table.size();
        size_t idx = bucketIndex(hash, n);
        Node* p = m_table[idx];
        if (p == nullptr) {
            return false;
        }

        Node* node = nullptr;
        Node* e;
        if (p->hash == hash && p->value == value) {
            node = p;
        } else if ((e = p->next) != nullptr) {
            if (p->isTree) {
                node = getTreeNode(p, hash, value);
            } else {
                do {
                    if (e->hash == hash && e->value == value) {
                        node = e;
                        break;
                    }
                    p = e;
                } while ((e = e->next) != nullptr);
            }
        }

        if (node != nullptr) {
            if (node->isTree) {
                removeTreeNode(node);
            } else if (node == p) {
                m_table[idx] = node->next;
            } else {
                p->next = node->next;
            }
            node->next = nullptr;
            --m_size;
            return true;
        }

        return false;
    }

    bool empty() const {
        return m_size == 0;
    }

    bool contains(const T& value) const {
        if (m_table.empty()) {
            return false;
        }

        int32_t hash = spread(value.hashCode());
        size_t idx = bucketIndex(hash, m_table.size());
        Node* current = m_table[idx];
        if (current == nullptr) {
            return false;
        }
        if (current->isTree) {
            return getTreeNode(current, hash, value) != nullptr;
        }
        while (current) {
            if (current->hash == hash && current->value == value) {
                return true;
            }
            current = current->next;
        }

        return false;
    }

    size_t size() const {
        return m_size;
    }

    const_iterator begin() const {
        return const_iterator(this, 0, nullptr);
    }

    const_iterator end() const {
        return const_iterator(this, m_table.size(), nullptr);
    }
};

} // namespace util
} // namespace minecraft
