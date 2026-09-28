// File: src/common/entity/Leashable.cpp
//
// The static half of MC Leashable (see Leashable.hpp), the holder-side Entity
// hooks, and the interaction entry points of MC Entity.interact, LeadItem and
// LeashFenceKnotEntity.
#include "common/entity/Leashable.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/decoration/LeashFenceKnot.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/npc/Merchant.hpp"
#include "common/entity/npc/Villager.hpp"
#include "common/world/pathfinder/NodeEvaluator.hpp"
#include "common/world/pathfinder/PathTypeTable.hpp"
#include "common/physics/Physics.hpp"
#include "common/sound/LevelSound.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockFriction.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"

#include <cmath>

namespace Game {

    // ── Entity: the holder side ────────────────────────────────────────────

    bool Entity::SupportQuadLeashAsHolder() const {
        // Ghast.supportQuadLeashAsHolder / HappyGhast.supportQuadLeashAsHolder.
        return m_type == EntityTypeId::Ghast || m_type == EntityTypeId::HappyGhast;
    }

    std::array<glm::dvec3, 4> Entity::GetQuadLeashHolderOffsets() const {
        // HappyGhast.getQuadLeashHolderOffsets — the harness's four rings.
        if (m_type == EntityTypeId::HappyGhast) {
            return Leash::CreateQuadLeashOffsets(*this, -0.03125, 0.4375, 0.46875, 0.03125);
        }
        // Entity.getQuadLeashHolderOffsets.
        return Leash::CreateQuadLeashOffsets(*this, 0.0, 0.5, 0.5, 0.0);
    }

    namespace Leash {

        glm::dvec3 YRot(const glm::dvec3& v, float angle) {
            // MC Vec3.yRot: Mth.cos / Mth.sin are float.
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            return glm::dvec3(v.x * c + v.z * s, v.y, v.z * c - v.x * s);
        }

        glm::dvec3 XRot(const glm::dvec3& v, float angle) {
            // MC Vec3.xRot.
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            return glm::dvec3(v.x, v.y * c + v.z * s, v.z * c - v.y * s);
        }

        bool CanInteractWithLevel(const Entity& e) {
            return e.IsAlive() && !e.IsRemoved() && !e.IsSpectator();
        }

        std::array<glm::dvec3, 4> CreateQuadLeashOffsets(const Entity& entity, double frontOffset,
                                                        double frontBack, double leftRight,
                                                        double height) {
            const float width = entity.GetBbWidth();
            const double frontOffsetScaled = frontOffset * static_cast<double>(width);
            const double frontBackScaled = frontBack * static_cast<double>(width);
            const double leftRightScaled = leftRight * static_cast<double>(width);
            const double heightScaled = height * static_cast<double>(entity.GetBbHeight());
            return {glm::dvec3(-leftRightScaled, heightScaled, frontBackScaled + frontOffsetScaled),
                    glm::dvec3(-leftRightScaled, heightScaled, -frontBackScaled + frontOffsetScaled),
                    glm::dvec3(leftRightScaled, heightScaled, -frontBackScaled + frontOffsetScaled),
                    glm::dvec3(leftRightScaled, heightScaled, frontBackScaled + frontOffsetScaled)};
        }

        Mob* AsLeashable(Entity* e) {
            auto* mob = dynamic_cast<Mob*>(e);
            return (mob && mob->IsLeashable()) ? mob : nullptr;
        }

        const Mob* AsLeashable(const Entity* e) {
            const auto* mob = dynamic_cast<const Mob*>(e);
            return (mob && mob->IsLeashable()) ? mob : nullptr;
        }

        HolderRef RefOf(const Entity& holder) {
            HolderRef ref;
            if (const auto* knot = dynamic_cast<const LeashFenceKnot*>(&holder)) {
                ref.knotPos = knot->GetBlockPos();
            } else if (!UuidIsNil(holder.GetUuid())) {
                ref.uuid = holder.GetUuid();
            }
            return ref;
        }

        void LeashableInArea(EntityLevel& level, const glm::dvec3& pos,
                             const std::function<bool(const Mob&)>& test, std::vector<Mob*>& out) {
            // MC: AABB.ofSize(pos, 32, 32, 32) over getEntitiesOfClass.
            const float size = static_cast<float>(kAreaScanHalf * 2.0);
            const AABB box{glm::vec3(pos), glm::vec3(size)};
            std::vector<Entity*> found;
            level.GetEntitiesInBox(box, nullptr, found);
            for (Entity* e : found) {
                Mob* mob = AsLeashable(e);
                if (!mob || mob->IsRemoved()) continue;
                if (test(*mob)) out.push_back(mob);
            }
        }

        void LeashableInArea(const Entity& entity, const std::function<bool(const Mob&)>& test,
                             std::vector<Mob*>& out) {
            if (!entity.Level()) return;
            const AABBd box = entity.GetAABBd();
            LeashableInArea(*entity.Level(), (box.min + box.max) * 0.5, test, out);
        }

        void LeashableLeashedTo(const Entity& holder, std::vector<Mob*>& out) {
            LeashableInArea(holder, [&holder](const Mob& m) { return m.GetLeashHolder() == &holder; }, out);
        }

        // ── Knots ───────────────────────────────────────────────────────

        LeashFenceKnot* GetKnot(EntityLevel& level, const glm::ivec3& pos) {
            return LeashFenceKnot::Find(&level, pos);
        }

        LeashFenceKnot* CreateKnot(EntityLevel& level, const glm::ivec3& pos) {
            // MC createKnot: made and added.
            std::unique_ptr<LeashFenceKnot> knot = LeashFenceKnot::Create(&level, pos);
            LeashFenceKnot* raw = knot.get();
            level.AddFreshEntity(std::move(knot));
            return raw;
        }

        LeashFenceKnot* GetOrCreateKnot(EntityLevel& level, const glm::ivec3& pos) {
            if (LeashFenceKnot* existing = GetKnot(level, pos)) return existing;
            return CreateKnot(level, pos);
        }

        // ── tickLeash ───────────────────────────────────────────────────

        namespace {

            // MC Leashable.restoreLeashFromSave.
            void RestoreLeashFromSave(Mob& mob, LeashData& data) {
                EntityLevel* level = mob.Level();
                if (!level || level->IsClientSide()) return;
                if (data.delayed.uuid) {
                    // ServerLevel.getEntity(uuid) — THIS level: a mob, or a
                    // player standing here.
                    const Uuid uuid = *data.delayed.uuid;
                    Entity* holder = nullptr;
                    std::vector<LivingEntity*> players;
                    level->GetPlayers(players);
                    for (LivingEntity* p : players) {
                        if (p && p->GetUuid() == uuid) { holder = p; break; }
                    }
                    if (!holder) holder = level->ResolveEntity(uuid);
                    if (holder && holder->Level() == level && holder != &mob && !holder->IsRemoved()) {
                        mob.SetLeashedTo(*holder, true);
                        return;
                    }
                } else if (data.delayed.knotPos) {
                    mob.SetLeashedTo(*GetOrCreateKnot(*level, *data.delayed.knotPos), true);
                    return;
                }
                // `if (entity.tickCount > 100)`: the holder never came — the
                // lead drops where the mob stands and the leash data goes.
                if (++data.restoreTicks > kRestoreGraceTicks) {
                    JavaRandom& rng = level->Random();
                    const double vx = rng.NextDouble() * 0.2 - 0.1;
                    const double vz = rng.NextDouble() * 0.2 - 0.1;
                    level->SpawnThrownItem(mob.position, glm::dvec3(vx, 0.2, vz),
                                           ItemStack(Items::Lead, 1), /*pickupDelay=*/10);
                    mob.SetDelayedLeashRef(HolderRef{});
                }
            }

            // ── Unbreakable leads: the tow teleport ─────────────────────
            //
            // DELIBERATE DIVERGENCE (the user's rule): a lead never snaps.
            // MC breaks it past leashSnapDistance with LEAD_BREAK and a
            // dropped lead; here the spring keeps pulling (its stretch capped
            // at that distance — Mob::CheckElasticInteractions), and a mob
            // left more than twice that far behind (the holder teleported,
            // pearled, flew off, or the mob is wedged behind a wall) is
            // brought to its holder the way a tamed pet follows its owner
            // (TamableAnimal.teleportToAroundBlockPos: ten rolls in the ±3
            // ring round the holder, ±1 up or down). No spot → the tow goes
            // on and the next tick tries again. A holder in another level is
            // waited for, never followed (TickLeash's early outs).
            constexpr double kTowTeleportFactor = 2.0;

            bool CanTowTo(const Mob& mob, int x, int y, int z) {
                const EntityLevel* level = mob.Level();
                const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
                if (!blocks) return false;
                // level.noCollision(mob, box at the spot).
                const double half = mob.GetBbWidth() * 0.5;
                AABBd box;
                box.min = glm::dvec3(x + 0.5 - half, static_cast<double>(y), z + 0.5 - half);
                box.max = glm::dvec3(x + 0.5 + half, y + static_cast<double>(mob.GetBbHeight()), z + 0.5 + half);
                std::vector<AABBd> colliders;
                CollectBlockColliders(box, level->Physics(), colliders);
                if (!colliders.empty()) return false;
                // Where this mob can stand: a walker on walkable ground (the
                // pet rule), a swimmer in water, a flier in open air.
                PathfindingContext ctx;
                ctx.blocks = blocks;
                ctx.mob = const_cast<Mob*>(&mob);
                const PathType type = WalkNodeEvaluator::GetPathTypeStatic(ctx, x, y, z);
                if (type == PathType::Walkable) return true;
                if (type == PathType::Water && mob.IsInWater()) return true;
                return type == PathType::Open && mob.IsNoGravity();
            }

            bool TowTeleportToHolder(Mob& mob, const Entity& holder) {
                EntityLevel* level = mob.Level();
                if (!level) return false;
                const glm::ivec3 target = holder.BlockPosition();
                JavaRandom& rng = level->Random();
                for (int attempt = 0; attempt < 10; ++attempt) {
                    const int xd = rng.NextInt(-3, 3);
                    const int zd = rng.NextInt(-3, 3);
                    if (std::abs(xd) < 2 && std::abs(zd) < 2) continue;
                    const int yd = rng.NextInt(-1, 1);
                    const int x = target.x + xd, y = target.y + yd, z = target.z + zd;
                    if (!CanTowTo(mob, x, y, z)) continue;
                    // snapTo + navigation.stop(), as the pet's; the tracker
                    // sends the jump as a teleport.
                    mob.position = glm::dvec3(x + 0.5, static_cast<double>(y), z + 0.5);
                    mob.oldPosition = mob.position;
                    mob.velocity = glm::dvec3(0.0);
                    mob.ResetFallDistance();
                    mob.physicsParked = false;
                    if (mob.HasAiControls()) mob.GetNavigation().Stop();
                    return true;
                }
                return false;
            }

            // MC Leashable.angularFriction.
            float AngularFriction(const Mob& mob) {
                if (mob.onGround) {
                    const EntityLevel* level = mob.Level();
                    if (level && level->Blocks()) {
                        // getBlockPosBelowThatAffectsMyMovement: 0.500001 down.
                        const int x = static_cast<int>(std::floor(mob.position.x));
                        const int y = static_cast<int>(std::floor(mob.position.y - 0.500001));
                        const int z = static_cast<int>(std::floor(mob.position.z));
                        return GetBlockFriction(level->Blocks()->GetBlock(x, y, z)) * 0.91f;
                    }
                    return 0.6f * 0.91f;
                }
                return mob.IsInLiquid() ? 0.8f : 0.91f;
            }

        } // namespace

        void TickLeash(Mob& mob) {
            EntityLevel* level = mob.Level();
            if (!level || level->IsClientSide()) return;

            LeashData* data = mob.GetLeashData();
            if (data && !data->delayed.Empty()) RestoreLeashFromSave(mob, *data);
            data = mob.GetLeashData();

            if (!data || !(data->holder || data->holderLost || data->awaitingPlayer)) return;

            // A player holder who left this level: back here → the lead is
            // theirs again; still online elsewhere → it waits, as MC's does
            // for a holder in another level; logged out → their entity is
            // gone, and the lead drops below.
            bool playerLoggedOut = false;
            if (data->awaitingPlayer && data->lastRef.uuid) {
                const Uuid uuid = *data->lastRef.uuid;
                std::vector<LivingEntity*> here;
                level->GetPlayers(here);
                LivingEntity* back = nullptr;
                for (LivingEntity* p : here) {
                    if (p && p->GetUuid() == uuid) { back = p; break; }
                }
                if (back) {
                    mob.SetLeashedTo(*back, true);
                    data = mob.GetLeashData();
                } else if (level->ResolvePlayer(uuid)) {
                    // Online in another level: the lead waits — unless this
                    // mob can no longer hold it (dead), handled below.
                    if (CanInteractWithLevel(mob)) return;
                } else {
                    playerLoggedOut = true;
                }
            }

            // A holder that stopped existing, died, or turned spectator; or a
            // leashed mob that did: the lead comes off — dropped as an item
            // unless the entity_drops rule is off.
            const bool holderGone = data->holderLost || playerLoggedOut ||
                                    (data->holder && !CanInteractWithLevel(*data->holder));
            if (!CanInteractWithLevel(mob) || holderGone) {
                if (level->DoEntityDrops()) mob.DropLeash();
                else                        mob.RemoveLeash();
            }

            Entity* holder = mob.GetLeashHolder();
            if (!holder || holder->Level() != mob.Level()) return;
            data = mob.GetLeashData();   // unchanged: a live holder means the data survived

            const double distanceTo = mob.LeashDistanceTo(*holder);
            mob.WhenLeashedTo(*holder);
            // The spring's momentum is read after the branch below; the
            // too-far branch drops the data (MC keeps applying it through its
            // local reference, which the object outlives in Java).
            double momentum = data->angularMomentum;
            // MC: past leashSnapDistance the lead breaks (LEAD_BREAK, the
            // lead dropped, leashTooFarBehaviour). Not here — see the tow
            // teleport above: far past it the mob is brought over, and in
            // between the capped spring below tows it.
            if (distanceTo > mob.LeashSnapDistance() * kTowTeleportFactor &&
                TowTeleportToHolder(mob, *holder)) {
                mob.CloseRangeLeashBehaviour(*holder);
            } else if (distanceTo > mob.LeashElasticDistance() - static_cast<double>(holder->GetBbWidth()) -
                                        static_cast<double>(mob.GetBbWidth()) &&
                       mob.CheckElasticInteractions(*holder, *data)) {
                momentum = data->angularMomentum;
                mob.OnElasticLeashPull();
            } else {
                mob.CloseRangeLeashBehaviour(*holder);
            }

            mob.yRot = static_cast<float>(static_cast<double>(mob.yRot) - momentum);
            if (LeashData* still = mob.GetLeashData()) {
                still->angularMomentum *= static_cast<double>(AngularFriction(mob));
            }
        }

        // ── Interactions ────────────────────────────────────────────────

        namespace {
            // A merchant whose right-click is its trade screen: a villager
            // with a working profession (not NONE, not the nitwit) that is
            // grown up, or any other merchant (the wandering trader). See the
            // divergence note in EntityInteract.
            bool TradesOnRightClick(const Mob& mob) {
                if (!dynamic_cast<const Merchant*>(&mob)) return false;
                if (const auto* villager = dynamic_cast<const Villager*>(&mob)) {
                    const VillagerProfession profession = villager->GetVillagerData().profession;
                    return !villager->IsBaby() && profession != VillagerProfession::None &&
                           profession != VillagerProfession::Nitwit;
                }
                return true;
            }

            void PlayEntitySound(Entity& e, const char* event) {
                // Entity.playSound(sound): volume 1, pitch 1.
                e.PlaySound(event, 1.0f, 1.0f);
            }
        } // namespace

        UseResult BindPlayerMobs(LivingEntity& player, EntityLevel& level, const glm::ivec3& pos) {
            if (level.IsClientSide()) return UseResult::Pass;
            std::vector<Mob*> entitiesToLeash;
            LeashableInArea(level, glm::dvec3(pos) + glm::dvec3(0.5),
                            [&player](const Mob& m) { return m.GetLeashHolder() == &player; },
                            entitiesToLeash);
            if (entitiesToLeash.empty()) return UseResult::Pass;

            // getKnot(...).orElseGet(createKnot). A new knot is added to the
            // level only if something ties on (MC adds it at once and
            // discards it unused — the same outcome, minus a frame in which a
            // watcher could be sent an empty knot).
            LeashFenceKnot* activeKnot = GetKnot(level, pos);
            std::unique_ptr<LeashFenceKnot> fresh;
            if (!activeKnot) {
                fresh = LeashFenceKnot::Create(&level, pos);
                activeKnot = fresh.get();
            }

            bool anyLeashed = false;
            for (Mob* leashable : entitiesToLeash) {
                if (leashable->CanHaveALeashAttachedTo(*activeKnot)) {
                    leashable->SetLeashedTo(*activeKnot, true);
                    anyLeashed = true;
                }
            }
            if (!anyLeashed) return UseResult::Pass;

            if (fresh) level.AddFreshEntity(std::move(fresh));
            activeKnot->PlayPlacementSound();
            // LeadItem.bindPlayerMobs — gameEvent(BLOCK_ATTACH, pos, Context.of(player)).
            if (ILevelWrite* write = level.MutableBlocks()) {
                write->GameEvent(GameEventId::BlockAttach, pos, GameEventContext::Of(&player));
            }
            return UseResult::SuccessServer;
        }

        bool DropAllLeashConnections(Entity& entity, LivingEntity* player) {
            (void)player;
            std::vector<Mob*> leashables;
            LeashableLeashedTo(entity, leashables);
            bool dropped = !leashables.empty();
            if (Mob* self = AsLeashable(&entity); self && self->IsLeashed()) {
                self->DropLeash();
                dropped = true;
            }
            for (Mob* leashable : leashables) leashable->DropLeash();
            // MC dropAllLeashConnections: gameEvent(SHEAR, player).
            if (dropped) entity.GameEvent(GameEventId::Shear, player);
            return dropped;
        }

        bool ShearOffAllLeashConnections(Entity& entity, LivingEntity* player) {
            const bool dropped = DropAllLeashConnections(entity, player);
            if (dropped) {
                if (EntityLevel* level = entity.Level(); level && !level->IsClientSide()) {
                    // serverLevel.playSound(null, blockPosition(), SHEARS_SNIP,
                    // player's source (PLAYERS) or the entity's own).
                    const SoundSource source = (player && player->IsPlayer()) ? SoundSource::Players
                                                                               : entity.GetSoundSource();
                    level->PlaySound(nullptr, entity.BlockPosition(), SoundEvents::SHEARS_SNIP, source,
                                     1.0f, 1.0f);
                }
            }
            return dropped;
        }

        UseResult EntityInteract(Mob& target, LivingEntity& player, ItemStack& held,
                                 bool secondaryUse, bool infiniteMaterials) {
            EntityLevel* level = target.Level();
            if (!level) return UseResult::Pass;
            const bool server = !level->IsClientSide();

            // A sneaking player hands the mobs they hold to this one (a happy
            // ghast carrying a herd, a camel leading a caravan).
            if (server && secondaryUse && target.IsLeashable()) {
                if (target.CanBeLeashed() && target.IsAlive() && !target.IsBaby()) {
                    std::vector<Mob*> mobsToLeash;
                    LeashableInArea(target, [&player](const Mob& m) { return m.GetLeashHolder() == &player; },
                                    mobsToLeash);
                    bool anyLeashed = false;
                    for (Mob* mob : mobsToLeash) {
                        if (mob->CanHaveALeashAttachedTo(target)) {
                            mob->SetLeashedTo(target, true);
                            anyLeashed = true;
                        }
                    }
                    if (anyLeashed) {
                        // MC: level.gameEvent(ENTITY_ACTION, blockPosition(),
                        // Context.of(player)).
                        if (ILevelWrite* write = level->MutableBlocks()) {
                            write->GameEvent(GameEventId::EntityAction, target.BlockPosition(),
                                             GameEventContext::Of(&player));
                        }
                        PlayEntitySound(target, SoundEvents::LEAD_TIED);
                        return UseResult::SuccessServer;   // .withoutItem()
                    }
                }
            }

            // Shears cut every lead on the mob — the one it wears and the ones
            // it holds.
            if (held.itemId == Items::Shears && !held.IsEmpty() && server &&
                ShearOffAllLeashConnections(target, &player)) {
                HurtAndBreak(held, 1, player, EquipmentSlot::MAINHAND);
                return UseResult::Success;
            }
            // Then Mob.attemptToShearEquipment: shears (not sneaking) take a
            // canBeSheared piece off — a saddle, a harness, horse or
            // nautilus armour, a llama's carpet — when the mob allows it
            // (canShearEquipment: nobody riding it).
            if (held.itemId == Items::Shears && !held.IsEmpty() && server && !secondaryUse &&
                target.CanShearEquipment(player) && target.AttemptToShearEquipment(player, held)) {
                return UseResult::Success;
            }

            // DELIBERATE DIVERGENCE (every mob takes a lead here): a merchant
            // that trades keeps its plain right-click for the trade screen —
            // tying it with a lead, and untying it, take a SHIFT+right-click.
            // An unemployed villager, a nitwit or a baby has no trades, so a
            // plain click leashes it like any other mob.
            if (!secondaryUse && TradesOnRightClick(target)) return UseResult::Pass;

            if (target.IsAlive() && target.IsLeashable()) {
                // The holder's own right-click unties it.
                if (target.GetLeashHolder() == &player) {
                    if (server) {
                        if (infiniteMaterials) target.RemoveLeash();
                        else                   target.DropLeash();
                        // MC: gameEvent(ENTITY_INTERACT, player).
                        target.GameEvent(GameEventId::EntityInteract, &player);
                        PlayEntitySound(target, SoundEvents::LEAD_UNTIED);
                    }
                    return UseResult::Success;   // .withoutItem()
                }
                // A lead in hand ties it to the player — taking it from a knot
                // or another mob, never from another player.
                const Entity* holder = target.GetLeashHolder();
                if (held.itemId == Items::Lead && !held.IsEmpty() && !(holder && holder->IsPlayer())) {
                    if (!server) return UseResult::Consume;
                    if (target.CanHaveALeashAttachedTo(player)) {
                        if (target.IsLeashed()) target.DropLeash();
                        target.SetLeashedTo(player, true);
                        PlayEntitySound(target, SoundEvents::LEAD_TIED);
                        held.count -= 1;   // itemStack.shrink(1) — creative restores it (Player.interactOn)
                        if (held.count <= 0) held.Clear();
                        return UseResult::SuccessServer;
                    }
                }
            }
            return UseResult::Pass;
        }

        UseResult KnotInteract(LeashFenceKnot& knot, LivingEntity& player, ItemStack& held,
                               bool secondaryUse) {
            EntityLevel* level = knot.Level();
            if (!level || level->IsClientSide()) return UseResult::Success;

            // Shears: Entity.interact's shear branch on the knot — every lead
            // tied to it drops (the knot goes with the last one).
            if (held.itemId == Items::Shears && !held.IsEmpty() &&
                ShearOffAllLeashConnections(knot, &player)) {
                HurtAndBreak(held, 1, player, EquipmentSlot::MAINHAND);
                return UseResult::Success;
            }

            // The player's mobs go onto the knot...
            bool attachedMob = false;
            std::vector<Mob*> playerLeashable;
            LeashableLeashedTo(player, playerLeashable);
            for (Mob* leashable : playerLeashable) {
                if (leashable->CanHaveALeashAttachedTo(knot)) {
                    leashable->SetLeashedTo(knot, true);
                    attachedMob = true;
                }
            }

            // ...or, with none to tie and not sneaking, the knot's mobs come
            // back to the player (the last one away discards the knot).
            bool anyDropped = false;
            if (!attachedMob && !secondaryUse) {
                std::vector<Mob*> knotLeashable;
                LeashableLeashedTo(knot, knotLeashable);
                for (Mob* mob : knotLeashable) {
                    if (mob->CanHaveALeashAttachedTo(player)) {
                        mob->SetLeashedTo(player, true);
                        anyDropped = true;
                    }
                }
            }

            if (!attachedMob && !anyDropped) return UseResult::Pass;
            // MC LeashFenceKnotEntity.interact: gameEvent(BLOCK_ATTACH, player).
            knot.GameEvent(GameEventId::BlockAttach, &player);
            PlayEntitySound(knot, SoundEvents::LEAD_TIED);
            return UseResult::Success;
        }

    } // namespace Leash

} // namespace Game
