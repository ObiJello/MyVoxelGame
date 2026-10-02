// File: src/common/entity/mobs/AnimatedMobs.cpp
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/entity/SpearItem.hpp"
#include "common/entity/Bucketable.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/data/DataComponents.hpp"
#include "common/particle/ParticleOptions.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/core/Log.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/sound/SoundType.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/ai/goals/BeeGoals.hpp"
#include "common/world/block/entity/BeehiveBlockEntity.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/entity/mobs/Fish.hpp"
#include "common/entity/ai/goals/LongJumpGoal.hpp"
#include "common/entity/ai/goals/HappyGhastGoals.hpp"
#include "common/entity/projectile/HurtingProjectile.hpp"
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/brain/Brain.hpp"
#include "common/world/level/World.hpp"
#include "common/entity/ai/brain/BreezeAi.hpp"
#include "common/entity/ai/brain/CamelAi.hpp"
#include "common/entity/HorseTaming.hpp"
#include "common/entity/Item.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/physics/Physics.hpp"
#include "common/entity/ai/brain/CopperGolemAi.hpp"
#include "common/entity/ai/brain/CreakingAi.hpp"
#include "common/entity/ai/brain/GoatAi.hpp"
#include "common/entity/ai/brain/HoglinAi.hpp"
#include "common/entity/ai/brain/SnifferAi.hpp"
#include "common/entity/ai/brain/TadpoleAi.hpp"
#include "common/entity/ai/brain/FrogAi.hpp"
#include "common/entity/ai/brain/WardenAi.hpp"
#include "common/entity/ai/brain/AllayAi.hpp"
#include "common/entity/ai/brain/ArmadilloAi.hpp"
#include "common/entity/ai/brain/AxolotlAi.hpp"
#include "common/entity/ai/brain/HappyGhastAi.hpp"
#include "common/entity/ai/brain/PiglinAi.hpp"
#include "common/entity/ai/brain/PiglinBruteAi.hpp"
#include "common/entity/ai/brain/ZoglinAi.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/ai/navigation/AmphibiousPathNavigation.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/crafting/RecipeManager.hpp"
#include "common/entity/FireworkItems.hpp"
#include "common/inventory/SimpleContainerOps.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include "common/world/damagesource/DamageSourceInfo.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/tags/DataTags.hpp"
#include "common/entity/MobEquipment.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/block/entity/CopperGolemStatueBlockEntity.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string_view>

namespace Game {

    // ══ Frog ═══════════════════════════════════════════════════════════════

    Frog::Frog(EntityLevel* level) : GenericAnimal(EntityTypeId::Frog, level) {
        // NO GOALS. MC's Frog never overrides registerGoals, so it has none —
        // its entire behaviour is the brain. GenericAnimal's constructor has
        // already registered the shared animal set, so it is cleared here.
        //
        // This is not tidiness. MC's Croak is gated on WALK_TARGET being
        // ABSENT, and that only means "not walking" if every movement goes
        // through a walk target. With the goal stroll still running the frog
        // croaked while walking, and MoveToTargetSink fought the stroll goal
        // for the navigation.
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        // MC Frog.createNavigation returns a FrogPathNavigation — amphibious,
        // so water costs nothing to swim through and land costs 6. On the
        // ground navigation the frog could not path THROUGH water at all: the
        // SWIM activity would pick targets the pathfinder refused to reach and
        // the frog would sit at the water's edge looking broken.
        m_navigation = std::make_unique<FrogPathNavigation>(this, level);

        m_brain = std::make_unique<Brain>();
        FrogAi::InitBrain(*this, *m_brain);
        FrogAi::InitMemories(*this);
    }

    namespace {
        // data/minecraft/tags/worldgen/biome/spawns_{warm,cold}_variant_frogs
        // .json, the nested tags (#is_jungle, #is_savanna, #is_nether,
        // #is_badlands, #is_end) flattened — this engine has no biome-tag
        // resolver.
        constexpr std::string_view kWarmFrogBiomes[] = {
            "desert", "warm_ocean",
            "bamboo_jungle", "jungle", "sparse_jungle",
            "savanna", "savanna_plateau", "windswept_savanna",
            "nether_wastes", "soul_sand_valley", "crimson_forest", "warped_forest", "basalt_deltas",
            "badlands", "eroded_badlands", "wooded_badlands",
            "mangrove_swamp",
        };
        constexpr std::string_view kColdFrogBiomes[] = {
            "snowy_plains", "ice_spikes", "frozen_peaks", "jagged_peaks", "snowy_slopes",
            "frozen_ocean", "deep_frozen_ocean", "grove", "deep_dark", "frozen_river",
            "snowy_taiga", "snowy_beach",
            "the_end", "end_highlands", "end_midlands", "small_end_islands", "end_barrens",
        };
        bool FrogBiomeIn(std::string_view biome, const std::string_view* list, size_t n) {
            for (size_t i = 0; i < n; ++i) if (list[i] == biome) return true;
            return false;
        }
        std::string_view StripMinecraftNs(std::string_view id) {
            constexpr std::string_view kNs = "minecraft:";
            if (id.substr(0, kNs.size()) == kNs) id.remove_prefix(kNs.size());
            return id;
        }
    } // namespace

    const char* Frog::VariantName(Variant v) {
        switch (v) {
            case Variant::Warm: return "warm";
            case Variant::Cold: return "cold";
            default:            return "temperate";
        }
    }

    bool Frog::VariantFromName(std::string_view id, Variant& out) {
        id = StripMinecraftNs(id);
        if (id == "temperate") { out = Variant::Temperate; return true; }
        if (id == "warm")      { out = Variant::Warm;      return true; }
        if (id == "cold")      { out = Variant::Cold;      return true; }
        return false;
    }

    const char* Frog::VariantTexture(Variant v) {
        // FrogVariants.bootstrap's asset ids (26.3: entity/frog/frog_<name>).
        switch (v) {
            case Variant::Warm: return "assets/textures/entity/frog/frog_warm.png";
            case Variant::Cold: return "assets/textures/entity/frog/frog_cold.png";
            default:            return "assets/textures/entity/frog/frog_temperate.png";
        }
    }

    Frog::Variant Frog::SelectVariantToSpawn(std::string_view biome, JavaRandom& random) {
        // PriorityProvider.pick: the highest matched priority's entries, in
        // the registry's listing (cold, temperate, warm — identifier order),
        // then Util.getRandomSafe's nextInt(size). The warm and cold tags do
        // not overlap, so one entry survives — the draw still happens.
        biome = StripMinecraftNs(biome);
        Variant pick = Variant::Temperate;
        if (FrogBiomeIn(biome, kColdFrogBiomes, std::size(kColdFrogBiomes))) {
            pick = Variant::Cold;
        } else if (FrogBiomeIn(biome, kWarmFrogBiomes, std::size(kWarmFrogBiomes))) {
            pick = Variant::Warm;
        }
        (void)random.NextInt(1);
        return pick;
    }

    std::shared_ptr<SpawnGroupData>
    Frog::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        if (m_level) {
            std::string_view biome = "plains";
            if (const IBlockAccess* blocks = m_level->Blocks()) {
                const glm::ivec3 p = BlockPosition();
                biome = BiomeRegistry::Get(blocks->GetBiome(p.x, p.y, p.z)).name;
            }
            SetVariant(SelectVariantToSpawn(biome, m_level->Random()));
        }
        return GenericAnimal::FinalizeSpawn(reason, std::move(groupData));
    }

    void Frog::PlayEatingSound() {
        if (m_level) m_level->PlaySoundFromEntity(nullptr, *this, SoundEvents::FROG_EAT, SoundSource::Neutral, 2.0f, 1.0f);
    }

    void Frog::UpdateBrainActivity() {
        // MC Frog.customServerAiStep: tick the brain, then re-pick the
        // activity. Mob::ServerAiStep does the first half.
        FrogAi::UpdateActivity(*this);
    }

    void Frog::Tick() {

        // MC Frog.tick runs this BEFORE super.tick(), and it is purely local —
        // no server state is involved, so every client decides for itself.
        if (m_level && m_level->IsClientSide()) {
            Anim(MobAnim::SwimIdle).AnimateWhen(
                IsInWater() && !walkAnimation.IsMoving(), tickCount);
        }
        GenericAnimal::Tick();
    }

    void Frog::OnPoseUpdated() {
        const Pose pose = GetPose();
        // MC starts (not startIfStopped) each of these: the pose only changes
        // on the transition, so a restart is exactly one clip from the top.
        if (pose == Pose::LongJumping) Anim(MobAnim::Jump).Start(tickCount);
        else                           Anim(MobAnim::Jump).Stop();

        if (pose == Pose::Croaking)    Anim(MobAnim::Croak).Start(tickCount);
        else                           Anim(MobAnim::Croak).Stop();

        if (pose == Pose::UsingTongue) Anim(MobAnim::Tongue).Start(tickCount);
        else                           Anim(MobAnim::Tongue).Stop();
    }

    void Frog::UpdateWalkAnimation(float distance) {
        // MC Frog.updateWalkAnimation.
        const float target = Anim(MobAnim::Jump).IsStarted()
                                 ? 0.0f
                                 : std::min(distance * 25.0f, 1.0f);
        walkAnimation.Update(target, 0.4f, IsBaby() ? 3.0f : 1.0f);
    }

    // ══ Camel ══════════════════════════════════════════════════════════════

    namespace {

        // MC Camel.CamelMoveControl: a seated camel told to walk somewhere
        // (and not held on a lead) gets up first, when it has the headroom.
        class CamelMoveControl final : public MoveControl {
        public:
            explicit CamelMoveControl(Camel* camel) : MoveControl(camel), m_camel(camel) {}
            void Tick() override {
                if (m_operation == Operation::MoveTo && !m_camel->IsLeashed() && m_camel->IsCamelSitting() &&
                    !m_camel->IsInPoseTransition() && m_camel->CanCamelChangePose()) {
                    m_camel->StandUp();
                }
                MoveControl::Tick();
            }
        private:
            Camel* m_camel;
        };

        // MC Camel.CamelLookControl: the head is the rider's while a player
        // steers.
        class CamelLookControl final : public LookControl {
        public:
            explicit CamelLookControl(Camel* camel) : LookControl(camel), m_camel(camel) {}
            void Tick() override {
                if (!m_camel->HasControllingPassenger()) LookControl::Tick();
            }
        private:
            Camel* m_camel;
        };

        // MC Camel.CamelBodyRotationControl: a camel that refuses to move
        // keeps its body where it is.
        class CamelBodyRotationControl final : public BodyRotationControl {
        public:
            explicit CamelBodyRotationControl(Camel* camel) : BodyRotationControl(camel), m_camel(camel) {}
            void ClientTick() override {
                if (!m_camel->RefuseToMove()) BodyRotationControl::ClientTick();
            }
        private:
            Camel* m_camel;
        };

        // MC AgeableMob.AgeableMobGroupData(0.2F) — what
        // AbstractHorse.finalizeSpawn hands a herd: the first camel is an
        // adult, each later one a calf on a 20 % roll.
        struct CamelGroupData : SpawnGroupData {
            float babySpawnChance = 0.2f;
            int   groupSize = 0;
        };

    } // namespace

    Camel::Camel(EntityLevel* level, EntityTypeId type) : GenericAnimal(type, level) {
        // NO GOALS — MC's Camel has a brain and never registers any.
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        // MC Camel.createAttributes = AbstractHorse.createBaseHorseAttributes
        // + MAX_HEALTH 32, MOVEMENT_SPEED 0.09, JUMP_STRENGTH 0.42,
        // STEP_HEIGHT 1.5 (the def carries health, speed and step): the base
        // horse's SAFE_FALL_DISTANCE 6 and FALL_DAMAGE_MULTIPLIER 0.5.
        m_attributes.SetBaseValue(Attribute::JumpStrength, 0.41999998688697815);
        m_attributes.SetBaseValue(Attribute::SafeFallDistance, 6.0);
        m_attributes.SetBaseValue(Attribute::FallDamageMultiplier, 0.5);

        // MC Camel's constructor: CamelMoveControl, CamelLookControl, the
        // body control, and a navigation that floats.
        SetMoveControl(std::make_unique<CamelMoveControl>(this));
        SetLookControl(std::make_unique<CamelLookControl>(this));
        SetBodyRotationControl(std::make_unique<CamelBodyRotationControl>(this));
        GetNavigation().SetCanFloat(true);

        m_brain = std::make_unique<Brain>();
        CamelAi::InitBrain(*this, *m_brain);

        // MC finalizeSpawn calls resetLastPoseChangeTickToFullStand, so a fresh
        // camel is already fully stood up rather than mid-transition.
        StandUpInstantly();
    }

    void Camel::UpdateBrainActivity() { CamelAi::UpdateActivity(*this); }

    std::shared_ptr<SpawnGroupData>
    Camel::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // Camel.finalizeSpawn: resetLastPoseChangeTickToFullStand.
        const int64_t now = m_level ? m_level->GetGameTime() : 0;
        ResetLastPoseChangeTick(std::max<int64_t>(0, now - kStandUpDuration - 1));
        // AbstractHorse.finalizeSpawn: AgeableMobGroupData(0.2) when none
        // came in; AgeableMob.finalizeSpawn rolls the calves (a camel husk
        // cannot be one — canBeABaby).
        if (!groupData) groupData = std::make_shared<CamelGroupData>();
        if (auto* herd = dynamic_cast<CamelGroupData*>(groupData.get())) {
            if (!IsHusk() && m_level && herd->groupSize > 0 &&
                m_level->Random().NextFloat() <= herd->babySpawnChance) {
                SetAge(kBabyStartAge);
            }
            ++herd->groupSize;
        }
        return GenericAnimal::FinalizeSpawn(reason, std::move(groupData));
    }

    // ── Dimensions ───────────────────────────────────────────────────────────

    float Camel::BaseBbWidth() const {
        if (GetPose() == Pose::Sitting) return IsBaby() ? 0.95f : GetEntityTypeInfo(EntityTypeId::Camel).width;
        return GenericAnimal::BaseBbWidth();
    }

    float Camel::BaseBbHeight() const {
        if (GetPose() == Pose::Sitting) {
            return IsBaby() ? 0.425f : GetEntityTypeInfo(EntityTypeId::Camel).height - kSittingHeightDifference;
        }
        return GenericAnimal::BaseBbHeight();
    }

    float Camel::BaseEyeHeight() const {
        if (GetPose() == Pose::Sitting) return IsBaby() ? 0.41f : 0.845f;
        return GenericAnimal::BaseEyeHeight();
    }

    double Camel::GetBodyAnchorAnimationYOffset(bool isFront, float partialTicks, float dimensionsHeight,
                                                float scale) const {
        // MC Camel.getBodyAnchorAnimationYOffset, verbatim.
        const double ageSitYOffset = IsBaby() ? 0.09375 : 0.375;
        double baseSitOffset = static_cast<double>(dimensionsHeight) - ageSitYOffset;
        const float sittingHeightDifference = scale * kSittingHeightDifference;
        const float verticalDrop = sittingHeightDifference - scale * 0.2f;
        const float bottomPoint = sittingHeightDifference - verticalDrop;
        const bool isInTransition = IsInPoseTransition();
        const bool isSitting = IsCamelSitting();
        if (isInTransition) {
            const int animationDuration = isSitting ? kSitDownDuration : kStandUpDuration;
            int halfPoint;
            float flexPointOffset;
            if (isSitting) {
                halfPoint = 28;
                flexPointOffset = isFront ? 0.5f : 0.1f;
            } else {
                halfPoint = isFront ? 24 : 32;
                flexPointOffset = isFront ? 0.6f : 0.35f;
            }
            const float poseTime = std::clamp(static_cast<float>(GetPoseTime()) + partialTicks, 0.0f,
                                              static_cast<float>(animationDuration));
            const bool isFirstPart = poseTime < static_cast<float>(halfPoint);
            const float part = isFirstPart
                ? poseTime / static_cast<float>(halfPoint)
                : (poseTime - static_cast<float>(halfPoint)) / static_cast<float>(animationDuration - halfPoint);
            const float flexPoint = sittingHeightDifference - flexPointOffset * verticalDrop;
            baseSitOffset += isSitting
                ? static_cast<double>(Mth::Lerp(part, isFirstPart ? sittingHeightDifference : flexPoint,
                                                isFirstPart ? flexPoint : bottomPoint))
                : static_cast<double>(Mth::Lerp(part, isFirstPart ? bottomPoint - sittingHeightDifference
                                                                  : bottomPoint - flexPoint,
                                                isFirstPart ? bottomPoint - flexPoint : 0.0f));
        }
        if (isSitting && !isInTransition) baseSitOffset += static_cast<double>(bottomPoint);
        return baseSitOffset;
    }

    // ── Pose ─────────────────────────────────────────────────────────────────

    int64_t Camel::GetPoseTime() const {
        const int64_t now = m_level ? m_level->GetGameTime() : 0;
        return now - (m_lastPoseChangeTick < 0 ? -m_lastPoseChangeTick : m_lastPoseChangeTick);
    }

    void Camel::ResetLastPoseChangeTick(int64_t syncedPoseTickTime) {
        m_lastPoseChangeTick = syncedPoseTickTime;
    }

    void Camel::SitDown() {
        if (IsCamelSitting()) return;
        MakeSound(GetSitDownSound());
        SetPose(Pose::Sitting);
        GameEvent(GameEventId::EntityAction);   // MC sitDown
        // NEGATIVE while sitting — that sign IS the sitting flag in MC.
        ResetLastPoseChangeTick(-(m_level ? m_level->GetGameTime() : 0));
    }

    void Camel::StandUp() {
        if (!IsCamelSitting()) return;
        MakeSound(GetStandUpSound());
        SetPose(Pose::Standing);
        GameEvent(GameEventId::EntityAction);   // MC standUp
        ResetLastPoseChangeTick(m_level ? m_level->GetGameTime() : 0);
    }

    void Camel::StandUpInstantly() {
        SetPose(Pose::Standing);
        GameEvent(GameEventId::EntityAction);   // MC standUpInstantly
        // MC backdates the change past the whole stand-up so the camel is not
        // considered "in transition" at all.
        const int64_t now = m_level ? m_level->GetGameTime() : 0;
        ResetLastPoseChangeTick(std::max<int64_t>(0, now - kStandUpDuration - 1));
    }

    bool Camel::CanCamelChangePose() const {
        // MC LivingEntity.wouldNotSuffocateAtTargetPose(isCamelSitting ?
        // STANDING : SITTING): the other pose's box at the feet meets no
        // block.
        if (!m_level) return true;
        const bool toStanding = IsCamelSitting();
        const EntityTypeInfo& type = GetEntityTypeInfo(GetType());
        float width, height;
        if (toStanding) {
            width = IsBaby() ? GetBabyWidth(GetType()) : type.width;
            height = IsBaby() ? GetBabyHeight(GetType()) : type.height;
        } else {
            width = IsBaby() ? 0.95f : GetEntityTypeInfo(EntityTypeId::Camel).width;
            height = IsBaby() ? 0.425f : GetEntityTypeInfo(EntityTypeId::Camel).height - kSittingHeightDifference;
        }
        const double halfWidth = static_cast<double>(width * scale) * 0.5;
        const AABBd box = AABBd::FromMinMax(position - glm::dvec3(halfWidth, 0.0, halfWidth),
                                            position + glm::dvec3(halfWidth, static_cast<double>(height * scale),
                                                                  halfWidth));
        return !CollidesAt(box, m_level->Physics());
    }

    bool Camel::IsCamelPanicking() const {
        // MC PathfinderMob.isPanicking: a brain that keeps IS_PANICKING
        // answers from the memory.
        if (const Brain* brain = GetBrain()) return brain->HasMemoryValue(MemoryModule::IsPanicking);
        return IsPanicking();
    }

    bool Camel::IsMobControlled() const {
        // AbstractHorse.isMobControlled is false; CamelHusk's: its first
        // passenger is a Mob (the husk that came riding it).
        if (!IsHusk()) return false;
        const Entity* first = GetFirstPassenger();
        return first && !first->IsPlayer() && dynamic_cast<const Mob*>(first) != nullptr;
    }

    // ── Sounds ───────────────────────────────────────────────────────────────

    void Camel::PlayStepSound(const glm::ivec3& pos, BlockState state) {
        (void)pos;
        // #camel_sand_step_sound_blocks = #sand + #concrete_powder.
        const std::string& slug = BlockRegistry::Get(state.Block()).registrySlug;
        const bool sandy = slug == "sand" || slug == "red_sand" || slug == "suspicious_sand"
            || (slug.size() > 16 && slug.compare(slug.size() - 16, 16, "_concrete_powder") == 0);
        if (IsHusk()) {
            PlaySound(sandy ? SoundEvents::CAMEL_HUSK_STEP_SAND : SoundEvents::CAMEL_HUSK_STEP, 0.4f, 1.0f);
        } else {
            PlaySound(sandy ? SoundEvents::CAMEL_STEP_SAND : SoundEvents::CAMEL_STEP, 1.0f, 1.0f);
        }
    }

    void Camel::PlayEatingSound() {
        // MC Camel.handleEating's tail: unless silent, level.playSound(null,
        // x, y, z, getEatingSound(), getSoundSource(), 1, 1 ± 0.2), then
        // gameEvent(EAT) — reached only from a feeding that took.
        if (!m_level) return;
        if (!IsSilent()) {
            JavaRandom& rng = m_level->Random();
            m_level->PlaySound(nullptr, position, GetEatingSound(), GetSoundSource(), 1.0f,
                               1.0f + (rng.NextFloat() - rng.NextFloat()) * 0.2f);
        }
        GameEvent(GameEventId::Eat);
    }

    // ── Interaction, feeding, breeding ───────────────────────────────────────

    UseResult Camel::MobInteract(LivingEntity& player, ItemStack& held) {
        // CamelHusk.interact: any click makes the husk persistent.
        if (IsHusk()) SetPersistenceRequired(true);
        // MC Camel.mobInteract.
        if (HorseTaming::IsSecondaryUseActive(player) && !IsBaby()) {
            OpenCustomInventoryScreen(player);
            return UseResult::Success;
        }
        if (!held.IsEmpty()) {
            if (const auto interact = ItemRegistry::Get(held.itemId).interactLivingEntity) {
                const UseResult r = interact(held, *this);
                if (ConsumesAction(r)) return r;
            }
        }
        if (IsFood(held.itemId)) return FedFood(player, held);
        if (GetPassengers().size() < 2 && !IsBaby()) {
            // AbstractHorse.doPlayerRide: setEating(false), clearStanding,
            // then (server) startRiding.
            SetEating(false);
            if (m_level && !m_level->IsClientSide()) m_level->StartPlayerRiding(player, *this);
            return UseResult::Consume;
        }
        if (IsBaby() && held.itemId == ItemRegistry::FromBlock(BlockID::GoldenDandelion)) {
            return GenericAnimal::MobInteract(player, held);
        }
        return UseResult::Fail;
    }

    void Camel::OpenCustomInventoryScreen(LivingEntity& player) {
        // MC Camel.openCustomInventoryScreen: !isClientSide →
        // player.openHorseInventory(this, inventory).
        if (m_level && !m_level->IsClientSide()) m_level->OpenMountInventory(player, *this);
    }

    UseResult Camel::FedFood(LivingEntity& player, ItemStack& held) {
        // MC AbstractHorse.fedFood.
        const bool ate = HandleEating(player, held);
        if (ate) UsePlayerItem(held);
        const bool clientSide = m_level && m_level->IsClientSide();
        return !ate && !clientSide ? UseResult::Pass : UseResult::SuccessServer;
    }

    bool Camel::HandleEating(LivingEntity& player, const ItemStack& held) {
        // MC Camel.handleEating.
        if (!IsFood(held.itemId)) return false;
        const bool clientSide = m_level && m_level->IsClientSide();
        const bool couldHeal = GetHealth() < GetMaxHealth();
        if (couldHeal) Heal(2.0f);
        // isTamed() is always true for a camel.
        const bool couldSetInLove = GetAge() == 0 && CanFallInLove();
        if (couldSetInLove && !clientSide) SetInLove(&player);
        const bool couldAgeUp = CanAgeUp();
        if (couldAgeUp && !clientSide) {
            // The HAPPY_VILLAGER puff over the calf (getRandomX(1),
            // getRandomY() + 0.5) — sent, the client does not run this.
            SendHappyVillagerParticle();
            AgeUp(10);
        }
        if (!couldHeal && !couldSetInLove && !couldAgeUp) return false;
        if (!clientSide) PlayEatingSound();
        return true;
    }

    bool Camel::CanParent() const {
        // MC AbstractHorse.canParent (isTamed is always true for a camel).
        return !IsVehicle() && !IsPassenger() && !IsBaby() && GetHealth() >= GetMaxHealth() && IsInLove();
    }

    bool Camel::CanMate(const Animal& other) const {
        // MC Camel.canMate; CamelHusk.canMate is false.
        if (IsHusk() || &other == this) return false;
        const auto* camel = dynamic_cast<const Camel*>(&other);
        return camel && !camel->IsHusk() && CanParent() && camel->CanParent();
    }

    // ── AbstractHorse's life ─────────────────────────────────────────────────

    void Camel::AiStep() {
        GenericAnimal::AiStep();
        // MC AbstractHorse.aiStep, server half: the slow self-heal and
        // grazing (canEatGrass is true for a camel too).
        if (!m_level || m_level->IsClientSide() || !IsAlive()) return;
        JavaRandom& rng = m_level->Random();
        if (rng.NextInt(900) == 0 && deathTime == 0) Heal(1.0f);
        if (!m_eating && !IsVehicle() && rng.NextInt(300) == 0) {
            const IBlockAccess* blocks = m_level->Blocks();
            const glm::ivec3 p = BlockPosition();
            if (blocks && blocks->GetBlock(p.x, p.y - 1, p.z) == BlockID::Grass) m_eating = true;
        }
        if (m_eating && ++m_eatingCounter > 50) {
            m_eatingCounter = 0;
            m_eating = false;
        }
    }

    bool Camel::CauseFallDamage(double fallDist, float damageMultiplier) {
        // MC AbstractHorse.causeFallDamage.
        if (m_level && m_level->IsClientSide()) return false;
        if (fallDist > 1.0) {
            PlaySound(IsBaby() ? SoundEvents::HORSE_LAND_BABY : SoundEvents::HORSE_LAND, 0.4f, 1.0f);
        }
        const int damage = CalculateFallDamage(fallDist, damageMultiplier);
        if (damage <= 0) return false;
        Hurt(MobDamageSource::Fall, static_cast<float>(damage), nullptr);
        // (propagateFallToPassengers rides Entity::CheckFallDamage, beside
        // this call.)
        PlayBlockFallSound();
        return true;
    }

    void Camel::ActuallyHurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC Camel.actuallyHurt: up at once, then the damage.
        StandUpInstantly();
        GenericAnimal::ActuallyHurt(source, amount, attacker);
    }

    void Camel::UpdateWalkAnimation(float distance) {
        // MC Camel.updateWalkAnimation.
        const float targetSpeed = (GetPose() == Pose::Standing && !Anim(MobAnim::Dash).IsStarted())
                                      ? std::min(distance * 6.0f, 1.0f)
                                      : 0.0f;
        walkAnimation.Update(targetSpeed, 0.2f, IsBaby() ? 3.0f : 1.0f);
    }

    void Camel::Travel(const glm::dvec3& input) {
        // MC Camel.travel: a camel that refuses to move keeps only its
        // vertical motion on the ground.
        glm::dvec3 in = input;
        if (RefuseToMove() && onGround) {
            velocity.x = 0.0;
            velocity.z = 0.0;
            in.x = 0.0;
            in.z = 0.0;
        }
        GenericAnimal::Travel(in);
    }

    // ── Riding ───────────────────────────────────────────────────────────────

    bool Camel::CanBeSteeredBy(const RiderControl& rider) const {
        // MC AbstractHorse.getControllingPassenger: saddled, a player first.
        (void)rider;
        return IsSaddled();
    }

    glm::dvec3 Camel::GetRiddenInput(const RiderControl& rider, const glm::dvec3& selfInput) {
        (void)selfInput;
        // MC Camel.getRiddenInput: nothing while it refuses to move; else
        // AbstractHorse's — half-speed strafing, a quarter backwards (the
        // rearing stop never applies: a camel cannot rear).
        if (RefuseToMove()) return glm::dvec3(0.0);
        const float sideways = rider.xxa * 0.5f;
        float forward = rider.zza;
        if (forward <= 0.0f) forward *= 0.25f;
        return glm::dvec3(static_cast<double>(sideways), 0.0, static_cast<double>(forward));
    }

    void Camel::TickRidden(const RiderControl& rider, const glm::dvec3& riddenInput) {
        (void)riddenInput;
        // AbstractHorse.tickRidden: getRiddenRotation — Camel's keeps its
        // own while it refuses to move, else (rider pitch / 2, rider yaw).
        float riddenXRot = xRot;
        float riddenYRot = yRot;
        if (!RefuseToMove()) {
            riddenXRot = rider.xRot * 0.5f;
            riddenYRot = rider.yRot;
        }
        // setRot(y % 360, x % 360); yRotO = yBodyRot = yHeadRot = yRot.
        yRot = std::fmod(riddenYRot, 360.0f);
        xRot = std::fmod(riddenXRot, 360.0f);
        yRotO = yBodyRot = yHeadRot = yRot;
        if (CanSimulateMountMovement() && onGround) {
            // The charged dash, spent on the ground (isJumping is the
            // mount's own jump key, never set under a rider).
            if (m_playerJumpPendingScale > 0.0f && !jumping) ExecuteRidersJump(m_playerJumpPendingScale);
            m_playerJumpPendingScale = 0.0f;
        }
        // Camel.tickRidden: pressing forward on a seated camel stands it up.
        if (rider.zza > 0.0f && IsCamelSitting() && !IsInPoseTransition()) StandUp();
    }

    float Camel::GetRiddenSpeed(const RiderControl& rider) const {
        // MC Camel.getRiddenSpeed: +0.1 while the rider sprints (a passenger
        // sprints on a forward impulse, Camel.canSprint) and the dash is off
        // cooldown.
        const bool sprinting = rider.sprinting && rider.zza > 1.0e-5f;
        const float movementBonus = sprinting && GetJumpCooldown() == 0 ? 0.1f : 0.0f;
        return static_cast<float>(GetAttributeValue(Attribute::MovementSpeed)) + movementBonus;
    }

    float Camel::GetBlockSpeedFactor() const {
        // MC Entity.getBlockSpeedFactor (soul sand and honey slow to 0.4: the
        // block at the feet, else — water and bubble columns excepted — the
        // one below that affects movement), lifted toward 1 by
        // MOVEMENT_EFFICIENCY (LivingEntity.getBlockSpeedFactor).
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (!blocks) return 1.0f;
        const auto factorOf = [](BlockID id) {
            return (id == BlockID::SoulSand || id == BlockID::HoneyBlock) ? 0.4f : 1.0f;
        };
        const glm::ivec3 feet = BlockPosition();
        const BlockID here = blocks->GetBlock(feet.x, feet.y, feet.z);
        float factor = factorOf(here);
        if (here != BlockID::Water && here != BlockID::BubbleColumn && factor == 1.0f) {
            const int belowY = static_cast<int>(std::floor(position.y - 0.500001));
            factor = factorOf(blocks->GetBlock(feet.x, belowY, feet.z));
        }
        const float efficiency = static_cast<float>(GetAttributeValue(Attribute::MovementEfficiency));
        return Mth::Lerp(efficiency, factor, 1.0f);
    }

    void Camel::ExecuteRidersJump(float amount) {
        // MC Camel.executeRidersJump: along the look (flattened), scaled by
        // the charge, speed and the block underfoot, plus a lift from the
        // jump power.
        const double jumpMomentum = static_cast<double>(GetJumpPower());
        const glm::vec3 look = Mth::ViewVector(xRot, yRot);
        glm::dvec3 flat(static_cast<double>(look.x), 0.0, static_cast<double>(look.z));
        const double length = std::sqrt(flat.x * flat.x + flat.z * flat.z);
        flat = length < 1.0e-5 ? glm::dvec3(0.0) : flat / length;
        const double push = static_cast<double>(22.2222f * amount) * GetAttributeValue(Attribute::MovementSpeed) *
                            static_cast<double>(GetBlockSpeedFactor());
        velocity += flat * push + glm::dvec3(0.0, static_cast<double>(1.4285f * amount) * jumpMomentum, 0.0);
        m_dashCooldown = kDashCooldownTicks;
        SetDashing(true);
        needsSync = true;
    }

    bool Camel::CanJump() const {
        return !RefuseToMove() && IsSaddled();
    }

    void Camel::OnPlayerJump(int jumpAmount) {
        // MC Camel.onPlayerJump → AbstractHorse.onPlayerJump (the stand it
        // would try is refused: a camel cannot rear).
        if (!IsSaddled() || m_dashCooldown > 0 || !onGround) return;
        if (jumpAmount < 0) jumpAmount = 0;
        // PlayerRideableJumping.getPlayerJumpPendingScale.
        m_playerJumpPendingScale = jumpAmount >= 90 ? 1.0f
                                                    : 0.4f + 0.4f * static_cast<float>(jumpAmount) / 90.0f;
    }

    void Camel::HandleStartJump(int jumpScale) {
        (void)jumpScale;
        // MC Camel.handleStartJump.
        MakeSound(GetDashingSound());
        GameEvent(GameEventId::EntityAction);
        SetDashing(true);
    }

    void Camel::SetDashing(bool v) {
        if (v == m_dashing) return;
        m_dashing = v;
        // MC Camel.onSyncedDataUpdated(DASH), which SynchedEntityData runs
        // on every change of the flag on either side (not on the first tick):
        // arm the cooldown unless one is running.
        if (tickCount > 0) m_dashCooldown = m_dashCooldown == 0 ? kDashCooldownTicks : m_dashCooldown;
    }

    // ── Seats ────────────────────────────────────────────────────────────────

    namespace {
        // Vec3.yRot(-yRot in radians) of (0, y, z).
        glm::dvec3 CamelSeatRotated(double y, double z, float yRot) {
            const double a = -static_cast<double>(yRot) * static_cast<double>(Mth::kDegToRad);
            return glm::dvec3(z * std::sin(a), y, z * std::cos(a));
        }
    }

    glm::dvec3 Camel::GetPassengerAttachmentPoint(const Entity& passenger) const {
        // MC Camel.getPassengerAttachmentPoint(passenger, getDimensions(pose),
        // getScale() * getAgeScale()).
        const auto& riders = GetPassengers();
        const auto it = std::find(riders.begin(), riders.end(), &passenger);
        const int index = it == riders.end() ? 0 : static_cast<int>(it - riders.begin());
        const bool driver = index == 0;
        const float ageScale = scale * GetCamelAgeScale();
        float offset = 0.5f;
        const double height = IsRemoved() ? 0.009999999776482582
                                          : GetBodyAnchorAnimationYOffset(driver, 0.0f, GetBbHeight(), ageScale);
        if (riders.size() > 1) {
            if (!driver) offset = -0.7f;
            if (dynamic_cast<const Animal*>(&passenger)) offset += 0.2f;
        }
        return CamelSeatRotated(height, static_cast<double>(offset * ageScale), yRot);
    }

    glm::dvec3 Camel::GetPassengerAttachmentForSlot(int slot, int total) const {
        // The same seat known by its place (a client's player — never an
        // Animal).
        const bool driver = slot <= 0;
        const float ageScale = scale * GetCamelAgeScale();
        float offset = 0.5f;
        const double height = IsRemoved() ? 0.009999999776482582
                                          : GetBodyAnchorAnimationYOffset(driver, 0.0f, GetBbHeight(), ageScale);
        if (total > 1 && !driver) offset = -0.7f;
        return CamelSeatRotated(height, static_cast<double>(offset * ageScale), yRot);
    }

    void Camel::PositionRider(Entity& passenger) {
        GenericAnimal::PositionRider(passenger);
        if (LivingEntity* living = passenger.AsLiving()) living->yBodyRot = yBodyRot;
    }

    glm::dvec3 Camel::GetDismountLocationForPassenger(const LivingEntity& passenger) const {
        return HorseTaming::EquineDismountLocation(*this, passenger);
    }

    void Camel::OnPoseUpdated() {
        // The CLIENT learns the pose from the wire and recomputes the tick from
        // it — see the note on m_lastPoseChangeTick.
        if (!m_level || !m_level->IsClientSide()) return;
        const int64_t now = m_level->GetGameTime();
        if (GetPose() == Pose::Sitting) {
            if (!IsCamelSitting()) ResetLastPoseChangeTick(-now);
        } else if (IsCamelSitting()) {
            ResetLastPoseChangeTick(now);
        }
    }

    void Camel::Tick() {
        GenericAnimal::Tick();

        // MC Camel.tick's dash bookkeeping, both sides: the dash flag clears
        // once the camel is back on the ground, in liquid or riding
        // something, and the cooldown has run 5 ticks (55 → below 50); the
        // cooldown itself counts down wherever it was armed.
        if (IsDashing() && m_dashCooldown < 50 && (onGround || IsInLiquid() || IsPassenger())) {
            SetDashing(false);
        }
        if (m_dashCooldown > 0) {
            --m_dashCooldown;
            // MC: at zero, level.playSound(null, blockPosition(),
            // getDashReadySound(), NEUTRAL, 1, 1).
            if (m_dashCooldown == 0 && m_level) {
                m_level->PlaySound(nullptr, BlockPosition(), GetDashReadySound(), SoundSource::Neutral, 1.0f, 1.0f);
            }
        }

        // MC Camel.tick: a camel that refuses to move keeps its head within
        // 30° of its body (Mob.clampHeadRotationToBody).
        if (RefuseToMove()) {
            const float limit = static_cast<float>(GetMaxHeadYRot());
            const float delta = Mth::WrapDegrees(yBodyRot - yHeadRot);
            const float targetDelta = std::clamp(Mth::WrapDegrees(yBodyRot - yHeadRot), -limit, limit);
            yHeadRot = yHeadRot + delta - targetDelta;
        }

        // MC Camel.tick: a sitting camel that ends up in water stands straight
        // back up, because the sitting hitbox would drown it.
        if (m_level && !m_level->IsClientSide() && IsCamelSitting() && IsInWater()) {
            StandUpInstantly();
        }
    }

    void Camel::SetAnimStateByte(uint8_t v) {
        // MC Camel.onSyncedDataUpdated's DASH branch rides SetDashing.
        SetDashing((v & 1) != 0);
    }

    void Camel::SetupAnimationStates() {
        // MC Camel.setupAnimationStates, now complete: the sit clips are live
        // because RandomSitting actually sits the camel down.
        if (m_idleAnimationTimeout <= 0) {
            m_idleAnimationTimeout =
                (m_level ? m_level->Random().NextInt(40) : 0) + 80;
            Anim(MobAnim::Idle).Start(tickCount);
        } else {
            --m_idleAnimationTimeout;
        }

        if (IsCamelVisuallySitting()) {
            Anim(MobAnim::SitUp).Stop();
            Anim(MobAnim::Dash).Stop();
            if (IsVisuallySittingDown()) {
                Anim(MobAnim::Sit).StartIfStopped(tickCount);
                Anim(MobAnim::SitPose).Stop();
            } else {
                Anim(MobAnim::Sit).Stop();
                Anim(MobAnim::SitPose).StartIfStopped(tickCount);
            }
        } else {
            Anim(MobAnim::Sit).Stop();
            Anim(MobAnim::SitPose).Stop();
            // MC's line verbatim: the dash flag rides the anim-state byte, so
            // this fires the moment a rider's dash (the steering client's own
            // executeRidersJump, the server's handleStartJump) sets it.
            Anim(MobAnim::Dash).AnimateWhen(IsDashing(), tickCount);
            Anim(MobAnim::SitUp).AnimateWhen(IsInPoseTransition() && GetPoseTime() >= 0,
                                             tickCount);
        }
    }

    // ══ Bat ════════════════════════════════════════════════════════════════

    namespace {

        // MC BlockState.isRedstoneConductor, as near as this engine gets: a
        // full-cube collision shape. MC additionally excludes signal sources
        // (a redstone block is not a conductor), which changes nothing here —
        // a bat's only use of it is "can I hang from this ceiling".
        bool IsCeilingBlock(const IBlockAccess* blocks, int x, int y, int z) {
            return blocks && blocks->IsBlockSolid(x, y, z);
        }

        // MC Level.getMinY for the overworld. A bat that picked a target
        // below the world would never reach it and would stop steering.
        constexpr int kBatMinY = -64;

        bool IsEmptyBlock(const IBlockAccess* blocks, int x, int y, int z) {
            return !blocks || blocks->GetBlock(x, y, z) == BlockID::Air;
        }

    } // namespace

    const char* Bat::GetAmbientSound() const {
        // MC Bat.getAmbientSound: `isResting() && random.nextInt(4) != 0 ?
        // null : BAT_AMBIENT`.
        if (IsResting() && m_level && m_level->Random().NextInt(4) != 0) return "";
        return SoundEvents::BAT_AMBIENT;
    }

    Bat::Bat(EntityLevel* level) : GenericMob(EntityTypeId::Bat, level) {
        // MC's Bat does not override registerGoals at all — its entire
        // behaviour is customServerAiStep below. GenericMob's default set
        // (float, look-at-player, look-around) would fight it: the look goals
        // write yHeadRot every tick while the flight code is steering yRot from
        // the velocity, so the bat would fly one way and face another.
        m_goalSelector.Clear();

        // MC's Bat constructor: `this.setResting(true)`. A bat spawns hanging.
        m_resting = true;
    }

    void Bat::Tick() {
        GenericMob::Tick();

        if (m_resting) {
            // MC pins a resting bat to the ceiling: no motion at all, and the
            // body hung from the block above rather than standing on the floor.
            velocity = glm::dvec3(0.0);
            position.y = std::floor(position.y) + 1.0
                       - static_cast<double>(GetBbHeight());
        } else {
            // MC damps the VERTICAL component only, which is what turns the
            // 0.7 upward push into a flutter instead of a climb.
            velocity.y *= 0.6;
        }
    }

    void Bat::SetupAnimationStates() {
        // MC Bat.setupAnimationStates.
        if (m_resting) {
            Anim(MobAnim::Fly).Stop();
            Anim(MobAnim::Rest).StartIfStopped(tickCount);
        } else {
            Anim(MobAnim::Rest).Stop();
            Anim(MobAnim::Fly).StartIfStopped(tickCount);
        }
    }

    void Bat::CustomServerAiStep() {
        // MC Bat.customServerAiStep, transcribed. This is the whole of a bat's
        // AI — no navigation, no goals, just a wandering target position it
        // steers toward — which is why it ports directly where the brain mobs
        // do not.
        if (!m_level) return;
        const IBlockAccess* blocks = m_level->Blocks();

        const glm::ivec3 pos = BlockPosition();
        const glm::ivec3 above(pos.x, pos.y + 1, pos.z);

        if (m_resting) {
            if (IsCeilingBlock(blocks, above.x, above.y, above.z)) {
                if (m_level->Random().NextInt(200) == 0) {
                    yHeadRot = static_cast<float>(m_level->Random().NextInt(360));
                }
                // MC BAT_RESTING_TARGETING is `forNonCombat().range(4)`.
                if (m_level->GetNearestPlayer(position.x, position.y, position.z,
                                              4.0) != nullptr) {
                    m_resting = false;
                }
            } else {
                // The block it was hanging from is gone.
                m_resting = false;
            }
            return;
        }

        if (m_hasTarget
            && (!IsEmptyBlock(blocks, m_targetPosition.x, m_targetPosition.y,
                              m_targetPosition.z)
                || m_targetPosition.y <= kBatMinY)) {
            m_hasTarget = false;
        }

        JavaRandom& rnd = m_level->Random();
        const glm::dvec3 targetCentre(
            static_cast<double>(m_targetPosition.x) + 0.5,
            static_cast<double>(m_targetPosition.y),
            static_cast<double>(m_targetPosition.z) + 0.5);
        const double dxT = targetCentre.x - position.x;
        const double dzT = targetCentre.z - position.z;
        const double dyT = targetCentre.y - position.y;

        if (!m_hasTarget || rnd.NextInt(30) == 0
            || (dxT * dxT + dyT * dyT + dzT * dzT) < 4.0) {
            // MC draws each axis from two independent nextInt(7)s, so the
            // offset is triangular rather than uniform — bats hover near where
            // they are far more often than they dart 6 blocks away.
            m_targetPosition = glm::ivec3(
                static_cast<int>(std::floor(position.x)) + rnd.NextInt(7) - rnd.NextInt(7),
                static_cast<int>(std::floor(position.y)) + rnd.NextInt(6) - 2,
                static_cast<int>(std::floor(position.z)) + rnd.NextInt(7) - rnd.NextInt(7));
            m_hasTarget = true;
        }

        const double dx = static_cast<double>(m_targetPosition.x) + 0.5 - position.x;
        const double dy = static_cast<double>(m_targetPosition.y) + 0.1 - position.y;
        const double dz = static_cast<double>(m_targetPosition.z) + 0.5 - position.z;

        const auto signum = [](double v) { return v > 0.0 ? 1.0 : (v < 0.0 ? -1.0 : 0.0); };
        velocity.x += (signum(dx) * 0.5 - velocity.x) * 0.1;
        velocity.y += (signum(dy) * 0.7 - velocity.y) * 0.1;
        velocity.z += (signum(dz) * 0.5 - velocity.z) * 0.1;

        const float wanted = static_cast<float>(
            std::atan2(velocity.z, velocity.x) * (180.0 / 3.14159265358979323846)) - 90.0f;
        yRot += Mth::WrapDegrees(wanted - yRot);
        zza = 0.5f;

        if (rnd.NextInt(100) == 0
            && IsCeilingBlock(blocks, above.x, above.y, above.z)) {
            m_resting = true;
        }
    }

    // ══ Tadpole / Goat / Hoglin ═══════════════════════════════════════════

    Tadpole::Tadpole(EntityLevel* level) : GenericPathfinderMob(EntityTypeId::Tadpole, level) {
        m_goalSelector.Clear();
        m_targetSelector.Clear();
        m_brain = std::make_unique<Brain>();
        TadpoleAi::InitBrain(*this, *m_brain);
    }
    void Tadpole::UpdateBrainActivity() { TadpoleAi::UpdateActivity(*this); }

    void Tadpole::BaseTick() {
        // MC WaterAnimal.baseTick (Tadpole extends AbstractFish): a stranded
        // tadpole suffocates like a beached fish.
        const int airSupply = GetAirSupply();
        GenericPathfinderMob::BaseTick();
        HandleWaterAnimalAirSupply(*this, airSupply);
    }

    void Tadpole::AiStep() {
        GenericPathfinderMob::AiStep();
        // MC Tadpole.aiStep: the server ages it a tick unless locked.
        if (m_level && !m_level->IsClientSide() && !m_ageLocked && !IsRemoved()) SetAge(m_age + 1);
        // MC AgeableMob.makeAgeLockedParticle: one PAUSE_MOB_GROWTH (locked)
        // or RESET_MOB_GROWTH (unlocked) every other tick of the burst, just
        // above the body (the locked one 0.2 higher, drifting down).
        if (m_ageLockParticleTimer > 0) {
            if ((m_ageLockParticleTimer % 2) == 0 && m_level) {
                JavaRandom& rng = m_level->Random();
                const double w = static_cast<double>(GetBbWidth());
                const double h = static_cast<double>(GetBbHeight());
                const double px = position.x + w * (2.0 * rng.NextDouble() - 1.0);
                const double py = position.y + h * 0.2 * rng.NextDouble() + h + (m_ageLocked ? 0.2 : 0.0);
                const double pz = position.z + w * (2.0 * rng.NextDouble() - 1.0);
                m_level->AddParticle(m_ageLocked ? ParticleKind::PauseMobGrowth : ParticleKind::ResetMobGrowth,
                                     px, py, pz, 0.0, 0.0, 0.0);
            }
            --m_ageLockParticleTimer;
        }
    }

    void Tadpole::SetAge(int age) {
        m_age = age;
        if (m_age >= kTicksToBeFrog) BecomeFrog();
    }

    void Tadpole::BecomeFrog() {
        // MC Tadpole.ageUp: convertTo(FROG, ConversionParams.single(this,
        // false, false), frog -> { finalizeSpawn(CONVERSION);
        // setPersistenceRequired(); fudgePositionAfterSizeChange; the
        // grow-up sound }). The frog (0.5 wide) centred on the tadpole's
        // spot (0.4) needs no nudge out of a wall a tadpole fitted in.
        if (!m_level || m_level->IsClientSide() || IsRemoved()) return;
        std::unique_ptr<Mob> frog = MakeGenericMob(EntityTypeId::Frog, m_level);
        if (!frog) return;
        CopyConversionState(*frog);
        frog->FinalizeSpawn(SpawnReason::Conversion, nullptr);
        frog->SetPersistenceRequired(true);
        PlaySound(SoundEvents::TADPOLE_GROW_UP, 0.15f, 1.0f);
        FinishConversion(std::move(frog));
    }

    UseResult Tadpole::MobInteract(LivingEntity& player, ItemStack& held) {
        const bool client = m_level && m_level->IsClientSide();
        // isFood: #frog_food (the slime ball), and not while locked.
        if (held.itemId == Items::SlimeBall && !m_ageLocked) {
            // MC feed: consume one (not in creative), age up by 10 % of what
            // is left (getSpeedUpSecondsWhenFeeding), and the HAPPY_VILLAGER
            // puff — a client-side addParticle in MC, so the client draws it.
            if (client) {
                if (m_level) {
                    JavaRandom& rng = m_level->Random();
                    const double w = static_cast<double>(GetBbWidth());
                    m_level->AddParticle(ParticleKind::HappyVillager,
                                         position.x + w * (2.0 * rng.NextDouble() - 1.0),
                                         position.y + GetBbHeight() * rng.NextDouble() + 0.5,
                                         position.z + w * (2.0 * rng.NextDouble() - 1.0), 0.0, 0.0, 0.0);
                }
                return UseResult::Success;
            }
            if (!player.IsCreative()) Animal::UsePlayerItem(held);
            const int ticksLeft = std::max(0, kTicksToBeFrog - m_age);
            SetAge(m_age + AgeableMob::GetSpeedUpSecondsWhenFeeding(ticksLeft) * 20);
            return UseResult::Success;
        }
        // AgeableMob.canUseGoldenDandelion(itemStack, true, timer, this) →
        // setAgeLocked: toggle, age back to 0, the 40-tick burst/cooldown,
        // the flower spent, persistence when locking, the sound.
        if (AgeableMob::CanUseGoldenDandelion(held, /*isBaby=*/true, m_ageLockParticleTimer, *this)) {
            if (client) return UseResult::Success;
            m_ageLocked = !m_ageLocked;
            m_age = 0;
            m_ageLockParticleTimer = 40;
            if (!player.IsCreative()) Animal::UsePlayerItem(held);
            if (m_ageLocked) SetPersistenceRequired(true);
            if (m_level) {
                m_level->PlaySound(nullptr, BlockPosition(),
                                   m_ageLocked ? SoundEvents::GOLDEN_DANDELION_USE
                                               : SoundEvents::GOLDEN_DANDELION_UNUSE,
                                   SoundSource::Players, 1.0f, 1.0f);
            }
            return UseResult::Success;
        }
        if (auto r = Bucketable::BucketMobPickup(*this, player, held, Items::TadpoleBucket,
                                                 SoundEvents::BUCKET_FILL_TADPOLE,
                                                 [this](ItemStack& bucket) { SaveToBucket(bucket); })) {
            return *r;
        }
        return GenericPathfinderMob::MobInteract(player, held);
    }

    void Tadpole::SaveToBucket(ItemStack& bucket) const {
        Bucketable::SaveDefaultDataToBucketTag(*this, bucket);
        BucketEntityData data = bucket.components.get(DataComponents::BUCKET_ENTITY_DATA)
                                    .value_or(BucketEntityData{});
        data.age = m_age;
        data.ageLocked = m_ageLocked;
        bucket.components.set(DataComponents::BUCKET_ENTITY_DATA, data);
    }

    void Tadpole::LoadFromBucket(const BucketEntityData& data) {
        Bucketable::LoadDefaultDataFromBucketTag(*this, data);
        // tag.getInt("Age").ifPresent(setAge); AgeLocked or false. (A
        // bucketed tadpole is always short of the frog age — it would have
        // converted — so this never converts a mob not yet in the level.)
        if (data.age) m_age = *data.age;
        m_ageLocked = data.ageLocked.value_or(false);
    }

    Goat::Goat(EntityLevel* level) : GenericAnimal(EntityTypeId::Goat, level) {
        m_goalSelector.Clear();
        m_targetSelector.Clear();
        m_brain = std::make_unique<Brain>();
        GoatAi::InitBrain(*this, *m_brain);
        GoatAi::InitMemories(*this);
    }
    void Goat::UpdateBrainActivity() { GoatAi::UpdateActivity(*this); }

    void Goat::PlayEatingSound() {
        if (!m_level) return;
        JavaRandom& rng = m_level->Random();
        m_level->PlaySoundFromEntity(nullptr, *this, SoundEvents::GOAT_EAT, SoundSource::Neutral, 1.0f,
                                     0.8f + rng.NextFloat() * 0.4f);
    }

    Hoglin::Hoglin(EntityLevel* level) : GenericAnimal(EntityTypeId::Hoglin, level) {
        m_goalSelector.Clear();
        m_targetSelector.Clear();
        m_brain = std::make_unique<Brain>();
        HoglinAi::InitBrain(*this, *m_brain);
    }
    void Hoglin::UpdateBrainActivity() { HoglinAi::UpdateActivity(*this); }

    const char* Hoglin::GetAmbientSound() const {
        if (!m_level || m_level->IsClientSide()) return "";
        return HoglinAi::SoundForCurrentActivity(*this);
    }

    const char* Piglin::GetAmbientSound() const {
        if (!m_level || m_level->IsClientSide()) return "";
        return PiglinAi::SoundForCurrentActivity(*this);
    }

    const char* Zoglin::GetAmbientSound() const {
        if (!m_level || m_level->IsClientSide()) return "";
        const Brain* brain = GetBrain();
        return brain && brain->HasMemoryValue(MemoryModule::AttackTarget) ? SoundEvents::ZOGLIN_ANGRY
                                                                           : SoundEvents::ZOGLIN_AMBIENT;
    }

    namespace {

        // MC HoglinBase.throwTarget: the knockback left after the target's
        // resistance flings it — a random 0.2..0.7 share horizontally, turned
        // by nextInt(21) - 10 (Vec3.yRot takes RADIANS), and up to half of it
        // upward.
        void HoglinThrowTarget(Mob& body, LivingEntity& target) {
            EntityLevel* level = body.Level();
            if (!level) return;
            const double knockbackPower = body.GetAttributeValue(Attribute::AttackKnockback);
            const double knockbackResistance = target.GetAttributeValue(Attribute::KnockbackResistance);
            const double effective = knockbackPower - knockbackResistance;
            if (effective <= 0.0) return;
            const double xd = target.position.x - body.position.x;
            const double zd = target.position.z - body.position.z;
            JavaRandom& random = level->Random();
            const float horizontalPushAngle = static_cast<float>(random.NextInt(21) - 10);
            const double horizontalScale = effective * static_cast<double>(random.NextFloat() * 0.5f + 0.2f);
            glm::dvec3 push(xd, 0.0, zd);
            const double len = glm::length(push);
            push = len < 1.0e-5 ? glm::dvec3(0.0) : push / len * horizontalScale;
            const double c = static_cast<double>(std::cos(horizontalPushAngle));
            const double sn = static_cast<double>(std::sin(horizontalPushAngle));
            const glm::dvec3 turned(push.x * c + push.z * sn, 0.0, push.z * c - push.x * sn);
            const double verticalScale = effective * static_cast<double>(random.NextFloat()) * 0.5;
            // target.push(...) + syncVelocity: the push reaches the client.
            target.AddDeltaMovement(glm::dvec3(turned.x, verticalScale, turned.z));
        }

        // MC HoglinBase.hurtAndThrowTarget — the hoglin's and the zoglin's
        // hit: an adult deals ATTACK_DAMAGE / 2 + nextInt(ATTACK_DAMAGE)
        // (a baby its flat 0.5) as a mob attack, then the post-attack
        // enchantment effects and, for an adult, the fling.
        bool HoglinHurtAndThrowTarget(Mob& body, LivingEntity& target) {
            EntityLevel* level = body.Level();
            if (!level) return false;
            const float attackDamage = static_cast<float>(body.GetAttributeValue(Attribute::AttackDamage));
            float actualDamage = attackDamage;
            if (!body.IsBaby() && static_cast<int>(attackDamage) > 0) {
                actualDamage = attackDamage / 2.0f +
                               static_cast<float>(level->Random().NextInt(static_cast<int>(attackDamage)));
            }
            const bool wasHurt = target.Hurt(MobDamageSource::MobAttack, actualDamage, &body);
            if (wasHurt) {
                if (!level->IsClientSide()) {
                    EnchantmentHelper::DoPostAttackEffects(
                        *level, target, DamageSourceInfo::Of(MobDamageSource::MobAttack, &body, nullptr));
                }
                if (!body.IsBaby()) HoglinThrowTarget(body, target);
            }
            return wasHurt;
        }

    } // namespace

    bool Hoglin::DoHurtTarget(Entity& target) {
        // MC Hoglin.doHurtTarget: only living targets; arm the headbutt clock
        // and broadcast event 4 BEFORE the hit lands, so the animation starts
        // on the same tick, with the HOGLIN_ATTACK grunt; then
        // HoglinAi.onHitTarget (the pack rally or retreat) and
        // HoglinBase.hurtAndThrowTarget.
        auto* living = dynamic_cast<LivingEntity*>(&target);
        if (!living) return false;
        m_attackAnimationRemainingTicks = 10;
        if (m_level) m_level->BroadcastEntityEvent(*this, 4);
        MakeSound(SoundEvents::HOGLIN_ATTACK);
        HoglinAi::OnHitTarget(*this, *living);
        return HoglinHurtAndThrowTarget(*this, *living);
    }

    void Hoglin::AiStep() {
        // MC Hoglin.aiStep: the clock counts down BEFORE super — both sides,
        // which is what animates a remote hoglin's headbutt.
        if (m_attackAnimationRemainingTicks > 0) {
            --m_attackAnimationRemainingTicks;
        }
        SyncAgeBoundary();
        GenericAnimal::AiStep();
    }

    void Hoglin::SyncAgeBoundary() {
        // MC Hoglin.ageBoundaryReached (AgeableMob.setAge on a crossing, and
        // on load): the baby's feeble 0.5 attack, the adult's 6.
        const int baby = IsBaby() ? 1 : 0;
        if (baby == m_lastBabyState) return;
        m_lastBabyState = baby;
        m_attributes.SetBaseValue(Attribute::AttackDamage, baby ? 0.5 : 6.0);
    }

    bool Hoglin::IsConverting() const {
        return !m_immuneToZombification && !IsNoAi() && m_level &&
               m_level->Dimension() != DimensionId::Nether;
    }

    std::shared_ptr<SpawnGroupData>
    Hoglin::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        if (m_level && m_level->Random().NextFloat() < 0.2f) SetBaby(true);
        SyncAgeBoundary();
        return GenericAnimal::FinalizeSpawn(reason, std::move(groupData));
    }

    bool Hoglin::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        const bool wasHurt = GenericAnimal::Hurt(source, amount, attacker);
        if (wasHurt && m_level && !m_level->IsClientSide()) {
            if (auto* living = dynamic_cast<LivingEntity*>(attacker)) {
                HoglinAi::WasHurtBy(*m_level, *this, *living);
            }
        }
        return wasHurt;
    }

    float Hoglin::GetWalkTargetValue(const glm::ivec3& pos) const {
        if (HoglinAi::IsPosNearNearestRepellent(*this, pos)) return -1.0f;
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        return blocks && blocks->GetBlock(pos.x, pos.y - 1, pos.z) == BlockID::CrimsonNylium ? 10.0f : 0.0f;
    }

    bool Hoglin::CanFallInLove() const {
        return !HoglinAi::IsPacified(*this) && GenericAnimal::CanFallInLove();
    }

    std::unique_ptr<Animal> Hoglin::CreateBaby() {
        std::unique_ptr<Animal> baby = GenericAnimal::CreateBaby();
        if (baby) baby->SetPersistenceRequired(true);
        return baby;
    }

    UseResult Hoglin::MobInteract(LivingEntity& player, ItemStack& held) {
        const UseResult result = GenericAnimal::MobInteract(player, held);
        if (ConsumesAction(result)) SetPersistenceRequired(true);
        return result;
    }

    void Hoglin::CustomServerAiStep() {
        // MC Hoglin.customServerAiStep, after the brain: the conversion clock.
        GenericAnimal::CustomServerAiStep();
        if (!m_level || m_level->IsClientSide()) return;
        if (IsConverting()) {
            ++m_timeInOverworld;
            if (m_timeInOverworld > kConversionTime) {
                MakeSound(SoundEvents::HOGLIN_CONVERTED_TO_ZOMBIFIED);
                FinishConversion();
            }
        } else {
            m_timeInOverworld = 0;
        }
    }

    void Hoglin::FinishConversion() {
        // MC Hoglin.finishConversion: convertTo(ZOGLIN,
        // ConversionParams.single(this, keepEquipment = true,
        // preserveCanPickUpLoot = false)), the zoglin then gets NAUSEA 200.
        auto zoglin = std::make_unique<Zoglin>(m_level);
        CopyConversionState(*zoglin);
        MoveEquipmentTo(*zoglin);
        zoglin->SetBaby(IsBaby());
        zoglin->AddEffect(MobEffectInstance(MobEffectId::Nausea, 200, 0));
        GenericAnimal::FinishConversion(std::move(zoglin));
    }

    void Hoglin::HandleEntityEvent(uint8_t id) {
        // MC Hoglin.handleEntityEvent(4) — restart the headbutt clock (its
        // client-side attack sound is MC's silent null-except playSound; the
        // server's DoHurtTarget voices it).
        if (id == 4) {
            m_attackAnimationRemainingTicks = 10;
        } else {
            GenericAnimal::HandleEntityEvent(id);
        }
    }

    // ── Zoglin ─────────────────────────────────────────────────────────────

    Zoglin::Zoglin(EntityLevel* level) : GenericMonster(EntityTypeId::Zoglin, level) {
        // NO GOALS — MC's Zoglin never registers any; its whole behaviour is
        // the brain. GenericMonster's constructor already registered the
        // def-driven monster set, so it is cleared here.
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        m_brain = std::make_unique<Brain>();
        ZoglinAi::InitBrain(*this, *m_brain);
    }

    void Zoglin::UpdateBrainActivity() { ZoglinAi::UpdateActivity(*this); }

    void Zoglin::SetBaby(bool baby) {
        // MC Zoglin.setBaby: the synched flag (the wire's shared baby bit
        // here), and on the server the attack-damage drop to BABY_ATTACK_DAMAGE.
        if (m_baby == baby) return;
        m_baby = baby;
        if (baby && m_level && !m_level->IsClientSide()) {
            m_attributes.SetBaseValue(Attribute::AttackDamage, 0.5);
        }
    }

    std::shared_ptr<SpawnGroupData>
    Zoglin::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC Zoglin.finalizeSpawn — a flat 20% baby roll, no group token.
        if (m_level && m_level->Random().NextFloat() < 0.2f) {
            SetBaby(true);
        }
        return GenericMonster::FinalizeSpawn(reason, std::move(groupData));
    }

    bool Zoglin::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        const bool hurt = GenericMonster::Hurt(source, amount, attacker);
        // MC Zoglin.hurtServer: retarget onto the attacker unless the current
        // target is much closer (BehaviorUtils.isOtherTargetMuchFurtherAway-
        // ThanCurrentAttackTarget, threshold 4).
        if (hurt && m_level && !m_level->IsClientSide()) {
            auto* living = dynamic_cast<LivingEntity*>(attacker);
            if (living && CanAttack(*living)) {
                bool muchFurther = false;
                if (const Brain* brain = GetBrain()) {
                    auto* current = dynamic_cast<LivingEntity*>(
                        brain->GetEntity(MemoryModule::AttackTarget));
                    if (current) {
                        // MC compares SQUARED distances plus threshold² — the
                        // exact expression, asymmetric as it is.
                        muchFurther = DistanceToSqr(*living)
                                    > DistanceToSqr(*current) + 4.0 * 4.0;
                    }
                }
                if (!muchFurther) {
                    ZoglinAi::SetAttackTarget(*this, *living);
                }
            }
        }
        return hurt;
    }

    bool Zoglin::DoHurtTarget(Entity& target) {
        // MC Zoglin.doHurtTarget, the Hoglin twin: only living targets, arm
        // the clock, broadcast event 4, ZOGLIN_ATTACK, then
        // HoglinBase.hurtAndThrowTarget.
        auto* living = dynamic_cast<LivingEntity*>(&target);
        if (!living) return false;
        m_attackAnimationRemainingTicks = 10;
        if (m_level) m_level->BroadcastEntityEvent(*this, 4);
        MakeSound(SoundEvents::ZOGLIN_ATTACK);
        return HoglinHurtAndThrowTarget(*this, *living);
    }

    void Zoglin::AiStep() {
        // MC Zoglin.aiStep: the clock counts down BEFORE super, both sides.
        if (m_attackAnimationRemainingTicks > 0) {
            --m_attackAnimationRemainingTicks;
        }
        GenericMonster::AiStep();
    }

    void Zoglin::HandleEntityEvent(uint8_t id) {
        // MC Zoglin.handleEntityEvent(4) — restart the headbutt clock.
        if (id == 4) {
            m_attackAnimationRemainingTicks = 10;
        } else {
            GenericMonster::HandleEntityEvent(id);
        }
    }

    // ── Piglin / PiglinBrute ───────────────────────────────────────────────

    namespace {
        // MC AbstractPiglin's PIGLINS_ZOMBIFY environment attribute: true
        // everywhere but the Nether.
        bool PiglinsZombify(const EntityLevel* level) {
            return level && level->Dimension() != DimensionId::Nether;
        }

        // MC AbstractPiglin.isHoldingMeleeWeapon: the main hand has a TOOL.
        bool IsHoldingMeleeWeapon(const Mob& mob) {
            const ItemStack& main = mob.GetMainHandEquipment();
            return !main.IsEmpty() && main.get(DataComponents::TOOL).has_value();
        }
    }

    Piglin::Piglin(EntityLevel* level) : GenericMonster(EntityTypeId::Piglin, level) {
        // NO GOALS — MC's Piglin never registers any; the brain is the whole
        // behaviour. AbstractPiglin: setCanPickUpLoot(true) and the fire
        // maluses (the door-opening ability is skipped with door
        // interaction).
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        SetCanPickUpLoot(true);
        SetPathfindingMalus(PathType::DangerFire, 16.0f);
        SetPathfindingMalus(PathType::DamageFire, -1.0f);

        m_brain = std::make_unique<Brain>();
        PiglinAi::InitBrain(*this, *m_brain);
    }

    void Piglin::UpdateBrainActivity() { PiglinAi::UpdateActivity(*this); }

    void Piglin::PlayAmbientSound() {
        // MC AbstractPiglin.playAmbientSound: PiglinAi.isIdle gates it.
        if (const Brain* brain = GetBrain(); brain && brain->IsActive(Activity::Idle)) {
            GenericMonster::PlayAmbientSound();
        }
    }

    void Piglin::SetBaby(bool baby) {
        // MC Piglin.setBaby — SPEED_MODIFIER_BABY: +20% ADD_MULTIPLIED_BASE.
        if (m_baby == baby) return;
        m_baby = baby;
        if (baby) {
            m_attributes.AddModifier(Attribute::MovementSpeed,
                AttributeModifier{ static_cast<uint32_t>(ModifierId::BabySpeedBoost), 0.2,
                                   AttributeOperation::AddMultipliedBase });
        } else {
            m_attributes.RemoveModifier(Attribute::MovementSpeed,
                                        ModifierId::BabySpeedBoost);
        }
    }

    bool Piglin::IsConverting() const {
        return !IsImmuneToZombification() && !IsNoAi() && PiglinsZombify(m_level);
    }

    std::shared_ptr<SpawnGroupData>
    Piglin::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC Piglin.finalizeSpawn. STRUCTURE spawns (the bastion's template
        // piglins) skip the baby / weapon roll: they keep the template's.
        if (m_level) {
            JavaRandom& random = m_level->Random();
            if (reason != SpawnReason::Structure) {
                if (random.NextFloat() < 0.2f) {
                    SetBaby(true);
                } else if (IsAdult()) {
                    // createSpawnWeapon: 50% a crossbow, else a golden sword
                    // (a golden spear one time in ten).
                    const ItemID weapon = static_cast<double>(random.NextFloat()) < 0.5
                        ? Items::Crossbow
                        : (random.NextInt(10) == 0 ? Items::GoldenSpear : Items::GoldenSword);
                    SetEquipment(EquipmentSlot::MAINHAND, ItemStack(weapon, 1));
                }
            }
            PiglinAi::InitMemories(*this);
            const DifficultyInstance difficulty = CurrentDifficulty();
            PopulateDefaultEquipmentSlots(random, difficulty);
            PopulateDefaultEquipmentEnchantments(random, difficulty);
        }
        return GenericMonster::FinalizeSpawn(reason, std::move(groupData));
    }

    void Piglin::PopulateDefaultEquipmentSlots(JavaRandom& random, const DifficultyInstance& difficulty) {
        (void)difficulty;
        if (!IsAdult()) return;
        // maybeWearArmor: CHANCE_OF_WEARING_EACH_ARMOUR_ITEM 0.1, head first.
        const auto maybeWear = [&](EquipmentSlot slot, ItemID item) {
            if (random.NextFloat() < 0.1f) SetEquipment(slot, ItemStack(item, 1));
        };
        maybeWear(EquipmentSlot::HEAD,  Items::GoldenHelmet);
        maybeWear(EquipmentSlot::CHEST, Items::GoldenChestplate);
        maybeWear(EquipmentSlot::LEGS,  Items::GoldenLeggings);
        maybeWear(EquipmentSlot::FEET,  Items::GoldenBoots);
    }

    bool Piglin::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        const bool hurt = GenericMonster::Hurt(source, amount, attacker);
        if (hurt && m_level && !m_level->IsClientSide()) {
            if (auto* living = dynamic_cast<LivingEntity*>(attacker)) {
                PiglinAi::WasHurtBy(*m_level, *this, *living);
            }
        }
        return hurt;
    }

    UseResult Piglin::MobInteract(LivingEntity& player, ItemStack& held) {
        const UseResult result = GenericMonster::MobInteract(player, held);
        if (ConsumesAction(result) || !m_level) return result;
        if (!m_level->IsClientSide()) {
            return PiglinAi::MobInteract(*m_level, *this, held) ? UseResult::Success : UseResult::Pass;
        }
        // The client: canAdmire (its memories are the server's — the arm
        // pose stands in for ADMIRING_ITEM) and not already admiring.
        const bool canAdmire = IsAdult() && PiglinAi::IsBarterCurrency(held) && GetPiglinArmPose() != 3;
        return canAdmire ? UseResult::Success : UseResult::Pass;
    }

    void Piglin::PerformRangedAttack(LivingEntity& target, float power) {
        (void)power;
        // MC performCrossbowAttack aims at getTarget() — AbstractPiglin's
        // brain target (GetTarget override).
        LivingEntity* aim = &target;
        if (LivingEntity* t = GetTarget()) aim = t;
        MobCrossbow::PerformCrossbowAttack(*this, *this, aim, MobCrossbow::kMobArrowPower);
    }

    bool Piglin::CanUseNonMeleeWeapon(const ItemStack& stack) const {
        // MC Piglin.canUseNonMeleeWeapon: the crossbow, or a KINETIC_WEAPON
        // (a spear — its charge is the brain's SpearAttack, not a melee).
        return stack.itemId == Items::Crossbow || Spear::Kinetic(stack).has_value();
    }

    bool Piglin::WantsToPickUp(const ItemStack& stack) const {
        return m_level && m_level->MobGriefing() && CanPickUpLoot() && PiglinAi::WantsToPickup(*this, stack);
    }

    bool Piglin::CanReplaceCurrentItem(const ItemStack& newStack, const ItemStack& current,
                                       EquipmentSlot slot) const {
        if (EnchantmentHelper::HasPreventArmorChange(current)) return false;
        const char* preferred = GetPreferredWeaponType();
        const auto inPreferred = [preferred](const ItemStack& s) {
            return preferred && !s.IsEmpty() &&
                   DataTags::HasTag(DataTags::Registry::Item, ItemRegistry::Slug(s.itemId), preferred);
        };
        const bool newItemWanted = PiglinAi::IsLovedItem(newStack) || inPreferred(newStack);
        const bool currentItemWanted = PiglinAi::IsLovedItem(current) || inPreferred(current);
        if (newItemWanted && !currentItemWanted) return true;
        if (!newItemWanted && currentItemWanted) return false;
        return GenericMonster::CanReplaceCurrentItem(newStack, current, slot);
    }

    bool Piglin::CanReplaceCurrentItem(const ItemStack& newStack) const {
        const EquipmentSlot slot = GetEquipmentSlotForItem(newStack);
        return CanReplaceCurrentItem(newStack, GetEquipment(slot), slot);
    }

    void Piglin::PickUpItem(int32_t itemEntityId, const ItemStack& stack) {
        if (!m_level) return;
        OnItemPickup(itemEntityId, stack);
        PiglinAi::PickUpItem(*m_level, *this, itemEntityId, stack);
    }

    void Piglin::HoldInMainHand(const ItemStack& stack) {
        SetItemSlotAndDropWhenKilled(EquipmentSlot::MAINHAND, stack);
        SetPersistenceRequired(true);
    }

    void Piglin::HoldInOffHand(const ItemStack& stack) {
        SetItemSlotAndDropWhenKilled(EquipmentSlot::OFFHAND, stack);
        if (!PiglinAi::IsBarterCurrency(stack)) SetPersistenceRequired(true);
    }

    ItemStack Piglin::AddToInventory(const ItemStack& stack) {
        return SimpleContainerOps::AddItem(m_inventory, stack);
    }

    bool Piglin::CanAddToInventory(const ItemStack& stack) const {
        return SimpleContainerOps::CanAddItem(m_inventory, stack);
    }

    void Piglin::DropCustomDeathLoot(EntityLevel& level) {
        GenericMonster::DropCustomDeathLoot(level);
        for (const ItemStack& stack : SimpleContainerOps::RemoveAllItems(m_inventory)) {
            DropItemStackAt(level.Dimension(), position, stack);   // spawnAtLocation
        }
    }

    int Piglin::GetPiglinArmPose() const {
        if (IsDancing()) return 4;                                             // DANCING
        if (PiglinAi::IsLovedItem(GetOffhandEquipment())) return 3;            // ADMIRING_ITEM
        if (IsAggressive() && IsHoldingMeleeWeapon(*this)) return 0;           // ATTACKING_WITH_MELEE_WEAPON
        if (IsChargingCrossbow()) return 2;                                    // CROSSBOW_CHARGE
        if (IsHoldingItem(Items::Crossbow) &&
            FireworkItems::IsCrossbowCharged(GetEquipment(MobCrossbow::WeaponHoldingHand(*this, Items::Crossbow)))) {
            return 1;                                                          // CROSSBOW_HOLD
        }
        return 5;                                                              // DEFAULT
    }

    void Piglin::Tick() {
        GenericMonster::Tick();
        if (m_level && m_level->IsClientSide()) {
            m_clientChargeTicks = m_chargingCrossbow ? m_clientChargeTicks + 1 : -1;
        }
    }

    void Piglin::FinishPiglinConversion() {
        // MC Piglin.finishConversion: cancelAdmiring, the pockets onto the
        // ground, then AbstractPiglin's.
        PiglinAi::CancelAdmiring(*m_level, *this);
        for (const ItemStack& stack : SimpleContainerOps::RemoveAllItems(m_inventory)) {
            DropItemStackAt(m_level->Dimension(), position, stack);
        }
        // MC AbstractPiglin.finishConversion: → ZOMBIFIED_PIGLIN with
        // ConversionParams.single(this, keepEquipment = true,
        // preserveCanPickUpLoot = true), whose afterConversion adds
        // NAUSEA 200 (after convertCommon copied the piglin's effects).
        auto zombified = std::make_unique<ZombifiedPiglin>(m_level);
        CopyConversionState(*zombified);
        MoveEquipmentTo(*zombified);
        zombified->SetBaby(IsBaby());
        zombified->AddEffect(MobEffectInstance(MobEffectId::Nausea, 200, 0));
        FinishConversion(std::move(zombified));
    }

    void Piglin::CustomServerAiStep() {
        // MC AbstractPiglin.customServerAiStep — the zombification clock.
        if (!m_level || m_level->IsClientSide()) return;
        m_timeInOverworld = IsConverting() ? m_timeInOverworld + 1 : 0;
        if (m_timeInOverworld > 300) {
            // playConvertedSound unless PEACEFUL; the conversion-shake visual
            // is skipped like the zombie→drowned one.
            if (m_level->GetDifficulty() != Difficulty::Peaceful) {
                MakeSound(SoundEvents::PIGLIN_CONVERTED_TO_ZOMBIFIED);
            }
            FinishPiglinConversion();
        }
    }

    PiglinBrute::PiglinBrute(EntityLevel* level)
        : GenericMonster(EntityTypeId::PiglinBrute, level) {
        // NO GOALS — the brain is the whole behaviour.
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        SetCanPickUpLoot(true);   // AbstractPiglin
        SetPathfindingMalus(PathType::DangerFire, 16.0f);
        SetPathfindingMalus(PathType::DamageFire, -1.0f);

        m_brain = std::make_unique<Brain>();
        PiglinBruteAi::InitBrain(*this, *m_brain);
    }

    void PiglinBrute::UpdateBrainActivity() { PiglinBruteAi::UpdateActivity(*this); }

    void PiglinBrute::PlayAmbientSound() {
        // MC AbstractPiglin.playAmbientSound: PiglinAi.isIdle gates it.
        if (const Brain* brain = GetBrain(); brain && brain->IsActive(Activity::Idle)) {
            GenericMonster::PlayAmbientSound();
        }
    }

    bool PiglinBrute::IsConverting() const {
        return !IsImmuneToZombification() && !IsNoAi() && PiglinsZombify(m_level);
    }

    std::shared_ptr<SpawnGroupData>
    PiglinBrute::FinalizeSpawn(SpawnReason reason,
                               std::shared_ptr<SpawnGroupData> groupData) {
        PiglinBruteAi::InitMemories(*this);
        if (m_level) PopulateDefaultEquipmentSlots(m_level->Random(), CurrentDifficulty());
        return GenericMonster::FinalizeSpawn(reason, std::move(groupData));
    }

    void PiglinBrute::PopulateDefaultEquipmentSlots(JavaRandom& random, const DifficultyInstance& difficulty) {
        (void)random; (void)difficulty;
        SetEquipment(EquipmentSlot::MAINHAND, ItemStack(Items::GoldenAxe, 1));
    }

    bool PiglinBrute::WantsToPickUp(const ItemStack& stack) const {
        return stack.itemId == Items::GoldenAxe && GenericMonster::WantsToPickUp(stack);
    }

    int PiglinBrute::GetPiglinArmPose() const {
        return IsAggressive() && IsHoldingMeleeWeapon(*this) ? 0 : 5;
    }

    bool PiglinBrute::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        const bool hurt = GenericMonster::Hurt(source, amount, attacker);
        if (hurt && m_level && !m_level->IsClientSide()) {
            if (auto* living = dynamic_cast<LivingEntity*>(attacker)) {
                PiglinBruteAi::WasHurtBy(*m_level, *this, *living);
            }
        }
        return hurt;
    }

    void PiglinBrute::CustomServerAiStep() {
        // MC AbstractPiglin.customServerAiStep — see Piglin::CustomServerAiStep.
        if (!m_level || m_level->IsClientSide()) return;
        // MC PiglinBrute.customServerAiStep → PiglinBruteAi.maybePlayActivitySound:
        // a 1.25% chance a tick of the angry snort while fighting.
        if (m_level->Random().NextFloat() < 0.0125f) {
            if (const Brain* brain = GetBrain(); brain && brain->GetActiveNonCoreActivity() == Activity::Fight) {
                MakeSound(SoundEvents::PIGLIN_BRUTE_ANGRY);
            }
        }
        m_timeInOverworld = IsConverting() ? m_timeInOverworld + 1 : 0;
        if (m_timeInOverworld > 300) {
            if (m_level->GetDifficulty() != Difficulty::Peaceful) {
                MakeSound(SoundEvents::PIGLIN_BRUTE_CONVERTED_TO_ZOMBIFIED);   // MC playConvertedSound
            }
            // MC AbstractPiglin.finishConversion: → ZOMBIFIED_PIGLIN with
            // ConversionParams.single(this, keepEquipment = true,
            // preserveCanPickUpLoot = true), whose afterConversion adds
            // NAUSEA 200 (after convertCommon copied the piglin's effects).
            auto zombified = std::make_unique<ZombifiedPiglin>(m_level);
            CopyConversionState(*zombified);
            MoveEquipmentTo(*zombified);
            zombified->SetBaby(IsBaby());
            zombified->AddEffect(MobEffectInstance(MobEffectId::Nausea, 200, 0));
            FinishConversion(std::move(zombified));
        }
    }

    // ── Axolotl ────────────────────────────────────────────────────────────

    namespace {

        // MC Axolotl.AxolotlMoveControl / AxolotlLookControl — the stock
        // smooth-swimming controls, frozen while playing dead.
        class AxolotlMoveControl : public SmoothSwimmingMoveControl {
        public:
            explicit AxolotlMoveControl(Axolotl* axolotl)
                : SmoothSwimmingMoveControl(axolotl, 85, 10, 0.1f, 0.5f, false),
                  m_axolotl(axolotl) {}
            void Tick() override {
                if (!m_axolotl->IsPlayingDead()) SmoothSwimmingMoveControl::Tick();
            }
        private:
            Axolotl* m_axolotl;
        };

        class AxolotlLookControl : public SmoothSwimmingLookControl {
        public:
            explicit AxolotlLookControl(Axolotl* axolotl)
                : SmoothSwimmingLookControl(axolotl, 20), m_axolotl(axolotl) {}
            void Tick() override {
                if (!m_axolotl->IsPlayingDead()) SmoothSwimmingLookControl::Tick();
            }
        private:
            Axolotl* m_axolotl;
        };

        // MC Axolotl.AxolotlGroupData — the pack token: two common variants
        // rolled once, and a member counter (AgeableMobGroupData's) that makes
        // the third axolotl onward spawn as a baby.
        struct AxolotlGroupData : SpawnGroupData {
            Axolotl::Variant types[2];
            int groupSize = 0;
        };

        // MC Axolotl.Variant.getCommonSpawnVariant — LUCY/WILD/GOLD/CYAN are
        // common; BLUE is the 1-in-1200 breeding rare.
        Axolotl::Variant CommonVariant(JavaRandom& rng) {
            return static_cast<Axolotl::Variant>(rng.NextInt(4));
        }

    } // namespace

    // MC EasingType.IN_OUT_SINE over the animator's 0..1 ramp.
    float Axolotl::BinaryAnimator::Factor(float partialTick) const {
        const float t = (static_cast<float>(ticksOld)
                         + (static_cast<float>(ticks) - static_cast<float>(ticksOld))
                               * partialTick)
                        / static_cast<float>(length);
        return -(std::cos(3.14159265358979f * t) - 1.0f) / 2.0f;
    }

    Axolotl::Axolotl(EntityLevel* level) : GenericAnimal(EntityTypeId::Axolotl, level) {
        // NO GOALS — MC's Axolotl never registers any; the brain is the whole
        // behaviour. The def's amphibious navigation stands (MC
        // createNavigation returns AmphibiousPathNavigation).
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        SetPathfindingMalus(PathType::Water, 0.0f);
        SetMoveControl(std::make_unique<AxolotlMoveControl>(this));
        SetLookControl(std::make_unique<AxolotlLookControl>(this));
        // The swim control owns the on-land slowdown (outsideWater 0.5), the
        // same division of labour ApplyLocomotion documents.
        SetLandSpeedFactor(1.0f);

        m_brain = std::make_unique<Brain>();
        AxolotlAi::InitBrain(*this, *m_brain);
    }

    void Axolotl::TickAnimations() {
        // MC Axolotl.tickAnimations — client-side only, one exclusive state a
        // tick, each animator easing toward its own state.
        enum class AnimState { PlayingDead, InWater, OnGround, InAir };
        AnimState s;
        if (IsPlayingDead())  s = AnimState::PlayingDead;
        else if (IsInWater()) s = AnimState::InWater;
        else if (onGround)    s = AnimState::OnGround;
        else                  s = AnimState::InAir;

        m_playingDeadAnimator.TickAnim(s == AnimState::PlayingDead);
        m_inWaterAnimator.TickAnim(s == AnimState::InWater);
        m_onGroundAnimator.TickAnim(s == AnimState::OnGround);
        const bool moving = walkAnimation.IsMoving()
                         || xRot != xRotO || yRot != yRotO;
        m_movingAnimator.TickAnim(moving);
    }

    void Axolotl::BaseTick() {
        // MC Axolotl.baseTick: capture the PRE-tick air (super's own block
        // would top a beached axolotl back up), run super, then apply the
        // axolotl's inverted air rule on the server.
        const int airSupply = GetAirSupply();
        GenericAnimal::BaseTick();

        if (m_level && !m_level->IsClientSide()) {
            // MC Axolotl.handleAirSupply: out of water AND rain
            // (isInWaterOrRain) the axolotl dries out; rain keeps it wet.
            if (IsAlive() && !IsInWaterOrRain()) {
                SetAirSupply(airSupply - 1);
                if (ShouldTakeDrowningDamage()) {
                    SetAirSupply(0);
                    // MC's dryOut damage source, 2.0 per tick once dry.
                    Hurt(MobDamageSource::Drown, 2.0f, nullptr);
                }
            } else {
                SetAirSupply(GetMaxAirSupply());
            }
        }

        if (m_level && m_level->IsClientSide()) {
            // MC 26.2: `if (isBaby()) tickBabyAnimations(); else
            // tickAdultAnimations();`. Both run here so the classic baby
            // mesh (the adult's animators) and the remodel (the keyframe
            // states) are each driven whichever look is drawn.
            TickAnimations();
            if (IsBaby()) TickBabyAnimations();
        }
    }

    void Axolotl::TickBabyAnimations() {
        // MC Axolotl.tickBabyAnimations + soloAnimation, verbatim: the
        // chosen state startIfStopped, every other one stopped.
        const bool inWater = IsInWater();
        const bool moving  = walkAnimation.IsMoving() || xRot != xRotO || yRot != yRotO;
        MobAnim solo;
        if (IsPlayingDead())          solo = MobAnim::PlayDead;
        else if (moving) {
            if (inWater && !onGround)      solo = MobAnim::Swim;
            else if (!inWater && onGround) solo = MobAnim::Walk;
            else                           solo = MobAnim::WalkUnderWater;
        }
        else if (inWater && !onGround) solo = MobAnim::IdleUnderWater;
        else if (inWater && onGround)  solo = MobAnim::IdleUnderWaterOnGround;
        else                           solo = MobAnim::IdleOnGround;

        static constexpr MobAnim kAll[] = {
            MobAnim::Swim, MobAnim::Walk, MobAnim::WalkUnderWater,
            MobAnim::IdleUnderWater, MobAnim::IdleUnderWaterOnGround,
            MobAnim::IdleOnGround, MobAnim::PlayDead,
        };
        for (MobAnim a : kAll) {
            if (a == solo) Anim(a).StartIfStopped(tickCount);
            else           Anim(a).Stop();
        }
    }

    void Axolotl::UpdateBrainActivity() {
        AxolotlAi::UpdateActivity(*this);
        // MC customServerAiStep's tail: mirror PLAY_DEAD_TICKS into the
        // synched playing-dead flag every tick.
        if (const Brain* brain = GetBrain()) {
            const std::optional<int> ticks = brain->GetInt(MemoryModule::PlayDeadTicks);
            SetPlayingDead(ticks.has_value() && *ticks > 0);
        }
    }

    bool Axolotl::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC Axolotl.hurtServer — the play-dead roll runs BEFORE the damage
        // lands, against the pre-hit health: 1-in-3, and only when the hit is
        // meaningful (damage beats a 0..2 roll, or already below half
        // health), non-lethal, from an entity, in water, and not already
        // playing dead.
        if (m_level && !m_level->IsClientSide()) {
            JavaRandom& rng = m_level->Random();
            const float health = GetHealth();
            if (rng.NextInt(3) == 0
                && (static_cast<float>(rng.NextInt(3)) < amount
                    || health / GetMaxHealth() < 0.5f)
                && amount < health && IsInWater() && attacker != nullptr
                && !IsPlayingDead()) {
                if (Brain* brain = GetBrain()) {
                    brain->SetMemory(MemoryModule::PlayDeadTicks, kTotalPlayDeadTime);
                }
            }
        }
        return GenericAnimal::Hurt(source, amount, attacker);
    }

    std::shared_ptr<SpawnGroupData>
    Axolotl::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC: `if (spawnReason == BUCKET) return groupData;` — no variant
        // roll, no baby, no super: the bucket says what it was.
        if (reason == SpawnReason::Bucket) return groupData;
        if (!m_level) return GenericAnimal::FinalizeSpawn(reason, std::move(groupData));
        JavaRandom& rng = m_level->Random();

        bool isBaby = false;
        auto data = std::dynamic_pointer_cast<AxolotlGroupData>(groupData);
        if (data) {
            // MC: the third pack member onward spawns as a baby.
            if (data->groupSize >= 2) isBaby = true;
        } else {
            data = std::make_shared<AxolotlGroupData>();
            data->types[0] = CommonVariant(rng);
            data->types[1] = CommonVariant(rng);
            groupData = data;
        }
        SetVariant(data->types[rng.NextInt(2)]);
        if (isBaby) SetAge(-24000);
        // MC AgeableMob.finalizeSpawn's member counter, run by super there.
        ++data->groupSize;

        return GenericAnimal::FinalizeSpawn(reason, std::move(groupData));
    }

    UseResult Axolotl::MobInteract(LivingEntity& player, ItemStack& held) {
        if (auto r = Bucketable::BucketMobPickup(*this, player, held, Items::AxolotlBucket,
                                                 SoundEvents::BUCKET_FILL_AXOLOTL,
                                                 [this](ItemStack& bucket) { SaveToBucket(bucket); })) {
            return *r;
        }
        // Animal.mobInteract feeds with #axolotl_food — the bucket of
        // tropical fish. MC Axolotl.usePlayerItem then hands back the water
        // (ItemUtils.createFilledResult(itemStack, player, WATER_BUCKET))
        // instead of eating the bucket: undo the plain shrink and refill.
        const ItemStack before = held;
        const UseResult result = GenericAnimal::MobInteract(player, held);
        const bool spent = held.itemId != before.itemId || held.count < before.count;
        if (before.itemId == Items::TropicalFishBucket && spent &&
            m_level && !m_level->IsClientSide()) {
            held = before;
            m_level->CreateFilledResult(player, held, ItemStack(Items::WaterBucket, 1));
        }
        return result;
    }

    void Axolotl::SaveToBucket(ItemStack& bucket) const {
        Bucketable::SaveDefaultDataToBucketTag(*this, bucket);
        // bucket.copyFrom(AXOLOTL_VARIANT, this).
        bucket.components.set(DataComponents::AXOLOTL_VARIANT, static_cast<int32_t>(m_variant));
        BucketEntityData data = bucket.components.get(DataComponents::BUCKET_ENTITY_DATA)
                                    .value_or(BucketEntityData{});
        data.age = GetAge();
        data.ageLocked = IsAgeLocked();
        // Brain HAS_HUNTING_COOLDOWN: its remaining TTL, when set.
        if (const Brain* brain = GetBrain(); brain && brain->HasMemoryValue(MemoryModule::HasHuntingCooldown)) {
            data.huntingCooldown = brain->GetTimeUntilExpiry(MemoryModule::HasHuntingCooldown);
        }
        bucket.components.set(DataComponents::BUCKET_ENTITY_DATA, data);
    }

    void Axolotl::LoadFromBucket(const BucketEntityData& data) {
        Bucketable::LoadDefaultDataFromBucketTag(*this, data);
        SetAge(data.age.value_or(0));
        SetAgeLocked(data.ageLocked.value_or(false));
        if (m_brain) {
            if (data.huntingCooldown) {
                m_brain->SetMemoryWithExpiry(MemoryModule::HasHuntingCooldown, true, *data.huntingCooldown);
            } else {
                m_brain->EraseMemory(MemoryModule::HasHuntingCooldown);
            }
        }
    }

    void Axolotl::SpawnChildFromBreeding(Animal& partner) {
        // Stash the partner for CreateBaby's variant coin flip — MC's
        // getBreedOffspring receives the partner directly.
        m_breedPartner = &partner;
        GenericAnimal::SpawnChildFromBreeding(partner);
        m_breedPartner = nullptr;
    }

    std::unique_ptr<Animal> Axolotl::CreateBaby() {
        auto baby = std::make_unique<Axolotl>(m_level);
        // MC getBreedOffspring: 1-in-1200 the rare variant (blue is the only
        // one), else a coin flip between the two parents.
        Variant v = GetVariant();
        if (m_level) {
            JavaRandom& rng = m_level->Random();
            if (rng.NextInt(1200) == 0) {
                v = Variant::Blue;
            } else if (!rng.NextBool()) {
                // MC: nextBoolean ? own variant : partner's.
                if (auto* p = dynamic_cast<Axolotl*>(m_breedPartner)) {
                    v = p->GetVariant();
                }
            }
        }
        baby->SetVariant(v);
        return baby;
    }

    void Axolotl::OnStopAttacking(EntityLevel& level, Axolotl& axolotl,
                                  LivingEntity& target) {
        // MC Axolotl.onStopAttacking: if the target died and the killing blow
        // traced to a player within 20 blocks, buff that player. MC reads the
        // target's lastDamageSource entity; the port's equivalent record is
        // lastHurtByMob.
        if (!target.IsDeadOrDying()) return;
        auto* player = dynamic_cast<LivingEntity*>(target.GetLastHurtByMob());
        if (!player || !player->IsPlayer()) return;

        std::vector<LivingEntity*> players;
        level.GetPlayers(players);
        for (LivingEntity* p : players) {
            if (p == player
                && axolotl.DistanceToSqr(*p) <= 20.0 * 20.0) {
                axolotl.ApplySupportingEffects(*p);
                return;
            }
        }
    }

    void Axolotl::ApplySupportingEffects(LivingEntity& player) {
        // MC Axolotl.applySupportingEffects: top regeneration up by 100 ticks
        // to a 2400-tick cap (skipping only a longer-running one), and clear
        // mining fatigue.
        const MobEffectInstance* regen = player.GetEffect(MobEffectId::Regeneration);
        // MC endsWithin(2399): a finite regen with 2399 ticks or fewer left.
        if (!regen || (regen->duration >= 0 && regen->duration <= 2399)) {
            const int previous = regen ? regen->duration : 0;
            const int duration = std::min(2400, 100 + previous);
            player.AddEffect(MobEffectInstance(MobEffectId::Regeneration, duration, 0),
                             this);
        }
        player.RemoveEffect(MobEffectId::MiningFatigue);
    }

    // ── Bee ────────────────────────────────────────────────────────────────

    Bee::Bee(EntityLevel* level)
        : GenericAnimal(EntityTypeId::Bee, level), NeutralMob(this) {
        // The def-driven goal set (flying stroll, float, look goals, tempt +
        // breed from BEE_FOOD) is registered by the base constructor. The
        // hive/flower/crop goals wait on their systems (see the header); the
        // combat trio is MC's own, priority for priority
        // (Bee.registerGoals):
        m_goalSelector.AddGoal(0, std::make_unique<BeeAttackGoal>(this, 1.4, true));
        auto hurtBy = std::make_unique<BeeHurtByOtherGoal>(this);
        hurtBy->SetAlertOthers();
        m_targetSelector.AddGoal(1, std::move(hurtBy));
        m_targetSelector.AddGoal(2, std::make_unique<BeeBecomeAngryTargetGoal>(this));
        m_targetSelector.AddGoal(3, std::make_unique<ResetUniversalAngerTargetGoal>(
                                        this, /*alertOthersOfSameType=*/true));

        // The flower/crop layer (Bee.registerGoals priorities; the hive goals
        // stay with the hive block-entity system — see BeeGoals.hpp). The
        // shared flower state carries MC's ctor cooldown roll.
        m_flowerState = std::make_shared<BeeFlowerState>();
        if (m_level) {
            m_flowerState->remainingCooldownBeforeLocatingNewFlower =
                m_level->Random().NextInt(20, 60);
        }
        // The hive cycle (BeehiveBlockEntity) at MC's priorities, in MC's
        // registration order.
        m_goalSelector.AddGoal(1, std::make_unique<BeeEnterHiveGoal>(this, m_flowerState));
        m_goalSelector.AddGoal(3, std::make_unique<ValidateHiveGoal>(this, m_flowerState));
        m_goalSelector.AddGoal(3, std::make_unique<ValidateFlowerGoal>(this, m_flowerState));
        auto pollinate = std::make_unique<BeePollinateGoal>(this, m_flowerState);
        m_pollinateGoal = pollinate.get();
        m_goalSelector.AddGoal(4, std::move(pollinate));
        m_goalSelector.AddGoal(5, std::make_unique<BeeLocateHiveGoal>(this, m_flowerState));
        auto goToHive = std::make_unique<BeeGoToHiveGoal>(this, m_flowerState);
        m_goToHiveGoal = goToHive.get();
        m_goalSelector.AddGoal(5, std::move(goToHive));
        m_goalSelector.AddGoal(6, std::make_unique<BeeGoToKnownFlowerGoal>(this, m_flowerState));
        m_goalSelector.AddGoal(7, std::make_unique<BeeGrowCropGoal>(this, m_flowerState));
        m_goalSelector.AddGoal(8, std::make_unique<BeeWanderGoal>(this, m_flowerState));
    }

    // ── Bee hive cycle ─────────────────────────────────────────────────────

    bool Bee::WantsToEnterHive() const {
        // MC wantsToEnterHive: not kept out, not pollinating, not stung, no
        // target; then nectar, tired of looking (3600 ticks), or
        // BEES_STAY_IN_HIVE — and never into a hive by a fire.
        if (m_stayOutOfHiveCountdown > 0) return false;
        if (m_pollinateGoal && m_pollinateGoal->IsPollinating()) return false;
        if (m_hasStung || GetTarget() != nullptr) return false;
        bool wants = HasNectar() || GetTicksWithoutNectar() > 3600;
        if (!wants && m_level) {
            if (ILevelWrite* write = m_level->MutableBlocks()) {
                wants = BeesStayInHive(*write, BlockPosition());
            }
        }
        if (!wants) return false;
        BeehiveBlockEntity* hive = GetBeehive();
        if (hive && m_level) {
            if (ILevelWrite* write = m_level->MutableBlocks(); write && hive->IsFireNearby(*write)) return false;
        }
        return true;
    }

    bool Bee::CloserThan(const glm::ivec3& pos, int distance) const {
        // MC Vec3i.closerThan: distSqr between block positions < d².
        const glm::dvec3 d = glm::dvec3(pos - BlockPosition());
        return glm::dot(d, d) < static_cast<double>(distance) * distance;
    }

    BeehiveBlockEntity* Bee::GetBeehive() const {
        if (!m_hivePos || IsTooFarAway(*m_hivePos) || !m_level) return nullptr;
        ILevelWrite* write = m_level->MutableBlocks();
        if (!write) return nullptr;
        return dynamic_cast<BeehiveBlockEntity*>(write->GetBlockEntity(*m_hivePos));
    }

    bool Bee::DoesHiveHaveSpace(const glm::ivec3& pos) const {
        ILevelWrite* write = m_level ? m_level->MutableBlocks() : nullptr;
        auto* hive = write ? dynamic_cast<BeehiveBlockEntity*>(write->GetBlockEntity(pos)) : nullptr;
        return hive && !hive->IsFull();
    }

    void Bee::DropOffNectar() {
        SetHasNectar(false);
        SetCropsGrownSincePollination(0);
    }

    void Bee::PathfindRandomlyTowards(const glm::ivec3& target) {
        // MC pathfindRandomlyTowards: a hop of 6/8 (half the Manhattan
        // distance under 15), lifted ±4 toward a target more than 2 above or
        // below, within 0.314 rad of the target's direction — an air cell
        // that is not water (AirRandomPos.getPosTowards).
        const glm::dvec3 targetVec(target.x + 0.5, static_cast<double>(target.y), target.z + 0.5);
        const glm::ivec3 beePos = BlockPosition();
        int yAdjust = 0;
        const int yDelta = static_cast<int>(targetVec.y) - beePos.y;
        if (yDelta > 2) yAdjust = 4;
        else if (yDelta < -2) yAdjust = -4;
        int xzDist = 6;
        int yDist = 8;
        const int dist = std::abs(beePos.x - target.x) + std::abs(beePos.y - target.y) + std::abs(beePos.z - target.z);
        if (dist < 15) {
            xzDist = dist / 2;
            yDist = dist / 2;
        }
        const glm::dvec3 dir = targetVec - position;
        std::optional<glm::dvec3> next = RandomPos::GetAirAndWaterPos(*this, xzDist, yDist, yAdjust, dir.x, dir.z,
                                                                      0.3141592741012573);
        if (!next) return;
        if (const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr) {
            const glm::ivec3 cell(static_cast<int>(std::floor(next->x)), static_cast<int>(std::floor(next->y)),
                                  static_cast<int>(std::floor(next->z)));
            const BlockID id = blocks->GetBlock(cell.x, cell.y, cell.z);
            if (id == BlockID::Water) return;
        }
        GetNavigation().MoveTo(next->x, next->y, next->z, 1.0);
    }

    uint8_t Bee::GetAnimStateByte() const {
        return static_cast<uint8_t>(
            (m_rolling ? 1 : 0) | (m_hasStung ? 2 : 0) |
            (((m_flowerState && m_flowerState->hasNectar) ||
              m_clientHasNectar) ? 4 : 0));
    }

    void Bee::SetAnimStateByte(uint8_t v) {
        m_rolling = (v & 1) != 0;
        m_hasStung = (v & 2) != 0;
        m_clientHasNectar = (v & 4) != 0;
    }

    // ── Bee save/load seam ─────────────────────────────────────────────────
    //
    // m_flowerState is shared with the goals and is null until RegisterGoals
    // runs, so every one of these tolerates its absence rather than asserting:
    // the load path can reach a bee whose goals have not been built yet.

    bool Bee::HasNectar() const {
        // The client's flower state is never written (the goals run on the
        // server); its nectar bit arrives on the anim byte — so either.
        return (m_flowerState && m_flowerState->hasNectar) || m_clientHasNectar;
    }

    void Bee::SetHasNectar(bool v) {
        if (m_flowerState) m_flowerState->hasNectar = v;
        m_clientHasNectar = v;
    }

    bool Bee::HasSavedFlowerPos() const {
        return m_flowerState && m_flowerState->hasSavedFlowerPos;
    }

    glm::ivec3 Bee::GetSavedFlowerPos() const {
        return m_flowerState ? m_flowerState->savedFlowerPos : glm::ivec3(0);
    }

    void Bee::SetSavedFlowerPos(const glm::ivec3& pos) {
        if (!m_flowerState) return;
        m_flowerState->savedFlowerPos = pos;
        m_flowerState->hasSavedFlowerPos = true;
    }

    int Bee::GetTicksWithoutNectar() const {
        return m_flowerState ? m_flowerState->ticksWithoutNectarSinceExitingHive : 0;
    }

    void Bee::SetTicksWithoutNectar(int ticks) {
        if (m_flowerState) m_flowerState->ticksWithoutNectarSinceExitingHive = ticks;
    }

    int Bee::GetCropsGrownSincePollination() const {
        return m_flowerState ? m_flowerState->numCropsGrownSincePollination : 0;
    }

    void Bee::SetCropsGrownSincePollination(int n) {
        if (m_flowerState) m_flowerState->numCropsGrownSincePollination = n;
    }

    void Bee::StartPersistentAngerTimer() {
        // MC PERSISTENT_ANGER_TIME = TimeUtil.rangeOfSeconds(20, 39).
        if (!m_level) return;
        SetTimeToRemainAngry(400 + m_level->Random().NextInt(381));
    }

    void Bee::Tick() {
        // MC Bee.tick: super, the nectar drips (a pollinated bee that has
        // grown fewer than 10 crops drips 1-2 FALLING_NECTAR 5% of ticks —
        // the client copy draws them), then updateRollAmount on BOTH sides.
        GenericAnimal::Tick();
        if (m_level && HasNectar() && GetCropsGrownSincePollination() < 10 && m_level->Random().NextFloat() < 0.05f) {
            JavaRandom& r = m_level->Random();
            const int n = r.NextInt(2) + 1;
            const double y = position.y + static_cast<double>(GetBbHeight()) * 0.5;
            for (int i = 0; i < n; ++i) {
                const double x = Mth::Lerp(r.NextDouble(), position.x - 0.30000001192092896, position.x + 0.30000001192092896);
                const double z = Mth::Lerp(r.NextDouble(), position.z - 0.30000001192092896, position.z + 0.30000001192092896);
                m_level->AddParticle(ParticleKind::FallingNectar, x, y, z, 0.0, 0.0, 0.0);
            }
        }
        UpdateRollAmount();
    }

    bool Bee::DoHurtTarget(Entity& target) {
        // MC Bee.doHurtTarget. The base call covers hurtServer + knockback
        // (MC hurts directly with the sting damage source; same numbers —
        // ATTACK_DAMAGE, zero attack knockback). setStingerCount on the
        // target is render-side stinger decals — nothing consumes it here.
        const bool wasHurt = GenericAnimal::DoHurtTarget(target);
        if (wasHurt) {
            if (auto* living = dynamic_cast<LivingEntity*>(&target)) {
                // MC: POISON_SECONDS_NORMAL 10 / POISON_SECONDS_HARD 18,
                // nothing on EASY.
                int poisonSeconds = 0;
                if (m_level) {
                    if (m_level->GetDifficulty() == Difficulty::Normal) poisonSeconds = 10;
                    else if (m_level->GetDifficulty() == Difficulty::Hard) poisonSeconds = 18;
                }
                if (poisonSeconds > 0) {
                    living->AddEffect(
                        MobEffectInstance(MobEffectId::Poison, poisonSeconds * 20, 0),
                        this);
                }
            }
            // MC: setHasStung(true), stopBeingAngry(), BEE_STING.
            m_hasStung = true;
            StopBeingAngry();
            PlaySound(SoundEvents::BEE_STING, 1.0f, 1.0f);
        }
        return wasHurt;
    }

    void Bee::AiStep() {
        // MC Bee.aiStep's server half, minus the hive/flower cooldowns that
        // belong to the skipped goals. shouldRoll is MC's exact condition
        // now that anger exists.
        GenericAnimal::AiStep();
        if (m_level && !m_level->IsClientSide()) {
            LivingEntity* target = GetTarget();
            const bool shouldRoll = IsAngry() && !m_hasStung &&
                target != nullptr && target->DistanceToSqr(*this) < 4.0;
            m_rolling = shouldRoll;

            // MC Bee.aiStep: the hive and new-flower cooldowns tick down
            // here, and every 20 ticks an invalid hive is forgotten.
            if (m_stayOutOfHiveCountdown > 0) --m_stayOutOfHiveCountdown;
            if (m_remainingCooldownBeforeLocatingNewHive > 0) --m_remainingCooldownBeforeLocatingNewHive;
            if (m_flowerState &&
                m_flowerState->remainingCooldownBeforeLocatingNewFlower > 0) {
                --m_flowerState->remainingCooldownBeforeLocatingNewFlower;
            }
            if (tickCount % 20 == 0 && !IsHiveValid()) m_hivePos.reset();
        }
    }

    void Bee::CustomServerAiStep() {
        GenericAnimal::CustomServerAiStep();

        // MC Bee.customServerAiStep: the ticks-without-nectar clock that
        // BeeWanderGoal reads.
        if (m_flowerState && !m_flowerState->hasNectar) {
            ++m_flowerState->ticksWithoutNectarSinceExitingHive;
        }

        // The bee's own drown rule: 20 ticks fully in water, then 1.0 drown
        // damage every tick — much faster than the generic air supply (which
        // also runs, harmlessly behind this).
        if (IsInWater()) {
            ++m_underWaterTicks;
        } else {
            m_underWaterTicks = 0;
        }
        if (m_underWaterTicks > 20) {
            Hurt(MobDamageSource::Drown, 1.0f, nullptr);
        }

        // A stung bee dies within STING_DEATH_COUNTDOWN (1200) ticks — every
        // 5th tick rolls nextInt(clamp(1200 - t, 1, 1200)) == 0, so the odds
        // rise as the countdown shrinks and death is certain by the end.
        if (m_hasStung) {
            ++m_timeSinceSting;
            if (m_timeSinceSting % 5 == 0 &&
                m_level->Random().NextInt(
                    std::clamp(1200 - m_timeSinceSting, 1, 1200)) == 0) {
                Hurt(MobDamageSource::Generic, GetHealth(), nullptr);
            }
        }

        // MC: false — a bee's anger runs out even while it still has a
        // target; the grudge is the timer, nothing else.
        UpdatePersistentAnger(/*stayAngryIfTargetPresent=*/false);
    }

    void Bee::UpdateRollAmount() {
        // MC Bee.updateRollAmount, constants verbatim.
        m_rollAmountO = m_rollAmount;
        if (IsRolling()) {
            m_rollAmount = std::min(1.0f, m_rollAmount + 0.2f);
        } else {
            m_rollAmount = std::max(0.0f, m_rollAmount - 0.24f);
        }
    }

    float Bee::GetRollAmount(float partialTick) const {
        // MC Bee.getRollAmount — the plain lerp.
        return m_rollAmountO + partialTick * (m_rollAmount - m_rollAmountO);
    }

    // ══ Breeze ═════════════════════════════════════════════════════════════

    Breeze::Breeze(EntityLevel* level) : GenericMonster(EntityTypeId::Breeze, level) {
        // NO GOALS — MC's Breeze is all brain.
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        // MC Breeze's constructor maluses.
        SetPathfindingMalus(PathType::DangerTrapdoor, -1.0f);
        SetPathfindingMalus(PathType::DamageFire, -1.0f);

        m_brain = std::make_unique<Brain>();
        BreezeAi::InitBrain(*this, *m_brain);
    }

    void Breeze::UpdateBrainActivity() { BreezeAi::UpdateActivity(*this); }

    void Breeze::PlayAmbientSound() {
        // MC Breeze.playAmbientSound: getTarget() == null || !onGround().
        if ((GetTarget() == nullptr || !onGround) && m_level) {
            m_level->PlayLocalSoundFromEntity(*this, GetAmbientSound(), GetSoundSource(), 1.0f, 1.0f);
        }
    }

    bool Breeze::CauseFallDamage(double fallDist, float damageMultiplier) {
        if (fallDist > 3.0) PlaySound(SoundEvents::BREEZE_LAND, 1.0f, 1.0f);
        return GenericMonster::CauseFallDamage(fallDist, damageMultiplier);
    }

    bool Breeze::CanAttack(const LivingEntity& target) const {
        // MC Breeze.canAttack — players and iron golems, nothing else. The
        // base adds MC's alive/attackable gate, which its callers apply.
        return (target.IsPlayer() || target.GetType() == EntityTypeId::IronGolem)
            && Mob::CanAttack(target);
    }

    bool Breeze::WithinInnerCircleRange(const glm::dvec3& target) const {
        // MC target.closerThan(blockPosition().getCenter(), 4.0, 10.0) — XZ
        // and Y tested separately.
        const glm::ivec3 bp = BlockPosition();
        const double dx = target.x - (bp.x + 0.5);
        const double dy = target.y - (bp.y + 0.5);
        const double dz = target.z - (bp.z + 0.5);
        return dx * dx + dz * dz < 4.0 * 4.0 && std::abs(dy) < 10.0;
    }

    LivingEntity* Breeze::GetHurtBy() const {
        // MC Breeze.getHurtBy reads HURT_BY's DamageSource entity; this port's
        // HurtBySensor parks the attacker in HURT_BY_ENTITY instead.
        const Brain* brain = GetBrain();
        return brain ? dynamic_cast<LivingEntity*>(
                           brain->GetEntity(MemoryModule::HurtByEntity))
                     : nullptr;
    }

    void Breeze::Tick() {
        // MC Breeze.tick runs this before super.tick(): the per-pose ground
        // and jump-trail dust, then the animation states. Timers are client
        // state and addParticle draws only there, so the block is
        // client-gated — MC runs it on both sides but only the client reads
        // the states.
        if (m_level && m_level->IsClientSide()) {
            const Pose pose = GetPose();
            if (pose == Pose::Shooting || pose == Pose::Inhaling || pose == Pose::Standing) {
                m_jumpTrailStartedTick = 0;
                EmitGroundParticles(1 + m_level->Random().NextInt(1));
            } else if (pose == Pose::Sliding) {
                EmitGroundParticles(20);
            } else if (pose == Pose::LongJumping) {
                Anim(MobAnim::LongJump).StartIfStopped(tickCount);
                EmitJumpTrailParticles();
            }
            Anim(MobAnim::Idle).StartIfStopped(tickCount);
            // MC: leaving SLIDING plays slideBack from the top — the little
            // recover shuffle after every slide.
            if (pose != Pose::Sliding && Anim(MobAnim::Slide).IsStarted()) {
                Anim(MobAnim::SlideBack).Start(tickCount);
                Anim(MobAnim::Slide).Stop();
            }
        }
        // MC Breeze.tick: the whirl, on a 1..80-tick timer — a local sound, so
        // only a client hears it (the server's playLocalSound is a no-op).
        if (m_level) {
            m_soundTick = m_soundTick == 0 ? m_level->Random().NextInt(1, 80) : m_soundTick - 1;
            if (m_soundTick == 0) {
                JavaRandom& rng = m_level->Random();
                const float pitch = 0.7f + 0.4f * rng.NextFloat();
                const float volume = 0.8f + 0.2f * rng.NextFloat();
                m_level->PlayLocalSoundFromEntity(*this, SoundEvents::BREEZE_WHIRL, GetSoundSource(), volume, pitch);
            }
        }
        GenericMonster::Tick();
    }

    BlockState Breeze::GroundStateForParticles() const {
        // !getInBlockState().isAir() ? getInBlockState() : getBlockStateOn().
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (!blocks) return BlockState();
        const glm::ivec3 in = BlockPosition();
        const BlockState inState = blocks->GetBlockState(in.x, in.y, in.z);
        if (inState.Block() != BlockID::Air) return inState;
        return blocks->GetBlockState(in.x, static_cast<int>(std::floor(position.y - 1.0e-5)), in.z);
    }

    namespace {
        bool InvisibleRenderShape(BlockID id) {
            return id == BlockID::Air || id == BlockID::Barrier || id == BlockID::Light ||
                   id == BlockID::StructureVoid || id == BlockID::MovingPiston || id == BlockID::Water ||
                   id == BlockID::Lava;
        }
    }

    void Breeze::EmitGroundParticles(int amount) {
        if (!m_level || GetVehicle()) return;
        const BlockState ground = GroundStateForParticles();
        if (InvisibleRenderShape(ground.Block())) return;
        for (int i = 0; i < amount; ++i) {
            m_level->AddParticle(ParticleOptions::Block(ground), position.x, position.y, position.z, 0.0, 0.0, 0.0);
        }
    }

    void Breeze::EmitJumpTrailParticles() {
        if (!m_level || ++m_jumpTrailStartedTick > 5) return;
        const BlockState ground = GroundStateForParticles();
        const glm::dvec3 c = position + velocity + glm::dvec3(0.0, 0.10000000149011612, 0.0);
        for (int i = 0; i < 3; ++i) {
            m_level->AddParticle(ParticleOptions::Block(ground), c.x, c.y, c.z, 0.0, 0.0, 0.0);
        }
    }

    void Breeze::ResetAnimations() {
        // MC Breeze.resetAnimations. Slide is deliberately NOT here — the
        // slide→slideBack transition in Tick has to see it still running.
        Anim(MobAnim::Shoot).Stop();
        Anim(MobAnim::Idle).Stop();
        Anim(MobAnim::Inhale).Stop();
        Anim(MobAnim::LongJump).Stop();
    }

    void Breeze::OnPoseUpdated() {
        // MC Breeze.onSyncedDataUpdated's DATA_POSE branch — client only.
        if (!m_level || !m_level->IsClientSide()) return;
        ResetAnimations();
        switch (GetPose()) {
            case Pose::Shooting: Anim(MobAnim::Shoot).StartIfStopped(tickCount); break;
            case Pose::Inhaling: Anim(MobAnim::Inhale).StartIfStopped(tickCount); break;
            case Pose::Sliding:  Anim(MobAnim::Slide).StartIfStopped(tickCount); break;
            default: break;
        }
    }

    void Breeze::ShootWindCharge(double xd, double yd, double zd, float inaccuracy) {
        if (!m_level || m_level->IsClientSide()) return;
        // MC Shoot.tick: BreezeWindCharge(breeze, level) — spawned at
        // (x, getFiringYPosition(), z) — then
        // Projectile.spawnProjectileUsingShoot(..., 0.7F, inaccuracy), which
        // is exactly Projectile::Shoot's normalize + triangle jitter + scale.
        auto charge = std::make_unique<BreezeWindCharge>(m_level);
        charge->SetOwner(this);
        charge->position = glm::dvec3(position.x, GetFiringYPosition(), position.z);
        charge->Shoot(xd, yd, zd, 0.7f, inaccuracy);
        m_level->AddFreshEntity(std::move(charge));
    }

    // ══ Warden ═════════════════════════════════════════════════════════════

    namespace {
        // MC's shared "this mob just landed a melee hit" entity event. Warden
        // and creaking both use it to start their attack animation on every
        // client that can see them.
        constexpr uint8_t kEventMobAttack = 4;
        // MC Warden's own events: 61 pulses the tendrils (a model hook the
        // generated warden model does not have), 62 starts the sonic boom.
        constexpr uint8_t kEventWardenTendrils = 61;
        constexpr uint8_t kEventWardenSonicBoom = 62;
        // MC Creaking's invulnerability shimmer.
        constexpr uint8_t kEventCreakingInvulnerable = 66;
    }

    bool Warden::CheckSpawnObstruction(EntityLevel& level) const {
        // MC Warden.checkSpawnObstruction: super && level.noCollision(this,
        // getType().getDimensions().makeBoundingBox(position())) — the TYPE's
        // box (not the current pose's), feet at the position.
        if (!Mob::CheckSpawnObstruction(level)) return false;
        const EntityTypeInfo& info = GetEntityTypeInfo(GetType());
        const double half = static_cast<double>(info.width) * 0.5;
        AABBd box;
        box.min = glm::dvec3(position.x - half, position.y, position.z - half);
        box.max = glm::dvec3(position.x + half, position.y + info.height, position.z + half);
        return !CollidesAt(box, level.Physics());
    }

    Warden::Warden(EntityLevel* level, EntityTypeId type)
        : GenericMonster(type, level),
          m_vibrationUser(*this),
          m_vibrationListener(*this),
          m_dynamicGameEventListener(m_vibrationListener) {
        // NO GOALS — MC's Warden is all brain.
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        // MC Warden's constructor maluses — it wades lava and walks fire.
        SetPathfindingMalus(PathType::UnpassableRail, 0.0f);
        SetPathfindingMalus(PathType::DamageOther, 8.0f);
        SetPathfindingMalus(PathType::PowderSnow, 8.0f);
        SetPathfindingMalus(PathType::Lava, 8.0f);
        SetPathfindingMalus(PathType::DamageFire, 0.0f);
        SetPathfindingMalus(PathType::DangerFire, 0.0f);

        m_brain = std::make_unique<Brain>();
        WardenAi::InitBrain(*this, *m_brain);
    }

    bool Warden::IsDiggingOrEmerging() const {
        return GetPose() == Pose::Digging || GetPose() == Pose::Emerging;
    }

    bool Warden::CanTargetEntity(const Entity* entity) const {
        // MC Warden.canTargetEntity: living, not another warden, not
        // creative/spectator, vulnerable and alive. (Armor stands and the
        // world border have no equivalents.)
        auto* living = dynamic_cast<const LivingEntity*>(entity);
        if (!living || living == this) return false;
        // MC `entity instanceof Warden` — the class, so the Silent Warden
        // and the vanilla warden leave each other alone.
        if (dynamic_cast<const Warden*>(living)) return false;
        if (living->IsCreative() || living->IsSpectator()) return false;
        return living->IsAttackable() && living->IsAlive();
    }

    std::shared_ptr<SpawnGroupData>
    Warden::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        if (Brain* brain = GetBrain()) {
            brain->SetMemoryWithExpiry(MemoryModule::DigCooldown, std::monostate{},
                                       WardenAi::kDiggingCooldown);
        }
        // MC Warden.finalizeSpawn: only EntitySpawnReason.TRIGGERED — a
        // sculk shrieker's summon, or here the echo core's (PlayerSession's
        // break path) — spawns the warden EMERGING: the pose starts the clip
        // client-side (OnPoseUpdated), the IS_EMERGING memory selects the
        // EMERGE activity for kEmergeDuration ticks, and WARDEN_AGITATED
        // plays at volume 5. Every other reason (/summon, a spawn egg)
        // starts above ground.
        if (reason == SpawnReason::Triggered) {
            SetPose(Pose::Emerging);
            if (Brain* brain = GetBrain()) {
                brain->SetMemoryWithExpiry(MemoryModule::IsEmerging, std::monostate{},
                                           WardenAi::kEmergeDuration);
            }
            PlaySound(SoundEvents::WARDEN_AGITATED, 5.0f, 1.0f);
        }
        return GenericMonster::FinalizeSpawn(reason, std::move(groupData));
    }

    void Warden::Tick() {
        // MC Warden.tick, server half: the VibrationSystem ticker (a heard
        // vibration flies to it and lands), then a persistent warden never
        // digs away (IsDespawnPersistent: a named one neither — digging away
        // is its despawn).
        World* serverWorld = nullptr;
        if (m_level && !m_level->IsClientSide()) {
            serverWorld = dynamic_cast<World*>(m_level->MutableBlocks());
            if (serverWorld) VibrationTicker::Tick(*serverWorld, m_vibrationData, m_vibrationUser);
            if (IsDespawnPersistent()) WardenAi::SetDigCooldown(*this);
        }
        GenericMonster::Tick();
        // MC ServerLevel's entity section callbacks (updateDynamicGameEvent-
        // Listener add / move / remove): the listener follows the warden
        // into whatever section — or level — it now stands in.
        if (serverWorld) {
            if (IsRemoved()) m_dynamicGameEventListener.Remove();
            else m_dynamicGameEventListener.Move(serverWorld->GameEvents());
        }
        // MC Warden.tick, client half: the heartbeat, every
        // getHeartBeatDelay() ticks — 40 calm, down to 10 at full anger —
        // as a local sound (5.0, the voice pitch) unless silent, and the
        // digging / emerging chips. (The tendril/heart animation counters
        // are model-layer work the generated warden has no hooks for.)
        if (m_level && m_level->IsClientSide()) {
            const float f = static_cast<float>(GetActiveAnger()) / static_cast<float>(kAngerAngry);
            const int delay = 40 - static_cast<int>(std::floor(std::clamp(f, 0.0f, 1.0f) * 30.0f));
            if (delay > 0 && tickCount % delay == 0 && !IsSilent()) {
                m_level->PlayLocalSound(position, SoundEvents::WARDEN_HEARTBEAT, GetSoundSource(),
                                        5.0f, GetVoicePitch(), false);
            }
            // MC clientDiggingParticles: while emerging / digging (the first
            // 4.5 s of the clip), 30 BLOCK chips of the block underfoot a tick.
            const Pose pose = GetPose();
            if (pose == Pose::Emerging || pose == Pose::Digging) {
                const AnimationState& clip = Anim(pose == Pose::Emerging ? MobAnim::Emerge : MobAnim::Digging);
                if (clip.IsStarted() && clip.ElapsedSeconds(static_cast<float>(tickCount)) < 4.5f) {
                    if (const IBlockAccess* blocks = m_level->Blocks()) {
                        const BlockState below = blocks->GetBlockState(
                            static_cast<int>(std::floor(position.x)),
                            static_cast<int>(std::floor(position.y - 1.0e-5)),
                            static_cast<int>(std::floor(position.z)));
                        if (!InvisibleRenderShape(below.Block())) {
                            JavaRandom& r = m_level->Random();
                            const ParticleOptions chip = ParticleOptions::Block(below);
                            for (int i = 0; i < 30; ++i) {
                                const double xx = position.x + (r.NextFloat() * 1.4f - 0.7f);
                                const double zz = position.z + (r.NextFloat() * 1.4f - 0.7f);
                                m_level->AddParticle(chip, xx, position.y, zz, 0.0, 0.0, 0.0);
                            }
                        }
                    }
                }
            }
        }
    }

    const char* Warden::GetAmbientSound() const {
        // MC Warden.getAmbientSound: silent while roaring, digging or
        // emerging; otherwise the AngerLevel's voice (CALM ambient,
        // AGITATED, ANGRY).
        if (GetPose() == Pose::Roaring || IsDiggingOrEmerging()) return "";
        const int anger = GetActiveAnger();
        if (anger >= kAngerAngry)    return SoundEvents::WARDEN_ANGRY;
        if (anger >= kAngerAgitated) return SoundEvents::WARDEN_AGITATED;
        return SoundEvents::WARDEN_AMBIENT;
    }

    void Warden::UpdateBrainActivity() {
        // MC Warden.customServerAiStep's order around the brain tick: the
        // darkness pulse, anger decays every 20 ticks, THEN the activity
        // switch reads the result.
        //
        // MC Warden.applyDarknessAround(level, position, this, 20): every 120
        // ticks (staggered by entity id), DARKNESS 260 ticks, no particles,
        // on every survival player within 20 blocks that does not already
        // have it for more than 199 more ticks (MobEffectUtil
        // .addEffectToPlayersAround's display limit of 200).
        if (m_level && !m_level->IsClientSide() && (tickCount + GetId()) % 120 == 0) {
            AddEffectToPlayersAround(*m_level, this, position, 20.0,
                                     MobEffectInstance(MobEffectId::Darkness, 260, 0,
                                                       /*ambient=*/false, /*visible=*/false),
                                     200);
        }
        if (tickCount % 20 == 0) TickAngerManagement();
        WardenAi::UpdateActivity(*this);
    }

    void Warden::TickAngerManagement() {
        // MC AngerManagement.tick: every suspect loses 1 anger per second and
        // drops off at ≤1, on death, or on becoming untargetable. The
        // UUID-persistence half is gone with mob saving.
        for (auto it = m_anger.begin(); it != m_anger.end();) {
            if (it->anger > 1 && !it->entity->IsRemoved()
                && CanTargetEntity(it->entity)) {
                --(it->anger);
                ++it;
            } else {
                it = m_anger.erase(it);
            }
        }
        SortAnger();
    }

    void Warden::SortAnger() {
        // MC AngerManagement.Sorter: angry suspects first, then players, then
        // by anger — which is why a warden mid-rampage swings to whichever
        // PLAYER made it angry rather than the zombie that shoved it.
        std::stable_sort(m_anger.begin(), m_anger.end(),
                         [](const AngerEntry& a, const AngerEntry& b) {
                             const bool angryA = a.anger >= kAngerAngry;
                             const bool angryB = b.anger >= kAngerAngry;
                             if (angryA != angryB) return angryA;
                             const bool playerA = a.entity->IsPlayer();
                             const bool playerB = b.entity->IsPlayer();
                             if (playerA != playerB) return playerA;
                             return a.anger > b.anger;
                         });
    }

    void Warden::IncreaseAngerAt(Entity* entity, int amount, bool playSound) {
        // MC Warden.increaseAngerAt, then playListeningSound (the AngerLevel's
        // WARDEN_LISTENING / WARDEN_LISTENING_ANGRY at 10.0, never mid-roar).
        if (IsNoAi() || !CanTargetEntity(entity)) return;
        WardenAi::SetDigCooldown(*this);

        Brain* brain = GetBrain();
        // MC: !(getTarget() instanceof Player) — the validated brain target.
        const LivingEntity* currentTarget = GetTarget();
        const bool maybeSwitchTarget = !(currentTarget && currentTarget->IsPlayer());

        // MC AngerManagement.increaseAnger — clamp at 150.
        int newAnger = 0;
        bool found = false;
        for (AngerEntry& e : m_anger) {
            if (e.entity == entity) {
                e.anger = std::min(150, e.anger + amount);
                newAnger = e.anger;
                found = true;
                break;
            }
        }
        if (!found) {
            newAnger = std::min(150, amount);
            m_anger.push_back({ entity, newAnger });
        }
        SortAnger();

        // MC: a player crossing the ANGRY line evicts a non-player target so
        // the roar/fight re-acquires against the player.
        if (brain && entity->IsPlayer() && maybeSwitchTarget
            && newAnger >= kAngerAngry) {
            brain->EraseMemory(MemoryModule::AttackTarget);
        }

        if (playSound && GetPose() != Pose::Roaring) {
            PlaySound(GetActiveAnger() >= kAngerAgitated ? SoundEvents::WARDEN_LISTENING_ANGRY
                                                         : SoundEvents::WARDEN_LISTENING,
                      10.0f, GetVoicePitch());
        }
    }

    void Warden::ClearAnger(const Entity* entity) {
        for (auto it = m_anger.begin(); it != m_anger.end(); ++it) {
            if (it->entity == entity) {
                m_anger.erase(it);
                break;
            }
        }
        SortAnger();
    }

    int Warden::GetActiveAnger() const {
        // MC AngerManagement.getActiveAnger(getTarget()): with a target its
        // anger, without one the highest on the books.
        const Entity* target = GetTarget();
        if (target) {
            for (const AngerEntry& e : m_anger) {
                if (e.entity == target) return e.anger;
            }
            return 0;
        }
        int highest = 0;
        for (const AngerEntry& e : m_anger) highest = std::max(highest, e.anger);
        return highest;
    }

    LivingEntity* Warden::GetEntityAngryAt() const {
        // MC Warden.getEntityAngryAt — only an ANGRY warden names a culprit,
        // and it is the sort's top targetable suspect.
        if (!IsAngry()) return nullptr;
        for (const AngerEntry& e : m_anger) {
            if (CanTargetEntity(e.entity)) {
                return dynamic_cast<LivingEntity*>(e.entity);
            }
        }
        return nullptr;
    }

    void Warden::SetAttackTarget(LivingEntity* target) {
        // MC Warden.setAttackTarget — also arms the 200-tick melee-first
        // window before the first sonic boom (TIME_TO_USE_MELEE_UNTIL_SONIC_BOOM).
        if (Brain* brain = GetBrain()) {
            brain->EraseMemory(MemoryModule::RoarTarget);
            brain->SetMemory(MemoryModule::AttackTarget, static_cast<Entity*>(target));
            brain->EraseMemory(MemoryModule::CantReachWalkTargetSince);
            brain->SetMemoryWithExpiry(MemoryModule::SonicBoomCooldown,
                                       std::monostate{}, 200);
        }
    }

    bool Warden::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC Warden.isInvulnerableTo: unhittable while digging or emerging,
        // except by BYPASSES_INVULNERABILITY damage (the void).
        if (IsDiggingOrEmerging() && source != MobDamageSource::Void) return false;

        const bool hurt = GenericMonster::Hurt(source, amount, attacker);
        // MC anger-boosts on every hurtServer call, NOT only when the damage
        // landed — a hit swallowed by the invulnerability window still angers.
        if (m_level && !m_level->IsClientSide() && !IsNoAi()) {
            // MC Warden.hurtServer: being hit is instant maximum anger.
            IncreaseAngerAt(attacker, kAngerAngry + 20, false);
            Brain* brain = GetBrain();
            auto* livingAttacker = dynamic_cast<LivingEntity*>(attacker);
            if (brain && !brain->HasMemoryValue(MemoryModule::AttackTarget)
                && livingAttacker) {
                // MC: a direct hit, or any hit from inside 5 blocks, is
                // answered immediately, anger ladder or no.
                const bool direct = source != MobDamageSource::Projectile;
                if (direct || DistanceToSqr(*livingAttacker) < 5.0 * 5.0) {
                    SetAttackTarget(livingAttacker);
                }
            }
        }
        return hurt;
    }

    bool Warden::DoHurtTarget(Entity& target) {
        // MC Warden.doHurtTarget broadcasts BEFORE delegating, so the animation
        // starts on the same tick the damage lands rather than the next one —
        // and every landed swing re-arms the 40-tick sonic-boom cooldown.
        if (m_level) m_level->BroadcastEntityEvent(*this, kEventMobAttack);
        PlaySound(SoundEvents::WARDEN_ATTACK_IMPACT, 10.0f, GetVoicePitch());
        if (Brain* brain = GetBrain()) {
            brain->SetMemoryWithExpiry(MemoryModule::SonicBoomCooldown,
                                       std::monostate{}, 40);
        }
        return GenericMonster::DoHurtTarget(target);
    }

    void Warden::HandleEntityEvent(uint8_t id) {
        if (id == kEventMobAttack) {
            // MC stops the roar first: the two clips write the same parts, and
            // a roar left running would fight the swing.
            Anim(MobAnim::Roar).Stop();
            Anim(MobAnim::Attack).Start(tickCount);
            return;
        }
        if (id == kEventWardenTendrils) {
            // MC pulses tendrilAnimation for 10 ticks; the generated model has
            // no tendril channel to read it. Swallowed so the base does not
            // mistake it for something else.
            return;
        }
        if (id == kEventWardenSonicBoom) {
            Anim(MobAnim::SonicBoom).Start(tickCount);
            return;
        }
        GenericMonster::HandleEntityEvent(id);
    }

    void Warden::OnPoseUpdated() {
        // MC Warden.onSyncedDataUpdated's DATA_POSE branch. `start`, not
        // `startIfStopped`: each of these fires exactly on the transition and
        // plays once from the top. Client only — the server sets these poses.
        if (!m_level || !m_level->IsClientSide()) return;
        switch (GetPose()) {
            case Pose::Emerging: Anim(MobAnim::Emerge).Start(tickCount); break;
            case Pose::Digging:  Anim(MobAnim::Digging).Start(tickCount); break;
            case Pose::Roaring:  Anim(MobAnim::Roar).Start(tickCount); break;
            case Pose::Sniffing: Anim(MobAnim::Sniff).Start(tickCount); break;
            default: break;
        }
    }

    // ── The vibration system (MC Warden.VibrationUser) ─────────────────────

    VibrationUser& Warden::GetVibrationUser() { return m_vibrationUser; }

    Warden::WardenVibrationUser::WardenVibrationUser(Warden& warden)
        : m_warden(warden),
          // MC: new EntityPositionSource(Warden.this, getEyeHeight()).
          m_source(PositionSource::OfEntity(&warden, warden.GetEyeHeight())) {}

    bool Warden::WardenVibrationUser::CanReceiveVibration(World& /*level*/, const glm::ivec3& /*pos*/,
                                                          GameEventId /*event*/, const GameEventContext& context) {
        if (m_warden.IsNoAi() || m_warden.IsDeadOrDying() || m_warden.IsDiggingOrEmerging()) return false;
        if (const Brain* brain = m_warden.GetBrain();
            brain && brain->HasMemoryValue(MemoryModule::VibrationCooldown)) {
            return false;
        }
        // A living source it could not target (a creative player, another
        // warden) is not heard at all. (No world border here.)
        if (context.sourceEntity && context.sourceEntity->AsLiving() &&
            !m_warden.CanTargetEntity(context.sourceEntity)) {
            return false;
        }
        return true;
    }

    void Warden::WardenVibrationUser::OnReceiveVibration(World& /*level*/, const glm::ivec3& pos, GameEventId /*event*/,
                                                         Entity* sourceEntity, Entity* projectileOwner,
                                                         float /*receivingDistance*/) {
        if (m_warden.IsDeadOrDying()) return;
        Brain* brain = m_warden.GetBrain();
        if (brain) brain->SetMemoryWithExpiry(MemoryModule::VibrationCooldown, std::monostate{}, 40);
        if (m_warden.m_level) m_warden.m_level->BroadcastEntityEvent(m_warden, kEventWardenTendrils);
        m_warden.PlaySound(SoundEvents::WARDEN_TENDRIL_CLICKS, 5.0f, m_warden.GetVoicePitch());

        glm::ivec3 suspiciousPos = pos;
        if (projectileOwner) {
            // closerThan(projectileOwner, 30).
            const glm::dvec3 d = projectileOwner->position - m_warden.position;
            if (glm::dot(d, d) < 30.0 * 30.0) {
                if (brain && brain->HasMemoryValue(MemoryModule::RecentProjectile)) {
                    if (m_warden.CanTargetEntity(projectileOwner)) suspiciousPos = projectileOwner->BlockPosition();
                    m_warden.IncreaseAngerAt(projectileOwner);
                } else {
                    m_warden.IncreaseAngerAt(projectileOwner, 10, true);
                }
            }
            if (brain) brain->SetMemoryWithExpiry(MemoryModule::RecentProjectile, std::monostate{}, 100);
        } else if (sourceEntity) {
            m_warden.IncreaseAngerAt(sourceEntity);
        }

        if (!m_warden.IsAngry()) {
            // AngerManagement.getActiveEntity: the top suspect, if living.
            const Entity* active = nullptr;
            if (!m_warden.m_anger.empty() && m_warden.m_anger.front().entity->AsLiving()) {
                active = m_warden.m_anger.front().entity;
            }
            if (projectileOwner || !active || active == sourceEntity) {
                WardenAi::SetDisturbanceLocation(m_warden, suspiciousPos);
            }
        }
    }

    void Warden::ClearReferenceTo(const Entity* entity) {
        GenericMonster::ClearReferenceTo(entity);
        // The anger map holds raw pointers; a removed entity must not survive
        // in it — same reasoning as Goal::ClearReferenceTo.
        for (auto it = m_anger.begin(); it != m_anger.end(); ++it) {
            if (it->entity == entity) {
                m_anger.erase(it);
                break;
            }
        }
    }

    // ══ Creaking ═══════════════════════════════════════════════════════════

    namespace {

        // MC Creaking's gated controls: a frozen creaking's steering, gaze and
        // pathing all no-op, which is what makes the freeze absolute instead
        // of merely slow.
        class CreakingMoveControl : public MoveControl {
        public:
            explicit CreakingMoveControl(Creaking* creaking)
                : MoveControl(creaking), m_creaking(creaking) {}
            void Tick() override {
                if (m_creaking->CanMove()) MoveControl::Tick();
            }
        private:
            Creaking* m_creaking;
        };

        class CreakingLookControl : public LookControl {
        public:
            explicit CreakingLookControl(Creaking* creaking)
                : LookControl(creaking), m_creaking(creaking) {}
            void Tick() override {
                if (m_creaking->CanMove()) LookControl::Tick();
            }
        private:
            Creaking* m_creaking;
        };

        class CreakingPathNavigation : public GroundPathNavigation {
        public:
            CreakingPathNavigation(Creaking* creaking, EntityLevel* level)
                : GroundPathNavigation(creaking, level), m_creaking(creaking) {}
            void Tick() override {
                if (m_creaking->CanMove()) GroundPathNavigation::Tick();
            }
        private:
            Creaking* m_creaking;
        };

    } // namespace

    Creaking::Creaking(EntityLevel* level) : GenericMonster(EntityTypeId::Creaking, level) {
        // NO GOALS — MC's Creaking is all brain.
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        SetMoveControl(std::make_unique<CreakingMoveControl>(this));
        SetLookControl(std::make_unique<CreakingLookControl>(this));
        SetNavigation(std::make_unique<CreakingPathNavigation>(this, level));
        // MC also gates JumpControl (its tick clears `jumping`). JumpControl's
        // Tick is virtual now (the rabbit needed it), but no override is
        // registered here: the only jump writer in this brain is Swim, which
        // CreakingAi already gates on canMove, so nothing can latch a jump
        // while frozen anyway.

        // MC's HomeNodeEvaluator (paths refuse to leave 32 blocks of the
        // creaking heart) needs a home position; heartless creakings have
        // none, in MC too.

        m_brain = std::make_unique<Brain>();
        CreakingAi::InitBrain(*this, *m_brain);
    }

    void Creaking::UpdateBrainActivity() { CreakingAi::UpdateActivity(*this); }

    bool Creaking::DoHurtTarget(Entity& target) {
        // MC refuses to swing at anything that is not alive, and the animation
        // is inside that guard — a creaking does not windmill at an item frame.
        if (!dynamic_cast<LivingEntity*>(&target)) return false;
        m_attackAnimationRemainingTicks = 15;
        if (m_level) m_level->BroadcastEntityEvent(*this, kEventMobAttack);
        return GenericMonster::DoHurtTarget(target);
    }

    bool Creaking::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC Creaking.hurtServer's heart-bound branch, minus the heart: the
        // hits a heart would eat (a living attacker or a projectile) still
        // flash the 8-tick invulnerability shimmer, but the damage lands —
        // there is no heart to absorb it. See the class comment.
        if (m_level && !m_level->IsClientSide()
            && m_invulnerabilityAnimationRemainingTicks <= 0 && !IsDeadOrDying()
            && (dynamic_cast<LivingEntity*>(attacker)
                || source == MobDamageSource::Projectile)) {
            m_invulnerabilityAnimationRemainingTicks = 8;
            m_level->BroadcastEntityEvent(*this, kEventCreakingInvulnerable);
            GameEvent(GameEventId::EntityAction);   // MC hurtServer's shimmer branch
        }
        return GenericMonster::Hurt(source, amount, attacker);
    }

    void Creaking::HandleEntityEvent(uint8_t id) {
        if (id == kEventMobAttack) {
            m_attackAnimationRemainingTicks = 15;
            return;
        }
        if (id == kEventCreakingInvulnerable) {
            m_invulnerabilityAnimationRemainingTicks = 8;
            return;
        }
        GenericMonster::HandleEntityEvent(id);
    }

    void Creaking::AiStep() {
        // MC Creaking.aiStep: the two animation counters run on both sides
        // (the server sets them directly, the clients from entity events)...
        if (m_invulnerabilityAnimationRemainingTicks > 0) {
            --m_invulnerabilityAnimationRemainingTicks;
        }
        if (m_attackAnimationRemainingTicks > 0) {
            --m_attackAnimationRemainingTicks;
        }

        // ...and the freeze gate re-evaluates every tick, server-side.
        // Freezing plants the creaking mid-step; MC plays CREAKING_FREEZE /
        // UNFREEZE on the transition.
        if (m_level && !m_level->IsClientSide()) {
            const bool couldMove = m_canMove;
            const bool nowCanMove = CheckCanMove();
            if (nowCanMove != couldMove) {
                GameEvent(GameEventId::EntityAction);   // MC aiStep: the freeze flip
                if (nowCanMove) {
                    MakeSound(SoundEvents::CREAKING_UNFREEZE);
                } else {
                    StopInPlace();
                    MakeSound(SoundEvents::CREAKING_FREEZE);
                }
            }
            m_canMove = nowCanMove;
        }
        GenericMonster::AiStep();
    }

    void Creaking::Tick() {
        GenericMonster::Tick();
        // MC Creaking.tick also validates the creaking heart (none here) and
        // flickers the emissive eyes while dying (no emissive layer).
    }

    void Creaking::SetupAnimationStates() {
        // MC Creaking.setupAnimationStates, all three lines.
        Anim(MobAnim::Attack).AnimateWhen(m_attackAnimationRemainingTicks > 0, tickCount);
        Anim(MobAnim::Invulnerability)
            .AnimateWhen(m_invulnerabilityAnimationRemainingTicks > 0, tickCount);
        Anim(MobAnim::Death).AnimateWhen(IsTearingDown(), tickCount);
    }

    void Creaking::Die(MobDamageSource source, Entity* attacker) {
        // DEVIATION (see the class comment): MC tears down only a heart-bound
        // creaking; the twitch death is this port's death for every creaking.
        m_tearingDown = true;
        GenericMonster::Die(source, attacker);
        // MC creakingDeathEffects: die, then CREAKING_TWITCH.
        if (m_level && !m_level->IsClientSide()) MakeSound(SoundEvents::CREAKING_TWITCH);
    }

    void Creaking::TickDeath() {
        // MC Creaking.tickDeath while tearing down: 45 ticks
        // (TWITCH_DEATH_DURATION) instead of the 20-tick fall-over, then
        // tearDown's crumble: 100 BLOCK_CRUMBLE of pale oak wood and 10 of an
        // awake creaking heart over the box (spread 0.3 of each side).
        ++deathTime;
        if (deathTime > 45 && m_level && !m_level->IsClientSide() && !IsRemoved()) {
            const AABB box = GetAABB();
            const glm::dvec3 c = (glm::dvec3(box.min) + glm::dvec3(box.max)) * 0.5;
            const glm::dvec3 spread = (glm::dvec3(box.max) - glm::dvec3(box.min)) * 0.3;
            m_level->SendParticles(ParticleOptions::Block(ParticleKind::BlockCrumble,
                                                          BlockStates::Default(BlockID::PaleOakWood)),
                                   false, false, c.x, c.y, c.z, 100, spread.x, spread.y, spread.z, 0.0);
            m_level->SendParticles(
                ParticleOptions::Block(ParticleKind::BlockCrumble,
                                       BlockStates::Default(BlockID::CreakingHeart)
                                           .SetName(PropertyId::CREAKING_HEART_STATE, "awake")),
                false, false, c.x, c.y, c.z, 10, spread.x, spread.y, spread.z, 0.0);
            Remove(RemovalReason::Killed);
        }
    }

    void Creaking::TickHeadTurn(float yBodyRotTarget) {
        // MC CreakingBodyRotationControl — the torso freezes with the rest.
        if (CanMove()) GenericMonster::TickHeadTurn(yBodyRotTarget);
    }

    void Creaking::Knockback(double power, double dx, double dz) {
        // MC Creaking.knockback — an unseen creaking cannot be shoved.
        if (CanMove()) GenericMonster::Knockback(power, dx, dz);
    }

    void Creaking::UpdateWalkAnimation(float distance) {
        // MC Creaking.updateWalkAnimation — ×25 with a 3.0 cap: the walk cycle
        // overdrives at speed, which is most of the creaking's scuttle.
        walkAnimation.Update(std::min(distance * 25.0f, 3.0f), 0.4f, 1.0f);
    }

    void Creaking::Activate(LivingEntity* player) {
        // MC Creaking.activate.
        if (Brain* brain = GetBrain()) {
            brain->SetMemory(MemoryModule::AttackTarget, static_cast<Entity*>(player));
        }
        GameEvent(GameEventId::EntityAction);   // MC activate
        m_isActive = true;
        MakeSound(SoundEvents::CREAKING_ACTIVATE);
    }

    void Creaking::Deactivate() {
        if (Brain* brain = GetBrain()) {
            brain->EraseMemory(MemoryModule::AttackTarget);
        }
        GameEvent(GameEventId::EntityAction);   // MC deactivate
        m_isActive = false;
        MakeSound(SoundEvents::CREAKING_DEACTIVATE);   // MC Creaking.deactivate
    }

    const char* Creaking::GetAmbientSound() const {
        // MC: `isActive() ? null : CREAKING_AMBIENT`.
        return IsActive() ? "" : SoundEvents::CREAKING_AMBIENT;
    }

    bool Creaking::CheckCanMove() {
        // MC Creaking.checkCanMove, verbatim minus the carved-pumpkin disguise
        // (no player equipment): an ACTIVE creaking freezes under any watching
        // eye; an inactive one activates on being watched inside 12 blocks
        // (ACTIVATION_RANGE_SQ = 144).
        const Brain* brain = GetBrain();
        const std::vector<Entity*>* players =
            brain ? brain->GetEntityList(MemoryModule::NearestPlayers) : nullptr;
        const bool active = IsActive();

        if (!players || players->empty()) {
            if (active) Deactivate();
            return true;
        }

        bool hasPotentialTarget = false;
        for (Entity* e : *players) {
            auto* player = dynamic_cast<LivingEntity*>(e);
            if (!player || !CanAttack(*player)) continue;
            hasPotentialTarget = true;
            if (IsLookingAtMe(*player)) {
                if (active) return false;
                if (player->DistanceToSqr(*this) < 144.0) {
                    Activate(player);
                    return false;
                }
            }
        }
        if (!hasPotentialTarget && active) Deactivate();
        return true;
    }

    bool Creaking::IsLookingAtMe(const LivingEntity& player) const {
        // MC isLookingAtMe(player, 0.5, scaleByDistance=false, visual=true,
        // eyeY, y + 0.5·scale, midpoint) — three heights so crouching behind a
        // half wall does not blind it.
        const glm::vec3 viewF = Mth::ViewVector(player.xRot, player.yRot);
        glm::dvec3 view(viewF.x, viewF.y, viewF.z);
        view = glm::normalize(view);

        const double candidates[3] = {
            GetEyeY(), position.y + 0.5, (GetEyeY() + position.y) / 2.0,
        };
        for (double y : candidates) {
            glm::dvec3 dir(position.x - player.position.x,
                           y - player.GetEyeY(),
                           position.z - player.position.z);
            const double len = glm::length(dir);
            if (len < 1.0e-8) continue;
            dir /= len;
            if (glm::dot(view, dir) > 1.0 - 0.5) {
                // The mutable line-of-sight cache is the same one every other
                // sight test uses; the const_cast is only ever this class
                // asking about itself.
                if (const_cast<Creaking*>(this)->GetSensing().HasLineOfSight(player)) {
                    return true;
                }
            }
        }
        return false;
    }

    // ══ Sniffer ════════════════════════════════════════════════════════════

    Sniffer::Sniffer(EntityLevel* level) : GenericAnimal(EntityTypeId::Sniffer, level) {
        // NO GOALS — MC's Sniffer is all brain.
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        // MC Sniffer's constructor maluses — it will not path into water at
        // all unless already burning or wet (that onPathfindingStart flip has
        // no hook here; the resting value is the behaviour that shows).
        SetPathfindingMalus(PathType::Water, -1.0f);
        SetPathfindingMalus(PathType::DangerPowderSnow, -1.0f);
        SetPathfindingMalus(PathType::DamageCautious, -1.0f);

        m_brain = std::make_unique<Brain>();
        SnifferAi::InitBrain(*this, *m_brain);
    }

    void Sniffer::UpdateBrainActivity() { SnifferAi::UpdateActivity(*this); }

    bool Sniffer::IsSnifferFood(uint32_t itemId) {
        // MC ItemTags.SNIFFER_FOOD — torchflower seeds only.
        static const ItemID seeds = RecipeManager::ItemFromSlug("torchflower_seeds");
        return seeds != Items::Air && itemId == static_cast<uint32_t>(seeds);
    }

    bool Sniffer::IsFood(uint32_t itemId) const { return IsSnifferFood(itemId); }

    Sniffer& Sniffer::TransitionTo(State state) {
        // MC Sniffer.transitionTo, with each branch's entry sound.
        switch (state) {
            case State::FeelingHappy: PlaySound(SoundEvents::SNIFFER_HAPPY, 1.0f, 1.0f); break;
            case State::Scenting:     PlaySound(SoundEvents::SNIFFER_SCENTING, 1.0f, IsBaby() ? 1.3f : 1.0f); break;
            case State::Sniffing:     PlaySound(SoundEvents::SNIFFER_SNIFFING, 1.0f, 1.0f); break;
            case State::Rising:       PlaySound(SoundEvents::SNIFFER_DIGGING_STOP, 1.0f, 1.0f); break;
            default: break;
        }
        if (state == State::Digging) {
            // MC onDiggingStart: DATA_DROP_SEED_AT_TICK = now + 120 — the
            // seed pops out mid-dig, not at the end.
            m_dropSeedAtTick = tickCount + 120;
        }
        m_state = state;
        return *this;
    }

    void Sniffer::SetAnimStateByte(uint8_t v) {
        // MC Sniffer.onSyncedDataUpdated(DATA_STATE) — client side of the
        // state machine. Guarded on change: the tracker resends unchanged
        // bytes alongside every health tick.
        const State state = v <= 6 ? static_cast<State>(v) : State::Idling;
        if (state == m_state) return;
        m_state = state;
        ResetAnimations();
        switch (state) {
            case State::FeelingHappy:
                Anim(MobAnim::FeelingHappy).StartIfStopped(tickCount);
                break;
            case State::Scenting:
                Anim(MobAnim::Scenting).StartIfStopped(tickCount);
                break;
            case State::Sniffing:
                Anim(MobAnim::Sniffing).StartIfStopped(tickCount);
                break;
            case State::Digging:
                Anim(MobAnim::Digging).StartIfStopped(tickCount);
                break;
            case State::Rising:
                Anim(MobAnim::Rising).StartIfStopped(tickCount);
                break;
            default:
                // IDLING and SEARCHING carry no clip — the walk cycle is the
                // whole of their motion. MC also refreshDimensions()es here
                // (a digging sniffer is 0.4 shorter); no per-state dimensions
                // exist in this port.
                break;
        }
    }

    void Sniffer::ResetAnimations() {
        // MC Sniffer.resetAnimations.
        Anim(MobAnim::Digging).Stop();
        Anim(MobAnim::Sniffing).Stop();
        Anim(MobAnim::Rising).Stop();
        Anim(MobAnim::FeelingHappy).Stop();
        Anim(MobAnim::Scenting).Stop();
    }

    bool Sniffer::IsTempted() const {
        const Brain* brain = GetBrain();
        return brain && brain->GetBool(MemoryModule::IsTempted).value_or(false);
    }

    // Brain mobs panic through the IS_PANICKING memory, not the PanicGoal the
    // base class polls for.
    static bool IsBrainPanicking(const LivingEntity& body) {
        const Brain* brain = body.GetBrain();
        return brain && brain->HasMemoryValue(MemoryModule::IsPanicking);
    }

    bool Sniffer::CanSniff() const {
        // MC Sniffer.canSniff.
        return !IsTempted() && !IsBrainPanicking(*this) && !IsInWater()
            && !IsInLove() && onGround && !IsPassenger() && !IsLeashed();
    }

    bool Sniffer::CanDig() const {
        return !IsBrainPanicking(*this) && !IsTempted() && !IsBaby() && !IsInWater()
            && onGround && CanDigAt(GetHeadBlock() - glm::ivec3(0, 1, 0));
    }

    glm::ivec3 Sniffer::GetHeadBlock() const {
        // MC Sniffer.getHeadBlock — 2.25 blocks along the view, 0.2 up.
        const glm::vec3 forward = Mth::ViewVector(xRot, yRot);
        const glm::dvec3 head = position + glm::dvec3(forward) * 2.25;
        return glm::ivec3(static_cast<int>(std::floor(head.x)),
                          static_cast<int>(std::floor(position.y + 0.2)),
                          static_cast<int>(std::floor(head.z)));
    }

    bool Sniffer::CanDigAt(const glm::ivec3& pos) const {
        if (!m_level) return false;
        const IBlockAccess* blocks = m_level->Blocks();
        if (!blocks) return false;

        // MC BlockTags.SNIFFER_DIGGABLE_BLOCK, flattened — the eight dirts.
        switch (blocks->GetBlock(pos.x, pos.y, pos.z)) {
            case BlockID::Dirt:
            case BlockID::Grass:
            case BlockID::Podzol:
            case BlockID::CoarseDirt:
            case BlockID::RootedDirt:
            case BlockID::MossBlock:
            case BlockID::Mud:
            case BlockID::MuddyMangroveRoots:
                break;
            default:
                return false;
        }
        // MC: never re-dig an explored column...
        for (const glm::ivec3& explored : m_exploredPositions) {
            if (explored == pos) return false;
        }
        // ...and the spot must actually be reachable (createPath(pos, 1)
        // .canReach()).
        auto* self = const_cast<Sniffer*>(this);
        std::optional<Path> path = self->GetNavigation().CreatePath(pos, 1);
        return path && path->CanReach();
    }

    std::optional<glm::ivec3> Sniffer::CalculateDigPosition() {
        // MC Sniffer.calculateDigPosition — five LandRandomPos rolls of
        // widening radius (10, 12, 14, 16, 18), first whose BELOW block is
        // diggable wins.
        for (int i = 0; i < 5; ++i) {
            const std::optional<glm::dvec3> pos =
                RandomPos::GetLandPos(*this, 10 + 2 * i, 3);
            if (!pos) continue;
            const glm::ivec3 below(static_cast<int>(std::floor(pos->x)),
                                   static_cast<int>(std::floor(pos->y)) - 1,
                                   static_cast<int>(std::floor(pos->z)));
            if (CanDigAt(below)) return below;
        }
        return std::nullopt;
    }

    void Sniffer::OnDiggingComplete(bool success) {
        if (!success) return;
        // MC storeExploredPosition(getOnPos()) — newest first, 20 remembered.
        if (m_exploredPositions.size() > 20) m_exploredPositions.resize(20);
        m_exploredPositions.insert(m_exploredPositions.begin(),
                                   BlockPosition() - glm::ivec3(0, 1, 0));
    }

    void Sniffer::Tick() {
        // MC Sniffer.tick: SEARCHING sniffs audibly once a second (a client-
        // local sound), DIGGING emits particles and drops the seed.
        if (m_level && m_level->IsClientSide() && m_state == State::Searching && tickCount % 20 == 0) {
            m_level->PlayLocalSoundFromEntity(*this, SoundEvents::SNIFFER_SEARCHING, GetSoundSource(), 1.0f, 1.0f);
        }
        // emitDiggingParticles: between 1.7 s and 6 s into the dig, 30
        // BLOCK chips of the block under the snout a tick (client copy).
        if (m_level && m_level->IsClientSide() && m_state == State::Digging) {
            const AnimationState& dig = Anim(MobAnim::Digging);
            const float t = dig.IsStarted() ? dig.ElapsedSeconds(static_cast<float>(tickCount)) : 0.0f;
            if (t > 1.7f && t < 6.0f) {
                if (const IBlockAccess* blocks = m_level->Blocks()) {
                    const glm::ivec3 head = GetHeadBlock();
                    const BlockState below = blocks->GetBlockState(head.x, head.y - 1, head.z);
                    if (!InvisibleRenderShape(below.Block())) {
                        const ParticleOptions chip = ParticleOptions::Block(below);
                        for (int i = 0; i < 30; ++i) {
                            m_level->AddParticle(chip, head.x + 0.5, head.y + 0.5 - 0.6499999761581421, head.z + 0.5,
                                                 0.0, 0.0, 0.0);
                        }
                    }
                }
            }
        }
        if (m_level && !m_level->IsClientSide() && m_state == State::Digging) {
            // MC emitDiggingParticles' tail: every 10 ticks of the dig,
            // level.gameEvent(ENTITY_ACTION, getHeadBlock(), Context.of(this)).
            if (tickCount % 10 == 0) {
                if (ILevelWrite* write = m_level->MutableBlocks()) {
                    write->GameEvent(GameEventId::EntityAction, GetHeadBlock(), GameEventContext::Of(this));
                }
            }
            DropSeed();
        }
        GenericAnimal::Tick();
    }

    void Sniffer::DropSeed() {
        // MC Sniffer.dropSeed — one pull from the SNIFFER_DIGGING loot table
        // (torchflower seeds or a pitcher pod, equal weight) at the head
        // block, exactly at DATA_DROP_SEED_AT_TICK.
        if (!m_level || m_dropSeedAtTick != tickCount) return;
        static const ItemID seeds = RecipeManager::ItemFromSlug("torchflower_seeds");
        static const ItemID pod   = RecipeManager::ItemFromSlug("pitcher_pod");
        const ItemID pick = m_level->Random().NextBool() ? seeds : pod;
        if (pick == Items::Air) return;
        const glm::ivec3 head = GetHeadBlock();
        m_level->SpawnItemDrop(glm::dvec3(head.x + 0.5, head.y + 0.5, head.z + 0.5),
                               static_cast<uint32_t>(pick), 1);
        PlaySound(SoundEvents::SNIFFER_DROP_SEED, 1.0f, 1.0f);
    }

    const char* Sniffer::GetAmbientSound() const {
        return (m_state == State::Digging || m_state == State::Searching) ? "" : SoundEvents::SNIFFER_IDLE;
    }

    void Sniffer::PlayEatingSound() {
        if (!m_level) return;
        JavaRandom& rng = m_level->Random();
        m_level->PlaySoundFromEntity(nullptr, *this, SoundEvents::SNIFFER_EAT, SoundSource::Neutral, 1.0f,
                                     0.8f + rng.NextFloat() * 0.4f);   // Mth.randomBetween(0.8, 1.2)
    }

    void Sniffer::Die(MobDamageSource source, Entity* attacker) {
        // MC Sniffer.die resets to IDLING first, so the corpse is not stuck
        // nose-down in the dirt.
        TransitionTo(State::Idling);
        GenericAnimal::Die(source, attacker);
    }

    // ══ CopperGolem ════════════════════════════════════════════════════════

    namespace {
        // MC CopperGolemOxidationLevels: UNAFFECTED and EXPOSED share the
        // plain voice, WEATHERED and OXIDIZED each have their own.
        struct CopperGolemVoice {
            const char* spinHead;
            const char* hurt;
            const char* death;
            const char* step;
        };

        const CopperGolemVoice& CopperGolemVoiceOf(CopperGolem::WeatherState state) {
            static const CopperGolemVoice kPlain{
                SoundEvents::COPPER_GOLEM_SPIN, SoundEvents::COPPER_GOLEM_HURT,
                SoundEvents::COPPER_GOLEM_DEATH, SoundEvents::COPPER_GOLEM_STEP};
            static const CopperGolemVoice kWeathered{
                SoundEvents::COPPER_GOLEM_WEATHERED_SPIN, SoundEvents::COPPER_GOLEM_WEATHERED_HURT,
                SoundEvents::COPPER_GOLEM_WEATHERED_DEATH, SoundEvents::COPPER_GOLEM_WEATHERED_STEP};
            static const CopperGolemVoice kOxidized{
                SoundEvents::COPPER_GOLEM_OXIDIZED_SPIN, SoundEvents::COPPER_GOLEM_OXIDIZED_HURT,
                SoundEvents::COPPER_GOLEM_OXIDIZED_DEATH, SoundEvents::COPPER_GOLEM_OXIDIZED_STEP};
            switch (state) {
                case CopperGolem::WeatherState::Weathered: return kWeathered;
                case CopperGolem::WeatherState::Oxidized:  return kOxidized;
                default:                                   return kPlain;
            }
        }

        // WeatheringCopper.WeatherState.next / previous (ordinal ± 1,
        // clamped at the ends).
        CopperGolem::WeatherState NextWeatherState(CopperGolem::WeatherState s) {
            return s == CopperGolem::WeatherState::Oxidized
                       ? s : static_cast<CopperGolem::WeatherState>(static_cast<uint8_t>(s) + 1);
        }
        CopperGolem::WeatherState PreviousWeatherState(CopperGolem::WeatherState s) {
            return s == CopperGolem::WeatherState::Unaffected
                       ? s : static_cast<CopperGolem::WeatherState>(static_cast<uint8_t>(s) - 1);
        }

        // RandomSource.nextIntBetweenInclusive(WEATHERING_TICK_FROM,
        // WEATHERING_TICK_TO).
        int64_t RollWeatheringDelay(JavaRandom& random) {
            return random.NextInt(CopperGolem::kWeatheringTickFrom, CopperGolem::kWeatheringTickTo);
        }

        bool IsAxe(const ItemStack& stack) {
            return !stack.IsEmpty() &&
                   DataTags::HasTag(DataTags::Registry::Item, ItemRegistry::Slug(stack.itemId), "minecraft:axes");
        }
    } // namespace

    const char* CopperGolem::WeatherStateName(WeatherState state) {
        // WeatheringCopper.WeatherState.CODEC (StringRepresentable).
        switch (state) {
            case WeatherState::Exposed:   return "exposed";
            case WeatherState::Weathered: return "weathered";
            case WeatherState::Oxidized:  return "oxidized";
            default:                      return "unaffected";
        }
    }

    CopperGolem::WeatherState CopperGolem::WeatherStateFromName(std::string_view name) {
        if (name == "exposed")   return WeatherState::Exposed;
        if (name == "weathered") return WeatherState::Weathered;
        if (name == "oxidized")  return WeatherState::Oxidized;
        return WeatherState::Unaffected;
    }

    CopperGolem::CopperGolem(EntityLevel* level)
        : GenericPathfinderMob(EntityTypeId::CopperGolem, level) {
        // NO GOALS — MC's CopperGolem is all brain (AbstractGolem registers
        // none of its own either).
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        // MC CopperGolem's constructor, line for line (nextWeatheringTick
        // starts UNSET — the first server tick rolls the deadline).
        GetNavigation().SetRequiredPathLength(48.0f);
        GetNavigation().SetCanOpenDoors(true);
        SetPersistenceRequired(true);
        SetState(State::Idle);
        SetPathfindingMalus(PathType::DangerFire, 16.0f);
        SetPathfindingMalus(PathType::DangerOther, 16.0f);
        SetPathfindingMalus(PathType::DamageFire, -1.0f);

        m_brain = std::make_unique<Brain>();
        CopperGolemAi::InitBrain(*this, *m_brain);
        // MC: getBrain().setMemory(TRANSPORT_ITEMS_COOLDOWN_TICKS,
        // random.nextInt(60, 100)) — the first trip waits out the spawn
        // cooldown. RandomSource.nextInt(origin, bound) excludes the bound.
        if (m_level) {
            m_brain->SetMemory(MemoryModule::TransportItemsCooldownTicks,
                               kSpawnCooldownMin +
                                   m_level->Random().NextInt(kSpawnCooldownMax - kSpawnCooldownMin));
        }
    }

    void CopperGolem::UpdateBrainActivity() { CopperGolemAi::UpdateActivity(*this); }

    const char* CopperGolem::GetHurtSound(MobDamageSource) const {
        return CopperGolemVoiceOf(m_weatherState).hurt;
    }

    const char* CopperGolem::GetDeathSound() const {
        return CopperGolemVoiceOf(m_weatherState).death;
    }

    void CopperGolem::PlayStepSound(const glm::ivec3&, BlockState) {
        PlaySound(CopperGolemVoiceOf(m_weatherState).step, 1.0f, 1.0f);
    }

    const char* CopperGolem::GetSpinHeadSound() const {
        return CopperGolemVoiceOf(m_weatherState).spinHead;
    }

    void CopperGolem::Spawn(WeatherState weatherState) {
        SetWeatherState(weatherState);
        PlaySpawnSound();
    }

    void CopperGolem::PlaySpawnSound() {
        PlaySound(SoundEvents::COPPER_GOLEM_SPAWN);
    }

    std::shared_ptr<SpawnGroupData>
    CopperGolem::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        PlaySpawnSound();
        return GenericPathfinderMob::FinalizeSpawn(reason, std::move(groupData));
    }

    void CopperGolem::Tick() {
        // MC CopperGolem.tick: super (whose client half runs
        // setupAnimationStates — Mob::Tick), then the server's weathering.
        GenericPathfinderMob::Tick();
        if (m_level && !m_level->IsClientSide() && !IsRemoved()) {
            UpdateWeathering();
        }
    }

    void CopperGolem::UpdateWeathering() {
        // MC CopperGolem.updateWeathering(level, level.getRandom(),
        // level.getGameTime()).
        if (m_nextWeatheringTick == kIgnoreWeatheringTick) return;   // waxed
        JavaRandom& random = m_level->Random();
        const int64_t gameTime = m_level->GetGameTime();
        if (m_nextWeatheringTick == kUnsetWeatheringTick) {
            m_nextWeatheringTick = gameTime + RollWeatheringDelay(random);
            return;
        }
        const WeatherState weatherState = m_weatherState;
        const bool isFullyOxidized = weatherState == WeatherState::Oxidized;
        if (gameTime >= m_nextWeatheringTick && !isFullyOxidized) {
            const WeatherState newState = NextWeatherState(weatherState);
            const bool isNewStateFullyOxidized = newState == WeatherState::Oxidized;
            SetWeatherState(newState);
            m_nextWeatheringTick = isNewStateFullyOxidized ? 0 : m_nextWeatheringTick + RollWeatheringDelay(random);
        }
        // The state read BEFORE this tick's step: a golem that oxidized this
        // tick waits a tick before it can set.
        if (isFullyOxidized && CanTurnToStatue()) {
            TurnToStatue();
        }
    }

    bool CopperGolem::CanTurnToStatue() {
        // MC: level.getBlockState(blockPosition()).isAir() &&
        // level.getRandom().nextFloat() <= TURN_TO_STATUE_CHANCE.
        const IBlockAccess* blocks = m_level->Blocks();
        if (!blocks) return false;
        const glm::ivec3 pos = BlockPosition();
        return blocks->GetBlock(pos.x, pos.y, pos.z) == BlockID::Air &&
               m_level->Random().NextFloat() <= kTurnToStatueChance;
    }

    void CopperGolem::TurnToStatue() {
        // MC CopperGolem.turnToStatue: the OXIDIZED statue (Blocks.
        // COPPER_GOLEM_STATUE.weathering().oxidized()) in a random pose,
        // facing the golem's yaw, written with setBlockAndUpdate.
        ILevelWrite* blocks = m_level->MutableBlocks();
        if (!blocks) return;
        const glm::ivec3 pos = BlockPosition();
        // CopperGolemStatueBlock.Pose.values()[random.nextInt(0, 4)].
        const int pose = m_level->Random().NextInt(4);
        BlockState statue = BlockStates::Default(BlockID::OxidizedCopperGolemStatue)
                                .SetIndex(PropertyId::COPPER_GOLEM_POSE, pose);
        statue = WithHorizontalFacing(statue, FromYRot(yRot));
        blocks->SetBlock(pos.x, pos.y, pos.z, statue, World::UpdateFlags::All);

        // `if (level.getBlockEntity(pos) instanceof
        // CopperGolemStatueBlockEntity statue)` — the rest only once the
        // statue really stands there.
        auto* statueEntity = dynamic_cast<CopperGolemStatueBlockEntity*>(blocks->GetBlockEntity(pos));
        if (!statueEntity) return;
        statueEntity->CreateStatue(*this);
        blocks->BlockEntityChanged(pos);
        MobEquipment::DropPreservedEquipment(*this);
        Discard();
        PlaySound(SoundEvents::COPPER_GOLEM_BECOME_STATUE);
        if (IsLeashed()) {
            if (m_level->DoEntityDrops()) {
                DropLeash();
            } else {
                RemoveLeash();
            }
        }
    }

    uint8_t CopperGolem::GetAnimStateByte() const {
        return static_cast<uint8_t>(static_cast<uint8_t>(m_state) & 0x7);
    }

    void CopperGolem::SetAnimStateByte(uint8_t v) {
        // Client side of MC's COPPER_GOLEM_STATE synched data. No clip starts
        // here: SetupAnimationStates below runs every client tick and derives
        // them from the state, exactly as MC's does.
        const uint8_t id = v & 0x7;
        m_state = id <= 4 ? static_cast<State>(id) : State::Idle;
    }

    void CopperGolem::SetupAnimationStates() {
        // MC CopperGolem.setupAnimationStates, case for case.
        switch (m_state) {
            case State::Idle:
                Anim(MobAnim::InteractionGetNoItem).Stop();
                Anim(MobAnim::InteractionGetItem).Stop();
                Anim(MobAnim::InteractionDropItem).Stop();
                Anim(MobAnim::InteractionDropNoItem).Stop();
                if (m_idleAnimationStartTick == tickCount) {
                    Anim(MobAnim::Idle).Start(tickCount);
                } else if (m_idleAnimationStartTick == 0) {
                    // random.nextInt(200, 240) — the bound is exclusive.
                    m_idleAnimationStartTick =
                        tickCount + kSpinAnimationMinCooldown +
                        m_level->Random().NextInt(kSpinAnimationMaxCooldown - kSpinAnimationMinCooldown);
                }
                // MC: SPIN_SOUND_TIME_INTERVAL_OFFSET ticks into the spin,
                // playHeadSpinSound (a local spin sound of the golem's weather
                // stage) and the re-arm that keeps the spin periodic.
                if (tickCount == m_idleAnimationStartTick + 10) {
                    if (m_level && !IsSilent()) {
                        m_level->PlayLocalSound(position, GetSpinHeadSound(), GetSoundSource(), 1.0f, 1.0f, false);
                    }
                    m_idleAnimationStartTick = 0;
                }
                break;
            case State::GettingItem:
                Anim(MobAnim::Idle).Stop();
                m_idleAnimationStartTick = 0;
                Anim(MobAnim::InteractionGetNoItem).Stop();
                Anim(MobAnim::InteractionDropItem).Stop();
                Anim(MobAnim::InteractionDropNoItem).Stop();
                Anim(MobAnim::InteractionGetItem).StartIfStopped(tickCount);
                break;
            case State::GettingNoItem:
                Anim(MobAnim::Idle).Stop();
                m_idleAnimationStartTick = 0;
                Anim(MobAnim::InteractionGetItem).Stop();
                Anim(MobAnim::InteractionDropNoItem).Stop();
                Anim(MobAnim::InteractionDropItem).Stop();
                Anim(MobAnim::InteractionGetNoItem).StartIfStopped(tickCount);
                break;
            case State::DroppingItem:
                Anim(MobAnim::Idle).Stop();
                m_idleAnimationStartTick = 0;
                Anim(MobAnim::InteractionGetItem).Stop();
                Anim(MobAnim::InteractionGetNoItem).Stop();
                Anim(MobAnim::InteractionDropNoItem).Stop();
                Anim(MobAnim::InteractionDropItem).StartIfStopped(tickCount);
                break;
            case State::DroppingNoItem:
                Anim(MobAnim::Idle).Stop();
                m_idleAnimationStartTick = 0;
                Anim(MobAnim::InteractionGetItem).Stop();
                Anim(MobAnim::InteractionGetNoItem).Stop();
                Anim(MobAnim::InteractionDropItem).Stop();
                Anim(MobAnim::InteractionDropNoItem).StartIfStopped(tickCount);
                break;
        }
    }

    UseResult CopperGolem::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC CopperGolem.mobInteract, in its order.
        // 1. An empty hand takes what the golem carries:
        //    BehaviorUtils.throwItem(this, equippedItem, player.position()).
        if (held.IsEmpty()) {
            const ItemStack equipped = GetMainHandItem();
            if (!equipped.IsEmpty()) {
                if (m_level && !m_level->IsClientSide()) {
                    const glm::dvec3 from(position.x, GetEyeY() - 0.30000001192092896, position.z);
                    glm::dvec3 dir = player.position - position;
                    const double len = glm::length(dir);
                    dir = len < 1.0e-5 ? glm::dvec3(0.0) : dir / len;
                    // setThrower(golem).
                    m_level->SpawnThrownItem(from, dir * 0.30000001192092896, equipped, 10, GetId());
                    SetItemInHand(ItemStack{});
                }
                return UseResult::Success;
            }
        }

        // 2. Shears on a golem wearing a flower: shear(PLAYERS), SHEAR, wear.
        if (held.itemId == Items::Shears && !held.IsEmpty() && ReadyForShearing()) {
            if (m_level && !m_level->IsClientSide()) {
                Shear(SoundSource::Players);
                GameEvent(GameEventId::Shear, &player);
                HurtAndBreak(held, 1, player, EquipmentSlot::MAINHAND);
            }
            return UseResult::Success;
        }
        if (!m_level || m_level->IsClientSide()) return UseResult::Pass;

        const glm::ivec3 pos = BlockPosition();
        // 3. Honeycomb on an unwaxed golem: level event 3003 (wax-on
        //    sparkles), HONEYCOMB_WAX_ON, waxed for good.
        if (held.itemId == Items::Honeycomb && !held.IsEmpty() &&
            m_nextWeatheringTick != kIgnoreWeatheringTick) {
            m_level->PlayLevelEvent(SoundExcept(static_cast<const Entity*>(this)),
                                    LevelEvent::PARTICLES_WAX_ON, pos, 0);
            m_level->PlaySound(nullptr, pos, SoundEvents::HONEYCOMB_WAX_ON,
                               SoundSource::Blocks, 1.0f, 1.0f);
            m_nextWeatheringTick = kIgnoreWeatheringTick;
            Animal::UsePlayerItem(held);   // usePlayerItem(player, hand, stack)
            return UseResult::SuccessServer;
        }

        // 4. An axe on a waxed golem takes the wax off: AXE_SCRAPE at the
        //    golem, level event 3004 (wax-off), the deadline unset.
        if (IsAxe(held) && m_nextWeatheringTick == kIgnoreWeatheringTick) {
            m_level->PlaySoundFromEntity(nullptr, *this, SoundEvents::AXE_SCRAPE, GetSoundSource(), 1.0f, 1.0f);
            m_level->PlayLevelEvent(SoundExcept(static_cast<const Entity*>(this)),
                                    LevelEvent::PARTICLES_WAX_OFF, pos, 0);
            m_nextWeatheringTick = kUnsetWeatheringTick;
            HurtAndBreak(held, 1, player, EquipmentSlot::MAINHAND);
            return UseResult::SuccessServer;
        }

        // 5. An axe on a weathered golem scrapes one stage back: level event
        //    3005 (scrape), the deadline unset.
        if (IsAxe(held) && m_weatherState != WeatherState::Unaffected) {
            m_level->PlaySoundFromEntity(nullptr, *this, SoundEvents::AXE_SCRAPE, GetSoundSource(), 1.0f, 1.0f);
            m_level->PlayLevelEvent(SoundExcept(static_cast<const Entity*>(this)),
                                    LevelEvent::PARTICLES_SCRAPE, pos, 0);
            m_nextWeatheringTick = kUnsetWeatheringTick;
            SetWeatherState(PreviousWeatherState(m_weatherState));
            HurtAndBreak(held, 1, player, EquipmentSlot::MAINHAND);
            return UseResult::SuccessServer;
        }

        return GenericPathfinderMob::MobInteract(player, held);
    }

    bool CopperGolem::ReadyForShearing() const {
        // getItemBySlot(ANTENNA).is(ItemTags.SHEARABLE_FROM_COPPER_GOLEM).
        const ItemStack& antenna = GetEquipment(kAntennaSlot);
        return !antenna.IsEmpty() &&
               DataTags::HasTag(DataTags::Registry::Item, ItemRegistry::Slug(antenna.itemId),
                                "minecraft:shearable_from_copper_golem");
    }

    void CopperGolem::Shear(SoundSource soundSource) {
        // MC CopperGolem.shear: COPPER_GOLEM_SHEAR at the golem, the antenna
        // emptied, its item spawnAtLocation(level, stack, 1.5F).
        if (!m_level || m_level->IsClientSide()) return;
        m_level->PlaySoundFromEntity(nullptr, *this, SoundEvents::COPPER_GOLEM_SHEAR, soundSource, 1.0f, 1.0f);
        const ItemStack antenna = GetEquipment(kAntennaSlot);
        SetEquipment(kAntennaSlot, ItemStack{});
        if (!antenna.IsEmpty()) {
            DropItemStackAt(m_level->Dimension(), position + glm::dvec3(0.0, 1.5, 0.0), antenna);
        }
    }

    void CopperGolem::ThunderHit(Entity* bolt) {
        // MC CopperGolem.thunderHit: super first (the fire and the 5
        // lightning damage), then one stage back per distinct bolt.
        GenericPathfinderMob::ThunderHit(bolt);
        if (!m_level || m_level->IsClientSide()) return;
        const Uuid boltUuid = bolt ? bolt->GetUuid() : Uuid{};
        if (m_hasLastLightningBolt && boltUuid == m_lastLightningBoltUuid) return;
        m_lastLightningBoltUuid = boltUuid;
        m_hasLastLightningBolt = true;
        if (m_weatherState != WeatherState::Unaffected) {
            m_nextWeatheringTick = kUnsetWeatheringTick;
            SetWeatherState(PreviousWeatherState(m_weatherState));
        }
    }

    void CopperGolem::ActuallyHurt(MobDamageSource source, float amount,
                                   Entity* attacker) {
        // MC CopperGolem.actuallyHurt — super first, then the pose reset.
        GenericPathfinderMob::ActuallyHurt(source, amount, attacker);
        SetState(State::Idle);
    }

    void CopperGolem::DropEquipment(EntityLevel& level) {
        // MC CopperGolem.dropEquipment: super (nothing), then
        // dropPreservedEquipment — the carried stack (TransportItems'
        // setGuaranteedDrop) and the antenna's flower (OfferFlowerGoal's).
        GenericPathfinderMob::DropEquipment(level);
        MobEquipment::DropPreservedEquipment(*this);
    }

    // ══ Armadillo ══════════════════════════════════════════════════════════

    namespace {

        // MC EntityTypeTags.UNDEAD, flattened from
        // data/minecraft/tags/entity_type/{undead,skeletons,zombies}.json.
        // A tag lookup would be the general answer, but entity-type tags are
        // not loaded anywhere else in this port and this is their only reader.
        bool IsUndeadType(EntityTypeId t) {
            switch (t) {
                case EntityTypeId::Skeleton:
                case EntityTypeId::Stray:
                case EntityTypeId::WitherSkeleton:
                case EntityTypeId::SkeletonHorse:
                case EntityTypeId::Bogged:
                case EntityTypeId::Parched:
                case EntityTypeId::ZombieHorse:
                case EntityTypeId::CamelHusk:
                case EntityTypeId::Zombie:
                case EntityTypeId::ZombieVillager:
                case EntityTypeId::ZombifiedPiglin:
                case EntityTypeId::Zoglin:
                case EntityTypeId::Drowned:
                case EntityTypeId::Husk:
                case EntityTypeId::ZombieNautilus:
                case EntityTypeId::Wither:
                case EntityTypeId::Phantom:
                    return true;
                default:
                    return false;
            }
        }

    } // namespace

    int Armadillo::AnimationDuration(State s) {
        // MC Armadillo.ArmadilloState's third constructor argument.
        switch (s) {
            case State::Idle:      return 0;
            case State::Rolling:   return 10;
            case State::Scared:    return 50;
            case State::Unrolling: return 30;
        }
        return 0;
    }

    bool Armadillo::ShouldHideInShell(State s, int64_t ticksInState) {
        // MC's per-constant override. The asymmetry is the point: rolling up
        // hides the body a little AFTER the roll starts (5 of 10 ticks), while
        // unrolling shows it a little BEFORE the unroll finishes (26 of 30), so
        // the swap never happens on a visible frame.
        switch (s) {
            case State::Idle:      return false;
            case State::Rolling:   return ticksInState > 5;
            case State::Scared:    return true;
            case State::Unrolling: return ticksInState < 26;
        }
        return false;
    }

    Armadillo::Armadillo(EntityLevel* level)
        : GenericAnimal(EntityTypeId::Armadillo, level) {
        // NO GOALS — MC's Armadillo never registers any; the whole roll-up
        // life (ArmadilloBallUp, the scare sensor, the peek timers) is
        // ArmadilloAi's brain now, where MC keeps it.
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        m_brain = std::make_unique<Brain>();
        ArmadilloAi::InitBrain(*this, *m_brain);
    }

    void Armadillo::UpdateBrainActivity() { ArmadilloAi::UpdateActivity(*this); }

    void Armadillo::SwitchToState(State s) {
        if (m_state == s) return;
        m_state = s;
        // MC resets inStateTicks in onSyncedDataUpdated, i.e. on BOTH sides,
        // which is what keeps the client's shouldHideInShell in step with the
        // animation it is playing.
        m_inStateTicks = 0;
    }

    int64_t Armadillo::DangerTicksRemaining() const {
        // MC brain.getTimeUntilExpiry(DANGER_DETECTED_RECENTLY).
        const Brain* brain = GetBrain();
        return brain ? brain->GetTimeUntilExpiry(MemoryModule::DangerDetectedRecently)
                     : 0;
    }

    bool Armadillo::IsScaredBy(const LivingEntity& other) const {
        // MC inflates the ARMADILLO's box, not the other entity's.
        AABB box = GetAABB();
        box.min -= glm::vec3(7.0f, 2.0f, 7.0f);
        box.max += glm::vec3(7.0f, 2.0f, 7.0f);
        if (!box.Intersects(other.GetAABB())) return false;

        // MC's three cases: anything in EntityTypeTags.UNDEAD, whatever last
        // hurt it, and a player who is sprinting or riding. A walking player
        // deliberately does NOT scare it — that is how you get close enough to
        // brush a scute off.
        if (IsUndeadType(other.GetType())) return true;
        // An identity compare, not a resolve: "did this thing hurt me" is a
        // question about who `other` IS, and answering it must not depend on
        // the attacker currently being loaded. Also keeps this method const.
        if (LastHurtByMobRef().Matches(other)) return true;
        if (other.IsPlayer()) {
            // MC Armadillo.java:227-228 — a spectator never scares it, checked
            // BEFORE the sprint test. This mattered even before the spectator
            // filter landed in GetEntitiesInBox, because isScaredBy is reached
            // from a sensor that uses MC's THREE-arg getEntitiesOfClass, which
            // carries no NO_SPECTATORS predicate.
            if (other.IsSpectator()) return false;
            // MC also counts a player who is a passenger; nothing here rides
            // anything, so sprinting is the whole of the reachable condition.
            return other.IsSprinting();
        }
        return false;
    }

    const char* Armadillo::GetAmbientSound() const {
        return IsScared() ? "" : SoundEvents::ARMADILLO_AMBIENT;
    }

    const char* Armadillo::GetHurtSound(MobDamageSource) const {
        return IsScared() ? SoundEvents::ARMADILLO_HURT_REDUCED : SoundEvents::ARMADILLO_HURT;
    }

    void Armadillo::RollUp() {
        if (IsScared()) return;
        StopInPlace();
        ResetLove();
        GameEvent(GameEventId::EntityAction);   // MC rollUp
        MakeSound(SoundEvents::ARMADILLO_ROLL);
        SwitchToState(State::Rolling);
    }

    void Armadillo::RollOut() {
        if (!IsScared()) return;
        GameEvent(GameEventId::EntityAction);   // MC rollOut
        MakeSound(SoundEvents::ARMADILLO_UNROLL_FINISH);
        SwitchToState(State::Idle);
    }

    void Armadillo::Tick() {
        GenericAnimal::Tick();

        // MC clamps the head to the body while scared, so a rolled-up
        // armadillo does not track you from inside its shell.
        if (IsScared()) {
            yHeadRot = yBodyRot;
        }

        ++m_inStateTicks;
    }

    void Armadillo::SetupAnimationStates() {
        // MC Armadillo.setupAnimationStates.
        switch (m_state) {
            case State::Idle:
                Anim(MobAnim::RollOut).Stop();
                Anim(MobAnim::RollUp).Stop();
                Anim(MobAnim::Peek).Stop();
                break;
            case State::Rolling:
                Anim(MobAnim::RollOut).Stop();
                Anim(MobAnim::RollUp).StartIfStopped(tickCount);
                Anim(MobAnim::Peek).Stop();
                break;
            case State::Scared:
                Anim(MobAnim::RollOut).Stop();
                Anim(MobAnim::RollUp).Stop();
                if (m_peekReceivedClient) {
                    Anim(MobAnim::Peek).Stop();
                    m_peekReceivedClient = false;
                }
                if (m_inStateTicks == 0) {
                    // Entering SCARED from a roll-up: the peek clip is started
                    // and immediately fast-forwarded past its whole duration,
                    // so the armadillo holds the closed pose instead of
                    // replaying the peek every time it re-hides.
                    Anim(MobAnim::Peek).Start(tickCount);
                    Anim(MobAnim::Peek).FastForward(
                        AnimationDuration(State::Scared), 1.0f);
                } else {
                    Anim(MobAnim::Peek).StartIfStopped(tickCount);
                }
                break;
            case State::Unrolling:
                Anim(MobAnim::RollOut).StartIfStopped(tickCount);
                Anim(MobAnim::RollUp).Stop();
                Anim(MobAnim::Peek).Stop();
                break;
        }
    }

    void Armadillo::HandleEntityEvent(uint8_t id) {
        // MC Armadillo.handleEntityEvent: 64 is "peek now".
        if (id == 64) {
            m_peekReceivedClient = true;
            // MC: the client's ARMADILLO_PEEK, a local sound.
            if (m_level && m_level->IsClientSide()) {
                m_level->PlayLocalSound(position, SoundEvents::ARMADILLO_PEEK, GetSoundSource(), 1.0f, 1.0f, false);
            }
            return;
        }
        GenericAnimal::HandleEntityEvent(id);
    }

    bool Armadillo::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC Armadillo.hurtServer — the shell is worth about half the damage.
        if (IsScared()) amount = (amount - 1.0f) / 2.0f;

        const bool hurt = GenericAnimal::Hurt(source, amount, attacker);

        // MC Armadillo.actuallyHurt: being hit by something alive re-arms the
        // danger memory for 80 ticks and curls it up on the spot.
        if (hurt && !IsNoAi() && IsAlive() && dynamic_cast<LivingEntity*>(attacker)) {
            if (Brain* brain = GetBrain()) {
                brain->SetMemoryWithExpiry(MemoryModule::DangerDetectedRecently,
                                           true, 80);
            }
            if (CanStayRolledUp()) RollUp();
        }
        return hurt;
    }

    // ══ Allay ══════════════════════════════════════════════════════════════
    // Allay.cpp.

    // ══ HappyGhast ═════════════════════════════════════════════════════════

    namespace {
        // MC HappyGhast.HappyGhastLookControl — the adult's: squared up to a
        // quarter turn while on its still timeout (a platform keeps its
        // edges on the grid), turned bodily toward a look target, else
        // facing where it drifts (Ghast.faceMovementDirection).
        class HappyGhastLookControl final : public LookControl {
        public:
            explicit HappyGhastLookControl(HappyGhast* ghast) : LookControl(ghast), m_ghast(ghast) {}

            void Tick() override {
                if (m_ghast->IsOnStillTimeout()) {
                    // Mth.wrapDegrees90.
                    float closeAngle = std::fmod(m_ghast->yRot, 90.0f);
                    if (closeAngle >= 45.0f) closeAngle -= 90.0f;
                    if (closeAngle < -45.0f) closeAngle += 90.0f;
                    m_ghast->yRot -= closeAngle;
                    m_ghast->SetYHeadRot(m_ghast->yRot);
                } else if (m_lookAtCooldown > 0) {
                    --m_lookAtCooldown;
                    const double xdd = m_wantedX - m_ghast->position.x;
                    const double zdd = m_wantedZ - m_ghast->position.z;
                    m_ghast->yRot = -static_cast<float>(std::atan2(xdd, zdd)) * 57.295776f;
                    m_ghast->yBodyRot = m_ghast->yRot;
                    m_ghast->SetYHeadRot(m_ghast->yBodyRot);
                } else {
                    Ghast::FaceMovementDirection(*m_ghast);
                }
            }

        private:
            HappyGhast* m_ghast;
        };

        // MC EntityTypes HAPPY_GHAST .passengerAttachments: the harness's
        // four corners, front / left / back / right, in seat order.
        constexpr glm::dvec3 kHappyGhastSeats[HappyGhast::kMaxPassengers] = {
            {0.0, 4.0, 1.7}, {-1.7, 4.0, 0.0}, {0.0, 4.0, -1.7}, {1.7, 4.0, 0.0},
        };
    } // namespace

    HappyGhast::HappyGhast(EntityLevel* level)
        : GenericAnimal(EntityTypeId::HappyGhast, level) {
        // A fresh happy ghast is an adult until FinalizeSpawn/CreateBaby says
        // otherwise; the age poll in CustomServerAiStep swaps setups at the
        // boundary — the port's AgeableMob has no ageBoundaryReached hook, so
        // a ghastling runs its first tick on the adult set (invisible: one
        // tick, no goal completes).
        RegisterAdultGoals();
        // MC's constructor: the adult's HappyGhastLookControl.
        SetLookControl(std::make_unique<HappyGhastLookControl>(this));
    }

    void HappyGhast::RegisterAdultGoals() {
        // GenericAnimal's constructor registered the def-driven goal set (the
        // flying wander included); MC's adult table adds the float at
        // priority 3. Its tempt (HAPPY_GHAST_FOOD, snowballs) already comes
        // from the def's food list.
        m_goalSelector.AddGoal(3, std::make_unique<HappyGhastFloatGoal>(this));
    }

    void HappyGhast::AdultSetup() {
        // MC HappyGhast.adultGhastSetup: back to the goal set and the
        // HappyGhastLookControl, brain stopped and dropped. MC also swaps to
        // the GhastMoveControl; the def's FlyingMoveControl stands in for both
        // ages here (its shouldBeStopped hook is CustomServerAiStep's).
        m_brain.reset();
        m_goalSelector.Clear();
        m_targetSelector.Clear();
        RegisterGoals();
        RegisterAdultGoals();
        SetLookControl(std::make_unique<HappyGhastLookControl>(this));
    }

    void HappyGhast::BabySetup() {
        // MC HappyGhast.babyGhastSetup: the plain LookControl, the still
        // timeout dropped, no goals at all — the ghastling is pure brain (MC
        // keeps FlyingMoveControl(180, true), which is what the def already
        // applied; the BabyFlyingPathNavigation variant is not modelled).
        SetLookControl(std::make_unique<LookControl>(this));
        SetServerStillTimeout(0);
        m_goalSelector.Clear();
        m_targetSelector.Clear();
        m_brain = std::make_unique<Brain>();
        HappyGhastAi::InitBrain(*this, *m_brain);
    }

    void HappyGhast::CustomServerAiStep() {
        // MC calls the setups from ageBoundaryReached; the port polls. The
        // brain itself (baby only, as in MC's customServerAiStep) is ticked
        // by Mob::ServerAiStep whenever m_brain exists.
        if (IsBaby() != m_wasBaby) {
            m_wasBaby = IsBaby();
            if (m_wasBaby) BabySetup();
            else           AdultSetup();
        }
        // MC customServerAiStep: checkRestriction.
        CheckRestriction();
        // MC Ghast.GhastMoveControl.tick's shouldBeStopped (the adult's
        // control, built with this::isOnStillTimeout): the wander parks and
        // the ghast stops dead — Mob::ServerAiStep ticks the move control
        // right after this.
        if (!IsBaby() && IsOnStillTimeout()) {
            GetMoveControl().SetWait();
            StopInPlace();
        }
    }

    void HappyGhast::UpdateBrainActivity() { HappyGhastAi::UpdateActivity(*this); }

    void HappyGhast::SetServerStillTimeout(int ticks) {
        // MC setServerStillTimeout: going still sends the exact position at
        // once (syncPacketPositionCodec + ClientboundEntityPositionSyncPacket
        // — the tracker's next update, flagged here), then syncStayStillFlag.
        if (m_serverStillTimeout <= 0 && ticks > 0 && m_level && !m_level->IsClientSide()) {
            needsSync = true;
        }
        m_serverStillTimeout = ticks;
        m_staysStill = m_serverStillTimeout > 0;
    }

    bool HappyGhast::IsWearingHarness() const {
        // MC isWearingBodyArmor: anything in the BODY slot — for a happy
        // ghast only a harness (#harnesses) is equippable there.
        return !GetEquipment(EquipmentSlot::BODY).IsEmpty();
    }

    bool HappyGhast::IsRidden() const {
        if (IsVehicle()) return true;
        return m_level && m_level->IsClientSide() && !SyncedRiders().empty();
    }

    void HappyGhast::Tick() {
        GenericAnimal::Tick();
        // MC HappyGhast.tick's server half.
        if (!m_level || m_level->IsClientSide() || IsRemoved()) return;
        if (m_leashHolderTime > 0) --m_leashHolderTime;
        m_isLeashHolder = m_leashHolderTime > 0;   // setLeashHolder
        if (m_serverStillTimeout > 0) {
            // STILL_TIMEOUT_ON_LOAD_GRACE_PERIOD: a saved still timeout holds
            // through the first 60 ticks after loading, so a player who
            // logged out standing on it does not fall when the ghast drifts
            // before their own first move arrives.
            if (tickCount > kStillTimeoutOnLoadGracePeriod) --m_serverStillTimeout;
            SetServerStillTimeout(m_serverStillTimeout);
        }
        if (ScanPlayerAboveGhast()) SetServerStillTimeout(kMaxStillTimeout);
    }

    bool HappyGhast::ScanPlayerAboveGhast() const {
        // MC scanPlayerAboveGhast: any non-spectator player whose ROOT
        // vehicle (itself when not riding) stands within a box one block
        // wider than the ghast on each side, from just under its top to half
        // its height above — unless that root is a happy ghast (a rider on
        // another ghast parked beside this one does not hold it).
        if (!m_level) return false;
        const AABBd bb = GetAABBd();
        const double minX = bb.min.x - 1.0, minY = bb.max.y - 9.999999747378752E-6, minZ = bb.min.z - 1.0;
        const double maxX = bb.max.x + 1.0, maxY = bb.max.y + (bb.max.y - bb.min.y) / 2.0, maxZ = bb.max.z + 1.0;
        static thread_local std::vector<LivingEntity*> t_players;
        t_players.clear();
        m_level->GetPlayers(t_players);
        for (LivingEntity* player : t_players) {
            if (!player || player->IsSpectator()) continue;
            const Entity* root = player;
            while (root->GetVehicle()) root = root->GetVehicle();
            if (root->GetType() == EntityTypeId::HappyGhast) continue;
            const glm::dvec3& p = root->position;
            // AABB.contains(Vec3): min inclusive, max exclusive.
            if (p.x >= minX && p.x < maxX && p.y >= minY && p.y < maxY && p.z >= minZ && p.z < maxZ) {
                return true;
            }
        }
        return false;
    }

    void HappyGhast::CheckRestriction() {
        // MC checkRestriction: unleashed and unridden, the ghast keeps a home
        // around where it floats — 64 blocks bare, 32 harnessed or a baby —
        // re-centred once it strays past the radius + 16.
        if (IsLeashed() || IsVehicle()) return;
        const int radius = !IsBaby() && GetEquipment(EquipmentSlot::BODY).IsEmpty()
                               ? kLargeRestrictionRadius : kSmallRestrictionRadius;
        const glm::ivec3 here = BlockPosition();
        bool reCentre = !HasHome() || radius != GetHomeRadius();
        if (!reCentre) {
            // Vec3i.closerThan(pos, radius + 16): distSqr < d².
            const glm::ivec3 d = GetHomePosition() - here;
            const double distSqr = static_cast<double>(d.x) * d.x + static_cast<double>(d.y) * d.y +
                                   static_cast<double>(d.z) * d.z;
            const double limit = static_cast<double>(radius + kRestrictionRadiusBuffer);
            reCentre = !(distSqr < limit * limit);
        }
        if (reCentre) SetHomeTo(here, radius);
    }

    void HappyGhast::NotifyLeashHolder(Mob& leashee) {
        // MC notifyLeashHolder: a quad-leashed mob (the harness's four ropes)
        // keeps IS_LEASH_HOLDER up for five ticks.
        if (leashee.SupportQuadLeash()) m_leashHolderTime = 5;
    }

    // ── Riding ─────────────────────────────────────────────────────────────

    UseResult HappyGhast::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC HappyGhast.mobInteract.
        if (IsBaby()) return GenericAnimal::MobInteract(player, held);
        if (!held.IsEmpty()) {
            if (const auto interact = ItemRegistry::Get(held.itemId).interactLivingEntity) {
                const UseResult r = interact(held, *this);
                if (ConsumesAction(r)) return r;
            }
        }
        const bool sneaking = m_level && m_level->IsPlayerSneaking(player);
        if (IsWearingHarness() && !sneaking) {
            // doPlayerRide: the server seats the player (startRiding).
            if (m_level && !m_level->IsClientSide()) m_level->StartPlayerRiding(player, *this);
            return UseResult::Success;
        }
        return GenericAnimal::MobInteract(player, held);
    }

    bool HappyGhast::CanBeSteeredBy(const RiderControl& rider) const {
        // MC getControllingPassenger: harnessed, not on the still timeout,
        // the first passenger a player (the resolver's).
        (void)rider;
        return IsWearingHarness() && !IsOnStillTimeout();
    }

    glm::dvec3 HappyGhast::GetRiddenInput(const RiderControl& rider, const glm::dvec3& selfInput) {
        // MC HappyGhast.getRiddenInput: strafe as pressed; forward flies
        // along the view (cos pitch forward, -sin pitch up), backward the
        // reverse at half; jump adds half a unit of lift; all of it scaled
        // by 3.9 * FLYING_SPEED.
        (void)selfInput;
        const float strafe = rider.xxa;
        float forward = 0.0f;
        float up = 0.0f;
        if (rider.zza != 0.0f) {
            const float pitch = rider.xRot * 0.017453292f;
            float forwardLook = std::cos(pitch);
            float upLook = -std::sin(pitch);
            if (rider.zza < 0.0f) {
                forwardLook *= -0.5f;
                upLook *= -0.5f;
            }
            up = upLook;
            forward = forwardLook;
        }
        if (rider.jumping) up += 0.5f;
        const double scale = 3.9000000953674316 * GetAttributeValue(Attribute::FlyingSpeed);
        return glm::dvec3(static_cast<double>(strafe), static_cast<double>(up), static_cast<double>(forward)) * scale;
    }

    void HappyGhast::TickRidden(const RiderControl& rider, const glm::dvec3& riddenInput) {
        // MC HappyGhast.tickRidden: getRiddenRotation = (rider pitch / 2,
        // rider yaw); the body eases 8% of the way to the rider's yaw each
        // tick (a slow, heavy turn), the pitch follows at once.
        (void)riddenInput;
        const float targetXRot = rider.xRot * 0.5f;
        float y = yRot;
        const float diff = Mth::WrapDegrees(rider.yRot - y);
        y += diff * 0.08f;
        yRot = std::fmod(y, 360.0f);          // setRot
        xRot = std::fmod(targetXRot, 360.0f);
        yRotO = y;
        yBodyRot = y;
        SetYHeadRot(y);
    }

    bool HappyGhast::CanAddPassenger(const Entity& passenger) const {
        (void)passenger;
        return static_cast<int>(GetPassengers().size()) < kMaxPassengers;
    }

    glm::dvec3 HappyGhast::SeatAttachment(int slot) const {
        // EntityAttachments.getClamped(PASSENGER, index, yRot): the seat,
        // scaled with the entity (getScale: age scale × size) and turned by
        // the yaw (Vec3.yRot(-yRot)).
        const glm::dvec3 local = kHappyGhastSeats[std::clamp(slot, 0, kMaxPassengers - 1)] *
                                 (static_cast<double>(IsBaby() ? kBabyScale : 1.0f) * static_cast<double>(scale));
        const double a = -static_cast<double>(yRot) * static_cast<double>(Mth::kDegToRad);
        const double c = std::cos(a), s = std::sin(a);
        return glm::dvec3(local.x * c + local.z * s, local.y, local.z * c - local.x * s);
    }

    glm::dvec3 HappyGhast::GetPassengerAttachmentPoint(const Entity& passenger) const {
        // Entity.getDefaultPassengerAttachmentPoint: the seat by the
        // passenger's place in the list (indexOf; -1 clamps to the first).
        const auto& riders = GetPassengers();
        int index = 0;
        for (size_t i = 0; i < riders.size(); ++i) {
            if (riders[i] == &passenger) { index = static_cast<int>(i); break; }
        }
        return SeatAttachment(index);
    }

    glm::dvec3 HappyGhast::GetPassengerAttachmentForSlot(int slot, int total) const {
        (void)total;
        return SeatAttachment(slot);
    }

    glm::dvec3 HappyGhast::GetDismountLocationForPassenger(const LivingEntity& passenger) const {
        // MC: straight up onto its top, at the centre — the harness platform.
        (void)passenger;
        return glm::dvec3(position.x, GetAABBd().max.y, position.z);
    }

    void HappyGhast::OnPassengerAdded(Entity& passenger, bool wasVehicle) {
        // MC HappyGhast.addPassenger: the goggles come down for the first
        // rider; then (server) no player on top → the still timeout ends now,
        // else it is cut back to MAX_STILL_TIMEOUT.
        (void)passenger;
        if (!wasVehicle) PlaySound(SoundEvents::HARNESS_GOGGLES_DOWN, 1.0f, 1.0f);
        if (m_level && !m_level->IsClientSide()) {
            if (!ScanPlayerAboveGhast()) {
                SetServerStillTimeout(0);
            } else if (m_serverStillTimeout > kMaxStillTimeout) {
                SetServerStillTimeout(kMaxStillTimeout);
            }
        }
    }

    void HappyGhast::OnPassengerRemoved(Entity& passenger) {
        // MC HappyGhast.removePassenger: (server) hold still a moment for
        // the one getting off; the last rider gone, the home is dropped (the
        // next checkRestriction re-centres it where it now floats) and the
        // goggles go up.
        (void)passenger;
        if (m_level && !m_level->IsClientSide()) SetServerStillTimeout(kMaxStillTimeout);
        if (!IsVehicle()) {
            ClearHome();
            PlaySound(SoundEvents::HARNESS_GOGGLES_UP, 1.0f, 1.0f);
        }
    }

    bool HappyGhast::CanBeCollidedWithPlayer(double otherFeetY) const {
        // MC HappyGhast.canBeCollidedWith(player), client side: never a baby
        // or a dead one; its top is always solid to a player at or above it;
        // otherwise the whole box only while it holds still.
        if (IsBaby() || !IsAlive()) return false;
        if (otherFeetY >= GetAABBd().max.y) return true;
        return IsOnStillTimeout();
    }

    void HappyGhast::Travel(const glm::dvec3& input) {
        // MC HappyGhast.travel → LivingEntity.travelFlying(input, speed,
        // speed, speed) with speed = FLYING_SPEED * 5/3: no gravity in any
        // medium, water drag 0.8, lava 0.5, air 0.91.
        const float speed = static_cast<float>(GetAttributeValue(Attribute::FlyingSpeed)) * 5.0f / 3.0f;
        // The medium is read before the move, as MC's branches do.
        const bool inWater = IsInWater();
        const bool inLava = !inWater && IsInLava();
        MoveRelative(speed, input);
        Move(velocity);
        if (inWater) {
            velocity *= 0.800000011920929;
        } else if (inLava) {
            velocity *= 0.5;
        } else {
            velocity *= 0.9100000262260437;
        }
    }

    int HappyGhast::GetAmbientSoundInterval() const {
        const int interval = GenericAnimal::GetAmbientSoundInterval();
        return IsRidden() ? interval * 6 : interval;
    }

    void HappyGhast::TickHeadTurn(float yBodyRotTarget) {
        // MC HappyGhastBodyRotationControl.clientTick.
        if (IsRidden()) {
            SetYHeadRot(yRot);
            yBodyRot = yRot;
        }
        GenericAnimal::TickHeadTurn(yBodyRotTarget);
    }

} // namespace Game
