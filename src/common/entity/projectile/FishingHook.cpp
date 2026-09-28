// File: src/common/entity/projectile/FishingHook.cpp
//
// MC FishingHook, transcribed — see the header for the shape and the engine
// divergences. Reference: minecraft_code_26.3-pre-2/decompiled_net/minecraft/
// world/entity/projectile/FishingHook.java.
#include "common/entity/projectile/FishingHook.hpp"

#include "common/core/Mth.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/ItemEntity.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace Game {

    namespace {

        // ── MC Player.fishing, as a registry ───────────────────────────────
        //
        // MC keeps the hook on its owner (updateOwnerInfo). The owner here is
        // a server-side view the level rebuilds, or on the client a player
        // this common code cannot see at all, so the link lives beside the
        // hooks instead: one entry per live hook with an owner, keyed by the
        // owner's net id and the side. Client and server share the process
        // (the integrated server), hence the side flag and the lock; each
        // side only dereferences its own entries, and an entry leaves the
        // list in the hook's destructor, before the memory goes.
        struct HookEntry {
            FishingHook*       hook = nullptr;
            const EntityLevel* level = nullptr;
            int32_t            owner = -1;
            bool               clientSide = false;
        };
        std::mutex             g_hooksMutex;
        std::vector<HookEntry> g_hooks;

        // MC Mth.nextInt(random, min, max) / Mth.nextFloat(random, min, max).
        int NextIntBetween(JavaRandom& r, int minInclusive, int maxInclusive) {
            return minInclusive >= maxInclusive ? minInclusive
                                                : r.NextInt(maxInclusive - minInclusive + 1) + minInclusive;
        }
        float NextFloatBetween(JavaRandom& r, float min, float max) {
            return min >= max ? min : r.NextFloat() * (max - min) + min;
        }

        bool IsFishingRod(const ItemStack* stack) {
            return stack && !stack->IsEmpty() && stack->itemId == Items::FishingRod;
        }

        // MC Entity.canInteractWithLevel.
        bool CanInteractWithLevel(const Entity& e) {
            return e.IsAlive() && !e.IsRemoved() && !e.IsSpectator();
        }

        // MC FishingHook.OpenWaterType.
        enum class OpenWaterType : uint8_t { AboveWater, InsideWater, Invalid };

        // MC getOpenWaterTypeForBlock.
        OpenWaterType OpenWaterTypeForBlock(const IBlockAccess& blocks, const glm::ivec3& pos) {
            const BlockState state = blocks.GetBlockState(pos.x, pos.y, pos.z);
            const BlockID id = state.Block();
            if (id == BlockID::Air || id == BlockID::LilyPad) return OpenWaterType::AboveWater;
            const FluidState fluid = FluidStateOf(state);
            const bool noCollision = !BlockRegistry::HasCollision(id) ||
                                     BlockRegistry::GetBlockCollisionShapeSet(state).count == 0;
            return fluid.Is(FluidType::Water) && fluid.IsSource() && noCollision
                ? OpenWaterType::InsideWater : OpenWaterType::Invalid;
        }

        // MC getOpenWaterTypeForArea: every block in the box agrees, or the
        // layer is INVALID.
        OpenWaterType OpenWaterTypeForArea(const IBlockAccess& blocks, const glm::ivec3& from,
                                           const glm::ivec3& to) {
            bool first = true;
            OpenWaterType result = OpenWaterType::Invalid;
            for (int x = from.x; x <= to.x; ++x) {
                for (int y = from.y; y <= to.y; ++y) {
                    for (int z = from.z; z <= to.z; ++z) {
                        const OpenWaterType t = OpenWaterTypeForBlock(blocks, glm::ivec3(x, y, z));
                        if (first) { result = t; first = false; continue; }
                        if (t != result) return OpenWaterType::Invalid;
                    }
                }
            }
            return result;
        }

    } // namespace

    FishingHook::FishingHook(EntityLevel* level)
        : Projectile(EntityTypeId::FishingBobber, level) {}

    FishingHook::~FishingHook() {
        Unregister();
    }

    // ── The owner link (MC updateOwnerInfo) ────────────────────────────────

    void FishingHook::Register(int32_t ownerNetId) {
        std::lock_guard<std::mutex> lock(g_hooksMutex);
        const bool clientSide = m_level && m_level->IsClientSide();
        for (HookEntry& e : g_hooks) {
            if (e.hook == this) {
                e.owner = ownerNetId;
                e.level = m_level;
                e.clientSide = clientSide;
                m_registered = true;
                return;
            }
        }
        g_hooks.push_back(HookEntry{this, m_level, ownerNetId, clientSide});
        m_registered = true;
    }

    void FishingHook::Unregister() {
        if (!m_registered) return;
        std::lock_guard<std::mutex> lock(g_hooksMutex);
        g_hooks.erase(std::remove_if(g_hooks.begin(), g_hooks.end(),
                                     [this](const HookEntry& e) { return e.hook == this; }),
                      g_hooks.end());
        m_registered = false;
    }

    FishingHook* FishingHook::FindForOwner(bool clientSide, int32_t ownerNetId, const EntityLevel* level) {
        if (ownerNetId < 0) return nullptr;
        std::lock_guard<std::mutex> lock(g_hooksMutex);
        for (const HookEntry& e : g_hooks) {
            if (e.clientSide != clientSide || e.owner != ownerNetId) continue;
            if (level && e.level != level) continue;
            // Only this side's hooks get here, so reading one is safe.
            if (e.hook->IsRemoved()) continue;
            return e.hook;
        }
        return nullptr;
    }

    // ── The cast (MC FishingHook(Player, Level, luck, lureSpeed)) ─────────

    void FishingHook::InitFromPlayer(LivingEntity& player, float playerYRot, float playerXRot,
                                     int luck, int lureSpeed) {
        m_luck = std::max(0, luck);
        m_lureSpeed = std::max(0, lureSpeed);
        SetOwner(&player);
        m_castBy = &player;
        m_ownerNetId = player.GetId();
        m_syncDirty = true;
        Register(m_ownerNetId);

        const float yCos = std::cos(-playerYRot * 0.017453292f - 3.1415927f);
        const float ySin = std::sin(-playerYRot * 0.017453292f - 3.1415927f);
        const float xCos = -std::cos(-playerXRot * 0.017453292f);
        const float xSin = std::sin(-playerXRot * 0.017453292f);
        const double x1 = player.position.x - static_cast<double>(ySin) * 0.3;
        const double y1 = player.GetEyeY();
        const double z1 = player.position.z - static_cast<double>(yCos) * 0.3;
        // snapTo(x1, y1, z1, yRot1, xRot1).
        position = glm::dvec3(x1, y1, z1);
        oldPosition = position;
        yRot = playerYRot;
        xRot = playerXRot;

        glm::dvec3 movement(static_cast<double>(-ySin),
                            static_cast<double>(Mth::Clamp(-(xSin / xCos), -5.0f, 5.0f)),
                            static_cast<double>(-yCos));
        const double dist = glm::length(movement);
        // Java evaluates multiply()'s three arguments left to right — the
        // three triangle draws land on x, y, z in that order.
        JavaRandom* rng = m_level ? &m_level->Random() : nullptr;
        const double sx = 0.6 / dist + (rng ? rng->Triangle(0.5, 0.0103365) : 0.5);
        const double sy = 0.6 / dist + (rng ? rng->Triangle(0.5, 0.0103365) : 0.5);
        const double sz = 0.6 / dist + (rng ? rng->Triangle(0.5, 0.0103365) : 0.5);
        movement = glm::dvec3(movement.x * sx, movement.y * sy, movement.z * sz);
        velocity = movement;
        const double horizontal = std::sqrt(movement.x * movement.x + movement.z * movement.z);
        yRot = static_cast<float>(std::atan2(movement.x, movement.z) * 57.2957763671875);
        xRot = static_cast<float>(std::atan2(movement.y, horizontal) * 57.2957763671875);
        yRotO = yRot;
        xRotO = xRot;
        yHeadRot = yBodyRot = yRot;
        needsSync = true;
    }

    LivingEntity* FishingHook::GetPlayerOwner() {
        Entity* owner = GetOwner();
        if (!owner || !owner->IsPlayer()) return nullptr;
        return owner->AsLiving();
    }

    bool FishingHook::ShouldStopFishing(LivingEntity& owner) {
        // MC shouldStopFishing. The level test is the engine's half of
        // "the player changed dimension": MC's ServerPlayer is the same
        // object in the new level and fails the distance test there by
        // accident of coordinates; here the owner resolves to the player's
        // view in the level they now stand in.
        if (CanInteractWithLevel(owner) && owner.Level() == m_level) {
            const bool mainHandIsFishing = IsFishingRod(owner.EquipmentInSlot(EquipmentSlot::MAINHAND));
            const bool offHandIsFishing  = IsFishingRod(owner.EquipmentInSlot(EquipmentSlot::OFFHAND));
            const glm::dvec3 d = owner.position - position;
            if ((mainHandIsFishing || offHandIsFishing) && glm::dot(d, d) <= kMaxOwnerDistanceSq) {
                // FishingHookRenderer.getHoldingArm: the main arm when the
                // main hand holds the rod, the other one otherwise.
                const uint8_t hand = mainHandIsFishing ? 0 : 1;
                if (hand != m_ownerHand) {
                    m_ownerHand = hand;
                    m_syncDirty = true;
                }
                return false;
            }
        }
        Discard();
        return true;
    }

    // ── The hooked target (MC hookedIn / DATA_HOOKED_ENTITY) ───────────────

    void FishingHook::SetHookedEntity(Entity* entity) {
        m_hookedIn = entity;
        m_hookedItemId = 0;
        const int32_t netId = entity ? entity->GetId() : -1;
        if (netId != m_hookedNetId) {
            m_hookedNetId = netId;
            m_syncDirty = true;
        }
    }

    void FishingHook::SetHookedItem(int32_t itemId) {
        m_hookedIn = nullptr;
        m_hookedItemId = itemId;
        const int32_t netId = itemId != 0 ? itemId : -1;
        if (netId != m_hookedNetId) {
            m_hookedNetId = netId;
            m_syncDirty = true;
        }
    }

    void FishingHook::ClearHooked() {
        SetHookedEntity(nullptr);
    }

    void FishingHook::ClearReferenceTo(const Entity* entity) {
        Projectile::ClearReferenceTo(entity);
        if (entity && entity == m_castBy) {
            // The fisher's view in this level is going away. MC's owner lookup
            // is per level (Projectile.getOwner), so from here on the hook has
            // no owner and its next tick discards it; do it now, before a view
            // of the same player in another level could be mistaken for one.
            m_castBy = nullptr;
            if (m_level && !m_level->IsClientSide() && !IsRemoved()) Discard();
        }
        if (entity && entity == m_hookedIn) {
            // MC keeps the reference and finds it removed on the next tick:
            // setHookedEntity(null), back to FLYING. The pointer cannot
            // outlive the entity here, so that tick's answer is given now.
            ClearHooked();
            if (m_state == State::HookedInEntity) m_state = State::Flying;
        }
    }

    bool FishingHook::ResolveHookedTarget(glm::dvec3& feet, double& height) const {
        if (!m_level) return false;
        if (m_hookedIn) {
            // MC: !isRemoved() && canInteractWithLevel() && same dimension.
            if (!CanInteractWithLevel(*m_hookedIn) || m_hookedIn->Level() != m_level) return false;
            feet = m_hookedIn->position;
            height = static_cast<double>(m_hookedIn->GetBbHeight());
            return true;
        }
        if (m_hookedItemId != 0) {
            // A dropped item is not an Entity here: find it by id near where
            // the hook last put it (it moves at most a few blocks a tick).
            const double r = 4.0;
            const AABBd box = AABBd::FromMinMax(position - glm::dvec3(r), position + glm::dvec3(r));
            std::vector<EntityLevel::NearbyItemEntity> items;
            m_level->GetItemEntitiesInBox(box, items);
            for (const auto& item : items) {
                if (item.id != m_hookedItemId || item.count <= 0) continue;
                feet = item.pos;
                height = static_cast<double>(ItemEntity::kHeight);
                return true;
            }
        }
        return false;
    }

    void FishingHook::PullHooked() {
        // MC pullEntity(hookedIn): owner != null && entity.canSimulateMovement().
        Entity* owner = GetOwner();
        if (!owner || !m_level) return;
        const glm::dvec3 delta = (owner->position - position) * 0.1;
        if (m_hookedIn) {
            // A player's motion is simulated by their own client (MC's
            // AUTHORITATIVE_SIDE_AND_SERVER: the ServerPlayer's copy never
            // reaches them) — entity event 31 pulls them there.
            if (m_hookedIn->IsPlayer()) return;
            m_hookedIn->velocity += delta;
            m_hookedIn->physicsParked = false;
            m_hookedIn->needsSync = true;
        } else if (m_hookedItemId != 0) {
            // ItemEntity: SERVER_AND_CLIENT — the server's copy, synced.
            m_level->AddItemEntityDeltaMovement(m_hookedItemId, delta);
        }
    }

    // ── Synced data ────────────────────────────────────────────────────────

    void FishingHook::SetBiting(bool biting) {
        if (biting == m_biting) return;
        m_biting = biting;
        m_syncDirty = true;
        // MC onSyncedDataUpdated(DATA_BITING) runs on the server's own set
        // too: the dip is applied on both sides.
        if (m_biting) {
            velocity.y = static_cast<double>(-0.4f * NextFloatBetween(m_syncRandom, 0.6f, 1.0f));
        }
    }

    void FishingHook::ApplySyncedData(int32_t ownerNetId, int32_t hookedNetId, bool biting, uint8_t ownerHand) {
        if (ownerNetId != m_ownerNetId || !m_registered) {
            m_ownerNetId = ownerNetId;
            if (ownerNetId >= 0) Register(ownerNetId);
            else Unregister();
        }
        m_ownerHand = ownerHand;
        if (hookedNetId != m_hookedNetId) {
            m_hookedNetId = hookedNetId;
            // Unhooked by the server: the client copy resumes its own flight
            // rather than hanging where the target was.
            if (hookedNetId < 0 && m_state == State::HookedInEntity) m_state = State::Flying;
        }
        if (biting != m_biting) {
            m_biting = biting;
            if (m_biting) {
                velocity.y = static_cast<double>(-0.4f * NextFloatBetween(m_syncRandom, 0.6f, 1.0f));
            }
        }
    }

    void FishingHook::HandleEntityEvent(uint8_t id) {
        // MC handleEntityEvent(31): pullEntity(hookedIn) on the client. The
        // only client-side target whose motion the client owns is its own
        // player, which common code cannot reach — the client's packet
        // handler applies that pull (Client::Fishing). Mobs and items follow
        // the server's motion.
        Projectile::HandleEntityEvent(id);
    }

    bool FishingHook::CanHitEntity(const Entity& entity) const {
        // MC Projectile.canHitEntity starts with target.canBeHitByProjectile()
        // — alive and pickable. This engine's projectiles report pickable for
        // the arrow's sake; MC's are not, bar the redirectable ones.
        if (!Projectile::CanHitEntity(entity)) return false;
        if (!entity.IsPickable()) return false;
        if (const auto* projectile = dynamic_cast<const Projectile*>(&entity)) {
            return projectile->GetPickRadius() > 0.0f;
        }
        return true;
    }

    // ── Tick ───────────────────────────────────────────────────────────────

    void FishingHook::Tick() {
        if (!m_level || !m_level->Blocks()) return;
        const IBlockAccess& blocks = *m_level->Blocks();
        const bool clientSide = m_level->IsClientSide();

        // syncronizedRandom.setSeed(uuid.leastSignificantBits ^ gameTime) —
        // the id stands in for the UUID (header).
        m_syncRandom.SetSeed(static_cast<int64_t>(static_cast<uint64_t>(static_cast<uint32_t>(GetId())) *
                                                  0x9E3779B97F4A7C15ULL) ^
                             m_level->GetGameTime());

        // super.tick(): Projectile.tick (the shot's game event, leftOwner)
        // then Entity.tick's baseTick.
        if (!clientSide && !m_hasBeenShot) {
            GameEvent(GameEventId::ProjectileShoot, GetOwner());
            m_hasBeenShot = true;
        }
        if (!m_leftOwner) m_leftOwner = CheckLeftOwner();
        Entity::BaseTick();
        if (IsRemoved()) return;

        if (!clientSide) {
            LivingEntity* owner = GetPlayerOwner();
            if (!owner) {
                Discard();
                return;
            }
            if (ShouldStopFishing(*owner)) return;
        } else if (m_ownerNetId < 0) {
            // MC recreateFromPacket discards a hook with no valid owner; the
            // client copy just waits (still) for its data or its removal.
            return;
        }

        if (onGround) {
            ++m_life;
            if (m_life >= kGroundedLife) {
                if (!clientSide) Discard();
                return;
            }
        } else {
            m_life = 0;
        }

        float liquidHeight = 0.0f;
        const glm::ivec3 blockPos = BlockPosition();
        const FluidState fluidState = GetFluidState(blocks, blockPos);
        if (fluidState.Is(FluidType::Water)) {
            liquidHeight = FluidHeight(blocks, blockPos, fluidState);
        }
        const bool isInWater = liquidHeight > 0.0f;

        // Client: hooked is the server's word (its data), not a pointer.
        const auto hooked = [&]() { return clientSide ? m_hookedNetId >= 0 : HasHookedTarget(); };

        if (m_state == State::Flying) {
            if (hooked()) {
                velocity = glm::dvec3(0.0);
                m_state = State::HookedInEntity;
                return;
            }
            if (isInWater) {
                velocity *= glm::dvec3(0.3, 0.2, 0.3);
                m_state = State::Bobbing;
                return;
            }
            CheckCollision();
            if (IsRemoved()) return;
        } else {
            if (m_state == State::HookedInEntity) {
                if (clientSide) {
                    // The client cannot resolve every target (players, items)
                    // by id; the server's position packets carry the hook.
                    return;
                }
                if (HasHookedTarget()) {
                    glm::dvec3 feet(0.0);
                    double height = 0.0;
                    if (ResolveHookedTarget(feet, height)) {
                        // setPos(hookedIn.getX(), hookedIn.getY(0.8), hookedIn.getZ()).
                        position = glm::dvec3(feet.x, feet.y + height * 0.8, feet.z);
                    } else {
                        ClearHooked();
                        m_state = State::Flying;
                    }
                }
                return;
            }

            if (m_state == State::Bobbing) {
                const glm::dvec3 movement = velocity;
                double force = position.y + movement.y - static_cast<double>(blockPos.y) -
                               static_cast<double>(liquidHeight);
                if (std::abs(force) < 0.01) {
                    force += (force > 0.0 ? 1.0 : (force < 0.0 ? -1.0 : 0.0)) * 0.1;
                }
                const double spring = force * static_cast<double>(m_level->Random().NextFloat()) * 0.2;
                velocity = glm::dvec3(movement.x * 0.9, movement.y - spring, movement.z * 0.9);

                if (m_nibble <= 0 && m_timeUntilHooked <= 0) {
                    m_openWater = true;
                } else if (!clientSide) {
                    // The survey only matters to the server's loot roll.
                    m_openWater = m_openWater && m_outOfWaterTime < kMaxOutOfWaterTime &&
                                  CalculateOpenWater(blockPos);
                }

                if (isInWater) {
                    m_outOfWaterTime = std::max(0, m_outOfWaterTime - 1);
                    if (m_biting) {
                        velocity.y += -0.1 * static_cast<double>(m_syncRandom.NextFloat()) *
                                      static_cast<double>(m_syncRandom.NextFloat());
                    }
                    if (!clientSide) CatchingFish(blockPos);
                } else {
                    m_outOfWaterTime = std::min(kMaxOutOfWaterTime, m_outOfWaterTime + 1);
                }
            }
        }

        if (!fluidState.Is(FluidType::Water) && !onGround && !hooked()) {
            velocity.y -= 0.03;   // getDefaultGravity
        }

        Move(velocity);
        // applyEffectsFromBlocks: the level's CheckInsideBlocks pass.
        RotateTowardsMovement(0.2f);
        if (m_state == State::Flying && (onGround || horizontalCollision)) {
            velocity = glm::dvec3(0.0);
        }

        velocity *= 0.92;
    }

    // MC checkCollision → ProjectileUtil.getHitResultOnMoveVector with
    // canHitEntity (every projectile target, plus dropped items), then
    // hitTargetOrDeflectSelf.
    void FishingHook::CheckCollision() {
        const bool clientSide = m_level->IsClientSide();
        const glm::dvec3 origin = position;
        const glm::dvec3 movement = velocity;
        HitResult hit = Clip(origin, movement, !clientSide);

        // Dropped items: not in the entity query here (not Entities), so
        // their boxes are swept apart, with the same 0.3 inflate. The
        // nearest of block, entity and item wins, as in MC.
        int32_t hitItem = 0;
        if (!clientSide && glm::dot(movement, movement) > 1.0e-18) {
            AABBd sweep = GetAABBd();
            sweep.min = glm::min(sweep.min, sweep.min + movement) - glm::dvec3(1.0);
            sweep.max = glm::max(sweep.max, sweep.max + movement) + glm::dvec3(1.0);
            std::vector<EntityLevel::NearbyItemEntity> items;
            m_level->GetItemEntitiesInBox(sweep, items);
            double best = hit.IsHit() ? hit.t : 1.0 + 1.0e-9;
            constexpr float kHalf = ItemEntity::kWidth * 0.5f;
            for (const auto& item : items) {
                if (item.count <= 0) continue;
                AABB box;
                box.min = glm::vec3(item.pos) - glm::vec3(kHalf, 0.0f, kHalf) - glm::vec3(0.3f);
                box.max = glm::vec3(item.pos) + glm::vec3(kHalf, ItemEntity::kHeight, kHalf) + glm::vec3(0.3f);
                const double t = RayAabb(origin, movement, box);
                if (t >= 0.0 && t < best) {
                    best = t;
                    hitItem = item.id;
                }
            }
        }

        if (hitItem != 0) {
            // onHitEntity: setHookedEntity (server only).
            SetHookedItem(hitItem);
            return;
        }
        if (hit.IsHit()) OnHit(hit);
    }

    void FishingHook::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        (void)hit;
        // MC onHitEntity: super (nothing), then setHookedEntity server-side.
        if (m_level && !m_level->IsClientSide()) SetHookedEntity(&target);
    }

    void FishingHook::OnHitBlock(const HitResult& hit) {
        // MC onHitBlock: setDeltaMovement(normalize(delta) * hit.distanceTo(this))
        // — next move lands on the surface; move() does the rest.
        const double len = glm::length(velocity);
        if (len < 1.0e-9) return;
        const double dist = glm::length(hit.location - position);
        velocity = velocity / len * dist;
    }

    // MC catchingFish — server only.
    void FishingHook::CatchingFish(const glm::ivec3& blockPos) {
        JavaRandom& random = m_level->Random();
        const IBlockAccess& blocks = *m_level->Blocks();

        int fishingSpeed = 1;
        const glm::ivec3 above = blockPos + glm::ivec3(0, 1, 0);
        // Rain: the engine has no weather, so the level answers false.
        if (random.NextFloat() < 0.25f && m_level->IsRainingAt(above)) ++fishingSpeed;
        if (random.NextFloat() < 0.5f && !m_level->CanSeeSky(above.x, above.y, above.z)) --fishingSpeed;

        const float bbWidth = GetBbWidth();

        if (m_nibble > 0) {
            --m_nibble;
            if (m_nibble <= 0) {
                m_timeUntilLured = 0;
                m_timeUntilHooked = 0;
                SetBiting(false);
            }
            return;
        }

        if (m_timeUntilHooked > 0) {
            m_timeUntilHooked -= fishingSpeed;
            if (m_timeUntilHooked > 0) {
                // The fish's wake closing in on the bobber.
                m_fishAngle += static_cast<float>(random.Triangle(0.0, 9.188));
                const float angleRad = m_fishAngle * 0.017453292f;
                const float sinA = std::sin(angleRad);
                const float cosA = std::cos(angleRad);
                const double fishX = position.x + static_cast<double>(sinA * static_cast<float>(m_timeUntilHooked) * 0.1f);
                const double fishY = static_cast<double>(static_cast<float>(std::floor(position.y)) + 1.0f);
                const double fishZ = position.z + static_cast<double>(cosA * static_cast<float>(m_timeUntilHooked) * 0.1f);
                const glm::ivec3 splashPos(static_cast<int>(std::floor(fishX)),
                                           static_cast<int>(std::floor(fishY - 1.0)),
                                           static_cast<int>(std::floor(fishZ)));
                if (blocks.GetBlock(splashPos.x, splashPos.y, splashPos.z) == BlockID::Water) {
                    if (random.NextFloat() < 0.15f) {
                        m_level->SendParticles(ParticleOptions(ParticleKind::Bubble), fishX, fishY - 0.10000000149011612,
                                               fishZ, 1, static_cast<double>(sinA), 0.1, static_cast<double>(cosA), 0.0);
                    }
                    const float particleXMovement = sinA * 0.04f;
                    const float particleZMovement = cosA * 0.04f;
                    m_level->SendParticles(ParticleOptions(ParticleKind::Fishing), fishX, fishY, fishZ, 0,
                                           static_cast<double>(particleZMovement), 0.01,
                                           static_cast<double>(-particleXMovement), 1.0);
                    m_level->SendParticles(ParticleOptions(ParticleKind::Fishing), fishX, fishY, fishZ, 0,
                                           static_cast<double>(-particleZMovement), 0.01,
                                           static_cast<double>(particleXMovement), 1.0);
                }
            } else {
                // The bite.
                PlaySound(SoundEvents::FISHING_BOBBER_SPLASH, 0.25f,
                          1.0f + (random.NextFloat() - random.NextFloat()) * 0.4f);
                const double y = position.y + 0.5;
                const int count = static_cast<int>(1.0f + bbWidth * 20.0f);
                m_level->SendParticles(ParticleOptions(ParticleKind::Bubble), position.x, y, position.z, count,
                                       static_cast<double>(bbWidth), 0.0, static_cast<double>(bbWidth),
                                       0.20000000298023224);
                m_level->SendParticles(ParticleOptions(ParticleKind::Fishing), position.x, y, position.z, count,
                                       static_cast<double>(bbWidth), 0.0, static_cast<double>(bbWidth),
                                       0.20000000298023224);
                m_nibble = NextIntBetween(random, 20, 40);
                SetBiting(true);
            }
        } else if (m_timeUntilLured > 0) {
            m_timeUntilLured -= fishingSpeed;
            float teaseChance = 0.15f;
            if (m_timeUntilLured < 20) {
                teaseChance += static_cast<float>(20 - m_timeUntilLured) * 0.05f;
            } else if (m_timeUntilLured < 40) {
                teaseChance += static_cast<float>(40 - m_timeUntilLured) * 0.02f;
            } else if (m_timeUntilLured < 60) {
                teaseChance += static_cast<float>(60 - m_timeUntilLured) * 0.01f;
            }

            if (random.NextFloat() < teaseChance) {
                // A splash somewhere out on the water: a fish nosing about.
                const float angle = NextFloatBetween(random, 0.0f, 360.0f) * 0.017453292f;
                const float dist = NextFloatBetween(random, 25.0f, 60.0f);
                const double fishX = position.x + static_cast<double>(std::sin(angle) * dist) * 0.1;
                const double fishY = static_cast<double>(static_cast<float>(std::floor(position.y)) + 1.0f);
                const double fishZ = position.z + static_cast<double>(std::cos(angle) * dist) * 0.1;
                const glm::ivec3 splashPos(static_cast<int>(std::floor(fishX)),
                                           static_cast<int>(std::floor(fishY - 1.0)),
                                           static_cast<int>(std::floor(fishZ)));
                if (blocks.GetBlock(splashPos.x, splashPos.y, splashPos.z) == BlockID::Water) {
                    m_level->SendParticles(ParticleOptions(ParticleKind::Splash), fishX, fishY, fishZ,
                                           2 + random.NextInt(2), 0.10000000149011612, 0.0,
                                           0.10000000149011612, 0.0);
                }
            }

            if (m_timeUntilLured <= 0) {
                m_fishAngle = NextFloatBetween(random, 0.0f, 360.0f);
                m_timeUntilHooked = NextIntBetween(random, 20, 80);
            }
        } else {
            // Lure: `lureSpeed` is getFishingTimeReduction * 20 — 100 ticks a
            // level off the 100..600 wait.
            m_timeUntilLured = NextIntBetween(random, 100, 600);
            m_timeUntilLured -= m_lureSpeed;
        }
    }

    // MC calculateOpenWater: the four layers y-1..y+2 of the 5x5 around the
    // bobber, each wholly water (INSIDE_WATER) or wholly air/lily pads
    // (ABOVE_WATER), with no water layer above an air one.
    bool FishingHook::CalculateOpenWater(const glm::ivec3& blockPos) const {
        const IBlockAccess& blocks = *m_level->Blocks();
        OpenWaterType previousLayer = OpenWaterType::Invalid;
        for (int y = -1; y <= 2; ++y) {
            const OpenWaterType layer = OpenWaterTypeForArea(blocks, blockPos + glm::ivec3(-2, y, -2),
                                                             blockPos + glm::ivec3(2, y, 2));
            switch (layer) {
                case OpenWaterType::AboveWater:
                    if (previousLayer == OpenWaterType::Invalid) return false;
                    break;
                case OpenWaterType::InsideWater:
                    if (previousLayer == OpenWaterType::AboveWater) return false;
                    break;
                case OpenWaterType::Invalid:
                    return false;
            }
            previousLayer = layer;
        }
        return true;
    }

} // namespace Game
