// File: src/server/advancements/PlayerAdvancements.hpp
//
// MC PlayerAdvancements: one player's progress over the loaded advancements.
//
//   • progress per advancement (AdvancementProgress), started on demand;
//   • the active criterion listeners, by trigger — every criterion that is
//     not done of every advancement that is not done (registerListeners /
//     unregisterListeners), which is what a trigger iterates;
//   • the visible set and the roots whose visibility must be re-evaluated
//     (AdvancementVisibilityEvaluator), and the progress that changed —
//     flushed to the client as one UpdateAdvancementsS2C (flushDirty);
//   • the last opened tab (setSelectedTab).
//
// Persisted as MC's advancements/<uuid>.json: per advancement with any
// progress, {"criteria": {name: "yyyy-MM-dd HH:mm:ss Z"}, "done": bool},
// plus "DataVersion" — the same file a vanilla server reads.
//
// Differences from MC, both deliberate: an advancement's rewards and its chat
// announcement run at the next flush (the end of the player's tick) rather
// than inside award(), so a trigger that fires from deep inside a mob's tick
// never re-enters the world from there; and the per-player trigger state MC
// keeps on ServerPlayer (where the player entered the nether, where a fall or
// a levitation began) lives here, beside the triggers that read it.
#pragma once

#include "CriteriaTriggers.hpp"
#include "common/advancements/AdvancementLoader.hpp"

#include <glm/glm.hpp>

#include <array>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Game { class Entity; }

namespace Server {
    class ServerPlayer;
    class ServerConnection;
}

namespace Server::Advancements {

    using Game::Advancements::AdvancementProgress;
    using Game::Advancements::CriterionDef;
    using Game::Advancements::Definition;
    using Game::Advancements::Registry;

    struct Listener {
        const Definition*   advancement = nullptr;
        const CriterionDef* criterion   = nullptr;
        bool operator==(const Listener& o) const { return advancement == o.advancement && criterion == o.criterion; }
    };

    // The trigger state MC keeps on ServerPlayer for the player-tick triggers.
    struct TrackedState {
        std::optional<glm::dvec3> enteredNetherPosition;     // NETHER_TRAVEL
        std::optional<glm::dvec3> levitationStartPos;        // LEVITATION
        int                       levitationStartTime = 0;
        int                       tickCount = 0;
        std::optional<glm::dvec3> startingToFallPosition;    // FALL_FROM_HEIGHT
        std::optional<glm::dvec3> currentExplosionImpactPos; // FALL_AFTER_EXPLOSION
        int32_t                   currentExplosionCauseId = 0;
        bool                      hasExplosionCause = false;
        std::optional<glm::dvec3> enteredLavaOnVehiclePosition;   // RIDE_ENTITY_IN_LAVA
        // INVENTORY_CHANGED: the inventory as the trigger last saw it.
        std::vector<Game::ItemStack> inventorySnapshot;
        bool                      inventorySnapshotValid = false;
        // Whether triggers awarded on the last unpaused tick: the tick that
        // finds them switched on again (advancements_with_cheats turned on,
        // or cheats turned off) re-evaluates the player like a fresh login.
        bool                      triggersWereEnabled = false;
    };

    class PlayerAdvancements {
    public:
        PlayerAdvancements(std::shared_ptr<const Registry> registry, std::filesystem::path file, uint32_t playerId);

        // MC load: the file's progress, then checkForAutomaticTriggers
        // (criterion-less advancements are granted) and registerListeners.
        void Load();
        // MC save. No-op without a file (a read-only world).
        void Save() const;
        // Awarding was just switched on for this player (the first tick of
        // the session, or the world's advancements_with_cheats rule / cheats
        // changed): every undone criterion listens again, as MC's
        // registerListeners does at login. Idempotent.
        void ReRegisterListeners();

        // MC award / revoke. True when the criterion changed state.
        bool Award(const Definition& advancement, const std::string& criterion);
        bool Revoke(const Definition& advancement, const std::string& criterion);
        AdvancementProgress& GetOrStartProgress(const Definition& advancement);

        // MC flushDirty: rewards and announcements of what completed since the
        // last flush, then one packet with what changed (the first flush is
        // the reset that rebuilds the client's tree).
        void FlushDirty(ServerPlayer& player, ServerConnection* connection, bool showAdvancements);
        // MC setSelectedTab: remembers a root with a display and tells the
        // client when it changed.
        void SetSelectedTab(const Definition* tab, ServerConnection* connection);

        // The listeners of one trigger (a copy-safe view: awarding changes
        // the lists, so triggers collect matches first, as MC does).
        const std::vector<Listener>& Listeners(TriggerType type) const {
            return m_listeners[static_cast<size_t>(type)];
        }

        std::recursive_mutex& Mutex() const { return m_mutex; }
        TrackedState& Tracked() { return m_tracked; }
        uint32_t PlayerId() const { return m_playerId; }
        const Registry& GetRegistry() const { return *m_registry; }

    private:
        // An advancement that just completed: its rewards, and its chat line
        // when `announce` (an automatic grant on load pays rewards only).
        struct Completion {
            const Definition* advancement;
            bool announce;
        };

        void StartProgress(const Definition& advancement, AdvancementProgress progress);
        void MarkForVisibilityUpdate(const Definition& advancement);
        void RegisterListeners(const Definition& advancement);
        void UnregisterListeners(const Definition& advancement);
        void AddListener(const Listener& listener);
        void RemoveListener(const Listener& listener);
        void CheckForAutomaticTriggers();
        void UpdateTreeVisibility(const Game::Advancements::Node& root,
                                  std::vector<const Game::Advancements::Node*>& added,
                                  std::vector<std::string>& removed);
        void RunCompletions(ServerPlayer& player);

        std::shared_ptr<const Registry> m_registry;
        std::filesystem::path m_file;
        uint32_t m_playerId = 0;

        // Keyed by definition (pointers into the registry, stable for its
        // life); m_order keeps MC's LinkedHashMap insertion order for the file.
        std::unordered_map<const Definition*, AdvancementProgress> m_progress;
        std::vector<const Definition*> m_progressOrder;
        std::set<const Definition*> m_visible;
        std::set<const Definition*> m_progressChanged;
        std::set<const Game::Advancements::Node*> m_rootsToUpdate;
        const Definition* m_lastSelectedTab = nullptr;
        bool m_isFirstPacket = true;
        std::array<std::vector<Listener>, static_cast<size_t>(TriggerType::Count)> m_listeners;
        std::vector<Completion> m_pendingCompletions;
        // Entries of the file this registry does not know (the recipe-unlock
        // advancements a vanilla server writes, another pack's): kept as
        // their JSON text and written back, so a save never drops them.
        std::vector<std::pair<std::string, std::string>> m_preserved;
        TrackedState m_tracked;
        mutable std::recursive_mutex m_mutex;
    };

} // namespace Server::Advancements
