// File: src/common/world/block/entity/VaultBlockEntity.hpp
//
// MC net.minecraft.world.level.block.entity.vault — VaultBlockEntity and its
// four halves:
//
//   VaultConfig      the loot table, the activation (4) / deactivation (4.5)
//                    ranges, the key item (a trial key; an ominous trial key
//                    for the ominous vaults) and an optional table the
//                    display cycles through instead of the loot table
//   VaultServerData  who has already been rewarded (a vault opens once per
//                    player, remembering the last 128), the items still to
//                    eject, and the pause that paces the state machine
//   VaultSharedData  what the client sees: the item on display, the players
//                    connected to the vault, and the particle range
//   VaultClientData  the display item's spin
//
// The vault_state property is the state machine (VaultState):
//   inactive ⇄ active     a non-rewarded player within 4 (activation) /
//                         4.5 (deactivation) blocks, rechecked every 20 ticks
//   active → unlocking    a valid key used on it (VaultBlock.useItemOn →
//                         tryInsertKey): the reward is rolled at once
//   unlocking → ejecting  14 ticks later, then one item every 20 ticks
//   ejecting → active / inactive  once empty
//
// Wire (MC getUpdateTag): shared_data only — the display item, the connected
// players (UUIDs) and the particle range.
#pragma once

#include "BlockEntity.hpp"
#include "common/core/Uuid.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Game {

    class JavaRandom;
    class IUsePlayer;
    struct EntityLevel;

    // MC VaultState, in its declaration (and block-property) order.
    enum class VaultState : uint8_t { Inactive = 0, Active, Unlocking, Ejecting };

    namespace VaultStates {
        const char* Name(VaultState state);
        VaultState Of(BlockState state);
        bool OminousOf(BlockState state);
    }

    // MC VaultConfig. Defaults are VaultConfig.DEFAULT.
    struct VaultConfig {
        static constexpr const char* kDefaultLootTable = "minecraft:chests/trial_chambers/reward";
        static constexpr double kDefaultActivationRange = 4.0;
        static constexpr double kDefaultDeactivationRange = 4.5;

        std::string lootTable = kDefaultLootTable;
        double activationRange = kDefaultActivationRange;
        double deactivationRange = kDefaultDeactivationRange;
        ItemStack keyItem = ItemStack(Items::TrialKey, 1);
        std::optional<std::string> overrideLootTableToDisplay;
    };

    // MC VaultServerData (persisted fields).
    struct VaultServerData {
        static constexpr size_t kMaxRewardPlayers = 128;

        std::vector<Uuid> rewardedPlayers;   // a linked set: oldest first
        int64_t stateUpdatingResumesAt = 0;
        std::vector<ItemStack> itemsToEject;
        int totalEjectionsNeeded = 0;
    };

    // MC VaultSharedData.
    struct VaultSharedData {
        ItemStack displayItem;
        std::vector<Uuid> connectedPlayers;   // a linked set
        double connectedParticlesRange = VaultConfig::kDefaultDeactivationRange;
    };

    class VaultBlockEntity : public BlockEntity {
    public:
        // VaultBlockEntity.Server / VaultState constants.
        static constexpr int kUnlockingDelayTicks = 14;
        static constexpr int kDisplayCycleTickRate = 20;
        static constexpr int kInsertFailSoundBufferTicks = 15;
        static constexpr int kUpdateConnectedPlayersTickRate = 20;
        static constexpr int kDelayBetweenEjectionsTicks = 20;
        // VaultClientData.ROTATION_SPEED and the client's rates.
        static constexpr float kRotationSpeed = 10.0f;
        static constexpr int   kParticleTickRate = 20;
        static constexpr float kIdleParticleChance = 0.5f;
        static constexpr float kAmbientSoundChance = 0.02f;

        VaultBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        bool NeedsTicking() const override { return true; }
        // VaultBlockEntity.Server.tick / Client.tick.
        void Tick(World* world, float deltaTime) override;
        void ClientTick(ILevelWrite& level) override;

        // VaultBlockEntity.Server.tryInsertKey — VaultBlock.useItemOn's
        // server half. `stack` is the player's held stack (consumed on a
        // successful unlock unless the player is creative).
        void TryInsertKey(ILevelWrite& level, IUsePlayer& player, ItemStack& stack);

        // VaultBlockEntity.Client.emitActivationParticles's connection half
        // (level event 3015 carries no shared data; the client's copy of this
        // entity has it).
        void EmitConnectionParticlesForNearbyPlayers(ILevelWrite& level) const;

        // ── Persistent state (loadAdditional / saveAdditional) ────────────
        const VaultConfig& GetConfig() const { return m_config; }
        void SetConfig(VaultConfig config) { m_config = std::move(config); }
        const VaultServerData& GetServerData() const { return m_serverData; }
        // MC VaultServerData.set — copies everything BUT totalEjectionsNeeded
        // (vanilla's own omission: a vault loaded mid-ejection pitches its
        // eject sounds from zero).
        void LoadServerData(const VaultServerData& data);
        const VaultSharedData& GetSharedData() const { return m_sharedData; }
        void LoadSharedData(const VaultSharedData& data) { m_sharedData = data; }

        // ── Client (VaultRenderer) ────────────────────────────────────────
        float GetCurrentSpin() const { return m_currentSpin; }
        float GetPreviousSpin() const { return m_previousSpin; }
        // VaultBlockEntity.Client.shouldDisplayActiveEffects.
        bool ShouldDisplayActiveEffects() const { return !m_sharedData.displayItem.IsEmpty(); }

        void CarryClientState(const BlockEntity& previous) override;

        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;

    private:
        // ── VaultBlockEntity.Server ─────────────────────────────────────────
        void SetVaultState(World& world, BlockState current, BlockState next);
        void CycleDisplayItemFromLootTable(World& world, VaultState state);
        ItemStack RandomDisplayItemFromLootTable(World& world, const std::string& lootTable);
        void Unlock(World& world, BlockState state, std::vector<ItemStack> itemsToEject);
        bool CanEjectReward(VaultState state) const;
        bool IsValidToInsert(const ItemStack& stack) const;
        void PlayInsertFailSound(ILevelWrite& level, const char* sound);

        // ── VaultState ──────────────────────────────────────────────────────
        VaultState TickAndGetNext(VaultState current, World& world);
        VaultState UpdateStateForConnectedPlayers(World& world, double range);
        void OnEnter(VaultState state, World& world, bool ominous);
        void OnExit(VaultState state, World& world);
        void EjectResultItem(World& world, const ItemStack& item, float ejectionSoundProgress);

        // ── VaultServerData / VaultSharedData ───────────────────────────────
        void PauseStateUpdatingUntil(int64_t tick);
        void SetItemsToEject(std::vector<ItemStack> items);
        ItemStack GetNextItemToEject() const;
        ItemStack PopNextItemToEject();
        float EjectionProgress() const;
        bool HasRewardedPlayer(const Uuid& uuid) const;
        void AddToRewardedPlayers(const Uuid& uuid);
        void SetDisplayItem(const ItemStack& stack);
        void UpdateConnectedPlayersWithinRange(World& world, double range);

        VaultConfig m_config;
        VaultServerData m_serverData;
        VaultSharedData m_sharedData;
        // Runtime (not saved, as in MC).
        int64_t m_lastInsertFailTimestamp = 0;
        bool m_serverDirty = false;
        bool m_sharedDirty = false;

        // VaultClientData.
        float m_currentSpin = 0.0f;
        float m_previousSpin = 0.0f;
    };

} // namespace Game
