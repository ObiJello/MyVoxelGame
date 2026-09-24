// File: src/client/renderer/gui/debug/FrameProfiler.cpp
#include "FrameProfiler.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace Render::DebugScreen::FrameProfiler {

    namespace {
        struct Node {
            std::string name;
            double timeMs = 0.0;
            std::chrono::steady_clock::time_point start;
            std::vector<Node> children;
        };

        bool g_active = false;
        bool g_inFrame = false;
        Node g_root;
        // Indices down from the root into the open sections; the current
        // section is the node reached by following them.
        std::vector<size_t> g_stack;

        Node& Current() {
            Node* n = &g_root;
            for (size_t idx : g_stack) n = &n->children[idx];
            return *n;
        }

        double Elapsed(const Node& n) {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - n.start).count();
        }
    }

    // MC's fps pie is a ContinuousProfiler: the ActiveProfiler it enables
    // when the chart comes up ACCUMULATES every path's duration across all
    // the frames since (ActiveProfiler.entries is never cleared by
    // startTick), and `getResults()` reports those running totals. That is
    // why vanilla's slices sit still — they are a since-enabled average,
    // not one frame's split. Enabling from disabled starts a fresh
    // profiler (Minecraft.constructProfiler), so the totals reset then.
    void SetActive(bool active) {
        if (active && !g_active) {
            g_root = Node{};
            g_root.name = "root";
        }
        g_active = active;
    }
    bool IsActive() { return g_active; }

    void BeginFrame() {
        if (!g_active) { g_inFrame = false; return; }
        if (g_root.name.empty()) g_root.name = "root";
        // Only the tick's start times are reset; the accumulated times stay.
        g_root.start = std::chrono::steady_clock::now();
        g_stack.clear();
        g_inFrame = true;
    }

    void Push(const char* name) {
        if (!g_active || !g_inFrame) return;
        Node& cur = Current();
        // MC merges repeated pushes of the same name under one parent into
        // one entry (its map is keyed by path); the tick loop pushes "tick"
        // per tick, and the entity lambda runs once per view.
        size_t idx = cur.children.size();
        for (size_t i = 0; i < cur.children.size(); ++i) {
            if (cur.children[i].name == name) { idx = i; break; }
        }
        if (idx == cur.children.size()) {
            Node n;
            n.name = name;
            cur.children.push_back(std::move(n));
        }
        cur.children[idx].start = std::chrono::steady_clock::now();
        g_stack.push_back(idx);
    }

    void Pop() {
        if (!g_active || !g_inFrame || g_stack.empty()) return;
        Node& cur = Current();
        cur.timeMs += Elapsed(cur);
        g_stack.pop_back();
    }

    void PopPush(const char* name) {
        Pop();
        Push(name);
    }

    void AddChildTime(const char* name, double milliseconds) {
        if (!g_active || !g_inFrame) return;
        Node& cur = Current();
        for (Node& c : cur.children) {
            if (c.name == name) { c.timeMs += milliseconds; return; }
        }
        Node n;
        n.name = name;
        n.timeMs = milliseconds;
        cur.children.push_back(std::move(n));
    }

    namespace {
        void Convert(const Node& in, ProfileNode& out) {
            out.name = in.name;
            out.timeMs = in.timeMs;
            out.children.reserve(in.children.size());
            for (const Node& c : in.children) {
                ProfileNode pc;
                Convert(c, pc);
                out.children.push_back(std::move(pc));
            }
        }
    }

    void EndFrame(ProfileNode& outRoot) {
        if (!g_active || !g_inFrame) { outRoot = ProfileNode{}; return; }
        while (!g_stack.empty()) Pop();   // an unbalanced push still closes
        g_root.timeMs += Elapsed(g_root);   // accumulated, like every other path
        Convert(g_root, outRoot);
        g_inFrame = false;
    }

} // namespace Render::DebugScreen::FrameProfiler
