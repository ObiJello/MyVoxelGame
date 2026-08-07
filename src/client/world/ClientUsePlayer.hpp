// File: src/client/world/ClientUsePlayer.hpp
//
// Game::IUsePlayer over the local ClientPlayer, so the shared item-use
// behaviours (hoe, shovel, axe, flint & steel, bone meal, bucket) can run
// client-side for prediction — the client half of what MC does inside
// MultiPlayerGameMode.startPrediction.
//
// Slot writes here hit the client's local inventory mirror. That mirror is
// already predictive (placement consumption does the same thing) and the
// server's InventorySetSlotS2C is authoritative, so a wrong guess self-heals
// on the next delta.
#pragma once

#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "../entity/Player.hpp"

namespace Client {

    class ClientUsePlayer : public Game::IUsePlayer {
    public:
        explicit ClientUsePlayer(Game::ClientPlayer* player) : m_player(player) {
            if (m_player) m_position = glm::dvec3(m_player->physics.position);
        }

        const glm::dvec3& getPosition() const override { return m_position; }
        float getYaw()   const override { return m_yaw; }
        float getPitch() const override { return m_pitch; }

        // The bucket's POV clip uses these; the server snaps its own rotation
        // to the click before running the same code, so feeding it the camera
        // angles the click was made at keeps both sides on the same ray.
        void setRotation(float yawDeg, float pitchDeg) { m_yaw = yawDeg; m_pitch = pitchDeg; }

        bool isCreative() const override {
            return m_player && m_player->IsCreative();
        }
        bool IsSneaking() const override {
            return m_player && m_player->physics.isSneaking;
        }

        Game::ItemStack& getItemInHand(uint32_t hand) override {
            static Game::ItemStack s_empty{};
            if (!m_player) return s_empty;
            const int slot = handSlotIndex(hand);
            if (slot < 0) return s_empty;
            return m_player->inventory.MutableSlot(slot);
        }

        int handSlotIndex(uint32_t hand) const override {
            if (!m_player) return -1;
            // Mirrors ServerPlayer::handSlotIndex — main hand is the selected
            // hotbar slot, offhand is the dedicated slot 45.
            return (hand == 0)
                ? Game::Inventory::HotbarToIndex(m_player->inventory.GetSelectedSlot())
                : Game::Inventory::OFFHAND_BEGIN;
        }

        // Server-only concept (queues a re-broadcast). The client's slot state
        // is already local, so there is nothing to mark.
        void markSlotDirty(int /*slotIndex*/) override {}

    private:
        Game::ClientPlayer* m_player = nullptr;
        glm::dvec3          m_position{0.0};
        float               m_yaw   = 0.0f;
        float               m_pitch = 0.0f;
    };

} // namespace Client
