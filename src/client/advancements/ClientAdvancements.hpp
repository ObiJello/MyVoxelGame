// File: src/client/advancements/ClientAdvancements.hpp
//
// MC ClientAdvancements: the client's mirror of the advancements the server
// made visible to this player — the tree (each node's display carrying its
// laid-out x/y), the progress per advancement and the selected tab — fed by
// UpdateAdvancementsS2C / SelectAdvancementsTabS2C on the main thread.
//
// A progress update that completes an advancement with show_toast queues an
// AdvancementToast (unless the packet is the join reset or a bulk command
// grant). The Advancements screen listens for changes (Listener).
#pragma once

#include "common/advancements/Advancement.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace Network {
    struct UpdateAdvancementsS2CPacket;
    struct SelectAdvancementsTabS2CPacket;
}

namespace Client {

    class ClientAdvancements {
    public:
        // MC ClientAdvancements.Listener.
        class Listener {
        public:
            virtual ~Listener() = default;
            virtual void OnAdvancementsUpdated() = 0;
            virtual void OnAdvancementsCleared() = 0;
            virtual void OnSelectedTabChanged(const Game::Advancements::Definition* tab) = 0;
        };

        static ClientAdvancements& Get();

        void Update(const Network::UpdateAdvancementsS2CPacket& packet);
        void OnSelectTab(const Network::SelectAdvancementsTabS2CPacket& packet);
        // The connection ended: nothing of the old world's tree stays.
        void Clear();

        const Game::Advancements::Tree& GetTree() const { return m_tree; }
        // Progress by advancement id.
        const std::map<std::string, Game::Advancements::AdvancementProgress>& Progress() const { return m_progress; }
        const Game::Advancements::Definition* Find(const std::string& id) const;

        // MC setSelectedTab: tells the server (ServerboundSeenAdvancements
        // OPENED_TAB) when `tellServer`, and the listener when it changed.
        void SetSelectedTab(const Game::Advancements::Definition* tab, bool tellServer);
        const Game::Advancements::Definition* SelectedTab() const { return m_selectedTab; }
        // ServerboundSeenAdvancementsPacket.closedScreen().
        void SendClosedScreen();

        // MC setListener: the new listener is brought up to date at once.
        void SetListener(Listener* listener);

    private:
        Game::Advancements::Tree m_tree;
        std::unordered_map<std::string, std::unique_ptr<Game::Advancements::Definition>> m_definitions;
        std::map<std::string, Game::Advancements::AdvancementProgress> m_progress;
        const Game::Advancements::Definition* m_selectedTab = nullptr;
        Listener* m_listener = nullptr;
    };

} // namespace Client
