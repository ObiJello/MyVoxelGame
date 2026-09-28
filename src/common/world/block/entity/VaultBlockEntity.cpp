// File: src/common/world/block/entity/VaultBlockEntity.cpp
//
// MC VaultBlockEntity.Server / Client, VaultState, VaultServerData and
// VaultSharedData over the VaultBlockEntity that owns them. See the header.
#include "VaultBlockEntity.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/network/ItemStackSerialization.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/DispenseItemBehavior.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/block/GeneratedBlockStates.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/loot/ChestLootTables.hpp"
#include "server/player/ServerPlayer.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string_view>

namespace Game {

    namespace {

        constexpr const char* kStateNames[] = { "inactive", "active", "unlocking", "ejecting" };

        double DistSqr(const glm::ivec3& a, const glm::ivec3& b) {
            const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return dx * dx + dy * dy + dz * dz;
        }

        // MC Mth.nextDouble(random, min, max).
        double NextDouble(JavaRandom& random, double min, double max) {
            return min >= max ? min : random.NextDouble() * (max - min) + min;
        }

        // MC Mth.wrapDegrees(float).
        float WrapDegrees(float angle) {
            float wrapped = std::fmod(angle, 360.0f);
            if (wrapped >= 180.0f) wrapped -= 360.0f;
            if (wrapped < -180.0f) wrapped += 360.0f;
            return wrapped;
        }

        bool SameUuidSet(const std::vector<Uuid>& a, const std::vector<Uuid>& b) {
            if (a.size() != b.size()) return false;
            for (const Uuid& id : a) {
                if (std::find(b.begin(), b.end(), id) == b.end()) return false;
            }
            return true;
        }

        // The rewarding player's identity and LUCK: the server's player behind
        // the IUsePlayer (its entity view carries the offline UUID the vault
        // remembers), as ConsumableBehavior reaches it.
        bool PlayerIdentity(IUsePlayer& player, Uuid& outUuid, float& outLuck) {
            auto* server = dynamic_cast<Server::ServerPlayer*>(&player);
            if (!server) return false;
            LivingEntity* view = server->effectEntity();
            if (!view) return false;
            outUuid = view->GetUuid();
            outLuck = server->getLuck();
            return true;
        }

    } // namespace

    // ── VaultState ──────────────────────────────────────────────────────────

    namespace VaultStates {

        const char* Name(VaultState state) {
            const size_t i = static_cast<size_t>(state);
            return i < std::size(kStateNames) ? kStateNames[i] : kStateNames[0];
        }

        VaultState Of(BlockState state) {
            if (!state.HasProperty(PropertyId::VAULT_STATE)) return VaultState::Inactive;
            const std::string_view name = state.GetName(PropertyId::VAULT_STATE);
            for (size_t i = 0; i < std::size(kStateNames); ++i) {
                if (name == kStateNames[i]) return static_cast<VaultState>(i);
            }
            return VaultState::Inactive;
        }

        bool OminousOf(BlockState state) {
            return state.HasProperty(PropertyId::OMINOUS) && state.GetName(PropertyId::OMINOUS) == "true";
        }

    } // namespace VaultStates

    // ── VaultServerData / VaultSharedData ───────────────────────────────────

    void VaultBlockEntity::LoadServerData(const VaultServerData& data) {
        m_serverData.stateUpdatingResumesAt = data.stateUpdatingResumesAt;
        m_serverData.itemsToEject = data.itemsToEject;
        m_serverData.rewardedPlayers = data.rewardedPlayers;
    }

    void VaultBlockEntity::PauseStateUpdatingUntil(int64_t tick) {
        m_serverData.stateUpdatingResumesAt = tick;
        m_serverDirty = true;
    }

    void VaultBlockEntity::SetItemsToEject(std::vector<ItemStack> items) {
        m_serverData.itemsToEject = std::move(items);
        m_serverData.totalEjectionsNeeded = static_cast<int>(m_serverData.itemsToEject.size());
        m_serverDirty = true;
    }

    ItemStack VaultBlockEntity::GetNextItemToEject() const {
        return m_serverData.itemsToEject.empty() ? ItemStack{} : m_serverData.itemsToEject.back();
    }

    ItemStack VaultBlockEntity::PopNextItemToEject() {
        if (m_serverData.itemsToEject.empty()) return ItemStack{};
        m_serverDirty = true;
        ItemStack out = std::move(m_serverData.itemsToEject.back());
        m_serverData.itemsToEject.pop_back();
        return out;
    }

    float VaultBlockEntity::EjectionProgress() const {
        // 1 - Mth.inverseLerp(itemsLeft, 1, totalEjectionsNeeded).
        const int total = m_serverData.totalEjectionsNeeded;
        if (total == 1) return 1.0f;
        const float left = static_cast<float>(m_serverData.itemsToEject.size());
        return 1.0f - (left - 1.0f) / (static_cast<float>(total) - 1.0f);
    }

    bool VaultBlockEntity::HasRewardedPlayer(const Uuid& uuid) const {
        return std::find(m_serverData.rewardedPlayers.begin(), m_serverData.rewardedPlayers.end(), uuid) !=
               m_serverData.rewardedPlayers.end();
    }

    void VaultBlockEntity::AddToRewardedPlayers(const Uuid& uuid) {
        if (!HasRewardedPlayer(uuid)) m_serverData.rewardedPlayers.push_back(uuid);
        // Past 128 the oldest entry goes (the linked set's first).
        if (m_serverData.rewardedPlayers.size() > VaultServerData::kMaxRewardPlayers) {
            m_serverData.rewardedPlayers.erase(m_serverData.rewardedPlayers.begin());
        }
        m_serverDirty = true;
    }

    void VaultBlockEntity::SetDisplayItem(const ItemStack& stack) {
        // ItemStack.matches: same item, count and components.
        const ItemStack& current = m_sharedData.displayItem;
        const bool same = current.IsEmpty() ? stack.IsEmpty()
                                            : (!stack.IsEmpty() && current.count == stack.count &&
                                               IsSameItemSameComponents(current, stack));
        if (same) return;
        m_sharedData.displayItem = stack;
        m_sharedDirty = true;
    }

    void VaultBlockEntity::UpdateConnectedPlayersWithinRange(World& world, double range) {
        // PlayerDetector.INCLUDING_CREATIVE_PLAYERS (no line of sight): every
        // non-spectator whose block position is closer than `range`, minus
        // the players already rewarded.
        EntityLevel* level = world.Entities();
        if (!level) return;
        const glm::ivec3 pos = GetWorldPos();
        std::vector<LivingEntity*> players;
        level->GetPlayers(players);
        std::vector<Uuid> current;
        for (LivingEntity* p : players) {
            if (!p || p->IsSpectator()) continue;
            if (!(DistSqr(p->BlockPosition(), pos) < range * range)) continue;
            if (HasRewardedPlayer(p->GetUuid())) continue;
            if (std::find(current.begin(), current.end(), p->GetUuid()) == current.end()) {
                current.push_back(p->GetUuid());
            }
        }
        if (!SameUuidSet(m_sharedData.connectedPlayers, current)) {
            m_sharedData.connectedPlayers = std::move(current);
            m_sharedDirty = true;
        }
    }

    // ── VaultBlockEntity.Server ─────────────────────────────────────────────

    bool VaultBlockEntity::CanEjectReward(VaultState state) const {
        return !m_config.keyItem.IsEmpty() && state != VaultState::Inactive;
    }

    bool VaultBlockEntity::IsValidToInsert(const ItemStack& stack) const {
        return !stack.IsEmpty() && IsSameItemSameComponents(stack, m_config.keyItem) &&
               stack.count >= m_config.keyItem.count;
    }

    void VaultBlockEntity::PlayInsertFailSound(ILevelWrite& level, const char* sound) {
        if (level.GameTime() >= m_lastInsertFailTimestamp + kInsertFailSoundBufferTicks) {
            level.PlaySound(nullptr, GetWorldPos(), sound, SoundSource::Blocks);
            m_lastInsertFailTimestamp = level.GameTime();
        }
    }

    ItemStack VaultBlockEntity::RandomDisplayItemFromLootTable(World& world, const std::string& lootTable) {
        // The table rolled with the VAULT parameter set at the vault's
        // centre (the level random), then one of the results at random.
        JavaRandom* random = world.Random();
        if (!random) return ItemStack{};
        const glm::ivec3 pos = GetWorldPos();
        ChestLoot::LootLevelContext lootLevel;
        lootLevel.dimensionId = DimensionToRaw(world.GetDimension());
        lootLevel.origin = glm::dvec3(pos) + glm::dvec3(0.5);
        std::vector<ItemStack> results;
        ChestLoot::GetRandomItems(lootTable, *random, 0.0f, results, &lootLevel);
        if (results.empty()) return ItemStack{};
        return results[static_cast<size_t>(random->NextInt(static_cast<int32_t>(results.size())))];
    }

    void VaultBlockEntity::CycleDisplayItemFromLootTable(World& world, VaultState state) {
        if (!CanEjectReward(state)) {
            SetDisplayItem(ItemStack{});
            return;
        }
        SetDisplayItem(RandomDisplayItemFromLootTable(
            world, m_config.overrideLootTableToDisplay.value_or(m_config.lootTable)));
    }

    void VaultBlockEntity::SetVaultState(World& world, BlockState current, BlockState next) {
        // setBlockAndUpdate, then the old state's exit and the new one's
        // enter (VaultState.onTransition).
        const glm::ivec3 pos = GetWorldPos();
        world.SetBlock(pos.x, pos.y, pos.z, next, World::UpdateFlags::All);
        OnExit(VaultStates::Of(current), world);
        OnEnter(VaultStates::Of(next), world, VaultStates::OminousOf(next));
    }

    void VaultBlockEntity::Unlock(World& world, BlockState state, std::vector<ItemStack> itemsToEject) {
        SetItemsToEject(std::move(itemsToEject));
        SetDisplayItem(GetNextItemToEject());
        PauseStateUpdatingUntil(world.GetGameTime() + kUnlockingDelayTicks);
        SetVaultState(world, state,
                      state.SetName(PropertyId::VAULT_STATE, VaultStates::Name(VaultState::Unlocking)));
    }

    void VaultBlockEntity::TryInsertKey(ILevelWrite& level, IUsePlayer& player, ItemStack& stack) {
        auto* world = dynamic_cast<World*>(&level);
        if (!world) return;
        const glm::ivec3 pos = GetWorldPos();
        const BlockState state = world->GetBlockState(pos.x, pos.y, pos.z);
        if (state.Block() != BlockID::Vault) return;
        if (!CanEjectReward(VaultStates::Of(state))) return;

        Uuid uuid{};
        float luck = 0.0f;
        if (!PlayerIdentity(player, uuid, luck)) return;

        if (!IsValidToInsert(stack)) {
            PlayInsertFailSound(level, SoundEvents::VAULT_INSERT_ITEM_FAIL);
        } else if (HasRewardedPlayer(uuid)) {
            PlayInsertFailSound(level, SoundEvents::VAULT_REJECT_REWARDED_PLAYER);
        } else {
            // resolveItemsToEject: the loot table with the player's LUCK,
            // ORIGIN at the vault's centre, the key as the TOOL (the level
            // random).
            JavaRandom* random = world->Random();
            if (!random) return;
            ChestLoot::LootLevelContext lootLevel;
            lootLevel.dimensionId = DimensionToRaw(world->GetDimension());
            lootLevel.origin = glm::dvec3(pos) + glm::dvec3(0.5);
            std::vector<ItemStack> itemsToEject;
            ChestLoot::GetRandomItems(m_config.lootTable, *random, luck, itemsToEject, &lootLevel);
            if (itemsToEject.empty()) return;
            // player.awardStat(ITEM_USED): no statistics here.
            // stackToInsert.consume(keyItem.count, player): creative keeps it.
            if (!player.isCreative()) {
                stack.count -= m_config.keyItem.count;
                if (stack.count <= 0) stack.Clear();
            }
            Unlock(*world, state, std::move(itemsToEject));
            AddToRewardedPlayers(uuid);
            UpdateConnectedPlayersWithinRange(*world, m_config.deactivationRange);
        }
        // The dirty halves are saved and sent by the next Tick, as MC's
        // isDirty flags are.
    }

    void VaultBlockEntity::Tick(World* world, float /*deltaTime*/) {
        if (!world) return;
        const glm::ivec3 pos = GetWorldPos();
        const BlockState blockState = world->GetBlockState(pos.x, pos.y, pos.z);
        if (blockState.Block() != BlockID::Vault) return;
        const VaultState currentState = VaultStates::Of(blockState);
        const int64_t now = world->GetGameTime();

        // shouldCycleDisplayItem: every 20 ticks while ACTIVE.
        if (now % kDisplayCycleTickRate == 0 && currentState == VaultState::Active) {
            CycleDisplayItemFromLootTable(*world, currentState);
        }

        if (now >= m_serverData.stateUpdatingResumesAt) {
            const BlockState next = blockState.SetName(
                PropertyId::VAULT_STATE, VaultStates::Name(TickAndGetNext(currentState, *world)));
            if (next != blockState) SetVaultState(*world, blockState, next);
        }

        if (m_serverDirty || m_sharedDirty) {
            MarkDirty();
            if (m_sharedDirty) world->BlockEntityChanged(pos);
            m_serverDirty = false;
            m_sharedDirty = false;
        }
    }

    // ── VaultState ──────────────────────────────────────────────────────────

    VaultState VaultBlockEntity::UpdateStateForConnectedPlayers(World& world, double range) {
        UpdateConnectedPlayersWithinRange(world, range);
        PauseStateUpdatingUntil(world.GetGameTime() + kUpdateConnectedPlayersTickRate);
        return m_sharedData.connectedPlayers.empty() ? VaultState::Inactive : VaultState::Active;
    }

    VaultState VaultBlockEntity::TickAndGetNext(VaultState current, World& world) {
        switch (current) {
            case VaultState::Inactive:
                return UpdateStateForConnectedPlayers(world, m_config.activationRange);
            case VaultState::Active:
                return UpdateStateForConnectedPlayers(world, m_config.deactivationRange);
            case VaultState::Unlocking:
                PauseStateUpdatingUntil(world.GetGameTime() + kDelayBetweenEjectionsTicks);
                return VaultState::Ejecting;
            case VaultState::Ejecting:
                if (m_serverData.itemsToEject.empty()) {
                    // markEjectionFinished.
                    m_serverData.totalEjectionsNeeded = 0;
                    m_serverDirty = true;
                    return UpdateStateForConnectedPlayers(world, m_config.deactivationRange);
                } else {
                    const float progress = EjectionProgress();
                    EjectResultItem(world, PopNextItemToEject(), progress);
                    SetDisplayItem(GetNextItemToEject());
                    // DELAY_AFTER_LAST_EJECTION_TICKS == DELAY_BETWEEN_EJECTIONS_TICKS.
                    PauseStateUpdatingUntil(world.GetGameTime() + kDelayBetweenEjectionsTicks);
                    return VaultState::Ejecting;
                }
        }
        return current;
    }

    void VaultBlockEntity::OnEnter(VaultState state, World& world, bool ominous) {
        const glm::ivec3 pos = GetWorldPos();
        switch (state) {
            case VaultState::Inactive:
                SetDisplayItem(ItemStack{});
                world.PlayLevelEvent(nullptr, LevelEvent::ANIMATION_VAULT_DEACTIVATE, pos, ominous ? 1 : 0);
                break;
            case VaultState::Active:
                if (m_sharedData.displayItem.IsEmpty()) CycleDisplayItemFromLootTable(world, VaultState::Active);
                world.PlayLevelEvent(nullptr, LevelEvent::ANIMATION_VAULT_ACTIVATE, pos, ominous ? 1 : 0);
                break;
            case VaultState::Unlocking:
                world.PlaySound(nullptr, pos, SoundEvents::VAULT_INSERT_ITEM, SoundSource::Blocks);
                break;
            case VaultState::Ejecting:
                world.PlaySound(nullptr, pos, SoundEvents::VAULT_OPEN_SHUTTER, SoundSource::Blocks);
                break;
        }
    }

    void VaultBlockEntity::OnExit(VaultState state, World& world) {
        if (state == VaultState::Ejecting) {
            world.PlaySound(nullptr, GetWorldPos(), SoundEvents::VAULT_CLOSE_SHUTTER, SoundSource::Blocks);
        }
    }

    void VaultBlockEntity::EjectResultItem(World& world, const ItemStack& item, float ejectionSoundProgress) {
        const glm::ivec3 pos = GetWorldPos();
        DispenseSpawnItem(world, item, 2, static_cast<int>(Direction::Up),
                          glm::dvec3(pos.x + 0.5, pos.y + 1.2, pos.z + 0.5));
        world.PlayLevelEvent(nullptr, LevelEvent::ANIMATION_VAULT_EJECT_ITEM, pos, 0);
        world.PlaySound(nullptr, pos, SoundEvents::VAULT_EJECT_ITEM, SoundSource::Blocks, 1.0f,
                        0.8f + 0.4f * ejectionSoundProgress);
    }

    // ── VaultBlockEntity.Client ─────────────────────────────────────────────

    void VaultBlockEntity::EmitConnectionParticlesForNearbyPlayers(ILevelWrite& level) const {
        if (m_sharedData.connectedPlayers.empty()) return;
        JavaRandom* random = level.Random();
        if (!random) return;
        const glm::ivec3 pos = GetWorldPos();
        // keyholePos: the bottom centre raised 1.75 and pushed half a block
        // out through the front face.
        const BlockState state = level.GetBlockState(pos.x, pos.y, pos.z);
        const std::string_view facing = state.GetValueByName("facing");
        int stepX = 0, stepZ = 0;
        if (facing == "north")      stepZ = -1;
        else if (facing == "south") stepZ = 1;
        else if (facing == "west")  stepX = -1;
        else if (facing == "east")  stepX = 1;
        const glm::dvec3 keyhole(pos.x + 0.5 + stepX * 0.5, pos.y + 1.75, pos.z + 0.5 + stepZ * 0.5);
        const double range = m_sharedData.connectedParticlesRange;

        for (const Uuid& uuid : m_sharedData.connectedPlayers) {
            glm::dvec3 playerPos;
            float bbHeight = 0.0f;
            if (!level.GetPlayerByUuid(uuid, playerPos, bbHeight)) continue;
            // isWithinConnectionRange: blockPosition().distSqr(vaultPos) <= range².
            const glm::ivec3 playerBlock(static_cast<int>(std::floor(playerPos.x)),
                                         static_cast<int>(std::floor(playerPos.y)),
                                         static_cast<int>(std::floor(playerPos.z)));
            if (DistSqr(playerBlock, pos) > range * range) continue;
            // emitConnectionParticlesForPlayer: 2..5 motes from the keyhole
            // toward the player's middle, each direction jittered by ±0.5.
            const glm::dvec3 direction = playerPos + glm::dvec3(0.0, bbHeight / 2.0f, 0.0) - keyhole;
            const int count = random->NextInt(2, 5);
            for (int i = 0; i < count; ++i) {
                const double dx = static_cast<double>((random->NextFloat() - 0.5f) * 1.0f);
                const double dy = static_cast<double>((random->NextFloat() - 0.5f) * 1.0f);
                const double dz = static_cast<double>((random->NextFloat() - 0.5f) * 1.0f);
                level.AddParticle(ParticleKind::VaultConnection, keyhole.x, keyhole.y, keyhole.z,
                                  direction.x + dx, direction.y + dy, direction.z + dz);
            }
        }
    }

    void VaultBlockEntity::ClientTick(ILevelWrite& level) {
        const glm::ivec3 pos = GetWorldPos();
        const BlockState state = level.GetBlockState(pos.x, pos.y, pos.z);
        if (state.Block() != BlockID::Vault) return;

        // VaultClientData.updateDisplayItemSpin.
        m_previousSpin = m_currentSpin;
        m_currentSpin = WrapDegrees(m_currentSpin + kRotationSpeed);

        if (level.GameTime() % kParticleTickRate == 0) EmitConnectionParticlesForNearbyPlayers(level);

        JavaRandom* random = level.Random();
        if (!random) return;
        // emitIdleParticles: smoke in the cage, a flame with it while there
        // is something on display.
        if (random->NextFloat() <= kIdleParticleChance) {
            const double x = pos.x + NextDouble(*random, 0.1, 0.9);
            const double y = pos.y + NextDouble(*random, 0.25, 0.75);
            const double z = pos.z + NextDouble(*random, 0.1, 0.9);
            level.AddParticle(ParticleKind::Smoke, x, y, z, 0.0, 0.0, 0.0);
            if (ShouldDisplayActiveEffects()) {
                level.AddParticle(VaultStates::OminousOf(state) ? ParticleKind::SoulFireFlame
                                                                : ParticleKind::SmallFlame,
                                  x, y, z, 0.0, 0.0, 0.0);
            }
        }
        // playIdleSounds.
        if (ShouldDisplayActiveEffects() && random->NextFloat() <= kAmbientSoundChance) {
            const float volume = random->NextFloat() * 0.25f + 0.75f;
            const float pitch = random->NextFloat() + 0.5f;
            level.PlayLocalSound(pos, SoundEvents::VAULT_AMBIENT, SoundSource::Blocks, volume, pitch, false);
        }
    }

    void VaultBlockEntity::CarryClientState(const BlockEntity& previous) {
        if (const auto* old = dynamic_cast<const VaultBlockEntity*>(&previous)) {
            m_currentSpin = old->m_currentSpin;
            m_previousSpin = old->m_previousSpin;
        }
    }

    // ── Wire (MC getUpdateTag: shared_data) ─────────────────────────────────

    void VaultBlockEntity::Save(Network::PacketBuffer& out) const {
        Network::Serialization::WriteItemStack(out, m_sharedData.displayItem);
        out.WriteVarInt(static_cast<uint32_t>(m_sharedData.connectedPlayers.size()));
        for (const Uuid& id : m_sharedData.connectedPlayers) {
            for (uint8_t b : id) out.WriteByte(b);
        }
        out.WriteDouble(m_sharedData.connectedParticlesRange);
    }

    void VaultBlockEntity::Load(Network::PacketReader& in) {
        m_sharedData.displayItem = Network::Serialization::ReadItemStack(in);
        const uint32_t count = std::min<uint32_t>(in.ReadVarInt(), 1024u);
        m_sharedData.connectedPlayers.clear();
        m_sharedData.connectedPlayers.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Uuid id{};
            for (uint8_t& b : id) b = in.ReadByte();
            m_sharedData.connectedPlayers.push_back(id);
        }
        m_sharedData.connectedParticlesRange = in.ReadDouble();
    }

} // namespace Game
