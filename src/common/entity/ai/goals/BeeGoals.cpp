// File: src/common/entity/ai/goals/BeeGoals.cpp
#include "common/entity/ai/goals/BeeGoals.hpp"
#include "common/sound/LevelEventSounds.hpp"

#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/world/pathfinder/Path.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/entity/ai/village/PoiManager.hpp"
#include "common/world/block/entity/BeehiveBlockEntity.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/block/BlockRegistry.hpp"

#include <algorithm>

#include <cmath>
#include <cstdlib>

namespace Game {

    // ── BeeAttackGoal ──────────────────────────────────────────────────────

    BeeAttackGoal::BeeAttackGoal(Bee* bee, double speedModifier, bool trackTarget)
        : MeleeAttackGoal(bee, speedModifier, trackTarget), m_bee(bee) {}

    bool BeeAttackGoal::CanUse() {
        return MeleeAttackGoal::CanUse() && m_bee->IsAngry() && !m_bee->HasStung();
    }

    bool BeeAttackGoal::CanContinueToUse() {
        return MeleeAttackGoal::CanContinueToUse() && m_bee->IsAngry() &&
               !m_bee->HasStung();
    }

    // ── BeeHurtByOtherGoal ─────────────────────────────────────────────────

    BeeHurtByOtherGoal::BeeHurtByOtherGoal(Bee* bee)
        : HurtByTargetGoal(bee), m_bee(bee) {}

    bool BeeHurtByOtherGoal::CanContinueToUse() {
        // MC: the grudge only lasts as long as the anger window.
        return m_bee->IsAngry() && HurtByTargetGoal::CanContinueToUse();
    }

    void BeeHurtByOtherGoal::AlertOther(Mob& other, LivingEntity& attacker) {
        // MC: `other instanceof Bee && this.mob.hasLineOfSight(hurtByMob)` —
        // the base's alert sweep already filters to the same TYPE; the LOS
        // test is the HURT bee's, not the alerted one's.
        if (m_mob->GetSensing().HasLineOfSight(attacker)) {
            other.SetTarget(&attacker);
        }
    }

    // ── BeeBecomeAngryTargetGoal ───────────────────────────────────────────

    BeeBecomeAngryTargetGoal::BeeBecomeAngryTargetGoal(Bee* bee)
        : NearestAttackableTargetGoal(bee, /*mustSee=*/true, /*mustReach=*/false,
                                      /*randomInterval=*/10),
          m_bee(bee) {
        // MC's sixth constructor argument: bee::isAngryAt.
        SetSelector([](Mob& mob, const LivingEntity& target) {
            return static_cast<Bee&>(mob).IsAngryAt(target);
        });
    }

    bool BeeBecomeAngryTargetGoal::BeeCanTarget() const {
        // MC beeCanTarget: angry and not yet stung.
        return m_bee->IsAngry() && !m_bee->HasStung();
    }

    bool BeeBecomeAngryTargetGoal::CanUse() {
        return BeeCanTarget() && NearestAttackableTargetGoal::CanUse();
    }

    bool BeeBecomeAngryTargetGoal::CanContinueToUse() {
        // MC: keep only while the bee can still target AND a target stands;
        // otherwise drop the remembered target outright.
        if (BeeCanTarget() && m_bee->GetTarget() != nullptr) {
            return NearestAttackableTargetGoal::CanContinueToUse();
        }
        m_targetMob = nullptr;
        return false;
    }

    // ── The flower/crop set ────────────────────────────────────────────────

    namespace {
        // Pack a block position into one key — the shape of MC's
        // BlockPos.asLong for the unreachable-flower cache (26/26/12 bits is
        // overkill here; any collision-free packing serves).
        uint64_t PackPos(const glm::ivec3& p) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(p.x)) << 38) ^
                   (static_cast<uint64_t>(static_cast<uint32_t>(p.z)) << 12) ^
                   static_cast<uint64_t>(static_cast<uint32_t>(p.y) & 0xFFF);
        }
    } // namespace

    bool AttractsBees(BlockState state) {
        // MC Bee.attractsBees over the BEE_ATTRACTIVE tag
        // (data/minecraft/tags/block/bee_attractive.json, vendored under
        // data/), waterlogged states excluded, sunflower upper-half only.
        switch (state.Block()) {
            // #small_flowers
            case BlockID::Dandelion:
            case BlockID::OpenEyeblossom:
            case BlockID::Poppy:
            case BlockID::BlueOrchid:
            case BlockID::Allium:
            case BlockID::AzureBluet:
            case BlockID::RedTulip:
            case BlockID::OrangeTulip:
            case BlockID::WhiteTulip:
            case BlockID::PinkTulip:
            case BlockID::OxeyeDaisy:
            case BlockID::Cornflower:
            case BlockID::LilyOfTheValley:
            case BlockID::WitherRose:
            case BlockID::Torchflower:
            // tall flowers and the rest of the tag
            case BlockID::Lilac:
            case BlockID::Peony:
            case BlockID::RoseBush:
            case BlockID::PitcherPlant:
            case BlockID::FloweringAzaleaLeaves:
            case BlockID::FloweringAzalea:
            case BlockID::MangrovePropagule:
            case BlockID::CherryLeaves:
            case BlockID::PinkPetals:
            case BlockID::Wildflowers:
            case BlockID::ChorusFlower:
            case BlockID::SporeBlossom:
            case BlockID::CactusFlower:
                break;
            case BlockID::Sunflower:
                // MC: only the UPPER half of a sunflower attracts.
                return state.GetName(PropertyId::DOUBLE_BLOCK_HALF) == "upper";
            default:
                return false;
        }
        // MC: getValueOrElse(WATERLOGGED, false) — a drowned flower is no
        // flower.
        if (state.HasProperty(PropertyId::WATERLOGGED) &&
            state.GetName(PropertyId::WATERLOGGED) == "true") {
            return false;
        }
        return true;
    }

    void BeeFlowerState::DropFlower(JavaRandom& rng) {
        // MC Bee.dropFlower: clear the memory and 20..60 ticks before the
        // next flower search.
        hasSavedFlowerPos = false;
        remainingCooldownBeforeLocatingNewFlower = rng.NextInt(20, 60);
    }

    // ── BaseBeeGoal ────────────────────────────────────────────────────────

    BaseBeeGoal::BaseBeeGoal(Bee* bee, std::shared_ptr<BeeFlowerState> state)
        : m_bee(bee), m_state(std::move(state)) {}

    bool BaseBeeGoal::CanUse() { return CanBeeUse() && !m_bee->IsAngry(); }

    bool BaseBeeGoal::CanContinueToUse() {
        return CanBeeContinueToUse() && !m_bee->IsAngry();
    }

    // ── BeeWanderGoal ──────────────────────────────────────────────────────

    BeeWanderGoal::BeeWanderGoal(Bee* bee, std::shared_ptr<BeeFlowerState> state)
        : m_bee(bee), m_state(std::move(state)) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool BeeWanderGoal::CanUse() {
        return m_bee->GetNavigation().IsDone() && m_bee->Level() &&
               m_bee->Level()->Random().NextInt(10) == 0;
    }

    bool BeeWanderGoal::CanContinueToUse() {
        return m_bee->GetNavigation().IsInProgress();
    }

    void BeeWanderGoal::Start() {
        // MC: moveTo(createPath(pos, 1), 1.0).
        if (std::optional<glm::dvec3> target = FindPos()) {
            const glm::ivec3 pos(static_cast<int>(std::floor(target->x)),
                                 static_cast<int>(std::floor(target->y)),
                                 static_cast<int>(std::floor(target->z)));
            m_bee->GetNavigation().MoveTo(m_bee->GetNavigation().CreatePath(pos, 1),
                                          1.0);
        }
    }

    std::optional<glm::dvec3> BeeWanderGoal::FindPos() const {
        // MC findPos: beyond the wander threshold (48 - 16 for a bee with no
        // hive and no flower, 48 - 24 otherwise) of a valid hive, steer
        // home; else along the view.
        glm::vec3 view = Mth::ViewVector(m_bee->xRot, m_bee->yRot);
        const int threshold = 48 - (!m_bee->HasHive() && !m_state->hasSavedFlowerPos ? 16 : 24);
        if (m_bee->IsHiveValid() && !m_bee->CloserThan(*m_bee->GetHivePos(), threshold)) {
            const glm::dvec3 hive = glm::dvec3(*m_bee->GetHivePos()) + glm::dvec3(0.5);
            const glm::dvec3 d = hive - m_bee->position;
            const double len = glm::length(d);
            view = len < 1.0e-5 ? glm::vec3(0.0f) : glm::vec3(d / len);
        }
        std::optional<glm::dvec3> groundBased = RandomPos::GetHoverPos(
            *m_bee, 8, 7, view.x, view.z, Mth::kPi / 2.0f, 3, 1);
        if (groundBased) return groundBased;
        return RandomPos::GetAirAndWaterPos(*m_bee, 8, 4, -2, view.x, view.z,
                                            Mth::kPi / 2.0f);
    }

    // ── BeePollinateGoal ───────────────────────────────────────────────────

    BeePollinateGoal::BeePollinateGoal(Bee* bee,
                                       std::shared_ptr<BeeFlowerState> state)
        : BaseBeeGoal(bee, std::move(state)) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool BeePollinateGoal::CanBeeUse() {
        if (m_state->remainingCooldownBeforeLocatingNewFlower > 0) return false;
        if (m_state->hasNectar) return false;
        // MC: level().isRaining() vetoes — no pollinating in the rain.
        if (m_bee->Level() && m_bee->Level()->IsRaining()) return false;
        std::optional<glm::ivec3> nearby = FindNearbyFlower();
        if (nearby) {
            m_state->hasSavedFlowerPos = true;
            m_state->savedFlowerPos = *nearby;
            m_bee->GetNavigation().MoveTo(nearby->x + 0.5, nearby->y + 0.5,
                                          nearby->z + 0.5, 1.2);
            return true;
        }
        // MC: nothing near — 20..60 ticks before trying again.
        m_state->remainingCooldownBeforeLocatingNewFlower =
            m_bee->Level()->Random().NextInt(20, 60);
        return false;
    }

    bool BeePollinateGoal::CanBeeContinueToUse() {
        if (!m_pollinating) return false;
        if (!m_state->hasSavedFlowerPos) return false;
        // MC isRaining veto: the rain breaks off a pollination.
        if (m_bee->Level() && m_bee->Level()->IsRaining()) return false;
        if (HasPollinatedLongEnough()) {
            return m_bee->Level()->Random().NextFloat() < 0.2f;
        }
        return true;
    }

    void BeePollinateGoal::Start() {
        m_successfulPollinatingTicks = 0;
        m_pollinatingTicks = 0;
        m_lastSoundPlayedTick = 0;
        m_pollinating = true;
        // (MC keeps hoverPos across uses.)
        // MC: resetTicksWithoutNectarSinceExitingHive.
        m_state->ticksWithoutNectarSinceExitingHive = 0;
    }

    void BeePollinateGoal::Stop() {
        if (HasPollinatedLongEnough()) {
            // MC setHasNectar(true) — the Bee mirrors this onto its anim
            // byte (bit 2) for the renderer's nectar texture swap.
            // MC setHasNectar(true) also restarts the no-nectar clock.
            m_state->hasNectar = true;
            m_state->ticksWithoutNectarSinceExitingHive = 0;
        }
        m_pollinating = false;
        m_bee->GetNavigation().Stop();
        m_state->remainingCooldownBeforeLocatingNewFlower = 200;
    }

    void BeePollinateGoal::Tick() {
        // MC BeePollinateGoal.tick, transcribed.
        if (!m_state->hasSavedFlowerPos) return;
        JavaRandom& rng = m_bee->Level()->Random();

        ++m_pollinatingTicks;
        if (m_pollinatingTicks > 600) {
            m_state->DropFlower(rng);
            m_pollinating = false;
            m_state->remainingCooldownBeforeLocatingNewFlower = 200;
            return;
        }

        // MC: hover 0.6 above the flower's bottom centre.
        const glm::dvec3 flowerPos(m_state->savedFlowerPos.x + 0.5,
                                   m_state->savedFlowerPos.y + 0.6,
                                   m_state->savedFlowerPos.z + 0.5);
        if (glm::length(flowerPos - m_bee->position) > 1.0) {
            m_hoverPos = flowerPos;
            m_hasHoverPos = true;
            SetWantedPos();
            return;
        }

        if (!m_hasHoverPos) {
            m_hoverPos = flowerPos;
            m_hasHoverPos = true;
        }

        const bool arrived = glm::length(m_hoverPos - m_bee->position) <= 0.1;
        bool shouldSetWantedPos = true;
        if (!arrived && m_pollinatingTicks > 600) {
            m_state->DropFlower(rng);
            return;
        }

        if (arrived) {
            // MC: 1-in-25 chance to jitter to a new hover point ±1/3 block.
            if (rng.NextInt(25) == 0) {
                m_hoverPos = glm::dvec3(flowerPos.x + GetOffset(), flowerPos.y,
                                        flowerPos.z + GetOffset());
                m_bee->GetNavigation().Stop();
            } else {
                shouldSetWantedPos = false;
            }
            m_bee->GetLookControl().SetLookAt(flowerPos.x, flowerPos.y,
                                              flowerPos.z);
        }

        if (shouldSetWantedPos) {
            SetWantedPos();
        }

        ++m_successfulPollinatingTicks;
        // MC: now and then, the BEE_POLLINATE buzz.
        if (rng.NextFloat() < 0.05f &&
            m_successfulPollinatingTicks > m_lastSoundPlayedTick + 60) {
            m_lastSoundPlayedTick = m_successfulPollinatingTicks;
            m_bee->PlaySound(SoundEvents::BEE_POLLINATE, 1.0f, 1.0f);
        }
    }

    void BeePollinateGoal::SetWantedPos() {
        m_bee->GetMoveControl().SetWantedPosition(m_hoverPos.x, m_hoverPos.y,
                                                  m_hoverPos.z, 0.35);
    }

    float BeePollinateGoal::GetOffset() const {
        // MC: (random * 2 - 1) * 0.33333334F.
        return (m_bee->Level()->Random().NextFloat() * 2.0f - 1.0f) * 0.33333334f;
    }

    std::optional<glm::ivec3> BeePollinateGoal::FindNearbyFlower() {
        // MC findNearbyFlower: findBlocksInBoxByManhattanDistance(pos, 5)
        // (BlockPos.manhattanOrdered — depth 0..15, x ascending, y
        // ascending, +z then its mirror), filtered to bee-attractive states,
        // then the unreachable-flower blacklist (600 ticks per failed path).
        EntityLevel* level = m_bee->Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (!blocks) return std::nullopt;

        const glm::ivec3 origin = m_bee->BlockPosition();
        std::unordered_map<uint64_t, int64_t> freshCache;
        std::optional<glm::ivec3> found;

        for (int dist = 0; dist <= 15 && !found; ++dist) {
            for (int dx = -5; dx <= 5 && !found; ++dx) {
                for (int dy = -5; dy <= 5 && !found; ++dy) {
                    const int adz = dist - std::abs(dx) - std::abs(dy);
                    if (adz < 0 || adz > 5) continue;
                    for (int s = 0; s < (adz == 0 ? 1 : 2); ++s) {
                        const int dz = (s == 0) ? adz : -adz;
                        const glm::ivec3 pos = origin + glm::ivec3(dx, dy, dz);
                        const uint64_t key = PackPos(pos);
                        if (!AttractsBees(
                                blocks->GetBlockState(pos.x, pos.y, pos.z))) {
                            continue;
                        }
                        const auto it = m_unreachableFlowerCache.find(key);
                        if (it != m_unreachableFlowerCache.end() &&
                            level->GetGameTime() < it->second) {
                            freshCache[key] = it->second;
                            continue;
                        }
                        std::optional<Path> path =
                            m_bee->GetNavigation().CreatePath(pos, 1);
                        if (path && path->CanReach()) {
                            found = pos;
                            break;
                        }
                        freshCache[key] = level->GetGameTime() + 600;
                    }
                }
            }
        }

        m_unreachableFlowerCache = std::move(freshCache);
        return found;
    }

    // ── ValidateFlowerGoal ─────────────────────────────────────────────────

    ValidateFlowerGoal::ValidateFlowerGoal(Bee* bee,
                                           std::shared_ptr<BeeFlowerState> state)
        : BaseBeeGoal(bee, std::move(state)),
          m_validateFlowerCooldown(
              bee->Level() ? bee->Level()->Random().NextInt(20, 40) : 30) {}

    bool ValidateFlowerGoal::CanBeeUse() {
        return m_bee->Level() &&
               m_bee->Level()->GetGameTime() >
                   m_lastValidateTick + m_validateFlowerCooldown;
    }

    void ValidateFlowerGoal::Start() {
        EntityLevel* level = m_bee->Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (blocks && m_state->hasSavedFlowerPos) {
            const glm::ivec3& p = m_state->savedFlowerPos;
            // MC: only judge a LOADED position — an unloaded flower is not a
            // missing flower.
            if (blocks->IsPositionLoaded(p.x, p.y, p.z) &&
                !AttractsBees(blocks->GetBlockState(p.x, p.y, p.z))) {
                m_state->DropFlower(level->Random());
            }
        }
        if (level) m_lastValidateTick = level->GetGameTime();
    }

    // ── BeeGrowCropGoal ────────────────────────────────────────────────────

    BeeGrowCropGoal::BeeGrowCropGoal(Bee* bee,
                                     std::shared_ptr<BeeFlowerState> state)
        : BaseBeeGoal(bee, std::move(state)) {}

    bool BeeGrowCropGoal::CanBeeUse() {
        // MC canBeeUse: under the 10-crop cap, a 70% start roll, carrying
        // nectar to a valid hive.
        if (m_state->numCropsGrownSincePollination >= 10) return false;
        if (!m_bee->Level()) return false;
        if (m_bee->Level()->Random().NextFloat() < 0.3f) return false;
        return m_state->hasNectar && m_bee->IsHiveValid();
    }

    void BeeGrowCropGoal::Tick() {
        // MC BeeGrowCropGoal.tick: a 1-in-adjusted(30) roll, then the two
        // blocks below the bee, aging the first BEE_GROWABLES one it finds.
        EntityLevel* level = m_bee->Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (!blocks) return;
        if (level->Random().NextInt(AdjustedTickDelay(30)) != 0) return;

        for (int i = 1; i <= 2; ++i) {
            const glm::ivec3 belowPos =
                m_bee->BlockPosition() - glm::ivec3(0, i, 0);
            const BlockState belowState =
                blocks->GetBlockState(belowPos.x, belowPos.y, belowPos.z);

            // MC's per-family growth (#bee_growables = #crops +
            // sweet_berry_bush + cave_vines). PITCHER_CROP is in #crops but
            // is neither a CropBlock nor a StemBlock, so MC grows nothing
            // there either; the cave vines take the MOB bonemeal path.
            BlockState growState = belowState;   // sentinel: unchanged
            switch (belowState.Block()) {
                case BlockID::CaveVines:
                case BlockID::CaveVinesPlant: {
                    const Block& block = BlockRegistry::Get(belowState.Block());
                    ILevelWrite* write = level->MutableBlocks();
                    if (write && block.isValidBonemealTarget && block.performBonemeal &&
                        block.isValidBonemealTarget(*write, belowPos, belowState)) {
                        block.performBonemeal(*write, belowPos, belowState, level->Random());
                        growState = write->GetBlockState(belowPos.x, belowPos.y, belowPos.z);
                        // MC: growState = the cell after the bonemeal — set
                        // back below even when unchanged-looking.
                        level->PlayLevelEvent(nullptr, LevelEvent::PARTICLES_BEE_GROWTH, belowPos, 15);
                        level->SetBlockState(belowPos, growState);
                        ++m_state->numCropsGrownSincePollination;
                    }
                    continue;
                }
                case BlockID::Wheat:
                case BlockID::Carrots:
                case BlockID::Potatoes:
                case BlockID::MelonStem:
                case BlockID::PumpkinStem: {
                    const int age = belowState.GetIndex(PropertyId::AGE_7);
                    if (age < 7) {
                        growState = belowState.SetIndex(PropertyId::AGE_7, age + 1);
                    }
                    break;
                }
                case BlockID::Beetroots:
                case BlockID::SweetBerryBush: {
                    const int age = belowState.GetIndex(PropertyId::AGE_3);
                    if (age < 3) {
                        growState = belowState.SetIndex(PropertyId::AGE_3, age + 1);
                    }
                    break;
                }
                case BlockID::TorchflowerCrop: {
                    // TorchflowerCropBlock: max age 2 over an AGE_1
                    // property, so it is never "max age" — age 1 grows into
                    // the torchflower itself (getStateForAge(2)).
                    const int age = belowState.GetIndex(PropertyId::AGE_1);
                    growState = age < 1 ? belowState.SetIndex(PropertyId::AGE_1, age + 1)
                                        : BlockStates::Default(BlockID::Torchflower);
                    break;
                }
                default:
                    continue;
            }

            if (!(growState == belowState)) {
                // MC: level event 2011 (the BEE_GROWING happy villagers, 15
                // of them), setBlockAndUpdate, incrementNumCropsGrown.
                level->PlayLevelEvent(nullptr, LevelEvent::PARTICLES_BEE_GROWTH, belowPos, 15);
                level->SetBlockState(belowPos, growState);
                ++m_state->numCropsGrownSincePollination;
            }
        }
    }

    // ── BeeGoToKnownFlowerGoal ─────────────────────────────────────────────

    BeeGoToKnownFlowerGoal::BeeGoToKnownFlowerGoal(Bee* bee, std::shared_ptr<BeeFlowerState> state)
        : BaseBeeGoal(bee, std::move(state)) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool BeeGoToKnownFlowerGoal::CanBeeUse() {
        // MC: a remembered flower, no home restriction, wantsToGoToKnownFlower
        // (ticksWithoutNectarSinceExitingHive > 600), not already within 2.
        return m_state->hasSavedFlowerPos && !m_bee->HasHome() &&
               m_state->ticksWithoutNectarSinceExitingHive > 600 &&
               !m_bee->CloserThan(m_state->savedFlowerPos, 2);
    }

    void BeeGoToKnownFlowerGoal::Start() {
        m_travellingTicks = 0;
        BaseBeeGoal::Start();
    }

    void BeeGoToKnownFlowerGoal::Stop() {
        m_travellingTicks = 0;
        m_bee->GetNavigation().Stop();
    }

    void BeeGoToKnownFlowerGoal::Tick() {
        if (!m_state->hasSavedFlowerPos || !m_bee->Level()) return;
        ++m_travellingTicks;
        if (m_travellingTicks > AdjustedTickDelay(kMaxTravellingTicks)) {
            m_state->DropFlower(m_bee->Level()->Random());
        } else if (!m_bee->GetNavigation().IsInProgress()) {
            if (m_bee->IsTooFarAway(m_state->savedFlowerPos)) {
                m_state->DropFlower(m_bee->Level()->Random());
            } else {
                m_bee->PathfindRandomlyTowards(m_state->savedFlowerPos);
            }
        }
    }

    // ── BeeEnterHiveGoal ───────────────────────────────────────────────────

    BeeEnterHiveGoal::BeeEnterHiveGoal(Bee* bee, std::shared_ptr<BeeFlowerState> state)
        : BaseBeeGoal(bee, std::move(state)) {}

    bool BeeEnterHiveGoal::CanBeeUse() {
        // MC: a hive, the wish to go in, and the bee within 2 of its centre;
        // a full hive is forgotten.
        if (!m_bee->HasHive() || !m_bee->WantsToEnterHive()) return false;
        const glm::dvec3 center = glm::dvec3(*m_bee->GetHivePos()) + glm::dvec3(0.5);
        const glm::dvec3 d = center - m_bee->position;
        if (glm::dot(d, d) >= 4.0) return false;
        if (BeehiveBlockEntity* hive = m_bee->GetBeehive()) {
            if (!hive->IsFull()) return true;
            m_bee->ClearHivePos();
        }
        return false;
    }

    void BeeEnterHiveGoal::Start() {
        EntityLevel* level = m_bee->Level();
        ILevelWrite* write = level ? level->MutableBlocks() : nullptr;
        if (BeehiveBlockEntity* hive = m_bee->GetBeehive(); hive && write) {
            hive->AddOccupant(*m_bee, *write);
        }
    }

    // ── ValidateHiveGoal ───────────────────────────────────────────────────

    ValidateHiveGoal::ValidateHiveGoal(Bee* bee, std::shared_ptr<BeeFlowerState> state)
        : BaseBeeGoal(bee, std::move(state)),
          m_validateHiveCooldown(bee->Level() ? bee->Level()->Random().NextInt(20, 40) : 30) {}

    bool ValidateHiveGoal::CanBeeUse() {
        return m_bee->Level() && m_bee->Level()->GetGameTime() > m_lastValidateTick + m_validateHiveCooldown;
    }

    void ValidateHiveGoal::Start() {
        EntityLevel* level = m_bee->Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (m_bee->HasHive() && blocks) {
            const glm::ivec3& p = *m_bee->GetHivePos();
            if (blocks->IsPositionLoaded(p.x, p.y, p.z) && !m_bee->IsHiveValid()) m_bee->DropHive();
        }
        if (level) m_lastValidateTick = level->GetGameTime();
    }

    // ── BeeLocateHiveGoal ──────────────────────────────────────────────────

    BeeLocateHiveGoal::BeeLocateHiveGoal(Bee* bee, std::shared_ptr<BeeFlowerState> state)
        : BaseBeeGoal(bee, std::move(state)) {}

    bool BeeLocateHiveGoal::CanBeeUse() {
        return m_bee->GetRemainingCooldownBeforeLocatingNewHive() == 0 && !m_bee->HasHive() &&
               m_bee->WantsToEnterHive();
    }

    void BeeLocateHiveGoal::Start() {
        // MC start: 200 ticks to the next search; the nearest hive with room
        // that is not blacklisted — or, all blacklisted, the nearest with
        // the blacklist cleared.
        m_bee->SetRemainingCooldownBeforeLocatingNewHive(200);
        const std::vector<glm::ivec3> hives = FindNearbyHivesWithSpace();
        if (hives.empty()) return;
        BeeGoToHiveGoal* goTo = m_bee->GoToHiveGoal();
        for (const glm::ivec3& pos : hives) {
            if (!goTo || !goTo->IsTargetBlacklisted(pos)) {
                m_bee->SetHivePos(pos);
                return;
            }
        }
        if (goTo) goTo->ClearBlacklist();
        m_bee->SetHivePos(hives.front());
    }

    std::vector<glm::ivec3> BeeLocateHiveGoal::FindNearbyHivesWithSpace() const {
        std::vector<glm::ivec3> out;
        EntityLevel* level = m_bee->Level();
        PoiManager* poi = level ? level->GetPoiManager() : nullptr;
        if (!poi) return out;
        const glm::ivec3 beePos = m_bee->BlockPosition();
        for (const PoiManager::Record* r : poi->GetInRange(IsBeeHome, beePos, 20, PoiManager::Occupancy::Any)) {
            if (r && m_bee->DoesHiveHaveSpace(r->pos)) out.push_back(r->pos);
        }
        // MC sorted(comparingDouble(pos.distSqr(beePos))) — a stable sort.
        std::stable_sort(out.begin(), out.end(), [&](const glm::ivec3& a, const glm::ivec3& b) {
            const glm::dvec3 da = glm::dvec3(a - beePos), db = glm::dvec3(b - beePos);
            return glm::dot(da, da) < glm::dot(db, db);
        });
        return out;
    }

    // ── BeeGoToHiveGoal ────────────────────────────────────────────────────

    BeeGoToHiveGoal::BeeGoToHiveGoal(Bee* bee, std::shared_ptr<BeeFlowerState> state)
        : BaseBeeGoal(bee, std::move(state)) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool BeeGoToHiveGoal::CanBeeUse() {
        if (!m_bee->HasHive()) return false;
        const glm::ivec3 hive = *m_bee->GetHivePos();
        if (m_bee->IsTooFarAway(hive) || m_bee->HasHome() || !m_bee->WantsToEnterHive()) return false;
        if (HasReachedTarget(hive)) return false;
        // BlockTags.BEEHIVES: bee_nest, beehive.
        EntityLevel* level = m_bee->Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (!blocks) return false;
        const BlockID id = blocks->GetBlock(hive.x, hive.y, hive.z);
        return id == BlockID::BeeNest || id == BlockID::Beehive;
    }

    void BeeGoToHiveGoal::Start() {
        m_travellingTicks = 0;
        m_ticksStuck = 0;
        BaseBeeGoal::Start();
    }

    void BeeGoToHiveGoal::Stop() {
        m_travellingTicks = 0;
        m_ticksStuck = 0;
        m_bee->GetNavigation().Stop();
    }

    void BeeGoToHiveGoal::Tick() {
        if (!m_bee->HasHive()) return;
        const glm::ivec3 hive = *m_bee->GetHivePos();
        ++m_travellingTicks;
        if (m_travellingTicks > AdjustedTickDelay(kMaxTravellingTicks)) {
            DropAndBlacklistHive();
            return;
        }
        if (m_bee->GetNavigation().IsInProgress()) return;
        if (!m_bee->CloserThan(hive, 16)) {
            if (m_bee->IsTooFarAway(hive)) m_bee->DropHive();
            else m_bee->PathfindRandomlyTowards(hive);
            return;
        }
        if (!PathfindDirectlyTowards(hive)) {
            DropAndBlacklistHive();
        } else if (const Path* path = m_bee->GetNavigation().GetPath(); m_lastPath && path && path->SameAs(*m_lastPath)) {
            if (++m_ticksStuck > 60) {
                m_bee->DropHive();
                m_ticksStuck = 0;
            }
        } else {
            if (path) m_lastPath = *path;
            else m_lastPath.reset();
        }
    }

    bool BeeGoToHiveGoal::PathfindDirectlyTowards(const glm::ivec3& target) {
        // MC: closeEnough 1 within 3 blocks, else 2; moveTo(x, y, z, it, 1.0).
        const int closeEnough = m_bee->CloserThan(target, 3) ? 1 : 2;
        PathNavigation& nav = m_bee->GetNavigation();
        nav.MoveTo(nav.CreatePath(target, closeEnough), 1.0);
        const Path* path = nav.GetPath();
        return path && path->CanReach();
    }

    bool BeeGoToHiveGoal::IsTargetBlacklisted(const glm::ivec3& pos) const {
        return std::find(m_blacklistedTargets.begin(), m_blacklistedTargets.end(), pos) != m_blacklistedTargets.end();
    }

    void BeeGoToHiveGoal::BlacklistTarget(const glm::ivec3& pos) {
        m_blacklistedTargets.push_back(pos);
        while (m_blacklistedTargets.size() > 3) m_blacklistedTargets.erase(m_blacklistedTargets.begin());
    }

    void BeeGoToHiveGoal::DropAndBlacklistHive() {
        if (m_bee->HasHive()) BlacklistTarget(*m_bee->GetHivePos());
        m_bee->DropHive();
    }

    bool BeeGoToHiveGoal::HasReachedTarget(const glm::ivec3& target) const {
        if (m_bee->CloserThan(target, 2)) return true;
        const Path* path = m_bee->GetNavigation().GetPath();
        return path && path->GetTarget() == target && path->CanReach() && path->IsDone();
    }

} // namespace Game
