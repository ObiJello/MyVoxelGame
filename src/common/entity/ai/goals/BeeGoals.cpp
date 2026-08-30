// File: src/common/entity/ai/goals/BeeGoals.cpp
#include "common/entity/ai/goals/BeeGoals.hpp"

#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/world/pathfinder/Path.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"

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
        // MC findPos. The isHiveValid() steer-home branch is skipped with the
        // hive goals — a hive-less vanilla bee takes this branch too:
        // wanderDirection = getViewVector(0).
        const glm::vec3 view = Mth::ViewVector(m_bee->xRot, m_bee->yRot);
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
        // MC: level().isRaining() vetoes — no rain state exists in this
        // engine, so a bee pollinates in any weather (documented skip).
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
        // MC isRaining veto — no rain state (see CanBeeUse).
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
        m_hasHoverPos = false;
        // MC: resetTicksWithoutNectarSinceExitingHive.
        m_state->ticksWithoutNectarSinceExitingHive = 0;
    }

    void BeePollinateGoal::Stop() {
        if (HasPollinatedLongEnough()) {
            // MC setHasNectar(true) — the Bee mirrors this onto its anim
            // byte (bit 2) for the renderer's nectar texture swap.
            m_state->hasNectar = true;
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
        // MC: the BEE_POLLINATE sound roll — no sound system; the roll is
        // kept so the RNG stream matches.
        if (rng.NextFloat() < 0.05f &&
            m_successfulPollinatingTicks > m_lastSoundPlayedTick + 60) {
            m_lastSoundPlayedTick = m_successfulPollinatingTicks;
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
        // MC findNearbyFlower: BlockPos.withinManhattan(pos, 5, 5, 5),
        // nearest-first, with the unreachable-flower blacklist (600 ticks per
        // failed path). MC's exact within-shell visiting order is
        // approximated by a per-shell coordinate sweep; nearest-shell-first
        // is preserved, which is the property the behaviour rests on.
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
                        const auto it = m_unreachableFlowerCache.find(key);
                        if (it != m_unreachableFlowerCache.end() &&
                            level->GetGameTime() < it->second) {
                            freshCache[key] = it->second;
                            continue;
                        }
                        if (!AttractsBees(
                                blocks->GetBlockState(pos.x, pos.y, pos.z))) {
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
        // nectar — and isHiveValid(), treated as satisfied (DEVIATION, see
        // the header).
        if (m_state->numCropsGrownSincePollination >= 10) return false;
        if (!m_bee->Level()) return false;
        if (m_bee->Level()->Random().NextFloat() < 0.3f) return false;
        return m_state->hasNectar;
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
            // sweet_berry_bush + cave_vines). PITCHER_CROP is skipped (its
            // growth is the two-block PitcherCropBlock.grow, not an age
            // bump); CAVE_VINES are skipped (their growth is the bonemeal
            // path, which needs an ILevelWrite this seam does not carry).
            BlockState growState = belowState;   // sentinel: unchanged
            switch (belowState.Block()) {
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
                    const int age = belowState.GetIndex(PropertyId::AGE_1);
                    if (age < 1) {
                        growState = belowState.SetIndex(PropertyId::AGE_1, age + 1);
                    }
                    break;
                }
                default:
                    continue;
            }

            if (!(growState == belowState)) {
                // MC: level event 2011 (BEE_GROWING particles — no particle
                // system), setBlockAndUpdate, incrementNumCropsGrown.
                level->SetBlockState(belowPos, growState);
                ++m_state->numCropsGrownSincePollination;
            }
        }
    }

} // namespace Game
