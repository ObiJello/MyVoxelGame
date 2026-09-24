// File: src/server/entity/MorphCarry.cpp
#include "MorphCarry.hpp"

#include "../control/RemoteControlManager.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/Inventory.hpp"
#include "common/entity/ItemEntity.hpp"
#include "common/entity/Morph.hpp"
#include "common/core/Mth.hpp"
#include "common/core/Log.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/network/packets/game/MorphHeldS2CPacket.hpp"
#include "common/network/packets/game/MorphPickupS2CPacket.hpp"
#include "common/world/level/DimensionId.hpp"

#include <algorithm>

namespace Server {

    namespace {
        struct Box { glm::dvec3 min, max; };

        Box BoxOf(const ServerPlayer& p) {
            const Game::Morph::Dims d = Game::Morph::DimsOf(p.getMorph());
            const double hw = d.width * p.getScale() * 0.5;
            const double h  = d.height * p.getScale();
            const glm::dvec3 pos = p.getPosition();
            return { { pos.x - hw, pos.y,     pos.z - hw },
                     { pos.x + hw, pos.y + h, pos.z + hw } };
        }

        bool Intersects(const Box& a, const Box& b) {
            return a.min.x < b.max.x && a.max.x > b.min.x &&
                   a.min.y < b.max.y && a.max.y > b.min.y &&
                   a.min.z < b.max.z && a.max.z > b.min.z;
        }

        std::shared_ptr<PlayerSession> Live(PlayerSessionManager& sessions, uint32_t id) {
            auto s = sessions.GetSession(id);
            if (!s || !s->GetPlayer() || !s->GetConnection()) return nullptr;
            return s;
        }

        void SendHeld(PlayerSession& to, bool held, uint32_t holderId, const glm::vec3& vel) {
            Network::MorphHeldS2CPacket out;
            out.held     = held;
            out.holderId = holderId;
            out.throwVel = vel;
            to.GetConnection()->SendPacket(static_cast<uint8_t>(Network::PacketId::MorphHeldS2C),
                                           Network::Serialization::Serialize(out));
        }

        // The marker's slot in an inventory, or -1.
        int FindMarker(const Game::Inventory& inv, const Game::ItemStack& marker) {
            for (int i = 0; i < Game::Inventory::TOTAL_SIZE; ++i) {
                const Game::ItemStack& s = inv.GetSlot(i);
                if (s.IsEmpty() || s.itemId != marker.itemId) continue;
                if (s.components.get(Game::DataComponents::CUSTOM_NAME) ==
                    marker.components.get(Game::DataComponents::CUSTOM_NAME)) return i;
            }
            return -1;
        }
    }

    void MorphCarry::SendRole(PlayerSession& to, uint8_t role, uint32_t otherId, const std::string& otherName) {
        Network::ControlS2CPacket out;
        out.role      = static_cast<Network::ControlRole>(role);
        out.otherId   = otherId;
        out.otherName = otherName;
        to.GetConnection()->SendPacket(static_cast<uint8_t>(Network::PacketId::ControlS2C),
                                       Network::Serialization::Serialize(out));
    }

    bool MorphCarry::IsMarker(const Game::ItemStack& stack, const Game::ItemStack& marker) {
        return !stack.IsEmpty() && stack.itemId == marker.itemId &&
               stack.components.get(Game::DataComponents::CUSTOM_NAME) ==
               marker.components.get(Game::DataComponents::CUSTOM_NAME);
    }

    bool MorphCarry::IsHeld(uint32_t playerId) const {
        for (const Held& h : m_held) if (h.held == playerId) return true;
        return false;
    }

    void MorphCarry::Pickup(uint32_t heldId, uint32_t holderId) {
        auto held   = Live(m_sessions, heldId);
        auto holder = Live(m_sessions, holderId);
        if (!held || !holder) return;
        ServerPlayer& heldPlayer   = *held->GetPlayer();
        ServerPlayer& holderPlayer = *holder->GetPlayer();

        // The item, named after the player: one of the morph's kind.
        Game::ItemStack marker(static_cast<Game::ItemID>(Game::Morph::IdOf(heldPlayer.getMorph())), 1);
        marker.components.set(Game::DataComponents::CUSTOM_NAME, heldPlayer.getName());
        // MC Inventory.add: no room, no pickup — the item stays on the floor.
        if (holderPlayer.getInventory().AddStack(marker) > 0) return;
        holder->SendInventoryFull();

        m_held.push_back({heldId, holderId, marker});
        heldPlayer.setInvisible(true);
        SendHeld(*held, true, holderId, glm::vec3(0.0f));

        // From the hand, the holder's HUD: their inventory and stats teed
        // over (as for a /control pair), their hotbar selection and hand
        // streamed in view frames.
        holder->GetConnection()->SetMirrorPlayerId(heldId);
        holder->GetConnection()->SetRelayViewTo(heldId);
        SendRole(*held, static_cast<uint8_t>(Network::ControlRole::HudMirror), holderId, holderPlayer.getName());
        SendRole(*holder, static_cast<uint8_t>(Network::ControlRole::Watched), heldId, heldPlayer.getName());
        holder->ResendStats();
        holder->SendInventoryFull();   // (teed to the held player; the holder's own copy is unchanged by it)

        // The fly-in, for everyone in the dimension.
        Network::MorphPickupS2CPacket pickup;
        pickup.heldPlayerId = heldId;
        pickup.holderId     = holderId;
        pickup.stack        = marker;
        const auto data = Network::Serialization::Serialize(pickup);
        const Game::DimensionId dim = Game::DimensionFromRaw(static_cast<int8_t>(held->GetDimensionId()));
        for (const auto& s : m_sessions.GetAllSessions()) {
            if (!s || !s->GetConnection() || !s->LoadsDimension(dim)) continue;
            s->GetConnection()->SendPacketIn(dim, static_cast<uint8_t>(Network::PacketId::MorphPickupS2C), data);
        }
        Log::Info("[MorphCarry] %s picked up %s",
                  holderPlayer.getName().c_str(), heldPlayer.getName().c_str());
    }

    bool MorphCarry::Release(uint32_t heldId, bool throwIt) {
        auto it = std::find_if(m_held.begin(), m_held.end(),
                               [&](const Held& h) { return h.held == heldId; });
        if (it == m_held.end()) return false;
        const Held pair = *it;
        m_held.erase(it);

        auto held   = Live(m_sessions, heldId);
        auto holder = Live(m_sessions, pair.holder);
        if (holder) {
            holder->GetConnection()->SetMirrorPlayerId(0);
            holder->GetConnection()->SetRelayViewTo(0);
            SendRole(*holder, static_cast<uint8_t>(Network::ControlRole::None), heldId, "");
            // The stack leaves the holder's hands (already gone when this
            // is a drop or the stack was placed / moved out).
            const int slot = FindMarker(holder->GetPlayer()->getInventory(), pair.marker);
            if (slot >= 0) {
                holder->GetPlayer()->getInventory().SetItem(slot, Game::ItemStack{});
                holder->SendInventoryFull();
            }
        }
        if (!held) return true;
        held->GetPlayer()->setInvisible(false);
        m_pickupDelay[heldId] = Game::ItemEntity::kThrowPickupDelay;
        // Their own HUD back: inventory, stats and abilities, over the
        // holder's that were teed onto them.
        SendRole(*held, static_cast<uint8_t>(Network::ControlRole::None), pair.holder, "");
        held->SendInventoryFull();
        held->ResendStats();
        held->GetConnection()->SendPlayerAbilities(*held->GetPlayer());

        glm::vec3 vel(0.0f);
        if (holder) {
            const ServerPlayer& hp = *holder->GetPlayer();
            // MC LivingEntity.createItemStackToDrop, randomly=false: from
            // just below the eye, 0.3 along the look plus the 0.1 lift.
            const glm::dvec3 eye = hp.getPosition() + glm::dvec3(0.0, hp.getEyeHeight(), 0.0);
            const glm::vec3 fwd = Game::Mth::ViewVector(hp.getPitch(), hp.getYaw());
            if (throwIt) vel = fwd * 0.3f + glm::vec3(0.0f, 0.1f, 0.0f);
            const glm::dvec3 at = throwIt ? glm::dvec3(eye.x, eye.y - 0.3, eye.z) : hp.getPosition();
            held->GetConnection()->Teleport(at.x, at.y, at.z,
                                            held->GetPlayer()->getYaw(), held->GetPlayer()->getPitch());
        }
        SendHeld(*held, false, 0, vel);
        return true;
    }

    bool MorphCarry::ReleaseByDrop(uint32_t holderId, const Game::ItemStack& stack) {
        for (const Held& h : m_held) {
            if (h.holder != holderId || !IsMarker(stack, h.marker)) continue;
            return Release(h.held, true);
        }
        return false;
    }

    void MorphCarry::Tick() {
        for (auto it = m_pickupDelay.begin(); it != m_pickupDelay.end();) {
            if (--it->second <= 0) it = m_pickupDelay.erase(it);
            else ++it;
        }

        // Held pairs that no longer hold: either side gone, the held player
        // no longer an item, or the stack gone from the holder's inventory
        // (placed, put away) — let go where the holder stands.
        for (size_t i = 0; i < m_held.size();) {
            const Held h = m_held[i];
            auto held   = Live(m_sessions, h.held);
            auto holder = Live(m_sessions, h.holder);
            const bool stillItem = held && Game::Morph::IsValid(held->GetPlayer()->getMorph()) &&
                                   Game::Morph::KindOf(held->GetPlayer()->getMorph()) == Game::Morph::Kind::Item;
            const bool stillHeld = holder && FindMarker(holder->GetPlayer()->getInventory(), h.marker) >= 0;
            if (!held || !holder || !stillItem || !stillHeld ||
                held->GetDimensionId() != holder->GetDimensionId()) {
                Release(h.held, false);
                continue;   // Release erased it
            }
            ++i;
        }

        // Pickups.
        const auto sessions = m_sessions.GetAllSessions();
        for (const auto& s : sessions) {
            if (!s || !s->GetPlayer() || !s->GetConnection()) continue;
            const ServerPlayer& item = *s->GetPlayer();
            const uint32_t code = item.getMorph();
            if (!Game::Morph::IsValid(code) || Game::Morph::KindOf(code) != Game::Morph::Kind::Item) continue;
            const uint32_t id = s->GetPlayerId();
            if (IsHeld(id) || m_pickupDelay.count(id)) continue;
            if (m_control.TargetOf(id) || m_control.ControllerOf(id)) continue;
            const Box itemBox = BoxOf(item);
            for (const auto& p : sessions) {
                if (!p || p == s || !p->GetPlayer() || !p->GetConnection()) continue;
                if (p->GetDimensionId() != s->GetDimensionId()) continue;
                const uint32_t pid = p->GetPlayerId();
                if (IsHeld(pid)) continue;                     // a carried player carries nobody
                // The holder's HUD stream is one per player: no pickup while
                // they are in a /control pair or already carrying someone.
                if (m_control.TargetOf(pid) || m_control.ControllerOf(pid)) continue;
                if (p->GetConnection()->MirrorPlayerId() != 0) continue;
                const ServerPlayer& picker = *p->GetPlayer();
                if (picker.getHealth() <= 0.0f) continue;
                Box reach = BoxOf(picker);                     // MC: box.inflate(1, 0.5, 1)
                reach.min -= glm::dvec3(1.0, 0.5, 1.0);
                reach.max += glm::dvec3(1.0, 0.5, 1.0);
                if (!Intersects(reach, itemBox)) continue;
                Pickup(id, pid);
                break;
            }
        }
    }

} // namespace Server
