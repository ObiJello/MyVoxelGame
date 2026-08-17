// File: src/common/world/pathfinder/Node.cpp
#include "common/world/pathfinder/Node.hpp"

namespace Game {

    void BinaryHeap::Insert(Node* node) {
        node->heapIdx = static_cast<int>(m_heap.size());
        m_heap.push_back(node);
        UpHeap(node->heapIdx);
    }

    Node* BinaryHeap::Pop() {
        if (m_heap.empty()) return nullptr;

        Node* top = m_heap.front();
        Node* last = m_heap.back();
        m_heap.pop_back();

        if (!m_heap.empty()) {
            m_heap[0] = last;
            last->heapIdx = 0;
            DownHeap(0);
        }

        top->heapIdx = -1;
        return top;
    }

    void BinaryHeap::ChangeCost(Node* node, float newF) {
        const float oldF = node->f;
        node->f = newF;
        if (newF < oldF) UpHeap(node->heapIdx);
        else             DownHeap(node->heapIdx);
    }

    void BinaryHeap::Clear() {
        // Deliberately does NOT touch the nodes — MC's BinaryHeap.clear() only
        // resets its size, and that is not an accident.
        //
        // Nodes live in the evaluator's per-search arena, which
        // NodeEvaluator::Done() destroys at the end of every FindPath. The heap
        // still holds pointers to them until the next search clears it, so
        // walking those pointers here writes into freed memory — four bytes per
        // node, on every path recompute, for every mob. It corrupted the heap
        // and crashed the server thread far away from the pathfinder (a mob's
        // control-object vtable pointer was the usual casualty).
        //
        // Resetting heapIdx is unnecessary regardless: each search allocates
        // fresh nodes and Node::heapIdx already defaults to -1.
        m_heap.clear();
    }

    void BinaryHeap::UpHeap(int index) {
        Node* node = m_heap[index];
        const float f = node->f;

        while (index > 0) {
            const int parent = (index - 1) >> 1;
            Node* parentNode = m_heap[parent];
            if (f >= parentNode->f) break;

            m_heap[index] = parentNode;
            parentNode->heapIdx = index;
            index = parent;
        }

        m_heap[index] = node;
        node->heapIdx = index;
    }

    void BinaryHeap::DownHeap(int index) {
        Node* node = m_heap[index];
        const float f = node->f;
        const int size = static_cast<int>(m_heap.size());

        while (true) {
            const int left = 1 + (index << 1);
            if (left >= size) break;

            const int right = left + 1;
            // Pick the smaller child, guarding the case where there is no
            // right sibling.
            const int child = (right < size && m_heap[right]->f < m_heap[left]->f) ? right : left;

            if (m_heap[child]->f >= f) break;

            m_heap[index] = m_heap[child];
            m_heap[child]->heapIdx = index;
            index = child;
        }

        m_heap[index] = node;
        node->heapIdx = index;
    }

} // namespace Game
