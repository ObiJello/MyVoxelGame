// File: src/common/world/block/BlockBehaviors.cpp
//
// Per-block interaction callbacks — the block-side mirror of ItemBehaviors.cpp.
// BlockRegistry::Initialize calls BlockRegistry_RegisterBehaviors once the
// block table exists, and this file fills in the `useWithoutItem` /
// `useItemOn` function pointers for the handful of blocks that react to a
// right-click.
//
// Each entry corresponds to a BlockBehaviour subclass override in MC. Keeping
// them here rather than in BlockRegistry.cpp keeps the registry file about
// registration and this one about behaviour, and gives new interactive blocks
// (chest, furnace, doors) an obvious home.
#include "BlockRegistry.hpp"
#include "BlockInteraction.hpp"
#include "BlockPlacement.hpp"
#include "RedstoneWire.hpp"
#include "RedstoneComponents.hpp"
#include "RedstoneStateUtil.hpp"   // FacingOf — the amethyst cluster hook
#include "Rails.hpp"
#include "piston/PistonBaseBlock.hpp"
#include "RedstoneContainers.hpp"
#include "LecternBlock.hpp"
#include "FallingBlock.hpp"
#include "RedstoneSignal.hpp"      // HasNeighborSignal — enchanted gravitite
#include "TntBlock.hpp"
#include "Stairs.hpp"
#include "PotentSulfurBlock.hpp"
#include "CrossCollision.hpp"
#include "Walls.hpp"
#include "Vine.hpp"
#include "MultifaceBlock.hpp"
#include "FenceGate.hpp"
#include "BedBlock.hpp"
#include "entity/ChestBlockEntity.hpp"
#include "entity/SpawnerBlockEntity.hpp"
#include "common/world/portal/PortalFamily.hpp"
#include "common/world/portal/PortalShape.hpp"
#include "common/world/portal/PortalState.hpp"
#include "common/world/portal/ModPortalBehaviors.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/World.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/LivingEntity.hpp"             // WitherRoseEntityInside
#include "common/entity/ai/brain/Brain.hpp"             // the bell's HEARD_BELL_TIME
#include "common/physics/Physics.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/projectile/Projectile.hpp"   // AercloudEntityInside's Projectile test
#include "common/world/level/WorldDrops.hpp"         // DropBlockLoot — the berry bush harvest
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/GeneratedItemList.hpp"   // Items::InkSac … the sign applicators
#include "common/sound/SoundEvents.hpp"
#include "common/sound/SoundType.hpp"
#include "common/world/block/BlockAmbientSounds.hpp"
#include "common/world/level/WorldMobSpawn.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"
#include "GeneratedBlockStates.hpp"
#include "common/world/lighting/BlockLightProperties.hpp"
#include "common/world/lighting/ChunkLight.hpp"
#include "common/world/ticks/ScheduledTickAccess.hpp"
#include <optional>   // DoubleDoorPartner
#include <string_view>
#include "common/inventory/MenuType.hpp"
#include "common/world/crafting/RecipeManager.hpp"
#include "common/core/Log.hpp"

#include <algorithm>
#include <array>
#include <random>
#include <string>

namespace Game {

    namespace {

        // MC level.getRandom().nextFloat() * 0.1F + 0.9F — the door, trapdoor
        // and fence-gate pitch. Both sides have a random (the client's rolls
        // its own for the predicted copy, as MC's ClientLevel does).
        float OpenCloseJitter(ILevelWrite& level) {
            JavaRandom* r = level.Random();
            return r ? r->NextFloat() * 0.1f + 0.9f : 1.0f;
        }

        // ── Bell ──────────────────────────────────────────────────────────
        // MC BellBlock.useItemOn → onHit: a hit on the swinging face rings it
        // (attemptToRing: BELL_BLOCK at volume 2 from the server, for
        // everyone). The swing itself is BellBlockEntity's, which this engine
        // does not carry, so the ring is the sound alone.
        //
        // MC isProperHit: never the top or bottom face, never above y 0.8124
        // (the yoke), and by attachment — a floor bell along its facing axis,
        // a wall bell across it, a ceiling bell from any side.
        bool BellIsProperHit(BlockState state, int face, double clickY) {
            if (face == 0 || face == 1 || clickY > 0.8124) return false;
            const bool clickedX = face == 4 || face == 5;
            const std::string_view facing = state.GetValueByName("facing");
            const bool facingX = facing == "east" || facing == "west";
            const std::string_view attachment = state.GetValueByName("attachment");
            if (attachment == "floor") return facingX == clickedX;
            if (attachment == "single_wall" || attachment == "double_wall") return facingX != clickedX;
            return true;   // ceiling
        }

        UseResult BellUse(ILevelWrite* world, const glm::ivec3& pos, IUsePlayer* /*player*/,
                          const BlockHitResult& hit) {
            if (!world) return UseResult::Pass;
            const BlockState state = world->GetBlockState(pos.x, pos.y, pos.z);
            if (!state.Is(BlockID::Bell)) return UseResult::Pass;
            if (!BellIsProperHit(state, hit.face, hit.hitPoint.y - pos.y)) return UseResult::Pass;
            // attemptToRing: `!level.isClientSide()` — the client only swings.
            if (!world->IsClientSide()) {
                world->PlaySound(nullptr, pos, SoundEvents::BELL_BLOCK, SoundSource::Blocks, 2.0f, 1.0f);
                // BellBlockEntity.updateEntities (via the ring's block event):
                // every living thing within 48 whose position is inside 32 of
                // the bell's centre hears it — a villager then runs to hide
                // (ReactToBell). setMemory on a brain without the memory is
                // a no-op in MC; the IsRegistered test is that.
                if (EntityLevel* level = world->Entities()) {
                    const glm::vec3 centre(pos.x + 0.5f, pos.y + 0.5f, pos.z + 0.5f);
                    std::vector<Entity*> nearby;
                    level->GetEntitiesInBox(AABB(centre, glm::vec3(97.0f)), nullptr, nearby);
                    for (Entity* e : nearby) {
                        LivingEntity* living = e ? e->AsLiving() : nullptr;
                        Brain* brain = living ? living->GetBrain() : nullptr;
                        if (!brain || !living->IsAlive() || living->IsRemoved()) continue;
                        const glm::dvec3 d = living->position - glm::dvec3(centre);
                        if (glm::dot(d, d) >= 32.0 * 32.0) continue;
                        if (!brain->IsRegistered(MemoryModule::HeardBellTime)) continue;
                        brain->SetMemory(MemoryModule::HeardBellTime, level->GetGameTime());
                    }
                }
            }
            return UseResult::Success;
        }
        UseResult BellUseItemOn(ItemStack& /*stack*/, ILevelWrite* world, const glm::ivec3& pos,
                                IUsePlayer* player, uint32_t /*hand*/, const BlockHitResult& hit) {
            return BellUse(world, pos, player, hit);
        }

        // MC CraftingTableBlock.useWithoutItem (CraftingTableBlock.java): open
        // the 3x3 menu for the player and consume the click.
        //
        // `player->OpenMenu` is a request, not the open itself: on the server
        // PlayerSession picks it up as soon as the use dispatch returns and
        // does the actual menu swap + packets; on the client — which runs this
        // same dispatch to predict — it is a no-op, and the screen appears when
        // the server's OpenScreenS2C lands. Either way returning Success here
        // is what stops the click falling through to block placement.
        UseResult CraftingTableUse(ILevelWrite* /*world*/, const glm::ivec3& pos,
                                   IUsePlayer* player, const BlockHitResult& /*hit*/) {
            if (!player) return UseResult::Pass;
            player->OpenMenu(MenuType::Crafting, pos);
            return UseResult::Success;
        }

        // MC DragonEggBlock.teleport — the egg blinks to a random air block
        // within ±15/±7/±15, up to 1000 attempts. MC triggers it from both
        // useWithoutItem and attack (a survival punch), and both are wired
        // below; only a creative instabreak actually mines it, exactly as in
        // vanilla.
        //
        // File-local RNG rather than a level random: ILevelWrite carries no
        // random, and the offsets are pure cosmetics — no vanilla RNG stream
        // runs through a block use.
        UseResult DragonEggUse(ILevelWrite* world, const glm::ivec3& pos,
                               IUsePlayer* /*player*/,
                               const BlockHitResult& /*hit*/) {
            if (!world) return UseResult::Pass;
            if (world->GetBlock(pos.x, pos.y, pos.z) != BlockID::DragonEgg) {
                return UseResult::Pass;
            }
            static JavaRandom rng(
                static_cast<int64_t>(std::random_device{}()));
            const int minY = DimensionMinY(world->GetDimension());
            const int maxY = minY + DimensionLogicalHeight(world->GetDimension()) - 1;
            for (int i = 0; i < 1000; ++i) {
                const glm::ivec3 testPos =
                    pos + glm::ivec3(rng.NextInt(16) - rng.NextInt(16),
                                     rng.NextInt(8) - rng.NextInt(8),
                                     rng.NextInt(16) - rng.NextInt(16));
                if (testPos.y < minY || testPos.y > maxY) continue;
                if (world->GetBlock(testPos.x, testPos.y, testPos.z) !=
                    BlockID::Air) {
                    continue;
                }
                if (!world->IsClientSide()) {
                    // MC: setBlock(dest, state, 2) + removeBlock(origin).
                    world->SetBlock(testPos.x, testPos.y, testPos.z,
                                    BlockID::DragonEgg,
                                    World::UpdateFlags::All);
                    world->SetBlock(pos.x, pos.y, pos.z, BlockID::Air,
                                    World::UpdateFlags::All);
                }
                // MC's client draws the 128-particle portal trail here.
                return UseResult::Success;
            }
            return UseResult::Success;
        }

        // MC DragonEggBlock.attack — the same teleport, from a punch.
        void DragonEggAttack(ILevelWrite& world, const glm::ivec3& pos) {
            DragonEggUse(&world, pos, nullptr, BlockHitResult{});
        }

        // MC RedstoneWireBlock.useWithoutItem: right-clicking a wire that is a
        // full CROSS collapses it to a dot, and one that is a bare DOT expands
        // it back to a cross. A wire with real connections is left alone — MC
        // returns PASS, which matters because it lets the click fall through
        // to placing whatever is in hand.
        //
        // Runs on both sides: the client predicts the toggle so the shape
        // changes on the same frame, and the server's authoritative state
        // follows. The power-side pokes inside RedstoneWireToggle are no-ops
        // on the client's level.
        UseResult RedstoneWireUse(ILevelWrite* world, const glm::ivec3& pos,
                                  IUsePlayer* player, const BlockHitResult& /*hit*/) {
            if (!world || !player) return UseResult::Pass;
            const BlockState state = world->GetBlockState(pos.x, pos.y, pos.z);
            if (!state.Is(BlockID::RedstoneWire)) return UseResult::Pass;
            return RedstoneWireToggle(*world, pos, state) ? UseResult::Success : UseResult::Pass;
        }

        // MC FaceAttachedHorizontalDirectionalBlock.updateShape: a button or
        // lever whose supporting surface went away is destroyed.
        //
        //   return getConnectedDirection(state).getOpposite() == direction
        //          && !state.canSurvive(level, pos)
        //       ? Blocks.AIR.defaultBlockState() : state;
        //
        // i.e. it only reacts to a change on the side it is ATTACHED to —
        // mining the block behind a wall button drops it, mining the one beside
        // it does nothing.
        bool FaceAttachedUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                         BlockState state,
                                         Direction toNeighbour, BlockID /*neighbourId*/,
                                         BlockState& outState,
                                         ScheduledTickAccess* /*ticks*/) {
            const BlockID id = state.Block();
            if (CanSurviveAt(level, pos, state)) return false;
            // Only the attachment side matters. CanSurviveAt already answered
            // false, so the support is gone whichever neighbour reported it;
            // checking the direction just avoids re-destroying on every one of
            // the six updates a single change fans out to.
            const std::string_view face = state.GetValueByName("face");
            Direction connected = Direction::North;
            if (face == "floor")        connected = Direction::Up;
            else if (face == "ceiling") connected = Direction::Down;
            else {
                const std::string_view f = state.GetValueByName("facing");
                if      (f == "east")  connected = Direction::East;
                else if (f == "south") connected = Direction::South;
                else if (f == "west")  connected = Direction::West;
                else                   connected = Direction::North;
            }
            if (Opposite(connected) != toNeighbour) return false;

            // Air's state — World turns this into a destroy-with-drops.
            outState = BlockState{};
            return true;
        }

        // MC BaseFireBlock.onPlace (BaseFireBlock.java:140-155) — the real
        // nether-portal ignition, and the reason the engine grew an onPlace
        // hook at all.
        //
        // Putting it on FIRE rather than on flint & steel is not a stylistic
        // choice: it is what makes every route to a fire block light a frame —
        // a fire charge, lava spreading, fire jumping from a burning block, a
        // ghast fireball. Vanilla players rely on all of them.
        void FireOnPlace(ILevelWrite& level, const glm::ivec3& pos,
                         BlockState newState, BlockState oldState,
                         bool /*movedByPiston*/) {
            // MC BaseFireBlock.onPlace: `if (!oldState.is(state.getBlock()))`.
            // Fire has an `age` property, so a state-only write reaches here
            // and must not re-run the portal search.
            if (oldState.Block() == newState.Block()) return;
            // MC inPortalDimension: overworld or nether only. An obsidian
            // frame in the End just holds a fire.
            if (!DimensionAllowsNetherPortal(level.GetDimension())) return;

            // Immersive mode: any closed obsidian loop becomes a see-through
            // surface, decided server-side (the handler finds the loop and
            // starts the far-side generation). No purple blocks are ever
            // placed; a vanilla rectangle is a closed loop too, so nothing
            // that used to light still fails to. Fire lights the NETHER
            // family only: a reinforced-deepslate frame answers to an echo
            // shard (ItemBehaviors' UseOn_EchoShard), never to a flame.
            if (Portals::ImmersiveNetherPortals()) {
                if (level.IsClientSide()) return;
                if (auto handler = Portals::GetImmersiveFrameLitHandler()) {
                    handler(level, pos, PortalFamilyId::Nether);
                }
                return;
            }

            // MC always probes X first and lets findPortalShape fall through
            // to Z. The clicked face never reaches here — onPlace has no idea
            // how the fire got placed — which is also why the axis preference
            // in CanFireBePlacedAt is only about whether the fire is ALLOWED,
            // not about which way the portal ends up facing.
            auto shape = PortalShape::FindEmptyPortalShape(level, pos, Axis::X, NetherFamily());
            if (!shape) return;

            shape->CreatePortalBlocks(level);
        }

        // MC NetherPortalBlock.entityInside (:92) and EndPortalBlock
        // .entityInside (:56), which are the same two lines.
        //
        // Neither teleports. They only record "this entity is standing in this
        // portal, this tick"; the timing lives in PortalState and the travel
        // itself is resolved by the server, which is the only layer that can
        // see another dimension.
        //
        // No client-side guard, matching MC: the client needs the same state
        // to ramp its warp overlay. What the client does NOT have is a server
        // to resolve the destination, so its processor simply accumulates and
        // is thrown away.
        void PortalEntityInside(ILevelWrite& /*level*/, const glm::ivec3& pos,
                                BlockState state, Entity& entity) {
            if (!entity.CanUsePortal(false)) return;
            // A frame-portal block whose family is immersive is scenery; the
            // see-through surface handles crossing. Per family: nether blocks
            // follow /gamerule immersive_portals, hush and aether blocks are
            // always scenery (Portals::FamilyIsImmersive).
            if (Portals::IsInertPortalBlock(state.Block())) return;
            entity.portal.SetAsInsidePortal(state.Block(), pos,
                                            entity.GetDimensionChangingDelay());
        }

        // ── The Aether (pass one): aerclouds ─────────────────────────────
        //
        // Aether AercloudBlock.entityInside: the cloud breaks a fall — fall
        // distance reset, a downward motion cut to 0.5 % — and a living thing
        // inside it counts as standing (a flying player does not).
        //
        // AercloudBlock's collision is a floor 0.01 pixels thick (0.9 tall
        // for something falling faster than 2.5 blocks, a full block under
        // another cloud) and BlueAercloudBlock's is empty for an entity, so
        // things sink INTO a cloud and this hook does the rest. That shape
        // is AercloudBlock.hpp's, applied by Physics.cpp for every mover:
        // Entity::Move hands it the mob's fall distance. AercloudBlock
        // .fallOn's empty body is FallDistanceReduction 1.0 (BlockBounce.hpp).
        //
        // Players are skipped for the MOTION half: the server's player view
        // is client-authoritative for movement, so writing its velocity does
        // nothing but corrupt the knockback accumulator — the local player's
        // physics applies both hooks itself (UpdatePlayerPhysics). The
        // fall-distance reset is still applied to the view — it is the
        // server-side half of the same MC call.
        bool IsProjectileEntity(Entity& entity) {
            return dynamic_cast<Projectile*>(&entity) != nullptr;
        }

        void AercloudEntityInside(ILevelWrite& /*level*/, const glm::ivec3& /*pos*/,
                                  BlockState /*state*/, Entity& entity) {
            entity.ResetFallDistance();
            if (entity.IsPlayer()) return;
            const bool projectile = IsProjectileEntity(entity);
            if (entity.velocity.y < 0.0 && !projectile) {
                entity.velocity.y *= 0.005;   // multiply(1.0, 0.005, 1.0)
            }
            // setOnGround(entity instanceof LivingEntity && (!(… Player) ||
            // !flying)) — players already returned above.
            entity.onGround = entity.AsLiving() != nullptr && !projectile;
        }

        // MC WitherRoseBlock.entityInside: on the server, outside Peaceful,
        // any living thing standing in the flower's shape that is not
        // invulnerable to wither damage gets WITHER for 40 ticks (2 s),
        // re-applied every tick it stays — the update rules keep it at 40.
        // isInvulnerableTo(wither()): a creative / spectator player
        // (abilities.invulnerable) or an entity flagged Invulnerable; the
        // wither boss refuses the effect itself (CanBeAffected).
        void WitherRoseEntityInside(ILevelWrite& /*level*/, const glm::ivec3& /*pos*/,
                                    BlockState /*state*/, Entity& entity) {
            EntityLevel* level = entity.Level();
            if (!level || level->IsClientSide()) return;
            if (level->GetDifficulty() == Difficulty::Peaceful) return;
            LivingEntity* living = entity.AsLiving();
            if (!living || living->IsRemoved()) return;
            if (living->IsInvulnerable() || living->IsCreative() || living->IsSpectator()) return;
            living->AddEffect(MobEffectInstance(MobEffectId::Wither, 40));
        }

        // MC EyeblossomBlock.entityInside: an OPEN eyeblossom (the only state
        // in #bee_attractive) poisons a bee flying through it — POISON 25
        // ticks, server side, not on Peaceful, only when the bee is not
        // already poisoned.
        void EyeblossomEntityInside(ILevelWrite& /*level*/, const glm::ivec3& /*pos*/,
                                    BlockState state, Entity& entity) {
            if (state.Block() != BlockID::OpenEyeblossom) return;
            if (entity.GetType() != EntityTypeId::Bee) return;
            EntityLevel* level = entity.Level();
            if (!level || level->IsClientSide()) return;
            if (level->GetDifficulty() == Difficulty::Peaceful) return;
            LivingEntity* bee = entity.AsLiving();
            if (!bee || bee->HasEffect(MobEffectId::Poison)) return;
            bee->AddEffect(MobEffectInstance(MobEffectId::Poison, 25));
        }

        // Aether BlueAercloudBlock.entityInside: anything not sneaking (no
        // mob sneaks) and not a vehicle steered by a player is launched —
        // fall distance reset, vertical motion SET to 2.0 blocks a tick — and
        // is no longer on the ground; otherwise it is an ordinary aercloud.
        // The bounce sound and the SPLASH particles are client cosmetics not
        // yet ported (the player's launch itself is UpdatePlayerPhysics').
        void BlueAercloudEntityInside(ILevelWrite& level, const glm::ivec3& pos,
                                      BlockState state, Entity& entity) {
            if (entity.IsPlayer()) {
                entity.ResetFallDistance();
                return;
            }
            const Entity* rider = entity.IsVehicle() ? entity.GetControllingPassenger() : nullptr;
            if (rider && rider->IsPlayer()) {
                AercloudEntityInside(level, pos, state, entity);
                return;
            }
            entity.ResetFallDistance();
            // AddDeltaMovement, not a bare write: it flags the motion for the
            // clients and un-parks a resting entity, as setDeltaMovement does.
            entity.AddDeltaMovement(glm::dvec3(0.0, 2.0 - entity.velocity.y, 0.0));
            if (!IsProjectileEntity(entity)) entity.onGround = false;
        }

        // ── The Aether (pass two): floating blocks ─────────────────────────
        //
        // Aether FloatingBlock (block/miscellaneous/FloatingBlock.java):
        // gravitite ore floats whenever the cell above is free
        // (powered = false), enchanted gravitite only while it has a
        // redstone signal (powered = true). onPlace and updateShape book a
        // tick 2 ticks out (getDelayAfterPlace); the tick either lifts the
        // block or books the next check — FloatingBlock re-schedules itself
        // unconditionally, which is how a powered block notices its signal.
        //
        // DEVIATION: the mod lifts the block as a FloatingBlockEntity (an
        // upside-down FallingBlockEntity accelerating at 0.04/tick until it
        // hits a ceiling). This engine's FallingBlockEntity has gravity and
        // the landing test built in, so the rise is done a cell at a time
        // instead: the block moves up one cell per scheduled tick, and the
        // moved block's own onPlace books the next step — the same rule
        // (rise while the cell above is free) at a steady 10 blocks/second.
        bool FloatingIsPowered(BlockID id) { return id == BlockID::EnchantedGravitite; }

        void FloatingSchedule(ScheduledTickAccess* ticks, const glm::ivec3& pos, BlockID id) {
            if (ticks) ticks->ScheduleTick(pos, id, 2);
        }

        void FloatingOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState newState,
                             BlockState /*oldState*/, bool /*movedByPiston*/) {
            FloatingSchedule(level.Ticks(), pos, newState.Block());
        }

        bool FloatingUpdateShape(const IBlockAccess& /*level*/, const glm::ivec3& pos,
                                 BlockState state, Direction /*toNeighbour*/,
                                 BlockID /*neighbourId*/, BlockState& /*outState*/,
                                 ScheduledTickAccess* ticks) {
            FloatingSchedule(ticks, pos, state.Block());
            return false;   // shape unchanged
        }

        void FloatingNeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                     BlockID /*sourceBlock*/, bool /*movedByPiston*/) {
            // A signal arriving through a wire reaches the block as a
            // neighborChanged, not an updateShape; book the check for it too.
            FloatingSchedule(level.Ticks(), pos, state.Block());
        }

        void FloatingTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                          JavaRandom& /*random*/) {
            const BlockID id = state.Block();
            const glm::ivec3 above = pos + glm::ivec3(0, 1, 0);
            const bool wantsToFloat = FloatingIsPowered(id)
                ? HasNeighborSignal(level, pos)
                : pos.y < World::MAX_Y;
            if (wantsToFloat && above.y <= World::MAX_Y &&
                FallingBlockIsFree(level.GetBlockState(above.x, above.y, above.z))) {
                level.SetBlock(above.x, above.y, above.z, id, World::UpdateFlags::All);
                level.SetBlock(pos.x, pos.y, pos.z, BlockID::Air, World::UpdateFlags::All);
                return;   // the lifted block's onPlace books its next step
            }
            // The mod re-books unconditionally. Only the powered block needs
            // the poll (a signal can arrive without a shape update); an
            // unpowered one is re-checked by the updateShape that fires when
            // the cell above changes, so ore buried in an island does not
            // keep a tick alive forever once a neighbour was mined.
            if (FloatingIsPowered(id)) FloatingSchedule(level.Ticks(), pos, id);
        }

        // ── The Aether (pass one): berry bush ──────────────────────────────
        //
        // Aether BerryBushBlock.useWithoutItem, in its berry_bush_consistency
        // mode (the one this port takes — the sweet-berry-like bush): an
        // empty-hand click pops the bush's loot (Block.dropResources with an
        // empty tool: blue berries 1-3, blocks/berry_bush.json) and leaves the
        // stem, which regrows by random tick (BlockGrowth.cpp). The loot and
        // the sound are the server's; the stem is predicted on both sides.
        UseResult BerryBushUse(ILevelWrite* world, const glm::ivec3& pos,
                               IUsePlayer* /*player*/, const BlockHitResult& /*hit*/) {
            if (!world) return UseResult::Pass;
            const BlockState state = world->GetBlockState(pos.x, pos.y, pos.z);
            if (state.Block() != BlockID::BerryBush) return UseResult::Pass;
            if (!world->IsClientSide()) {
                DropBlockLoot(*world, pos, state);
                // SweetBerryBushBlock.useWithoutItem:110 — playSound(null, pos,
                // SWEET_BERRY_BUSH_PICK_BERRIES, BLOCKS, 1.0, 0.8 + nextFloat() * 0.4).
                JavaRandom* r = world->Random();
                world->PlaySound(nullptr, pos, SoundEvents::SWEET_BERRY_BUSH_PICK_BERRIES, SoundSource::Blocks,
                                 1.0f, 0.8f + (r ? r->NextFloat() : 0.5f) * 0.4f);
            }
            // setBlock(pos, BERRY_BUSH_STEM, 1 | 2)
            world->SetBlock(pos.x, pos.y, pos.z, BlockID::BerryBushStem, World::UpdateFlags::All);
            return UseResult::Success;
        }

        // MC NetherPortalBlock.updateShape (NetherPortalBlock.java:85) — the
        // rule that makes a portal vanish when someone mines its frame. One
        // body for both portal families: the block says which family it is,
        // and the family says what its frame and its portal block are.
        //
        // Two guards before the expensive part:
        //   * a change along the horizontal axis PERPENDICULAR to the portal
        //     plane is somebody walking past with a block, not a frame edit,
        //     so it is ignored outright (MC's `wrongAxis`);
        //   * a neighbouring portal block changing is the collapse already in
        //     progress, and re-walking the shape for every one of up to 441
        //     cells per cell would be quadratic.
        // Only then does it re-walk the frame, and only a NON-complete shape
        // deletes the block. World turns the AIR answer into a destroy, which
        // notifies ITS neighbours — that cascade is what takes the whole
        // portal down from one broken frame block, exactly as in vanilla.
        bool FamilyPortalUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                     BlockState state,
                                     Direction toNeighbour, BlockID neighbourId,
                                     BlockState& outState,
                                     ScheduledTickAccess* /*ticks*/) {
            const PortalFamily* family = FamilyOfPortalBlock(state.Block());
            if (!family) return false;

            const Axis updateAxis = AxisOf(toNeighbour);
            const Axis axis = (state.GetName(PropertyId::HORIZONTAL_AXIS) == "z")
                                  ? Axis::Z : Axis::X;

            const bool wrongAxis = (axis != updateAxis) && IsHorizontal(toNeighbour);
            if (wrongAxis) return false;
            if (neighbourId == family->portalBlock) return false;

            if (PortalShape::FindAnyShape(level, pos, axis, *family).IsComplete()) return false;

            outState = BlockState{};   // air — World destroys with drops (there are none)
            return true;
        }

        // MC NetherPortalBlock.animateTick (NetherPortalBlock.java:187), for
        // the hush portal: four particles a tick, each thrown out of the
        // portal's face — sideways along whichever horizontal axis the
        // portal is NOT continuous on — and, one tick in a hundred, the
        // ambient hum. The particle is the Hush's own (ParticleKind::
        // HushPortal, the cyan cousin of MC's PORTAL); the geometry is
        // vanilla's to the letter. The nether portal has no emitter here:
        // vanilla's purple shimmer is not wired, and the immersive surface
        // has its own.
        void HushPortalAnimateTick(EntityLevel& level, const glm::ivec3& pos,
                                   BlockState state, JavaRandom& random) {
            // MC: random.nextInt(100) == 0 → level.playLocalSound(centre,
            // PORTAL_AMBIENT, BLOCKS, 0.5, nextFloat() * 0.4 + 0.8, false).
            // The Hush portal's own event (assets/sound_overlays/obeycraft)
            // is the nether portal's hum, pitched down into the deep.
            if (random.NextInt(100) == 0) {
                level.PlayLocalSound(glm::dvec3(pos.x + 0.5, pos.y + 0.5, pos.z + 0.5),
                                     "obeycraft:block.hush_portal.ambient", SoundSource::Blocks,
                                     0.5f, random.NextFloat() * 0.4f + 0.8f, false);
            }

            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            const BlockID self = state.Block();
            // MC: `!level.getBlockState(pos.west()).is(this) &&
            //      !level.getBlockState(pos.east()).is(this)` — no portal
            // block on either side along X means the portal runs along Z
            // (its face is normal to X), so the particle leaves along X.
            const bool noPortalAlongX =
                blocks->GetBlock(pos.x - 1, pos.y, pos.z) != self &&
                blocks->GetBlock(pos.x + 1, pos.y, pos.z) != self;

            for (int i = 0; i < 4; ++i) {
                // MC draws in this exact order; keep it so the same seed
                // gives the same shimmer.
                double x = static_cast<double>(pos.x) + random.NextDouble();
                double y = static_cast<double>(pos.y) + random.NextDouble();
                double z = static_cast<double>(pos.z) + random.NextDouble();
                double xa = (static_cast<double>(random.NextFloat()) - 0.5) * 0.5;
                double ya = (static_cast<double>(random.NextFloat()) - 0.5) * 0.5;
                double za = (static_cast<double>(random.NextFloat()) - 0.5) * 0.5;
                const int flip = random.NextInt(2) * 2 - 1;
                if (noPortalAlongX) {
                    x  = static_cast<double>(pos.x) + 0.5 + 0.25 * static_cast<double>(flip);
                    xa = static_cast<double>(random.NextFloat() * 2.0f * static_cast<float>(flip));
                } else {
                    z  = static_cast<double>(pos.z) + 0.5 + 0.25 * static_cast<double>(flip);
                    za = static_cast<double>(random.NextFloat() * 2.0f * static_cast<float>(flip));
                }
                level.AddParticle(ParticleKind::HushPortal, x, y, z, xa, ya, za);
            }
        }

        // MC AmethystClusterBlock.updateShape (amethyst_cluster, the three
        // buds, The Hush's resonant_cluster):
        //
        //   return directionToNeighbour == state.getValue(FACING).getOpposite()
        //          && !state.canSurvive(level, pos)
        //       ? Blocks.AIR.defaultBlockState() : super.updateShape(...);
        //
        // Only the support side is consulted — mining the block a cluster
        // grows out of drops it; a change on any other side is ignored. The
        // waterlogged fluid tick vanilla books here rides the engine's fluid
        // path already (docs/fluids.md), so it is not repeated.
        bool AmethystClusterUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                        BlockState state,
                                        Direction toNeighbour, BlockID /*neighbourId*/,
                                        BlockState& outState,
                                        ScheduledTickAccess* /*ticks*/) {
            if (toNeighbour != Opposite(FacingOf(state))) return false;
            if (AmethystClusterCanSurvive(level, pos, state)) return false;
            outState = BlockStates::Default(BlockID::Air);
            return true;
        }

        // MC VineBlock.updateShape — re-derive which faces still have support.
        // A vine that loses its last face returns AIR, and World turns that into
        // a destroy-with-drops, which is how a vine curtain falls when the wall
        // behind it is mined.
        bool VineUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                 BlockState state,
                                 Direction toNeighbour, BlockID /*neighbourId*/,
                                 BlockState& outState,
                                         ScheduledTickAccess* /*ticks*/) {
            const BlockState next = VineUpdateShape(level, pos, state, toNeighbour);
            if (next == state) return false;
            outState = next;
            return true;
        }

        // MC MultifaceBlock.updateShape — the clump drops the one face whose
        // surface went, and goes with it when that was the last.
        bool MultifaceUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                      BlockState state,
                                      Direction toNeighbour, BlockID /*neighbourId*/,
                                      BlockState& outState,
                                         ScheduledTickAccess* /*ticks*/) {
            const BlockState next = MultifaceUpdateShape(level, pos, state, toNeighbour);
            if (next == state) return false;
            outState = next;
            return true;
        }

        // MC StairBlock.updateShape — re-derive SHAPE when a horizontal
        // neighbour changes. This is what makes two stairs meeting at a right
        // angle grow into a corner, and what un-corners them again when one is
        // mined. Vertical changes are ignored, exactly as vanilla's
        // `directionToNeighbour.getAxis().isHorizontal()` gate does.
        bool StairUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                  BlockState state,
                                  Direction toNeighbour, BlockID /*neighbourId*/,
                                         BlockState& outState,
                                         ScheduledTickAccess* /*ticks*/) {
            const BlockState next = StairsUpdateShape(level, pos, state, toNeighbour);
            if (next == state) return false;
            outState = next;
            return true;
        }

        // MC FenceBlock / IronBarsBlock.updateShape — re-resolve the ONE side
        // the change came from, which is what joins a fence line together as
        // it is built and opens it again when a post is mined.
        bool CrossCollisionUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                           BlockState state,
                                           Direction toNeighbour, BlockID /*neighbourId*/,
                                         BlockState& outState,
                                         ScheduledTickAccess* /*ticks*/) {
            const BlockState next = CrossUpdateShape(level, pos, state, toNeighbour);
            if (next == state) return false;
            outState = next;
            return true;
        }

        // MC WallBlock.updateShape. A wall reacts to the block ABOVE as well as
        // beside it — that is what turns its arms tall and drops its post when
        // something is set on top of it.
        bool WallUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                 BlockState state,
                                 Direction toNeighbour, BlockID /*neighbourId*/,
                                         BlockState& outState,
                                         ScheduledTickAccess* /*ticks*/) {
            const BlockState next = WallUpdateShape(level, pos, state, toNeighbour);
            if (next == state) return false;
            outState = next;
            return true;
        }

        // MC FenceGateBlock.updateShape — only IN_WALL can change, and only
        // when the change is along the gate's hinge axis.
        bool FenceGateUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                      BlockState state,
                                      Direction toNeighbour, BlockID /*neighbourId*/,
                                         BlockState& outState,
                                         ScheduledTickAccess* /*ticks*/) {
            const BlockState next = FenceGateUpdateShape(level, pos, state, toNeighbour);
            if (next == state) return false;
            outState = next;
            return true;
        }

        // MC FenceGateBlock.useWithoutItem — the swing.
        //
        //   if (OPEN)  -> OPEN = false
        //   else       -> if (FACING == player.getDirection().getOpposite())
        //                     FACING = player.getDirection();
        //                 OPEN = true
        //
        // The re-aim is what makes a gate always swing away from whoever opened
        // it rather than through them.
        //
        // Runs on both sides: the client predicts the swing so the gate moves
        // on the same frame, and the server's authoritative state follows.
        UseResult FenceGateUse(ILevelWrite* world, const glm::ivec3& pos,
                               IUsePlayer* player, const BlockHitResult& /*hit*/) {
            if (!world || !player) return UseResult::Pass;
            const BlockID id = world->GetBlock(pos.x, pos.y, pos.z);
            if (!IsFenceGateBlock(id)) return UseResult::Pass;

            const BlockState state = world->GetBlockState(pos.x, pos.y, pos.z);
            const Direction facing = FromYRot(player->getYaw());
            const BlockState next = FenceGateToggle(state, facing);
            if (next == state) return UseResult::Pass;

            // MC uses flag 10 (UPDATE_CLIENTS | UPDATE_KNOWN_SHAPE) — no
            // neighbour notification, because opening a gate changes nothing
            // any neighbour cares about. UpdateFlags::All is what every other
            // handler here passes and the extra notify is harmless; the fences
            // beside it re-resolve to the same connection either way.
            world->SetBlock(pos.x, pos.y, pos.z, next, World::UpdateFlags::All);
            // MC FenceGateBlock.useWithoutItem:129 — playSound(player, ...):
            // the opener hears their own prediction, the rest the server's.
            if (const WoodType* wood = WoodTypeOf(id)) {
                const bool opens = next.GetIndex(PropertyId::OPEN) == 0;   // booleans list true first
                world->PlaySound(player, pos, opens ? wood->fenceGateOpen : wood->fenceGateClose,
                                 SoundSource::Blocks, 1.0f, OpenCloseJitter(*world));
            }
            return UseResult::Success;
        }

        // ── Nether portal: zombified piglins ─────────────────────────────
        // MC NetherPortalBlock.randomTick: in a natural dimension (the
        // Overworld), with mob spawning on, each random tick of a portal
        // block has `difficulty / 2000` odds of putting a zombified piglin
        // in the air above it. Peaceful is 0, so nothing on peaceful.
        bool NetherPortalTicksRandomly(BlockState /*state*/) { return true; }
        void NetherPortalRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                                    BlockState /*state*/, JavaRandom& random) {
            if (level.IsClientSide()) return;
            if (level.GetDimension() != DimensionId::Overworld) return;
            const World* world = dynamic_cast<const World*>(&level);
            if (!world || !world->GetDoMobSpawning()) return;
            const int difficultyId = static_cast<int>(world->GetDifficulty());
            if (random.NextInt(2000) >= difficultyId) return;
            const glm::ivec3 above = pos + glm::ivec3(0, 1, 0);
            if (level.GetBlock(above.x, above.y, above.z) != BlockID::Air) return;
            // MC: the spawned piglin gets setPortalCooldown (300 ticks), so
            // it does not step straight back through the portal it stands in.
            SpawnMobFromItem(EntityTypeId::ZombifiedPiglin, above, /*tryMoveDown=*/false,
                             /*movedUp=*/false, level.GetDimension(), /*portalCooldownTicks=*/300);
        }

        // ── Doors ─────────────────────────────────────────────────────────
        // MC DoorBlock.useWithoutItem: a wooden door swings on a click from
        // either half, and setOpen mirrors the other half. Runs on both
        // sides like the gate: the client predicts the swing.
        // MC DoorBlock.setOpen for one door: this half, then the other half
        // of the same door (MC does the second through updateShape; this
        // engine has no double-block linkage, so it is explicit).
        void SetDoorOpen(ILevelWrite& world, const glm::ivec3& pos, BlockState state,
                         std::string_view to) {
            const BlockID id = state.Block();
            world.SetBlock(pos.x, pos.y, pos.z, state.SetName(PropertyId::OPEN, to), World::UpdateFlags::All);

            const bool lower = state.GetName(PropertyId::DOUBLE_BLOCK_HALF) == "lower";
            const glm::ivec3 other = pos + glm::ivec3(0, lower ? 1 : -1, 0);
            const BlockState otherState = world.GetBlockState(other.x, other.y, other.z);
            if (otherState.Block() == id &&
                otherState.GetName(PropertyId::DOUBLE_BLOCK_HALF) == (lower ? "upper" : "lower")) {
                world.SetBlock(other.x, other.y, other.z, otherState.SetName(PropertyId::OPEN, to),
                               World::UpdateFlags::All);
            }
        }

        // The other leaf of a double door, if `pos` is one: the door in the
        // next cell on this door's FREE side (the side away from its hinge —
        // DoorBlock.getHinge puts the hinges of a pair on the outer edges,
        // so the leaves meet in the middle), facing the same way, hinged on
        // the opposite side, and currently in the same open state so the
        // pair swings together. Any wooden door qualifies, not only the same
        // wood. Not vanilla — MC swings one leaf per click.
        std::optional<glm::ivec3> DoubleDoorPartner(const ILevelWrite& world, const glm::ivec3& pos,
                                                    BlockState state) {
            const Direction facing =
                HorizontalFacingFromIndex(state.GetIndex(PropertyId::HORIZONTAL_FACING));
            const bool hingeLeft = state.GetName(PropertyId::HINGE) == "left";
            // getHinge: a lower door on the counter-clockwise side makes the
            // new door hinge RIGHT — so a right-hinged leaf's partner is
            // counter-clockwise of it, and a left-hinged one's is clockwise.
            const Direction toward = hingeLeft ? ClockWise(facing) : Opposite(ClockWise(facing));
            const glm::ivec3 p(pos.x + StepX(toward), pos.y, pos.z + StepZ(toward));
            const BlockState s = world.GetBlockState(p.x, p.y, p.z);
            if (!IsWoodenDoorBlock(s.Block())) return std::nullopt;
            if (s.GetIndex(PropertyId::HORIZONTAL_FACING) != state.GetIndex(PropertyId::HORIZONTAL_FACING)) return std::nullopt;
            if ((s.GetName(PropertyId::HINGE) == "left") == hingeLeft) return std::nullopt;
            if (s.GetName(PropertyId::DOUBLE_BLOCK_HALF) != state.GetName(PropertyId::DOUBLE_BLOCK_HALF)) return std::nullopt;
            if (s.GetIndex(PropertyId::OPEN) != state.GetIndex(PropertyId::OPEN)) return std::nullopt;
            return p;
        }

        UseResult DoorUse(ILevelWrite* world, const glm::ivec3& pos,
                          IUsePlayer* player, const BlockHitResult& /*hit*/) {
            if (!world || !player) return UseResult::Pass;
            const BlockID id = world->GetBlock(pos.x, pos.y, pos.z);
            if (!IsWoodenDoorBlock(id)) return UseResult::Pass;

            const BlockState state = world->GetBlockState(pos.x, pos.y, pos.z);
            const bool wasOpen = state.GetIndex(PropertyId::OPEN) == 0;   // booleans list true first
            const std::string_view to = wasOpen ? "false" : "true";
            // The partner is found BEFORE this leaf moves: the match tests
            // the two leaves' open states against each other.
            const std::optional<glm::ivec3> partner = DoubleDoorPartner(*world, pos, state);
            SetDoorOpen(*world, pos, state, to);
            if (partner) {
                SetDoorOpen(*world, *partner, world->GetBlockState(partner->x, partner->y, partner->z), to);
            }
            // MC DoorBlock.useWithoutItem:163 → playSound(player, ...), the
            // BlockSetType's door sound.
            if (const BlockSetType* set = BlockSetTypeOf(id)) {
                world->PlaySound(player, pos, wasOpen ? set->doorClose : set->doorOpen,
                                 SoundSource::Blocks, 1.0f, OpenCloseJitter(*world));
            }
            return UseResult::Success;
        }

        // ── Signs ─────────────────────────────────────────────────────────
        // MC SignBlock.useWithoutItem: an empty-hand click asks to edit the
        // face you are standing in front of (SignBlock.openTextEdit). The
        // waxed / someone-else-is-editing gates need the block entity, which
        // lives with the server — IUsePlayer::OpenSignEditor carries the
        // request there; the client just sees the click consumed.
        UseResult SignUse(ILevelWrite* world, const glm::ivec3& pos,
                          IUsePlayer* player, const BlockHitResult& /*hit*/) {
            if (!world || !player) return UseResult::Pass;
            if (!IsSignBlock(world->GetBlock(pos.x, pos.y, pos.z))) return UseResult::Pass;
            player->OpenSignEditor(pos);
            return UseResult::SuccessServer;
        }

        // MC SignBlock.useItemOn: a SignApplicator in hand — a dye, an ink
        // sac, a glow ink sac, honeycomb — is applied to the face you face;
        // anything else falls through to the empty-hand edit.
        UseResult SignUseItemOn(ItemStack& stack, ILevelWrite* world, const glm::ivec3& pos,
                                IUsePlayer* player, uint32_t hand, const BlockHitResult& /*hit*/) {
            if (!world || !player) return UseResult::Pass;
            if (!IsSignBlock(world->GetBlock(pos.x, pos.y, pos.z))) return UseResult::Pass;
            const ItemID id = stack.itemId;
            const bool applicator =
                id == Items::InkSac || id == Items::GlowInkSac || id == Items::Honeycomb ||
                (id >= Items::WhiteDye && id <= Items::BlackDye);
            if (!applicator) return UseResult::TryEmptyHandInteraction;
            player->ApplySignItem(pos, hand);
            return UseResult::SuccessServer;
        }

        // ── Trapdoors ─────────────────────────────────────────────────────
        // MC TrapDoorBlock.useWithoutItem → toggle: OPEN cycles, the set
        // type's open/close sound plays. Iron is not hand-openable (PASS);
        // copper is. Runs on both sides — the client predicts the swing.
        UseResult TrapDoorUse(ILevelWrite* world, const glm::ivec3& pos,
                              IUsePlayer* player, const BlockHitResult& /*hit*/) {
            if (!world || !player) return UseResult::Pass;
            const BlockID id = world->GetBlock(pos.x, pos.y, pos.z);
            if (!IsHandOpenableTrapdoor(id)) return UseResult::Pass;

            const BlockState state = world->GetBlockState(pos.x, pos.y, pos.z);
            const bool wasOpen = state.GetIndex(PropertyId::OPEN) == 0;   // booleans list true first
            world->SetBlock(pos.x, pos.y, pos.z, state.SetName(PropertyId::OPEN, wasOpen ? "false" : "true"),
                            World::UpdateFlags::All);

            // MC TrapDoorBlock.toggle:94 → playSound(player, ...), the
            // BlockSetType's trapdoor sound.
            if (const BlockSetType* set = BlockSetTypeOf(id)) {
                world->PlaySound(player, pos, wasOpen ? set->trapdoorClose : set->trapdoorOpen,
                                 SoundSource::Blocks, 1.0f, OpenCloseJitter(*world));
            }
            return UseResult::Success;
        }

        // Every container block does the same thing on a right-click: ask for
        // its menu. MC spreads this across ChestBlock.useWithoutItem,
        // BarrelBlock, DispenserBlock, HopperBlock, AbstractFurnaceBlock… each
        // calling player.openMenu(state.getMenuProvider(...)). The menu type is
        // the only thing that varies, so one template covers all of them.
        template <MenuType kType>
        UseResult OpenContainerUse(ILevelWrite* /*world*/, const glm::ivec3& pos,
                                   IUsePlayer* player, const BlockHitResult& /*hit*/) {
            if (!player) return UseResult::Pass;
            player->OpenMenu(kType, pos);
            return UseResult::Success;
        }

        // ── Chest lids (chest, trapped chest, ender chest) ────────────────
        // MC ChestBlock.tick / EnderChestBlock.tick: the scheduled tick the
        // opener counter books every 5 ticks while the chest is open.
        void ChestTick(ILevelWrite& level, const glm::ivec3& pos, BlockState /*state*/,
                       JavaRandom& /*random*/) {
            if (auto* chest = dynamic_cast<ChestBlockEntity*>(level.GetBlockEntity(pos))) {
                chest->RecheckOpen(level);
            }
        }

        // MC BaseEntityBlock.triggerEvent: the event belongs to the block
        // entity (for a chest, event 1 = the opener count that aims the lid).
        bool BlockEntityTriggerEvent(ILevelWrite& level, const glm::ivec3& pos,
                                     BlockState /*state*/, int b0, int b1) {
            BlockEntity* be = level.GetBlockEntity(pos);
            return be && be->TriggerEvent(b0, b1);
        }

        // MC SpawnerBlock (a BaseEntityBlock): block events go to the
        // SpawnerBlockEntity (event 1, the client's delay reset), plus the
        // engine's spawn-burst event standing in for MC LevelEvent 2004
        // (PARTICLES_MOBBLOCK_SPAWN — this port has no level-event packet):
        // the server reports it handled so it is broadcast, and the client
        // throws LevelEventHandler's 20 SMOKE + FLAME around the cage.
        bool SpawnerTriggerEvent(ILevelWrite& level, const glm::ivec3& pos,
                                 BlockState state, int b0, int b1) {
            if (b0 != SpawnerBlockEntity::kEventSpawnParticles) {
                return BlockEntityTriggerEvent(level, pos, state, b0, b1);
            }
            if (level.IsClientSide()) {
                if (JavaRandom* random = level.Random()) {
                    for (int i = 0; i < 20; ++i) {
                        const double x = pos.x + 0.5 + (random->NextDouble() - 0.5) * 2.0;
                        const double y = pos.y + 0.5 + (random->NextDouble() - 0.5) * 2.0;
                        const double z = pos.z + 0.5 + (random->NextDouble() - 0.5) * 2.0;
                        level.AddParticle(ParticleKind::Smoke, x, y, z, 0.0, 0.0, 0.0);
                        level.AddParticle(ParticleKind::Flame, x, y, z, 0.0, 0.0, 0.0);
                    }
                }
            }
            return true;
        }

        // MC TrappedChestBlock.getSignal: the number of players with it open,
        // clamped to 15. MC reads the block entity off the BlockGetter; here
        // only ILevelWrite carries block entities, and every level that
        // evaluates redstone is one — anything else reads as unpowered.
        int TrappedChestGetSignal(const IBlockAccess& level, const glm::ivec3& pos,
                                  BlockState /*state*/, Direction /*direction*/) {
            const auto* writable = dynamic_cast<const ILevelWrite*>(&level);
            if (!writable) return 0;
            const auto* chest = dynamic_cast<const ChestBlockEntity*>(
                const_cast<ILevelWrite*>(writable)->GetBlockEntity(pos));
            return chest ? std::clamp(chest->GetOpenerCount(), 0, 15) : 0;
        }

        // MC TrappedChestBlock.getDirectSignal: strong power only upward.
        int TrappedChestGetDirectSignal(const IBlockAccess& level, const glm::ivec3& pos,
                                        BlockState state, Direction direction) {
            return direction == Direction::Up
                ? TrappedChestGetSignal(level, pos, state, direction) : 0;
        }

        // MC CampfireBlock.useItemOn (CampfireBlock.java:81-98): right-clicking
        // a campfire with something that has a campfire_cooking recipe lays one
        // of it on the fire.
        //
        // Deliberately NOT gated on `lit`. MC lets you load an unlit campfire —
        // the food just sits there until someone lights it, because the block
        // state is what picks the cooking ticker, not a flag on the item.
        //
        // The recipe test is the one branch that must run on BOTH sides: it
        // decides between "this click was food" and "this click was a block
        // placement", and only the client can answer that in time to predict.
        // Whether a slot is actually free needs the block entity, so that part
        // is deferred — and MC returns CONSUME for the full-campfire case
        // anyway, which is what we return uniformly. (MC's success path returns
        // SUCCESS_SERVER; the difference is the arm swing and a stat award,
        // neither of which exists here.)
        UseResult CampfireUseItemOn(ItemStack& stack, ILevelWrite* /*world*/,
                                    const glm::ivec3& pos, IUsePlayer* player,
                                    uint32_t hand, const BlockHitResult& /*hit*/) {
            if (!player) return UseResult::Pass;
            if (!RecipeManager::FindCooking(CookingKind::CampfireCooking, stack)) {
                // Not food — fall through to the empty-hand path exactly as MC
                // does, so a click holding a block still places it.
                return UseResult::TryEmptyHandInteraction;
            }
            player->PlaceCampfireFood(pos, hand);
            return UseResult::Consume;
        }

        // ── Frosted ice (MC FrostedIceBlock) ─────────────────────────────
        //
        // What Frost Walker lays over water: it ages 0..3 on scheduled
        // ticks while the light is bright enough (or it is thinly
        // surrounded), then melts back to water, taking weakly supported
        // neighbours with it.

        // Direction.values(): down, up, north, south, west, east.
        constexpr glm::ivec3 kFrostedIceNeighbours[6] = {
            {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0},
        };

        // Mth.nextInt(random, min, max).
        int FrostedIceNextInt(JavaRandom& random, int min, int max) {
            return min >= max ? min : random.NextInt(max - min + 1) + min;
        }

        // IceBlock.melt: a dimension where water evaporates (the Nether's
        // WATER_EVAPORATES) just loses the block; anywhere else it becomes
        // water and tells the cell (neighborChanged(pos, WATER)).
        void FrostedIceMelt(ILevelWrite& level, const glm::ivec3& pos) {
            if (level.GetDimension() == DimensionId::Nether) {
                level.SetBlock(pos.x, pos.y, pos.z, BlockStates::Default(BlockID::Air), World::UpdateFlags::All);
                return;
            }
            level.SetBlock(pos.x, pos.y, pos.z, BlockStates::Default(BlockID::Water), World::UpdateFlags::All);
            level.NeighborChanged(pos, BlockID::Water);
        }

        // FrostedIceBlock.fewerNeigboursThan (sic): fewer than `limit` of the
        // six neighbours are frosted ice.
        bool FrostedIceFewerNeighboursThan(const IBlockAccess& level, const glm::ivec3& pos, int limit) {
            int result = 0;
            for (const glm::ivec3& d : kFrostedIceNeighbours) {
                const glm::ivec3 n = pos + d;
                if (level.GetBlockState(n.x, n.y, n.z).Is(BlockID::FrostedIce) && ++result >= limit) {
                    return false;
                }
            }
            return true;
        }

        // FrostedIceBlock.slightlyMelt: age by one (UPDATE_CLIENTS), or melt
        // at age 3. True when it melted.
        bool FrostedIceSlightlyMelt(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            const int age = state.GetIndex(PropertyId::AGE_3);
            if (age < 3) {
                level.SetBlock(pos.x, pos.y, pos.z, state.SetIndex(PropertyId::AGE_3, age + 1),
                               World::UpdateFlags::UpdateClients);
                return false;
            }
            FrostedIceMelt(level, pos);
            return true;
        }

        void FrostedIceOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState /*newState*/,
                               BlockState /*oldState*/, bool /*movedByPiston*/) {
            // scheduleTick(pos, this, Mth.nextInt(level.getRandom(), 60, 120)).
            ScheduledTickAccess* ticks = level.Ticks();
            JavaRandom* random = level.Random();
            if (!ticks || !random) return;
            ticks->ScheduleTick(pos, BlockID::FrostedIce, FrostedIceNextInt(*random, 60, 120));
        }

        void FrostedIceTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            ScheduledTickAccess* ticks = level.Ticks();
            if (!ticks) return;
            if (random.NextInt(3) == 0 || FrostedIceFewerNeighboursThan(level, pos, 4)) {
                // The End reads block light alone; elsewhere
                // getMaxLocalRawBrightness (block, or sky less the darkening).
                int brightness;
                if (level.GetDimension() == DimensionId::End) {
                    brightness = level.GetBrightness(Lighting::LightLayer::Block, pos.x, pos.y, pos.z);
                } else {
                    const World* world = dynamic_cast<const World*>(&level);
                    brightness = level.GetMaxLocalRawBrightness(pos.x, pos.y, pos.z,
                                                                world ? world->GetSkyDarken() : 0);
                }
                const int threshold = 11 - state.GetIndex(PropertyId::AGE_3) -
                                      Lighting::BlockLightProperties::Dampening(state);
                if (brightness > threshold && FrostedIceSlightlyMelt(level, pos, state)) {
                    for (const glm::ivec3& d : kFrostedIceNeighbours) {
                        const glm::ivec3 n = pos + d;
                        const BlockState neighbour = level.GetBlockState(n.x, n.y, n.z);
                        if (neighbour.Is(BlockID::FrostedIce) && !FrostedIceSlightlyMelt(level, n, neighbour)) {
                            ticks->ScheduleTick(n, BlockID::FrostedIce, FrostedIceNextInt(random, 20, 40));
                        }
                    }
                    return;
                }
            }
            ticks->ScheduleTick(pos, BlockID::FrostedIce, FrostedIceNextInt(random, 20, 40));
        }

        void FrostedIceNeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState /*state*/,
                                       BlockID sourceBlock, bool /*movedByPiston*/) {
            // A frosted-ice neighbour changed and this one now has fewer
            // than two frosted neighbours: it melts at once.
            if (sourceBlock == BlockID::FrostedIce && FrostedIceFewerNeighboursThan(level, pos, 2)) {
                FrostedIceMelt(level, pos);
            }
        }

    } // namespace

    // Declared at file scope in BlockRegistry.cpp, same as
    // ItemRegistry_RegisterBehaviors is in Item.cpp.
    void BlockRegistry_RegisterBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        // Matched on registrySlug, not modelName: several blocks deliberately
        // SHARE a model name (Water→"water_still", every Infested* variant
        // borrows its host block's model), so a modelName scan can attach a
        // behaviour to the wrong block — it is the same trap that had
        // RecipeManager resolving "stone" to InfestedStone.
        auto forSlug = [&blocks](const char* slug) -> Block* {
            for (auto& block : blocks) {
                if (block.registrySlug == slug) return &block;
            }
            return nullptr;
        };

        // Attach one container menu to every block that opens it. A slug that
        // matches nothing is a silent "this block never opens", so say so —
        // that failure mode is invisible in play and looks identical to a
        // broken menu.
        int attached = 0, missing = 0;
        auto attachContainers = [&](std::initializer_list<const char*> slugs,
                                    BlockUseWithoutItemFn fn) {
            for (const char* slug : slugs) {
                if (Block* b = forSlug(slug)) { b->useWithoutItem = fn; ++attached; }
                else {
                    ++missing;
                    Log::Warning("[BlockBehaviors] no block with registrySlug '%s' — "
                                 "it will not open a menu", slug);
                }
            }
        };

        // ── Campfires ─────────────────────────────────────────────────────
        // useItemOn only: there is no empty-hand interaction on a campfire in
        // MC, so an empty hand correctly does nothing.
        for (const char* slug : {"campfire", "soul_campfire"}) {
            if (Block* b = forSlug(slug)) { b->useItemOn = &CampfireUseItemOn; ++attached; }
            else {
                ++missing;
                Log::Warning("[BlockBehaviors] no block with registrySlug '%s' — "
                             "food cannot be placed on it", slug);
            }
        }

        if (Block* craftingTable = forSlug("crafting_table")) {
            craftingTable->useWithoutItem = &CraftingTableUse;
            // MC's CraftingTableBlock has no useItemOn override, so a click
            // holding an item routes through TryEmptyHandInteraction — which
            // is exactly what the item dispatch already does when the held
            // item declines. Leaving useItemOn null means "sneak + item still
            // places the block", matching vanilla.
        }

        // ── Redstone dust ─────────────────────────────────────────────────
        // The right-click dot/cross toggle, plus the neighbour hook that makes
        // two wires laid next to each other join up. See RedstoneWire.cpp.
        // The shape and power hooks are wired in RegisterRedstoneBehaviors.
        if (Block* wire = forSlug("redstone_wire")) {
            wire->useWithoutItem  = &RedstoneWireUse;
        }

        // ── Dragon egg ────────────────────────────────────────────────────
        // MC DragonEggBlock routes BOTH useWithoutItem and attack into the
        // same teleport() — right-click or punch, the egg blinks away. Its
        // FALLING behaviour needs no entry: IsFallingBlock lists DragonEgg.
        if (Block* egg = forSlug("dragon_egg")) {
            egg->useWithoutItem = &DragonEggUse;
            egg->attack         = &DragonEggAttack;
        }

        // ── Nether portal ─────────────────────────────────────────────────
        // The only behaviour the BLOCK itself owns. Lighting a portal lives in
        // the fire block's onPlace below (MC BaseFireBlock.onPlace), and
        // walking through one is an entity-side check, not a block callback.
        if (Block* portal = forSlug("nether_portal")) {
            portal->updateShape  = &FamilyPortalUpdateShape;
            portal->entityInside = &PortalEntityInside;
        } else {
            Log::Warning("[BlockBehaviors] no block with registrySlug "
                         "'nether_portal' — portals will not close when their "
                         "frame is broken");
        }

        // ── Hush portal ───────────────────────────────────────────────────
        // The nether portal's twin (PortalFamily::Hush): the same collapse
        // rule against a reinforced-deepslate frame, the same contact hook,
        // plus the ambient shimmer. Lit by the echo shard (ItemBehaviors),
        // never by fire. No randomTick: the nether portal's is the zombified
        // piglin spawn, and nothing comes out of the Hush.
        if (Block* portal = forSlug("hush_portal")) {
            portal->updateShape  = &FamilyPortalUpdateShape;
            portal->entityInside = &PortalEntityInside;
            portal->animateTick  = &HushPortalAnimateTick;
        } else {
            Log::Warning("[BlockBehaviors] no block with registrySlug "
                         "'hush_portal' — the Hush is unreachable by vanilla portal");
        }
        // ── Aether portal ─────────────────────────────────────────────────
        // AetherPortalBlock is a nether-portal twin (HORIZONTAL_AXIS, a
        // glowstone frame, docs/mod-ports.md). The collapse rule is family-
        // driven and a no-op until a PortalFamily names aether_portal, so it
        // is safe to wire now. The contact hook is NOT: PortalTravel sends a
        // portal block with no family to the End, so it is attached only
        // once the family exists. The Twilight portal is a pool mechanic, not
        // a frame family; its hooks belong to that port.
        if (Block* portal = forSlug("aether_portal")) {
            portal->updateShape = &FamilyPortalUpdateShape;
            if (FamilyOfPortalBlock(BlockID::AetherPortal)) {
                portal->entityInside = &PortalEntityInside;
            }
        }

        // ── Frosted ice (MC FrostedIceBlock — Frost Walker's ice) ─────────
        if (Block* ice = forSlug("frosted_ice")) {
            ice->onPlace         = &FrostedIceOnPlace;
            ice->tick            = &FrostedIceTick;
            ice->neighborChanged = &FrostedIceNeighborChanged;
        }

        // ── The wither rose (MC WitherRoseBlock) ──────────────────────────
        if (Block* rose = forSlug("wither_rose")) rose->entityInside = &WitherRoseEntityInside;
        if (Block* eyeblossom = forSlug("open_eyeblossom")) {
            eyeblossom->entityInside = &EyeblossomEntityInside;
        }

        // ── The Aether: aerclouds and the berry bush ──────────────────────
        for (const char* slug : {"cold_aercloud", "golden_aercloud"}) {
            if (Block* cloud = forSlug(slug)) cloud->entityInside = &AercloudEntityInside;
        }
        if (Block* cloud = forSlug("blue_aercloud")) {
            cloud->entityInside = &BlueAercloudEntityInside;
        }
        if (Block* bush = forSlug("berry_bush")) {
            bush->useWithoutItem = &BerryBushUse;
        }
        // Pass two: gravitite ore and enchanted gravitite float (FloatingBlock).
        for (const char* slug : {"gravitite_ore", "enchanted_gravitite"}) {
            if (Block* b = forSlug(slug)) {
                b->onPlace         = &FloatingOnPlace;
                b->updateShape     = &FloatingUpdateShape;
                b->neighborChanged = &FloatingNeighborChanged;
                b->tick            = &FloatingTick;
            }
        }

        // Lighting a portal. Soul fire deliberately does NOT get this — MC
        // only overrides onPlace on BaseFireBlock, but SoulFireBlock can never
        // sit inside an obsidian frame (it needs soul sand or soul soil under
        // it), so vanilla's shared implementation is unreachable for it.
        if (Block* fire = forSlug("fire")) {
            fire->onPlace = &FireOnPlace;
        } else {
            Log::Warning("[BlockBehaviors] no block with registrySlug 'fire' — "
                         "nether portals can never be lit");
        }

        // ── End portal ────────────────────────────────────────────────────
        // Only the contact hook. The portal blocks themselves are placed by
        // the Eye of Ender (ItemBehaviors) and are unbreakable, so there is
        // nothing for onPlace or neighborChanged to do.
        if (Block* endPortal = forSlug("end_portal")) {
            endPortal->entityInside = &PortalEntityInside;
        } else {
            Log::Warning("[BlockBehaviors] no block with registrySlug "
                         "'end_portal' — the End is unreachable");
        }

        // ── End gateway ───────────────────────────────────────────────────
        // Same contact hook; the travel itself is PortalTravel's EndGateway
        // branch (a same-dimension hop to the outer islands, any entity). MC
        // gates entityInside on the block entity's 40-tick cooldown at
        // contact time; here the same BE cooldown gates the traverse itself —
        // one use per gateway per two seconds either way.
        if (Block* gateway = forSlug("end_gateway")) {
            gateway->entityInside = &PortalEntityInside;
        } else {
            Log::Warning("[BlockBehaviors] no block with registrySlug "
                         "'end_gateway' — gateways will not teleport");
        }

        // ── Vines ─────────────────────────────────────────────────────────
        // Faces drop as their support goes, and the last one taking the vine
        // with it. Without this a vine hangs in mid-air forever once the wall
        // behind it is mined.
        if (Block* vine = forSlug("vine")) {
            vine->updateShape = &VineUpdateShape;
        }
        for (const char* slug : { "glow_lichen", "sculk_vein", "resin_clump" }) {
            if (Block* b = forSlug(slug)) b->updateShape = &MultifaceUpdateShape;
        }

        // ── Amethyst clusters (and The Hush's resonant cluster) ───────────
        // A cluster survives only on the block behind its FACING; matched by
        // IsAmethystClusterBlock so the buds and the Hush twin share the rule.
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (IsAmethystClusterBlock(static_cast<BlockID>(i))) {
                blocks[i].updateShape = &AmethystClusterUpdateShape;
            }
        }

        // ── Stairs ────────────────────────────────────────────────────────
        // Corner formation. Matched by IsStairs (the "_stairs" model-name
        // test the rest of the stair code shares) rather than a 58-entry slug
        // list, so a new stair from an MC version bump is picked up with the
        // block definition alone.
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (IsStairs(static_cast<BlockID>(i))) {
                blocks[i].updateShape = &StairUpdateShape;
            }
        }

        // ── Fences, glass panes, iron bars ────────────────────────────────
        // Connection tracking. Same matching argument as the stairs above —
        // IsCrossCollisionBlock is the one definition of the family, shared
        // with the state declaration and the shape builder.
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (IsCrossCollisionBlock(static_cast<BlockID>(i))) {
                blocks[i].updateShape = &CrossCollisionUpdateShape;
            }
        }

        // ── Walls ─────────────────────────────────────────────────────────
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (IsWallBlock(static_cast<BlockID>(i))) {
                blocks[i].updateShape = &WallUpdateShape;
            }
        }

        // ── Fence gates ───────────────────────────────────────────────────
        // The swing, plus the IN_WALL tracking. `useWithoutItem` rather than
        // `useItemOn`, matching MC — which is what lets you open a gate while
        // holding a block instead of placing the block.
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (IsFenceGateBlock(static_cast<BlockID>(i))) {
                blocks[i].useWithoutItem  = &FenceGateUse;
                blocks[i].updateShape = &FenceGateUpdateShape;
            }
        }

        // ── Nether portal: the piglin spawn tick ─────────────────────────
        {
            auto& portal = blocks[static_cast<size_t>(BlockID::NetherPortal)];
            portal.isRandomlyTicking = &NetherPortalTicksRandomly;
            portal.randomTick        = &NetherPortalRandomTick;
        }

        // ── Doors ─────────────────────────────────────────────────────────
        // The swing, by hand, for every door but iron.
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (IsWoodenDoorBlock(static_cast<BlockID>(i))) {
                blocks[i].useWithoutItem = &DoorUse;
            }
        }

        // ── Signs ─────────────────────────────────────────────────────────
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (IsSignBlock(static_cast<BlockID>(i))) {
                blocks[i].useWithoutItem = &SignUse;
                blocks[i].useItemOn      = &SignUseItemOn;
            }
        }

        // ── Trapdoors ─────────────────────────────────────────────────────
        // The swing, by hand, for every trapdoor but iron.
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (IsHandOpenableTrapdoor(static_cast<BlockID>(i))) {
                blocks[i].useWithoutItem = &TrapDoorUse;
            }
        }

        // ── Beds (MC AbstractBedBlock.useWithoutItem) ─────────────────────
        // Sleep / set spawn in the Overworld, explode elsewhere. The block
        // half is in BedBlock.cpp; the sleeping is the server's.
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (IsBedBlock(static_cast<BlockID>(i))) {
                blocks[i].useWithoutItem = &BedUse;
            }
        }

        // ── Falling blocks (MC FallingBlock family) ───────────────────────
        //
        // Three hooks per block and nothing per-frame: onPlace and
        // neighborChanged book a scheduled tick, and the tick decides whether
        // to fall. See FallingBlock.hpp for why that indirection is the whole
        // mechanism rather than an optimisation.
        //
        // Wired by BlockID rather than by slug because IsFallingBlock already
        // owns the membership question (and concrete powder's sixteen colours
        // would otherwise be sixteen more string literals to keep in step).
        {
            size_t fallingWired = 0;
            for (size_t i = 0; i < blocks.size(); ++i) {
                const BlockID id = static_cast<BlockID>(i);
                if (!IsFallingBlock(id)) continue;

                switch (id) {
                    case BlockID::Scaffolding:
                        // Its own rule: falls on the `distance` state machine,
                        // not on whether the cell below is free.
                        blocks[i].onPlace         = &ScaffoldingOnPlace;
                        blocks[i].updateShape = &ScaffoldingUpdateShape;
                        blocks[i].tick            = &ScaffoldingTick;
                        break;
                    case BlockID::PointedDripstone:
                        // Also its own rule: supported from BEHIND the tip, and
                        // a stalactite collapses as a whole column.
                        //
                        // NO onPlace. PointedDripstoneBlock extends Block, not
                        // FallingBlock, and overrides neither onPlace nor
                        // getDelayAfterPlace — its only scheduling comes from
                        // updateShape, and only when the support side actually
                        // broke. Borrowing FallingBlockOnPlace here booked a
                        // tick on EVERY placement (World::ProcessBlockUpdates
                        // fires onPlace on any block-id change), and
                        // PointedDripstoneTick then fell straight through to
                        // the collapse path, so placed dripstone dropped on the
                        // spot.
                        blocks[i].updateShape = &PointedDripstoneUpdateShape;
                        blocks[i].tick            = &PointedDripstoneTick;
                        break;
                    default:
                        blocks[i].onPlace = &FallingBlockOnPlace;
                        // Concrete powder solidifies on water contact BEFORE it
                        // would schedule a fall, so it gets the combined hook.
                        blocks[i].updateShape = IsConcretePowder(id)
                            ? &ConcretePowderUpdateShape
                            : &FallingBlockUpdateShape;
                        blocks[i].tick = &FallingBlockTick;
                        break;
                }
                // Falling dust comes from FallingBlock.animateTick, so it
                // belongs to the blocks that actually extend FallingBlock:
                // sand, gravel, the suspicious pair (BrushableBlock copies it
                // verbatim), concrete powder, the anvils and the dragon egg.
                //
                // Scaffolding and pointed dripstone are NOT FallingBlocks in
                // MC — both extend Block directly and implement Fallable — so
                // neither has any dust. Dripstone has an animateTick of its
                // own, but it is the water/lava DRIP particle, which needs the
                // getFluidAboveStalactite column walk and the cauldron rules;
                // that is a separate mechanism from falling and is not wired
                // yet. Giving them falling dust in the meantime was not a
                // stand-in for it, just a wrong particle.
                if (id != BlockID::Scaffolding && id != BlockID::PointedDripstone) {
                    blocks[i].animateTick = &FallingBlockAnimateTick;
                }
                ++fallingWired;
            }
            // 3 sand/gravel + 2 suspicious + 16 concrete powder + 3 anvils +
            // dragon egg + scaffolding + dripstone = 27. A different number
            // means BlockDefs.inc lost a row or IsFallingBlock drifted.
            Log::Info("[BlockBehaviors] falling blocks wired: %zu", fallingWired);
        }

        // ── TNT ───────────────────────────────────────────────────────────
        //
        // useItemOn beats the ITEM's own useOn, which is the whole reason
        // flint & steel lights TNT instead of putting a fire block on top of
        // it — see the contract note on BlockUseItemOnFn.
        if (Block* bell = forSlug("bell")) {
            bell->useItemOn      = &BellUseItemOn;
            bell->useWithoutItem = &BellUse;
        }

        if (Block* tnt = forSlug("tnt")) {
            tnt->useItemOn       = &TntUseItemOn;
            tnt->onPlace         = &TntOnPlace;
            // neighborChanged (a lever thrown next to placed TNT) is wired
            // with the rest of redstone in RegisterRedstoneBehaviors.
        } else {
            Log::Warning("[BlockBehaviors] no block 'tnt' — it can never be lit");
        }

        // ── Buttons and levers ────────────────────────────────────────────
        // Break when the surface they are stuck to is mined. Matched by model
        // name rather than a slug list because there is one button per wood
        // type plus stone/polished blackstone — the same test CanSurviveAt and
        // the placement rule use.
        for (auto& b : blocks) {
            const std::string& n = b.modelName;
            if (n.find("_button") != std::string::npos || n == "lever") {
                b.updateShape = &FaceAttachedUpdateShape;
            }
        }

        // ── Storage ───────────────────────────────────────────────────────
        // Chest, trapped chest, barrel and every shulker box are all 3-row
        // (MenuType.GENERIC_9x3 in MC). Ender chest shares the screen but in
        // vanilla is backed by the PLAYER's own ender inventory rather than the
        // block — until that exists it opens its own block container, which is
        // the same UI with per-block storage.
        attachContainers({"chest", "trapped_chest", "ender_chest", "barrel",
                          "shulker_box",
                          "white_shulker_box",      "orange_shulker_box",
                          "magenta_shulker_box",    "light_blue_shulker_box",
                          "yellow_shulker_box",     "lime_shulker_box",
                          "pink_shulker_box",       "gray_shulker_box",
                          "light_gray_shulker_box", "cyan_shulker_box",
                          "purple_shulker_box",     "blue_shulker_box",
                          "brown_shulker_box",      "green_shulker_box",
                          "red_shulker_box",        "black_shulker_box"},
                         &OpenContainerUse<MenuType::Generic9x3>);

        // The chest lid: its recheck tick and the block event that carries
        // the opener count (ChestBlockEntity.cpp). A trapped chest is also a
        // redstone source, powered by that count.
        for (BlockID id : {BlockID::Chest, BlockID::TrappedChest, BlockID::EnderChest}) {
            Block& chest = blocks[static_cast<size_t>(id)];
            chest.tick         = &ChestTick;
            chest.triggerEvent = &BlockEntityTriggerEvent;
        }
        // The monster spawner's block events (SpawnerBlockEntity).
        blocks[static_cast<size_t>(BlockID::Spawner)].triggerEvent = &SpawnerTriggerEvent;
        {
            Block& trapped = blocks[static_cast<size_t>(BlockID::TrappedChest)];
            trapped.isSignalSource  = true;
            trapped.getSignal       = &TrappedChestGetSignal;
            trapped.getDirectSignal = &TrappedChestGetDirectSignal;
        }

        // Dispenser / dropper — 3x3 (MenuType.GENERIC_3x3).
        attachContainers({"dispenser", "dropper"}, &OpenContainerUse<MenuType::Generic3x3>);

        // Hopper — 5 slots in a row (MenuType.HOPPER).
        attachContainers({"hopper"}, &OpenContainerUse<MenuType::Hopper>);

        // Furnace family. Each gets its own menu type so the screen can pick
        // the right panel texture; the recipe kind comes off the block entity.
        attachContainers({"furnace"},       &OpenContainerUse<MenuType::Furnace>);
        attachContainers({"blast_furnace"}, &OpenContainerUse<MenuType::BlastFurnace>);
        attachContainers({"smoker"},        &OpenContainerUse<MenuType::Smoker>);

        // ── Utility blocks ────────────────────────────────────────────────
        // No block entity behind these: the menu owns its inputs and hands
        // them back when the screen closes (ItemCombinerMenu::Removed).
        attachContainers({"stonecutter"},       &OpenContainerUse<MenuType::Stonecutter>);
        attachContainers({"grindstone"},        &OpenContainerUse<MenuType::Grindstone>);
        attachContainers({"cartography_table"}, &OpenContainerUse<MenuType::CartographyTable>);
        attachContainers({"loom"},              &OpenContainerUse<MenuType::Loom>);
        attachContainers({"smithing_table"},    &OpenContainerUse<MenuType::Smithing>);
        // Every damage stage of an anvil opens the same menu (MC AnvilBlock
        // covers anvil / chipped_anvil / damaged_anvil).
        attachContainers({"anvil", "chipped_anvil", "damaged_anvil"},
                         &OpenContainerUse<MenuType::Anvil>);

        // ── Blocks with a gameplay system behind them ─────────────────────
        attachContainers({"enchanting_table"}, &OpenContainerUse<MenuType::Enchantment>);
        attachContainers({"brewing_stand"},    &OpenContainerUse<MenuType::BrewingStand>);
        attachContainers({"beacon"},           &OpenContainerUse<MenuType::Beacon>);
        attachContainers({"crafter"},          &OpenContainerUse<MenuType::Crafter3x3>);

        Log::Info("[BlockBehaviors] %d container blocks wired (%d slugs unmatched)",
                  attached, missing);

        // ── Redstone ──────────────────────────────────────────────────────
        // Last, so a family loop above (buttons' FaceAttachedUpdateShape, the
        // fence-gate swing) never overwrites a redstone hook.
        RegisterRedstoneBehaviors(blocks);
        // The two mod portals (aether_portal, twilight_portal): after the
        // generic portal wiring above, so their own hooks win.
        RegisterModPortalBehaviors(blocks);
        RegisterRailBehaviors(blocks);
        RegisterPistonBehaviors(blocks);
        RegisterContainerBehaviors(blocks);
        // The lectern: book placing, its reading menu, the page-turn pulse
        // and the comparator reading (LecternBlock.cpp).
        RegisterLecternBehaviors(blocks);
        // Potent sulfur: the geyser's state, eruption start, bubbles and
        // hiss (PotentSulfurBlock.cpp; the tickers are its block entity's).
        RegisterPotentSulfurBehaviors(blocks);
        // The ambient block sounds (fire, furnaces, candles, portals…):
        // truly last, because they CHAIN onto whatever animateTick the
        // registrations above installed.
        RegisterAmbientBlockSounds(blocks);
    }

} // namespace Game
