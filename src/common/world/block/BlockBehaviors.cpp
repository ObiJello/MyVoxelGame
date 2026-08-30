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
#include "FallingBlock.hpp"
#include "TntBlock.hpp"
#include "Stairs.hpp"
#include "CrossCollision.hpp"
#include "Walls.hpp"
#include "Vine.hpp"
#include "MultifaceBlock.hpp"
#include "FenceGate.hpp"
#include "common/world/portal/PortalShape.hpp"
#include "common/world/level/World.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/inventory/MenuType.hpp"
#include "common/world/crafting/RecipeManager.hpp"
#include "common/core/Log.hpp"

#include <array>
#include <string>

namespace Game {

    namespace {

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

        // MC RedStoneWireBlock.useWithoutItem: right-clicking a wire that is a
        // full CROSS collapses it to a dot, and one that is a bare DOT expands
        // it back to a cross. A wire with real connections is left alone — MC
        // returns PASS, which matters because it lets the click fall through
        // to placing whatever is in hand.
        //
        // Runs on both sides: the client predicts the toggle so the shape
        // changes on the same frame, and the server's authoritative state
        // follows. Both call the same RedstoneToggleShape against their own
        // block access, so they agree.
        UseResult RedstoneWireUse(ILevelWrite* world, const glm::ivec3& pos,
                                  IUsePlayer* player, const BlockHitResult& /*hit*/) {
            if (!world || !player) return UseResult::Pass;
            const BlockState state = world->GetBlockState(pos.x, pos.y, pos.z);
            BlockState next;
            if (!RedstoneToggleShape(*world, pos, state, next)) return UseResult::Pass;
            // MC uses flag 3 (UPDATE_NEIGHBORS | UPDATE_CLIENTS) here, then
            // calls updatesOnShapeChange to poke the wires around it.
            world->SetBlock(pos.x, pos.y, pos.z, next, World::UpdateFlags::All);
            return UseResult::Success;
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
        bool FaceAttachedNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
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

        // MC RedStoneWireBlock.updateShape — a neighbour changed, so re-resolve
        // this wire's connections. Wired through the generic neighborChanged
        // hook, which is what makes two wires laid side by side join up.
        bool RedstoneWireNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
                                         BlockState state,
                                         Direction toNeighbour, BlockID /*neighbourId*/,
                                         BlockState& outState,
                                         ScheduledTickAccess* /*ticks*/) {
            // DOWN is the support case: MC returns AIR when the block below can
            // no longer hold dust. The engine's support-collapse rule already
            // handles that (redstone has a modelled canSurvive), so leave it.
            if (toNeighbour == Direction::Down) return false;

            const BlockState next = RedstoneUpdateShape(level, pos, state, toNeighbour);
            if (next == state) return false;
            outState = next;
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
                         BlockState /*newState*/, BlockState /*oldState*/) {
            // MC inPortalDimension: overworld or nether only. An obsidian
            // frame in the End just holds a fire.
            if (!DimensionAllowsNetherPortal(level.GetDimension())) return;

            // MC always probes X first and lets findPortalShape fall through
            // to Z. The clicked face never reaches here — onPlace has no idea
            // how the fire got placed — which is also why the axis preference
            // in CanFireBePlacedAt is only about whether the fire is ALLOWED,
            // not about which way the portal ends up facing.
            auto shape = PortalShape::FindEmptyPortalShape(level, pos, Axis::X);
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
            entity.portal.SetAsInsidePortal(state.Block(), pos,
                                            entity.GetDimensionChangingDelay());
        }

        // MC NetherPortalBlock.updateShape (NetherPortalBlock.java:85) — the
        // rule that makes a portal vanish when someone mines its frame.
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
        // portal down from one broken obsidian block, exactly as in vanilla.
        bool NetherPortalNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
                                         BlockState state,
                                         Direction toNeighbour, BlockID neighbourId,
                                         BlockState& outState,
                                         ScheduledTickAccess* /*ticks*/) {
            const Axis updateAxis = AxisOf(toNeighbour);
            const Axis axis = (state.GetName(PropertyId::HORIZONTAL_AXIS) == "z")
                                  ? Axis::Z : Axis::X;

            const bool wrongAxis = (axis != updateAxis) && IsHorizontal(toNeighbour);
            if (wrongAxis) return false;
            if (neighbourId == BlockID::NetherPortal) return false;

            if (PortalShape::FindAnyShape(level, pos, axis).IsComplete()) return false;

            outState = BlockState{};   // air — World destroys with drops (there are none)
            return true;
        }

        // MC VineBlock.updateShape — re-derive which faces still have support.
        // A vine that loses its last face returns AIR, and World turns that into
        // a destroy-with-drops, which is how a vine curtain falls when the wall
        // behind it is mined.
        bool VineNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
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
        bool MultifaceNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
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
        bool StairNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
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
        bool CrossCollisionNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
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
        bool WallNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
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
        bool FenceGateNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
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
        if (Block* wire = forSlug("redstone_wire")) {
            wire->useWithoutItem  = &RedstoneWireUse;
            wire->neighborChanged = &RedstoneWireNeighborChanged;
        }

        // ── Nether portal ─────────────────────────────────────────────────
        // The only behaviour the BLOCK itself owns. Lighting a portal lives in
        // the flint-and-steel item behaviour (MC puts it in BaseFireBlock
        // .onPlace, which this engine has no equivalent of), and walking
        // through one is an entity-side check, not a block callback.
        if (Block* portal = forSlug("nether_portal")) {
            portal->neighborChanged = &NetherPortalNeighborChanged;
            portal->entityInside    = &PortalEntityInside;
        } else {
            Log::Warning("[BlockBehaviors] no block with registrySlug "
                         "'nether_portal' — portals will not close when their "
                         "frame is broken");
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

        // ── Vines ─────────────────────────────────────────────────────────
        // Faces drop as their support goes, and the last one taking the vine
        // with it. Without this a vine hangs in mid-air forever once the wall
        // behind it is mined.
        if (Block* vine = forSlug("vine")) {
            vine->neighborChanged = &VineNeighborChanged;
        }
        for (const char* slug : { "glow_lichen", "sculk_vein", "resin_clump" }) {
            if (Block* b = forSlug(slug)) b->neighborChanged = &MultifaceNeighborChanged;
        }

        // ── Stairs ────────────────────────────────────────────────────────
        // Corner formation. Matched by IsStairs (the "_stairs" model-name
        // test the rest of the stair code shares) rather than a 58-entry slug
        // list, so a new stair from an MC version bump is picked up with the
        // block definition alone.
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (IsStairs(static_cast<BlockID>(i))) {
                blocks[i].neighborChanged = &StairNeighborChanged;
            }
        }

        // ── Fences, glass panes, iron bars ────────────────────────────────
        // Connection tracking. Same matching argument as the stairs above —
        // IsCrossCollisionBlock is the one definition of the family, shared
        // with the state declaration and the shape builder.
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (IsCrossCollisionBlock(static_cast<BlockID>(i))) {
                blocks[i].neighborChanged = &CrossCollisionNeighborChanged;
            }
        }

        // ── Walls ─────────────────────────────────────────────────────────
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (IsWallBlock(static_cast<BlockID>(i))) {
                blocks[i].neighborChanged = &WallNeighborChanged;
            }
        }

        // ── Fence gates ───────────────────────────────────────────────────
        // The swing, plus the IN_WALL tracking. `useWithoutItem` rather than
        // `useItemOn`, matching MC — which is what lets you open a gate while
        // holding a block instead of placing the block.
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (IsFenceGateBlock(static_cast<BlockID>(i))) {
                blocks[i].useWithoutItem  = &FenceGateUse;
                blocks[i].neighborChanged = &FenceGateNeighborChanged;
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
                        blocks[i].neighborChanged = &ScaffoldingNeighborChanged;
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
                        blocks[i].neighborChanged = &PointedDripstoneNeighborChanged;
                        blocks[i].tick            = &PointedDripstoneTick;
                        break;
                    default:
                        blocks[i].onPlace = &FallingBlockOnPlace;
                        // Concrete powder solidifies on water contact BEFORE it
                        // would schedule a fall, so it gets the combined hook.
                        blocks[i].neighborChanged = IsConcretePowder(id)
                            ? &ConcretePowderNeighborChanged
                            : &FallingBlockNeighborChanged;
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
        if (Block* tnt = forSlug("tnt")) {
            tnt->useItemOn       = &TntUseItemOn;
            tnt->onPlace         = &TntOnPlace;
            tnt->neighborChanged = &TntNeighborChanged;
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
                b.neighborChanged = &FaceAttachedNeighborChanged;
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
    }

} // namespace Game
