// File: src/common/entity/mobs/Allay.cpp
//
// MC net.minecraft.world.entity.animal.allay.Allay, method by method — see
// the class comment in AnimatedMobs.hpp for its two game-event listeners
// (the vibration listener on the level's dispatcher, the polled jukebox one).
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/world/level/World.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/ai/brain/AllayAi.hpp"
#include "common/entity/ai/brain/Brain.hpp"
#include "common/entity/alchemy/Potions.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/entity/JukeboxBlockEntity.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include "common/world/level/BlockClip.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace Game {

    namespace {

        // MC BlockPos.containing(Vec3).
        glm::ivec3 Containing(const glm::dvec3& p) {
            return glm::ivec3(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)),
                              static_cast<int>(std::floor(p.z)));
        }

        // MC BlockPos.distSqr — between two integer positions.
        int64_t BlockDistSqr(const glm::ivec3& a, const glm::ivec3& b) {
            const int64_t dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return dx * dx + dy * dy + dz * dz;
        }

        // MC ItemStack.isSameItem: the same item, components ignored.
        bool IsSameItem(const ItemStack& a, const ItemStack& b) { return a.itemId == b.itemId; }

    } // namespace

    // ══ Construction / brain ══════════════════════════════════════════════

    Allay::Allay(EntityLevel* level)
        : GenericPathfinderMob(EntityTypeId::Allay, level),
          m_vibrationUser(*this),
          m_vibrationListener(*this),
          m_dynamicGameEventListener(m_vibrationListener) {
        // NO GOALS — MC's Allay never registers any; its whole behaviour is
        // the brain (the def's flying navigation + hover move control stand).
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        m_brain = std::make_unique<Brain>();
        AllayAi::InitBrain(*this, *m_brain);
    }

    void Allay::UpdateBrainActivity() { AllayAi::UpdateActivity(*this); }

    const char* Allay::GetAmbientSound() const {
        return HasItemInHand() ? SoundEvents::ALLAY_AMBIENT_WITH_ITEM
                               : SoundEvents::ALLAY_AMBIENT_WITHOUT_ITEM;
    }

    // ══ The held item / inventory ═════════════════════════════════════════

    void Allay::SetMainHandItem(const ItemStack& stack) {
        m_handItem = stack;
        if (m_handItem.count <= 0) m_handItem = ItemStack{};
        m_handItemDirty = true;
        m_clientHoldingItem = !m_handItem.IsEmpty();
    }

    bool Allay::HasItemInHand() const {
        if (m_level && m_level->IsClientSide()) return m_clientHoldingItem || !m_handItem.IsEmpty();
        return !m_handItem.IsEmpty();
    }

    ItemStack* Allay::EquipmentInSlot(EquipmentSlot slot) {
        return slot == EquipmentSlot::MAINHAND ? &m_handItem : nullptr;
    }

    bool Allay::InventoryCanAddItem(const ItemStack& stack) const {
        // MC SimpleContainer.canAddItem: an empty slot, or the same item and
        // components with room left in the stack.
        for (int i = 0; i < m_inventory.GetContainerSize(); ++i) {
            const ItemStack& slot = m_inventory.GetItem(i);
            if (slot.IsEmpty()) return true;
            if (IsSameItemSameComponents(slot, stack) &&
                slot.count < ItemRegistry::Get(slot.itemId).maxStackSize) {
                return true;
            }
        }
        return false;
    }

    ItemStack Allay::InventoryAddItem(const ItemStack& stack) {
        // MC SimpleContainer.addItem: moveItemToOccupiedSlotsWithSameType,
        // then moveItemToEmptySlots; the remainder comes back.
        if (stack.IsEmpty()) return {};
        ItemStack rest = stack;
        const int maxStack = std::min(m_inventory.GetMaxStackSize(rest),
                                      ItemRegistry::Get(rest.itemId).maxStackSize);
        for (int i = 0; i < m_inventory.GetContainerSize() && !rest.IsEmpty(); ++i) {
            ItemStack& slot = m_inventory.GetItem(i);
            if (slot.IsEmpty() || !IsSameItemSameComponents(slot, rest)) continue;
            const int moved = std::min(rest.count, maxStack - slot.count);
            if (moved <= 0) continue;
            slot.count += moved;
            rest.count -= moved;
        }
        for (int i = 0; i < m_inventory.GetContainerSize() && !rest.IsEmpty(); ++i) {
            if (!m_inventory.GetItem(i).IsEmpty()) continue;
            m_inventory.SetItem(i, rest);
            rest = ItemStack{};
        }
        if (rest.count <= 0) rest = ItemStack{};
        return rest;
    }

    bool Allay::CanPickUpLootNow() const {
        // MC canPickUpLoot: !isOnPickupCooldown() && hasItemInHand().
        const Brain* brain = GetBrain();
        const bool onCooldown = brain && brain->HasMemoryValue(MemoryModule::ItemPickupCooldownTicks);
        return !onCooldown && HasItemInHand();
    }

    bool Allay::WantsToPickUp(const ItemStack& stack) const {
        // MC wantsToPickUp: a held item, mobGriefing, room, and
        // allayConsidersItemEqual (same item, and the same POTION_CONTENTS —
        // hasNonMatchingPotion compares the two optionals, so a plain stack
        // never matches a potion and two different potions never match).
        if (m_handItem.IsEmpty() || !m_level || !m_level->MobGriefing()) return false;
        if (!InventoryCanAddItem(stack)) return false;
        if (!IsSameItem(m_handItem, stack)) return false;
        return m_handItem.get(DataComponents::POTION_CONTENTS) == stack.get(DataComponents::POTION_CONTENTS);
    }

    void Allay::PickUpItem(int32_t itemEntityId, const ItemStack& stack) {
        // MC Allay.pickUpItem → InventoryCarrier.pickUpItem, reached from
        // Mob.aiStep's looting (Mob::TickLooting: canPickUpLoot, alive, not
        // dead, mobGriefing; the box grown by (1, 1, 1); past the pickup
        // delay; wanted): room or nothing, then the part that fits goes in
        // and is taken from the item entity.
        if (!InventoryCanAddItem(stack)) return;
        OnItemPickup(itemEntityId, stack);
        const ItemStack remainder = InventoryAddItem(stack);
        const int taken = stack.count - remainder.count;
        if (taken > 0) TakeItemEntity(itemEntityId, taken);   // take: the fly-in packet too
    }

    // ══ LIKED_PLAYER ══════════════════════════════════════════════════════

    void Allay::SetLikedPlayerUuid(std::optional<Uuid> uuid) {
        m_likedPlayer = std::move(uuid);
        if (Brain* brain = GetBrain()) {
            if (m_likedPlayer) brain->SetMemory(MemoryModule::LikedPlayer, std::monostate{});
            else               brain->EraseMemory(MemoryModule::LikedPlayer);
        }
    }

    bool Allay::IsLikedPlayer(const Entity* other) const {
        // MC isLikedPlayer: a Player whose UUID is the LIKED_PLAYER memory.
        return other && other->IsPlayer() && m_likedPlayer && other->GetUuid() == *m_likedPlayer;
    }

    LivingEntity* Allay::GetLikedPlayer() const {
        // MC AllayAi.getLikedPlayer: serverLevel.getEntity(uuid) — THIS
        // level — a ServerPlayer whose game mode is survival-like or creative
        // (not spectator), within 64 blocks.
        if (!m_level || m_level->IsClientSide() || !m_likedPlayer) return nullptr;
        LivingEntity* player = m_level->ResolvePlayer(*m_likedPlayer);
        if (!player || player->Level() != m_level || player->IsSpectator()) return nullptr;
        if (player->DistanceToSqr(*this) >= 64.0 * 64.0) return nullptr;
        return player;
    }

    // ══ Hurt / interact / death ═══════════════════════════════════════════

    bool Allay::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC hurtServer: `isLikedPlayer(source.getEntity()) ? false : super`.
        if (IsLikedPlayer(attacker)) return false;
        return GenericPathfinderMob::Hurt(source, amount, attacker);
    }

    UseResult Allay::MobInteract(LivingEntity& player, ItemStack& held) {
        const bool client = m_level && m_level->IsClientSide();
        // MC plays the three interaction sounds with `except = player`, the
        // acting client predicting its own copy in its mobInteract. This
        // engine's client does not run mobInteract, so the server sends
        // them to everyone, the acting player included.

        // Branch 1: dancing, an item in #duplicates_allays, off cooldown.
        if (IsDancing() && !held.IsEmpty() && CanDuplicate() &&
            DataTags::HasTag(DataTags::Registry::Item, ItemRegistry::Slug(held.itemId),
                             "minecraft:duplicates_allays")) {
            if (client) return UseResult::Success;
            DuplicateAllay();
            m_level->BroadcastEntityEvent(*this, 18);
            m_level->PlaySoundFromEntity(nullptr, *this, SoundEvents::AMETHYST_BLOCK_CHIME,
                                         SoundSource::Neutral, 2.0f, 1.0f);
            // removeInteractionItem → itemStack.consume(1, player): the
            // server's creative restore gives it back.
            held.count -= 1;
            if (held.count <= 0) held = ItemStack{};
            return UseResult::Success;
        }

        // Branch 2: an empty allay hand and a non-empty player hand — one
        // of it becomes the allay's, and the player its liked player.
        if (m_handItem.IsEmpty() && !held.IsEmpty()) {
            if (client) return UseResult::Success;
            ItemStack one = held;
            one.count = 1;
            SetMainHandItem(one);
            held.count -= 1;
            if (held.count <= 0) held = ItemStack{};
            m_level->PlaySoundFromEntity(nullptr, *this, SoundEvents::ALLAY_ITEM_GIVEN,
                                         SoundSource::Neutral, 2.0f, 1.0f);
            SetLikedPlayerUuid(player.GetUuid());
            return UseResult::Success;
        }

        // Branch 3 (main hand only — the only hand this engine interacts
        // with): an empty player hand takes the held item back; the carried
        // stacks are thrown down at the allay and the liking ends.
        if (!m_handItem.IsEmpty() && held.IsEmpty()) {
            if (client) return UseResult::Success;
            const ItemStack itemInHand = m_handItem;
            SetMainHandItem(ItemStack{});
            m_level->PlaySoundFromEntity(nullptr, *this, SoundEvents::ALLAY_ITEM_TAKEN,
                                         SoundSource::Neutral, 2.0f, 1.0f);
            // swing(MAIN_HAND, itemInHand.getInteractAnimation()) — the arm.
            Swing();
            for (int i = 0; i < m_inventory.GetContainerSize(); ++i) {
                const ItemStack stack = m_inventory.GetItem(i);
                m_inventory.SetItem(i, ItemStack{});
                if (stack.IsEmpty()) continue;
                // BehaviorUtils.throwItem(this, itemStack, this.position()):
                // default 0.3 hand offset; the throw vector toward its own
                // position normalizes to zero.
                m_level->SpawnThrownItem(glm::dvec3(position.x, GetEyeY() - 0.3, position.z),
                                         glm::dvec3(0.0), stack, 10);
            }
            SetLikedPlayerUuid(std::nullopt);
            // player.addItem(itemInHand) — MC Inventory.add.
            m_level->AddItemToPlayer(player, itemInHand);
            return UseResult::Success;
        }

        return GenericPathfinderMob::MobInteract(player, held);
    }

    void Allay::DropEquipment(EntityLevel& level) {
        // MC dropEquipment: every inventory stack spawnAtLocation, then the
        // main hand unless it carries PREVENT_EQUIPMENT_DROP (Curse of
        // Vanishing), cleared either way.
        for (int i = 0; i < m_inventory.GetContainerSize(); ++i) {
            const ItemStack stack = m_inventory.GetItem(i);
            m_inventory.SetItem(i, ItemStack{});
            if (!stack.IsEmpty()) level.SpawnItemStackDrop(position, stack);
        }
        if (!m_handItem.IsEmpty() && !EnchantmentHelper::HasPreventEquipmentDrop(m_handItem)) {
            level.SpawnItemStackDrop(position, m_handItem);
            SetMainHandItem(ItemStack{});
        }
    }

    void Allay::HandleEntityEvent(uint8_t id) {
        if (id == 18) {
            // MC spawnHeartParticle x3: gaussian * 0.02 velocities,
            // getRandomX(1.0), getRandomY() + 0.5, getRandomZ(1.0), in Java's
            // argument order (xd, yd, zd, then the position draws).
            if (!m_level) return;
            JavaRandom& rng = m_level->Random();
            const double w = static_cast<double>(GetBbWidth());
            const double h = static_cast<double>(GetBbHeight());
            for (int i = 0; i < 3; ++i) {
                const double xd = rng.NextGaussian() * 0.02;
                const double yd = rng.NextGaussian() * 0.02;
                const double zd = rng.NextGaussian() * 0.02;
                const double px = position.x + w * (2.0 * rng.NextDouble() - 1.0);
                const double py = position.y + h * rng.NextDouble() + 0.5;
                const double pz = position.z + w * (2.0 * rng.NextDouble() - 1.0);
                m_level->AddParticle(ParticleKind::Heart, px, py, pz, xd, yd, zd);
            }
            return;
        }
        GenericPathfinderMob::HandleEntityEvent(id);
    }

    // ══ Synched data ══════════════════════════════════════════════════════

    uint8_t Allay::GetAnimStateByte() const {
        uint8_t b = 0;
        if (!m_handItem.IsEmpty()) b |= 0x01;
        if (m_dancing)             b |= 0x02;
        if (m_canDuplicate)        b |= 0x04;
        return b;
    }

    void Allay::SetAnimStateByte(uint8_t v) {
        m_clientHoldingItem = (v & 0x01) != 0;
        m_dancing           = (v & 0x02) != 0;
        m_canDuplicate      = (v & 0x04) != 0;
    }

    // ══ Dancing / duplication ═════════════════════════════════════════════

    bool Allay::IsBrainPanicking() const {
        // MC LivingEntity.isPanicking for a brain mob: IS_PANICKING present.
        const Brain* brain = GetBrain();
        return brain && brain->HasMemoryValue(MemoryModule::IsPanicking);
    }

    void Allay::SetDancing(bool dancing) {
        if (!m_level || m_level->IsClientSide() || !IsEffectiveAi()) return;
        if (dancing && IsBrainPanicking()) return;
        m_dancing = dancing;
    }

    void Allay::SetJukeboxPlaying(const glm::ivec3& jukebox, bool playing) {
        if (playing) {
            if (!IsDancing()) {
                m_jukeboxPos = jukebox;
                SetDancing(true);
            }
        } else if (!m_jukeboxPos || *m_jukeboxPos == jukebox) {
            m_jukeboxPos.reset();
            SetDancing(false);
        }
    }

    bool Allay::ShouldStopDancing() const {
        // MC shouldStopDancing: no jukebox, the jukebox's centre no closer
        // than JUKEBOX_PLAY's notification radius, or no longer a jukebox.
        if (!m_jukeboxPos) return true;
        const glm::dvec3 centre = glm::dvec3(*m_jukeboxPos) + glm::dvec3(0.5);
        const glm::dvec3 d = centre - position;
        if (!(glm::dot(d, d) < static_cast<double>(kJukeboxNotificationRadius) * kJukeboxNotificationRadius)) {
            return true;
        }
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        return !blocks || blocks->GetBlock(m_jukeboxPos->x, m_jukeboxPos->y, m_jukeboxPos->z) != BlockID::Jukebox;
    }

    void Allay::SetDuplicationCooldown(int64_t ticks) {
        // MC setDuplicationCooldown — DATA_CAN_DUPLICATE follows it.
        m_duplicationCooldown = ticks;
        m_canDuplicate = ticks == 0;
    }

    void Allay::DuplicateAllay() {
        // MC duplicateAllay: EntityType.create(BREEDING) — no finalizeSpawn —
        // snapped to this allay's position, persistence required, and both
        // on the 6000-tick cooldown.
        if (!m_level) return;
        auto allay = std::make_unique<Allay>(m_level);
        allay->position = position;
        allay->oldPosition = position;
        allay->SetPersistenceRequired(true);
        allay->SetDuplicationCooldown(kDuplicationCooldownTicks);
        SetDuplicationCooldown(kDuplicationCooldownTicks);
        m_level->AddFreshEntity(std::move(allay));
    }

    float Allay::GetHoldingItemAnimationProgress(float partialTick) const {
        return Mth::Lerp(partialTick, m_holdingItemAnimationTicks0, m_holdingItemAnimationTicks) /
               kLiftingItemAnimationDuration;
    }

    bool Allay::IsSpinning() const {
        return std::fmod(m_dancingAnimationTicks, kDancingLoopDuration) < kSpinningAnimationDuration;
    }

    float Allay::GetSpinningProgress(float partialTick) const {
        return Mth::Lerp(partialTick, m_spinningAnimationTicks0, m_spinningAnimationTicks) /
               kSpinningAnimationDuration;
    }

    // ══ Tick / aiStep ═════════════════════════════════════════════════════

    void Allay::Tick() {
        GenericPathfinderMob::Tick();
        if (!m_level) return;
        if (m_level->IsClientSide()) {
            // MC tick's client half: the lift-the-item ramp and the dance's
            // spin ramp.
            m_holdingItemAnimationTicks0 = m_holdingItemAnimationTicks;
            m_holdingItemAnimationTicks = std::clamp(
                m_holdingItemAnimationTicks + (HasItemInHand() ? 1.0f : -1.0f),
                0.0f, kLiftingItemAnimationDuration);
            if (IsDancing()) {
                ++m_dancingAnimationTicks;
                m_spinningAnimationTicks0 = m_spinningAnimationTicks;
                if (IsSpinning()) ++m_spinningAnimationTicks;
                else              --m_spinningAnimationTicks;
                m_spinningAnimationTicks = std::clamp(m_spinningAnimationTicks, 0.0f,
                                                      kSpinningAnimationDuration);
            } else {
                m_dancingAnimationTicks = 0.0f;
                m_spinningAnimationTicks = 0.0f;
                m_spinningAnimationTicks0 = 0.0f;
            }
        } else {
            // MC tick's server half: VibrationSystem.Ticker.tick, and no
            // dancing while panicking. The jukebox listener rides here too.
            TickVibrations();
            TickJukeboxListener();
            if (IsBrainPanicking()) SetDancing(false);
        }
    }

    void Allay::AiStep() {
        // MC Mob.aiStep (with its item pickup — Mob::TickLooting →
        // PickUpItem), then Allay.aiStep.
        GenericPathfinderMob::AiStep();
        if (!m_level) return;
        if (!m_level->IsClientSide() && IsAlive() && tickCount % 10 == 0) Heal(1.0f);
        if (IsDancing() && ShouldStopDancing() && tickCount % 20 == 0) {
            SetDancing(false);
            m_jukeboxPos.reset();
        }
        // updateDuplicationCooldown.
        if (!m_level->IsClientSide() && m_duplicationCooldown > 0) {
            SetDuplicationCooldown(m_duplicationCooldown - 1);
        }
    }

    // ══ The jukebox listener ══════════════════════════════════════════════

    void Allay::TickJukeboxListener() {
        // MC JukeboxListener.handleGameEvent, reached through the song
        // registry: JukeboxSongPlayer.tick raises JUKEBOX_PLAY on every 20th
        // tick from the song's start (its tick counter before the ++, i.e.
        // the registry's count after it is ≡ 1 mod 20), and stop raises
        // JUKEBOX_STOP_PLAY. GameEventDispatcher reaches a listener whose
        // position (the allay's eye, as a block) is within the listener's
        // radius (10) of the event's block.
        const DimensionId dimension = m_level->Dimension();
        const glm::ivec3 listener = Containing(GetEyePosition());
        const int64_t radiusSqr = static_cast<int64_t>(kJukeboxNotificationRadius) * kJukeboxNotificationRadius;

        std::vector<glm::ivec3> heard;
        std::vector<glm::ivec3> playEvents;
        JukeboxSongRegistry::ForEach([&](const JukeboxSongRegistry::Entry& e) {
            if (e.dimension != dimension || e.songId < 0) return;
            if (BlockDistSqr(listener, e.pos) > radiusSqr) return;
            heard.push_back(e.pos);
            if (e.ticks % JukeboxSongPlayer::kPlayEventIntervalTicks == 1) playEvents.push_back(e.pos);
        });

        // Songs heard last pass that are no longer playing: their stop event.
        for (const glm::ivec3& pos : m_heardJukeboxes) {
            bool stillPlaying = false;
            JukeboxSongRegistry::ForEach([&](const JukeboxSongRegistry::Entry& e) {
                if (e.dimension == dimension && e.pos == pos && e.songId >= 0) stillPlaying = true;
            });
            if (!stillPlaying) SetJukeboxPlaying(pos, false);
        }
        for (const glm::ivec3& pos : playEvents) SetJukeboxPlaying(pos, true);
        m_heardJukeboxes = std::move(heard);
    }

    // ══ The vibration listener (MC Allay.VibrationUser) ═════════════════

    VibrationUser& Allay::GetVibrationUser() { return m_vibrationUser; }

    Allay::AllayVibrationUser::AllayVibrationUser(Allay& allay)
        : m_allay(allay),
          // MC: new EntityPositionSource(Allay.this, getEyeHeight()).
          m_source(PositionSource::OfEntity(&allay, allay.GetEyeHeight())) {}

    bool Allay::AllayVibrationUser::CanReceiveVibration(World& /*level*/, const glm::ivec3& pos,
                                                        GameEventId /*event*/, const GameEventContext& /*context*/) {
        // MC: not with NoAI; with a liked noteblock, only that noteblock (and
        // only while within 1024 blocks of it in this dimension).
        if (m_allay.IsNoAi()) return false;
        const Brain* brain = m_allay.GetBrain();
        const auto liked = brain ? brain->GetBlockPos(MemoryModule::LikedNoteblockPosition) : std::nullopt;
        if (!liked) return true;
        const glm::dvec3 d = glm::dvec3(*liked) - glm::dvec3(m_allay.BlockPosition());
        const double limit = static_cast<double>(kMaxNoteblockDistance);
        return glm::dot(d, d) < limit * limit && *liked == pos;
    }

    void Allay::AllayVibrationUser::OnReceiveVibration(World& /*level*/, const glm::ivec3& pos, GameEventId event,
                                                       Entity* /*sourceEntity*/, Entity* /*projectileOwner*/,
                                                       float /*receivingDistance*/) {
        if (event == GameEventId::NoteBlockPlay) AllayAi::HearNoteblock(m_allay, pos);
    }

    void Allay::TickVibrations() {
        // MC Allay.tick's server half: VibrationSystem.Ticker.tick, then the
        // listener follows the allay (ServerLevel's section callbacks).
        auto* world = m_level ? dynamic_cast<World*>(m_level->MutableBlocks()) : nullptr;
        if (!world) return;
        VibrationTicker::Tick(*world, m_vibrationData, m_vibrationUser);
        if (IsRemoved()) m_dynamicGameEventListener.Remove();
        else m_dynamicGameEventListener.Move(world->GameEvents());
    }

} // namespace Game
