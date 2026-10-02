// File: src/common/advancements/Advancement.cpp
#include "Advancement.hpp"

#include "common/core/Log.hpp"
#include "common/network/PacketRegistry.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace Game::Advancements {

    // ── AdvancementType ─────────────────────────────────────────────────────

    const char* FrameName(FrameType type) {
        switch (type) {
            case FrameType::Task:      return "task";
            case FrameType::Challenge: return "challenge";
            case FrameType::Goal:      return "goal";
        }
        return "task";
    }

    std::optional<FrameType> FrameByName(std::string_view name) {
        if (name == "task")      return FrameType::Task;
        if (name == "challenge") return FrameType::Challenge;
        if (name == "goal")      return FrameType::Goal;
        return std::nullopt;
    }

    uint32_t FrameChatColor(FrameType type) {
        return type == FrameType::Challenge
                   ? Text::FormattingRgb(static_cast<int>(Text::Formatting::DarkPurple))
                   : Text::FormattingRgb(static_cast<int>(Text::Formatting::Green));
    }

    std::string DisplayInfo::BackgroundTexturePath() const {
        if (!background) return {};
        // ClientAsset.ResourceTexture: "<ns>:<path>" → textures/<path>.png
        // under the namespace's assets. This engine keeps one asset root.
        std::string path = *background;
        if (const size_t colon = path.find(':'); colon != std::string::npos) path = path.substr(colon + 1);
        return "assets/textures/" + path + ".png";
    }

    // ── AdvancementRequirements ─────────────────────────────────────────────

    bool Requirements::Test(const std::function<bool(const std::string&)>& done) const {
        if (sets.empty()) return false;
        for (const auto& set : sets) {
            if (std::none_of(set.begin(), set.end(), done)) return false;
        }
        return true;
    }

    int Requirements::Count(const std::function<bool(const std::string&)>& done) const {
        int count = 0;
        for (const auto& set : sets) {
            if (std::any_of(set.begin(), set.end(), done)) ++count;
        }
        return count;
    }

    std::vector<std::string> Requirements::Names() const {
        std::vector<std::string> names;
        std::set<std::string> seen;
        for (const auto& set : sets) {
            for (const auto& name : set) {
                if (seen.insert(name).second) names.push_back(name);
            }
        }
        return names;
    }

    Requirements Requirements::AllOf(const std::vector<std::string>& criteria) {
        Requirements r;
        r.sets.reserve(criteria.size());
        for (const auto& c : criteria) r.sets.push_back({c});
        return r;
    }

    // ── AdvancementProgress ─────────────────────────────────────────────────

    void AdvancementProgress::Update(const Requirements& requirements) {
        const std::vector<std::string> names = requirements.Names();
        const std::set<std::string> nameSet(names.begin(), names.end());
        for (auto it = m_criteria.begin(); it != m_criteria.end();) {
            if (!nameSet.count(it->first)) it = m_criteria.erase(it);
            else ++it;
        }
        for (const auto& name : names) m_criteria.try_emplace(name);
        m_requirements = requirements;
    }

    bool AdvancementProgress::IsDone() const {
        return m_requirements.Test([this](const std::string& c) { return IsCriterionDone(c); });
    }

    bool AdvancementProgress::HasProgress() const {
        for (const auto& [name, progress] : m_criteria) {
            if (progress.IsDone()) return true;
        }
        return false;
    }

    bool AdvancementProgress::Grant(const std::string& criterion, int64_t nowMs) {
        auto it = m_criteria.find(criterion);
        if (it == m_criteria.end() || it->second.IsDone()) return false;
        it->second.obtainedMs = nowMs;
        return true;
    }

    bool AdvancementProgress::Revoke(const std::string& criterion) {
        auto it = m_criteria.find(criterion);
        if (it == m_criteria.end() || !it->second.IsDone()) return false;
        it->second.obtainedMs.reset();
        return true;
    }

    const CriterionProgress* AdvancementProgress::GetCriterion(const std::string& name) const {
        auto it = m_criteria.find(name);
        return it == m_criteria.end() ? nullptr : &it->second;
    }

    bool AdvancementProgress::IsCriterionDone(const std::string& name) const {
        const CriterionProgress* progress = GetCriterion(name);
        return progress && progress->IsDone();
    }

    int AdvancementProgress::CountCompletedRequirements() const {
        return m_requirements.Count([this](const std::string& c) { return IsCriterionDone(c); });
    }

    float AdvancementProgress::GetPercent() const {
        if (m_criteria.empty()) return 0.0f;
        const float total = static_cast<float>(m_requirements.Size());
        if (total <= 0.0f) return 0.0f;
        return static_cast<float>(CountCompletedRequirements()) / total;
    }

    std::optional<std::pair<int, int>> AdvancementProgress::GetProgressCounts() const {
        if (m_criteria.empty()) return std::nullopt;
        const int total = static_cast<int>(m_requirements.Size());
        if (total <= 1) return std::nullopt;
        return std::make_pair(CountCompletedRequirements(), total);
    }

    std::vector<std::string> AdvancementProgress::RemainingCriteria() const {
        std::vector<std::string> out;
        for (const auto& [name, progress] : m_criteria) {
            if (!progress.IsDone()) out.push_back(name);
        }
        return out;
    }

    std::vector<std::string> AdvancementProgress::CompletedCriteria() const {
        std::vector<std::string> out;
        for (const auto& [name, progress] : m_criteria) {
            if (progress.IsDone()) out.push_back(name);
        }
        return out;
    }

    std::optional<int64_t> AdvancementProgress::FirstProgressDate() const {
        std::optional<int64_t> first;
        for (const auto& [name, progress] : m_criteria) {
            if (progress.obtainedMs && (!first || *progress.obtainedMs < *first)) first = progress.obtainedMs;
        }
        return first;
    }

    void AdvancementProgress::SetCriterion(const std::string& name, std::optional<int64_t> obtainedMs) {
        m_criteria[name].obtainedMs = obtainedMs;
    }

    void AdvancementProgress::Write(Network::PacketBuffer& buffer) const {
        buffer.WriteVarInt(static_cast<uint32_t>(m_criteria.size()));
        for (const auto& [name, progress] : m_criteria) {
            buffer.WriteString(name);
            buffer.WriteByte(progress.obtainedMs ? 1 : 0);
            if (progress.obtainedMs) buffer.WriteLong(static_cast<uint64_t>(*progress.obtainedMs));
        }
    }

    AdvancementProgress AdvancementProgress::Read(Network::PacketReader& reader) {
        AdvancementProgress progress;
        const uint32_t count = reader.ReadVarInt();
        for (uint32_t i = 0; i < count; ++i) {
            std::string name = reader.ReadString(32767);
            std::optional<int64_t> obtained;
            if (reader.ReadByte() != 0) obtained = static_cast<int64_t>(reader.ReadLong());
            progress.m_criteria[std::move(name)].obtainedMs = obtained;
        }
        return progress;
    }

    // ── Advancement ─────────────────────────────────────────────────────────

    const CriterionDef* Definition::FindCriterion(std::string_view name) const {
        for (const auto& c : criteria) {
            if (c.name == name) return &c;
        }
        return nullptr;
    }

    Text::Component Definition::Name() const {
        if (!display) return Text::Component::Literal(id);
        // Advancement.decorateName.
        const Text::TextColor color = Text::TextColor::FromFormatting(
            static_cast<int>(display->type == FrameType::Challenge ? Text::Formatting::DarkPurple
                                                                   : Text::Formatting::Green));
        Text::Component tooltip = display->title;
        if (!tooltip.style.color) tooltip.style.color = color;   // ComponentUtils.mergeStyles
        tooltip.Append(Text::Component::Literal("\n"));
        tooltip.Append(display->description);
        Text::Component title = display->title;
        title.style.hoverText = std::make_shared<const Text::Component>(std::move(tooltip));
        Text::Component wrapped = Text::Component::Translatable("chat.square_brackets", {std::move(title)});
        wrapped.style.color = color;
        return wrapped;
    }

    // ── AdvancementNode / AdvancementTree ───────────────────────────────────

    const Node* Node::Root() const {
        const Node* node = this;
        while (node->parent) node = node->parent;
        return node;
    }

    bool Tree::TryInsert(const Definition* def) {
        Node* parentNode = nullptr;
        if (def->parent) {
            auto it = m_nodes.find(*def->parent);
            if (it == m_nodes.end()) return false;
            parentNode = it->second.get();
        }
        auto node = std::make_unique<Node>();
        node->def = def;
        node->parent = parentNode;
        Node* raw = node.get();
        if (parentNode) parentNode->children.push_back(raw);
        else m_roots.push_back(raw);
        m_ordered.push_back(raw);
        m_nodes[def->id] = std::move(node);
        return true;
    }

    void Tree::AddAll(const std::vector<const Definition*>& defs) {
        // MC AdvancementTree.addAll: insert what can be inserted, repeat
        // while anything changed; the rest name a parent that never came.
        std::vector<const Definition*> pending;
        for (const Definition* def : defs) {
            if (!def) continue;
            if (auto it = m_nodes.find(def->id); it != m_nodes.end()) RemoveNode(it->second.get());
            pending.push_back(def);
        }
        bool changed = true;
        while (!pending.empty() && changed) {
            changed = false;
            for (auto it = pending.begin(); it != pending.end();) {
                if (TryInsert(*it)) {
                    it = pending.erase(it);
                    changed = true;
                } else {
                    ++it;
                }
            }
        }
        for (const Definition* def : pending) {
            Log::Error("[Advancements] Couldn't load advancement %s: parent %s is missing",
                       def->id.c_str(), def->parent ? def->parent->c_str() : "?");
        }
    }

    void Tree::RemoveNode(Node* node) {
        // Children first (a copy: RemoveNode edits the parent's list).
        const std::vector<Node*> children = node->children;
        for (Node* child : children) RemoveNode(child);
        if (node->parent) {
            auto& siblings = node->parent->children;
            siblings.erase(std::remove(siblings.begin(), siblings.end(), node), siblings.end());
        } else {
            m_roots.erase(std::remove(m_roots.begin(), m_roots.end(), node), m_roots.end());
        }
        m_ordered.erase(std::remove(m_ordered.begin(), m_ordered.end(), node), m_ordered.end());
        m_nodes.erase(node->def->id);
    }

    void Tree::Remove(const std::vector<std::string>& ids) {
        for (const std::string& id : ids) {
            if (auto it = m_nodes.find(id); it != m_nodes.end()) RemoveNode(it->second.get());
        }
    }

    void Tree::Clear() {
        m_nodes.clear();
        m_roots.clear();
        m_ordered.clear();
    }

    Node* Tree::Get(const std::string& id) {
        auto it = m_nodes.find(id);
        return it == m_nodes.end() ? nullptr : it->second.get();
    }

    const Node* Tree::Get(const std::string& id) const {
        auto it = m_nodes.find(id);
        return it == m_nodes.end() ? nullptr : it->second.get();
    }

    std::vector<const Node*> Tree::Tasks() const {
        std::vector<const Node*> out;
        for (const Node* node : m_ordered) {
            if (node->parent) out.push_back(node);
        }
        return out;
    }

    // ── TreeNodePosition ────────────────────────────────────────────────────

    namespace {

        // A straight port of MC TreeNodePosition: the fields and walks keep
        // their names so the two can be read side by side.
        class TreeNodePosition {
        public:
            TreeNodePosition(const Node& node, TreeNodePosition* parent, TreeNodePosition* previousSibling,
                             int childIndex, int depth,
                             std::vector<std::unique_ptr<TreeNodePosition>>& arena)
                : m_node(node), m_parent(parent), m_previousSibling(previousSibling),
                  m_childIndex(childIndex), m_ancestor(this), m_x(depth) {
                TreeNodePosition* previous = nullptr;
                for (const Node* child : node.children) previous = AddChild(*child, previous, arena);
            }

            static void Run(const Node& root, const std::function<DisplayInfo*(const Node&)>& display) {
                std::vector<std::unique_ptr<TreeNodePosition>> arena;
                auto rootPos = std::make_unique<TreeNodePosition>(root, nullptr, nullptr, 1, 0, arena);
                rootPos->FirstWalk();
                const float min = rootPos->SecondWalk(0.0f, 0, rootPos->m_y);
                if (min < 0.0f) rootPos->ThirdWalk(-min);
                rootPos->FinalizePosition(display);
            }

        private:
            TreeNodePosition* AddChild(const Node& node, TreeNodePosition* previous,
                                       std::vector<std::unique_ptr<TreeNodePosition>>& arena) {
                if (node.def->display) {
                    arena.push_back(std::make_unique<TreeNodePosition>(
                        node, this, previous, static_cast<int>(m_children.size()) + 1, m_x + 1, arena));
                    previous = arena.back().get();
                    m_children.push_back(previous);
                } else {
                    // An invisible advancement's children hang off the
                    // nearest visible ancestor.
                    for (const Node* grandchild : node.children) previous = AddChild(*grandchild, previous, arena);
                }
                return previous;
            }

            void FirstWalk() {
                if (m_children.empty()) {
                    m_y = m_previousSibling ? m_previousSibling->m_y + 1.0f : 0.0f;
                    return;
                }
                TreeNodePosition* defaultAncestor = nullptr;
                for (TreeNodePosition* child : m_children) {
                    child->FirstWalk();
                    defaultAncestor = child->Apportion(defaultAncestor ? defaultAncestor : child);
                }
                ExecuteShifts();
                const float midpoint = (m_children.front()->m_y + m_children.back()->m_y) / 2.0f;
                if (m_previousSibling) {
                    m_y = m_previousSibling->m_y + 1.0f;
                    m_mod = m_y - midpoint;
                } else {
                    m_y = midpoint;
                }
            }

            float SecondWalk(float modSum, int depth, float min) {
                m_y += modSum;
                m_x = depth;
                if (m_y < min) min = m_y;
                for (TreeNodePosition* child : m_children) min = child->SecondWalk(modSum + m_mod, depth + 1, min);
                return min;
            }

            void ThirdWalk(float offset) {
                m_y += offset;
                for (TreeNodePosition* child : m_children) child->ThirdWalk(offset);
            }

            void ExecuteShifts() {
                float shift = 0.0f;
                float change = 0.0f;
                for (int i = static_cast<int>(m_children.size()) - 1; i >= 0; --i) {
                    TreeNodePosition* child = m_children[static_cast<size_t>(i)];
                    child->m_y += shift;
                    child->m_mod += shift;
                    change += child->m_change;
                    shift += child->m_shift + change;
                }
            }

            TreeNodePosition* PreviousOrThread() const {
                if (m_thread) return m_thread;
                return m_children.empty() ? nullptr : m_children.front();
            }

            TreeNodePosition* NextOrThread() const {
                if (m_thread) return m_thread;
                return m_children.empty() ? nullptr : m_children.back();
            }

            TreeNodePosition* Apportion(TreeNodePosition* defaultAncestor) {
                if (!m_previousSibling) return defaultAncestor;
                TreeNodePosition* vir = this;
                TreeNodePosition* vor = this;
                TreeNodePosition* vil = m_previousSibling;
                TreeNodePosition* vol = m_parent->m_children.front();
                float sir = m_mod;
                float sor = m_mod;
                float sil = vil->m_mod;
                float sol = vol->m_mod;
                while (vil->NextOrThread() && vir->PreviousOrThread()) {
                    vil = vil->NextOrThread();
                    vir = vir->PreviousOrThread();
                    vol = vol->PreviousOrThread();
                    vor = vor->NextOrThread();
                    vor->m_ancestor = this;
                    const float shift = vil->m_y + sil - (vir->m_y + sir) + 1.0f;
                    if (shift > 0.0f) {
                        vil->GetAncestor(this, defaultAncestor)->MoveSubtree(this, shift);
                        sir += shift;
                        sor += shift;
                    }
                    sil += vil->m_mod;
                    sir += vir->m_mod;
                    sol += vol->m_mod;
                    sor += vor->m_mod;
                }
                if (vil->NextOrThread() && !vor->NextOrThread()) {
                    vor->m_thread = vil->NextOrThread();
                    vor->m_mod += sil - sor;
                } else {
                    if (vir->PreviousOrThread() && !vol->PreviousOrThread()) {
                        vol->m_thread = vir->PreviousOrThread();
                        vol->m_mod += sir - sol;
                    }
                    defaultAncestor = this;
                }
                return defaultAncestor;
            }

            void MoveSubtree(TreeNodePosition* right, float shift) {
                const float subtrees = static_cast<float>(right->m_childIndex - m_childIndex);
                if (subtrees != 0.0f) {
                    right->m_change -= shift / subtrees;
                    m_change += shift / subtrees;
                }
                right->m_shift += shift;
                right->m_y += shift;
                right->m_mod += shift;
            }

            TreeNodePosition* GetAncestor(TreeNodePosition* other, TreeNodePosition* defaultAncestor) {
                if (m_ancestor && other->m_parent) {
                    const auto& siblings = other->m_parent->m_children;
                    if (std::find(siblings.begin(), siblings.end(), m_ancestor) != siblings.end()) return m_ancestor;
                }
                return defaultAncestor;
            }

            void FinalizePosition(const std::function<DisplayInfo*(const Node&)>& display) {
                if (DisplayInfo* info = display(m_node)) {
                    info->x = static_cast<float>(m_x);
                    info->y = m_y;
                }
                for (TreeNodePosition* child : m_children) child->FinalizePosition(display);
            }

            const Node& m_node;
            TreeNodePosition* m_parent;
            TreeNodePosition* m_previousSibling;
            int m_childIndex;
            std::vector<TreeNodePosition*> m_children;
            TreeNodePosition* m_ancestor;
            TreeNodePosition* m_thread = nullptr;
            int   m_x;
            float m_y = -1.0f;
            float m_mod = 0.0f;
            float m_change = 0.0f;
            float m_shift = 0.0f;
        };

    } // namespace

    void LayoutTree(const Node& root, const std::function<DisplayInfo*(const Node&)>& display) {
        // MC run(): an invisible root has nothing to lay out.
        if (!root.def || !root.def->display) return;
        TreeNodePosition::Run(root, display);
    }

} // namespace Game::Advancements
