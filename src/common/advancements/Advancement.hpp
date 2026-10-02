// File: src/common/advancements/Advancement.hpp
//
// MC's advancement data model (net.minecraft.advancements), shared by the
// server (which owns progress) and the client (which mirrors the visible part
// of the tree for the Advancements screen and the toasts):
//
//   AdvancementType          → FrameType       (task / challenge / goal)
//   DisplayInfo              → DisplayInfo     (icon, title, description,
//                                               background, frame, flags, x/y)
//   AdvancementRequirements  → Requirements    (AND of ORs over criteria names)
//   CriterionProgress        → CriterionProgress (when it was obtained)
//   AdvancementProgress      → AdvancementProgress
//   Advancement + Holder     → Definition      (id, parent, display, criteria,
//                                               requirements, rewards)
//   AdvancementNode / Tree   → Node / Tree     (parent/child links, roots)
//   TreeNodePosition         → LayoutTree      (the Advancements screen x/y)
//
// Definitions come from the data pack (data/<ns>/advancement/**.json) through
// AdvancementLoader.hpp. A criterion keeps its trigger id and its raw
// `conditions` JSON: the server's trigger code (server/advancements) reads the
// conditions; the client never sees them.
#pragma once

#include "common/entity/Item.hpp"
#include "common/text/TextComponent.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace Network { class PacketBuffer; class PacketReader; }

namespace Game::Advancements {

    // ── AdvancementType ─────────────────────────────────────────────────────
    // MC AdvancementType: id, serialized name, chat colour. The toast's
    // header is "advancements.toast.<name>", the chat line
    // "chat.type.advancement.<name>".
    enum class FrameType : uint8_t { Task = 0, Challenge = 1, Goal = 2 };
    const char* FrameName(FrameType type);
    std::optional<FrameType> FrameByName(std::string_view name);
    // ChatFormatting.GREEN for task and goal, DARK_PURPLE for challenge
    // (0xRRGGBB).
    uint32_t FrameChatColor(FrameType type);

    // ── DisplayInfo ─────────────────────────────────────────────────────────
    struct DisplayInfo {
        ItemStack       icon;          // ItemStackTemplate.create()
        Text::Component title;
        Text::Component description;
        // ClientAsset.ResourceTexture id ("minecraft:gui/advancements/
        // backgrounds/stone"); only a root's background is drawn.
        std::optional<std::string> background;
        FrameType type = FrameType::Task;
        bool showToast      = true;
        bool announceToChat = true;
        bool hidden         = false;
        // TreeNodePosition's result (AdvancementNode.setLocation): column
        // and row in the tab, scaled by 28 x 27 on screen.
        float x = 0.0f;
        float y = 0.0f;

        // The texture path the background id names, relative to the asset
        // root: "assets/textures/gui/advancements/backgrounds/stone.png".
        std::string BackgroundTexturePath() const;
    };

    // ── AdvancementRequirements ─────────────────────────────────────────────
    // A list of OR-sets: the advancement is done when every set has at least
    // one criterion done. An empty list is never done (MC test()).
    struct Requirements {
        std::vector<std::vector<std::string>> sets;

        size_t Size() const { return sets.size(); }
        bool   Empty() const { return sets.empty(); }
        bool   Test(const std::function<bool(const std::string&)>& done) const;
        int    Count(const std::function<bool(const std::string&)>& done) const;
        std::vector<std::string> Names() const;
        // MC AdvancementRequirements.allOf — one set per criterion.
        static Requirements AllOf(const std::vector<std::string>& criteria);
    };

    // ── CriterionProgress / AdvancementProgress ─────────────────────────────
    struct CriterionProgress {
        // When the criterion was obtained, as Unix epoch milliseconds; empty
        // while it is not done.
        std::optional<int64_t> obtainedMs;
        bool IsDone() const { return obtainedMs.has_value(); }
    };

    class AdvancementProgress {
    public:
        // MC update(requirements): drop criteria the requirements no longer
        // name, add the missing ones (not done), remember the requirements.
        void Update(const Requirements& requirements);

        bool IsDone() const;
        bool HasProgress() const;
        // Grant / revoke one criterion; false when it was already in that
        // state or is unknown. `nowMs` is the obtained time to record.
        bool Grant(const std::string& criterion, int64_t nowMs);
        bool Revoke(const std::string& criterion);

        const CriterionProgress* GetCriterion(const std::string& name) const;
        bool IsCriterionDone(const std::string& name) const;
        // Completed requirement sets over all sets (0 with no criteria).
        float GetPercent() const;
        // MC getProgressText: "advancements.progress" (done / total), or
        // nothing when the advancement has at most one requirement set.
        std::optional<std::pair<int, int>> GetProgressCounts() const;
        int CountCompletedRequirements() const;
        std::vector<std::string> RemainingCriteria() const;
        std::vector<std::string> CompletedCriteria() const;
        // The earliest obtained time (MC getFirstProgressDate), for ordering.
        std::optional<int64_t> FirstProgressDate() const;

        const std::map<std::string, CriterionProgress>& Criteria() const { return m_criteria; }
        // Direct access for loading / the wire (no requirements check).
        void SetCriterion(const std::string& name, std::optional<int64_t> obtainedMs);

        // MC AdvancementProgress.STREAM_CODEC: VarInt count, then per
        // criterion its name and an optional obtained time (long millis).
        void Write(Network::PacketBuffer& buffer) const;
        static AdvancementProgress Read(Network::PacketReader& reader);

    private:
        std::map<std::string, CriterionProgress> m_criteria;
        Requirements m_requirements;
    };

    // ── Advancement ─────────────────────────────────────────────────────────
    struct CriterionDef {
        std::string name;
        // The trigger's id, namespaced ("minecraft:inventory_changed").
        std::string trigger;
        // The trigger instance's conditions object as written in the data
        // pack (null when the criterion has none). Server only.
        std::shared_ptr<const nlohmann::json> conditions;
    };

    // MC AdvancementRewards.
    struct Rewards {
        int experience = 0;
        std::vector<std::string> loot;      // loot table ids
        std::vector<std::string> recipes;   // recipe ids
        std::optional<std::string> function;
        bool Empty() const { return experience == 0 && loot.empty() && recipes.empty() && !function; }
    };

    struct Definition {
        std::string id;                       // "minecraft:story/root"
        std::optional<std::string> parent;
        std::optional<DisplayInfo> display;
        std::vector<CriterionDef> criteria;   // data-pack order
        Requirements requirements;
        Rewards rewards;
        bool sendsTelemetryEvent = false;

        bool IsRoot() const { return !parent.has_value(); }
        const CriterionDef* FindCriterion(std::string_view name) const;
        // MC Advancement.name: the display title in square brackets, in
        // the type's chat colour, its hover the title and description
        // (ChatComponents.wrapInSquareBrackets); the bare id without a
        // display.
        Text::Component Name() const;
    };

    // ── AdvancementNode / AdvancementTree ───────────────────────────────────
    class Tree;
    struct Node {
        const Definition* def = nullptr;
        Node* parent = nullptr;
        std::vector<Node*> children;   // in insertion order
        const Node* Root() const;
    };

    // MC AdvancementTree: nodes keyed by id. AddAll inserts parents before
    // children and drops (with a log line) anything whose parent never
    // arrives; Remove takes the node and every descendant with it.
    class Tree {
    public:
        void AddAll(const std::vector<const Definition*>& defs);
        void Remove(const std::vector<std::string>& ids);
        void Clear();
        Node*       Get(const std::string& id);
        const Node* Get(const std::string& id) const;
        const std::vector<Node*>& Roots() const { return m_roots; }
        // Every non-root node, in insertion order (MC tasks()).
        std::vector<const Node*> Tasks() const;
        // Every node, in insertion order.
        const std::vector<Node*>& Ordered() const { return m_ordered; }

    private:
        bool TryInsert(const Definition* def);
        void RemoveNode(Node* node);

        std::unordered_map<std::string, std::unique_ptr<Node>> m_nodes;
        std::vector<Node*> m_roots;
        std::vector<Node*> m_ordered;
    };

    // MC TreeNodePosition.run: lays a root's visible subtree out (Walker's
    // tidy-tree algorithm, Buchheim's linear form) and stores each node's
    // column/row in its display. `display` is the Definition the node's
    // def points at, made writable by the caller (the loader owns them).
    void LayoutTree(const Node& root, const std::function<DisplayInfo*(const Node&)>& display);

} // namespace Game::Advancements
