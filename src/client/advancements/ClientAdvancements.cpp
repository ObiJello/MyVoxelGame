// File: src/client/advancements/ClientAdvancements.cpp
#include "ClientAdvancements.hpp"

#include "client/network/ClientConnection.hpp"
#include "client/network/NetworkClient.hpp"
#include "client/renderer/gui/toasts/ToastManager.hpp"

#include "common/core/Log.hpp"
#include "common/network/PacketTypes.hpp"

#include <vector>

namespace Client {

    namespace {
        void Send(Network::PacketId id, const std::vector<uint8_t>& data) {
            if (!g_networkClient) return;
            if (auto connection = g_networkClient->GetConnection()) {
                connection->SendPacket(static_cast<uint8_t>(id), data);
            }
        }
    } // namespace

    ClientAdvancements& ClientAdvancements::Get() {
        static ClientAdvancements s_instance;
        return s_instance;
    }

    const Game::Advancements::Definition* ClientAdvancements::Find(const std::string& id) const {
        const Game::Advancements::Node* node = m_tree.Get(id);
        return node ? node->def : nullptr;
    }

    void ClientAdvancements::Update(const Network::UpdateAdvancementsS2CPacket& packet) {
        if (packet.reset) {
            m_tree.Clear();
            m_definitions.clear();
            m_progress.clear();
            m_selectedTab = nullptr;
            if (m_listener) m_listener->OnAdvancementsCleared();
        }

        // tree.remove(removed): each id and everything under it.
        if (!packet.removed.empty()) {
            m_tree.Remove(packet.removed);
            for (auto it = m_definitions.begin(); it != m_definitions.end();) {
                if (!m_tree.Get(it->first)) {
                    if (m_selectedTab == it->second.get()) m_selectedTab = nullptr;
                    m_progress.erase(it->first);
                    it = m_definitions.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // tree.addAll(added) — each carries its display's x/y already.
        std::vector<const Game::Advancements::Definition*> added;
        added.reserve(packet.added.size());
        for (const Network::AdvancementEntryData& entry : packet.added) {
            auto def = std::make_unique<Game::Advancements::Definition>();
            def->id = entry.id;
            def->parent = entry.parent;
            def->display = entry.display;
            def->requirements = entry.requirements;
            def->sendsTelemetryEvent = entry.sendsTelemetryEvent;
            if (auto existing = m_definitions.find(entry.id); existing != m_definitions.end()) {
                // Already in the tree: refresh it in place, so the nodes that
                // point at it stay valid.
                *existing->second = std::move(*def);
                continue;
            }
            added.push_back(def.get());
            m_definitions[entry.id] = std::move(def);
        }
        if (!added.empty()) m_tree.AddAll(added);
        // A definition whose parent never arrived is not in the tree.
        for (auto it = m_definitions.begin(); it != m_definitions.end();) {
            if (!m_tree.Get(it->first)) it = m_definitions.erase(it);
            else ++it;
        }

        for (const auto& [id, incoming] : packet.progress) {
            const Game::Advancements::Node* node = m_tree.Get(id);
            if (!node) {
                Log::Warning("[Advancements] Server informed client about progress for unknown advancement %s", id.c_str());
                continue;
            }
            Game::Advancements::AdvancementProgress progress = incoming;
            progress.Update(node->def->requirements);
            const bool done = progress.IsDone();
            m_progress[id] = std::move(progress);
            if (!packet.reset && done && packet.showAdvancements && node->def->display && node->def->display->showToast) {
                Render::GetToastManager().AddToast(std::make_unique<Render::AdvancementToast>(*node->def->display));
            }
        }

        if (m_listener) m_listener->OnAdvancementsUpdated();
    }

    void ClientAdvancements::OnSelectTab(const Network::SelectAdvancementsTabS2CPacket& packet) {
        // MC handleSelectAdvancementsTab: the server's choice, not echoed.
        const Game::Advancements::Definition* tab = packet.tab ? Find(*packet.tab) : nullptr;
        SetSelectedTab(tab, false);
    }

    void ClientAdvancements::Clear() {
        m_tree.Clear();
        m_definitions.clear();
        m_progress.clear();
        m_selectedTab = nullptr;
        if (m_listener) m_listener->OnAdvancementsCleared();
    }

    void ClientAdvancements::SetSelectedTab(const Game::Advancements::Definition* tab, bool tellServer) {
        if (tab && tellServer) {
            Network::SeenAdvancementsC2SPacket packet;
            packet.action = Network::SeenAdvancementsC2SPacket::Action::OpenedTab;
            packet.tab = tab->id;
            Send(Network::PacketId::SeenAdvancementsC2S, Network::Serialization::Serialize(packet));
        }
        if (m_selectedTab != tab) {
            m_selectedTab = tab;
            if (m_listener) m_listener->OnSelectedTabChanged(tab);
        }
    }

    void ClientAdvancements::SendClosedScreen() {
        Network::SeenAdvancementsC2SPacket packet;
        packet.action = Network::SeenAdvancementsC2SPacket::Action::ClosedScreen;
        Send(Network::PacketId::SeenAdvancementsC2S, Network::Serialization::Serialize(packet));
    }

    void ClientAdvancements::SetListener(Listener* listener) {
        m_listener = listener;
        if (listener) {
            listener->OnAdvancementsUpdated();
            listener->OnSelectedTabChanged(m_selectedTab);
        }
    }

} // namespace Client
