// File: src/common/world/portal/ModPortalBehaviors.cpp
//
// Java references (mods_reference/, git-ignored):
//   aether/src/main/java/com/aetherteam/aether/block/portal/AetherPortalBlock.java
//   aether/src/main/java/com/aetherteam/aether/event/hooks/DimensionHooks.java
//   twilightforest/src/main/java/twilightforest/block/TFPortalBlock.java
//   twilightforest/src/main/java/twilightforest/world/TFTeleporter.java
//   twilightforest/src/generated/resources/data/twilightforest/tags/block/portal/*.json

#include "common/world/level/ModDimensions.hpp"
#include "ModPortalBehaviors.hpp"

#include "PortalFamily.hpp"
#include "PortalShape.hpp"
#include "PortalState.hpp"
#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS
#include "ImmersiveFrame.hpp"
#endif

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/sound/SoundSource.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Game {

    namespace {

        // ── Tag sets ────────────────────────────────────────────────────────
        //
        // The engine has no block-tag registry, so the three TF portal tags
        // are resolved once into per-BlockID flags: explicit slugs from the
        // tag files (null-tolerant — a slug this engine does not have is
        // skipped), plus every block the data pack's vanilla tags put in the
        // referenced #minecraft tags (DataTags, which follows nested tags).
        // A small built-in list backs the vanilla part so a missing data/
        // directory degrades to "most pools still work", not "none do".

        // #twilightforest:portal/edge, minus the #minecraft:substrate_overworld
        // reference (resolved through the tags below).
        constexpr std::string_view kEdgeSlugs[] = {
            "farmland", "dirt_path",
            // #minecraft:dirt, the built-in backstop for the tag lookup.
            "dirt", "grass_block", "podzol", "coarse_dirt", "mycelium", "rooted_dirt",
            "moss_block", "pale_moss_block", "mud", "muddy_mangrove_roots",
        };
        constexpr std::string_view kEdgeTags[] = {
            "#minecraft:substrate_overworld", "#minecraft:dirt",
        };

        // #twilightforest:portal/decoration, verbatim, minus its four tag
        // references (kDecorationTags).
        constexpr std::string_view kDecorationSlugs[] = {
            "bamboo", "short_grass", "tall_grass", "fern", "large_fern", "dead_bush",
            "sugar_cane", "chorus_plant", "chorus_flower", "sweet_berry_bush",
            "nether_wart", "cocoa", "vine", "glow_lichen", "red_mushroom",
            "brown_mushroom", "warped_fungus", "crimson_fungus", "attached_melon_stem",
            "attached_pumpkin_stem", "moss_carpet", "pink_petals", "big_dripleaf",
            "big_dripleaf_stem", "small_dripleaf",
            // twilightforest: entries.
            "fiddlehead", "moss_patch", "mayapple", "clover_patch", "mushgloom",
            "fallen_leaves", "giant_leaves", "steeleaf_block", "hardened_dark_leaves",
            // Built-in backstop for #minecraft:flowers / crops (leaves and
            // saplings are caught by suffix below).
            "dandelion", "poppy", "blue_orchid", "allium", "azure_bluet", "red_tulip",
            "orange_tulip", "white_tulip", "pink_tulip", "oxeye_daisy", "cornflower",
            "lily_of_the_valley", "torchflower", "wither_rose", "sunflower", "lilac",
            "rose_bush", "peony", "pitcher_plant", "spore_blossom", "mangrove_propagule",
            "cherry_leaves", "flowering_azalea_leaves", "flowering_azalea", "azalea",
            "wildflowers", "open_eyeblossom", "closed_eyeblossom", "cactus_flower",
            "wheat", "carrots", "potatoes", "beetroots", "melon_stem", "pumpkin_stem",
            "torchflower_crop", "pitcher_crop",
        };
        constexpr std::string_view kDecorationTags[] = {
            "#minecraft:flowers", "#minecraft:leaves", "#minecraft:saplings", "#minecraft:crops",
        };

        // #twilightforest:portal/generated_decoration, verbatim.
        constexpr std::string_view kGeneratedDecorationSlugs[] = {
            "brown_mushroom", "red_mushroom", "short_grass", "fern", "blue_orchid",
            "azure_bluet", "lily_of_the_valley", "oxeye_daisy", "allium", "cornflower",
            "white_tulip", "pink_tulip", "orange_tulip", "red_tulip",
            "mushgloom", "mayapple", "fiddlehead",
        };

        struct PortalTagSets {
            std::array<uint8_t, BlockRegistry::Size> edge{};
            std::array<uint8_t, BlockRegistry::Size> decoration{};
            std::vector<BlockID> generated;
        };

        // A slug to its block, or Air when the engine has no such block.
        BlockID BlockOfSlug(std::string_view slug) {
            return BlockStates::FromSlug(slug).Block();
        }

        bool EndsWith(std::string_view s, std::string_view suffix) {
            return s.size() >= suffix.size() &&
                   s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        PortalTagSets BuildTagSets() {
            PortalTagSets sets;
            auto mark = [](std::array<uint8_t, BlockRegistry::Size>& set, BlockID id) {
                if (id == BlockID::Air) return;
                const size_t i = static_cast<size_t>(id);
                if (i < set.size()) set[i] = 1;
            };

            for (std::string_view slug : kEdgeSlugs)       mark(sets.edge, BlockOfSlug(slug));
            for (std::string_view slug : kDecorationSlugs) mark(sets.decoration, BlockOfSlug(slug));
            for (std::string_view slug : kGeneratedDecorationSlugs) {
                const BlockID id = BlockOfSlug(slug);
                if (id != BlockID::Air) sets.generated.push_back(id);
            }

            // The vanilla tags, block by block.
            for (size_t i = 1; i < BlockRegistry::Size; ++i) {
                const BlockID id = static_cast<BlockID>(i);
                const std::string& slug = BlockRegistry::Get(id).registrySlug;
                if (slug.empty()) continue;
                if (EndsWith(slug, "_leaves") || EndsWith(slug, "_sapling")) {
                    mark(sets.decoration, id);
                }
                const std::vector<std::string>& tags =
                    DataTags::TagsFor(DataTags::Registry::Block, slug);
                for (const std::string& tag : tags) {
                    for (std::string_view want : kEdgeTags) {
                        if (tag == want) mark(sets.edge, id);
                    }
                    for (std::string_view want : kDecorationTags) {
                        if (tag == want) mark(sets.decoration, id);
                    }
                }
            }
            return sets;
        }

        // Built on first use (thread-safe static init): DataTags reads the
        // data pack from disk, which is not something block registration
        // should do.
        const PortalTagSets& TagSets() {
            static const PortalTagSets sets = BuildTagSets();
            return sets;
        }

        struct IVec3Hash {
            size_t operator()(const glm::ivec3& v) const noexcept {
                uint64_t h = static_cast<uint64_t>(static_cast<uint32_t>(v.x));
                h = h * 0x9E3779B97F4A7C15ull + static_cast<uint64_t>(static_cast<uint32_t>(v.y));
                h = h * 0x9E3779B97F4A7C15ull + static_cast<uint64_t>(static_cast<uint32_t>(v.z));
                h ^= h >> 29;
                return static_cast<size_t>(h);
            }
        };
        using CheckedMap = std::unordered_map<glm::ivec3, bool, IVec3Hash>;

        // MC Direction.from2DDataValue(0..3): SOUTH, WEST, NORTH, EAST.
        constexpr glm::ivec3 kHorizontal2D[4] = {
            { 0, 0,  1}, {-1, 0,  0}, { 0, 0, -1}, { 1, 0,  0},
        };

        // TFPortalBlock.recursivelyValidatePortal. `checked` maps each cell
        // looked at to true (pool) or false (ring); the answer is whether
        // the pool is enclosed. The size counter is TF's MutableInt: bumped
        // on entry, so the walk stops the moment the pool exceeds the cap.
        bool RecursivelyValidatePortal(const IBlockAccess& level, const glm::ivec3& pos,
                                       CheckedMap& checked, int& portalSize,
                                       BlockState poolBlock) {
            if (++portalSize > TwilightPortalShape::kMaxCells) return false;

            bool isPoolProbablyEnclosed = true;
            for (int i = 0; i < 4 && portalSize <= TwilightPortalShape::kMaxCells; ++i) {
                const glm::ivec3 positionCheck = pos + kHorizontal2D[i];
                if (checked.count(positionCheck)) continue;

                const BlockState state = level.GetBlockState(positionCheck.x, positionCheck.y,
                                                             positionCheck.z);
                const BlockState below = level.GetBlockState(positionCheck.x, positionCheck.y - 1,
                                                             positionCheck.z);
                if (state == poolBlock && TwilightPortalBlocks::IsSturdyTop(below)) {
                    checked[positionCheck] = true;
                    if (isPoolProbablyEnclosed) {
                        isPoolProbablyEnclosed = RecursivelyValidatePortal(
                            level, positionCheck, checked, portalSize, poolBlock);
                    }
                } else if (TwilightPortalBlocks::IsEdge(state.Block()) &&
                           TwilightPortalBlocks::IsDecoration(level.GetBlock(
                               positionCheck.x, positionCheck.y + 1, positionCheck.z))) {
                    checked[positionCheck] = false;
                } else {
                    return false;
                }
            }
            return isPoolProbablyEnclosed;
        }

        // ── aether_portal ───────────────────────────────────────────────────

        // AetherPortalBlock.updateShape — NetherPortalBlock's rule against
        // the glowstone frame: a change along the portal's own plane (or
        // vertically) that is not another portal block re-walks the frame
        // (AetherPortalShape.isComplete), and an incomplete one turns the
        // cell to air, which cascades through the rest of the portal.
        bool AetherPortalUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                     BlockState state, Direction toNeighbour, BlockID neighbourId,
                                     BlockState& outState, ScheduledTickAccess* /*ticks*/) {
            const PortalFamily& family = AetherFamily();
            const Axis updateAxis = AxisOf(toNeighbour);
            const Axis axis = (state.GetName(PropertyId::HORIZONTAL_AXIS) == "z") ? Axis::Z : Axis::X;

            // MC: `blockAxis != directionAxis && directionAxis.isHorizontal()`.
            const bool wrongAxis = (axis != updateAxis) && IsHorizontal(toNeighbour);
            if (wrongAxis) return false;
            if (neighbourId == family.portalBlock) return false;

            if (PortalShape::FindAnyShape(level, pos, axis, family).IsComplete()) return false;

            outState = BlockState{};   // air
            return true;
        }

        // AetherPortalBlock.entityInside: `if (entity.canUsePortal(false))
        // entity.setAsInsidePortal(this, pos)`. The Aether family is always
        // a vanilla block portal (Portals::FamilyIsImmersive is false for it
        // whatever /gamerule immersive_portals says), so the inert-block
        // check below never trips for it; it stays for symmetry with the
        // nether portal's hook.
        void AetherPortalEntityInside(ILevelWrite& /*level*/, const glm::ivec3& pos,
                                      BlockState state, Entity& entity) {
            if (!entity.CanUsePortal(false)) return;
            if (Portals::IsInertPortalBlock(state.Block())) return;
            entity.portal.SetAsInsidePortal(state.Block(), pos, entity.GetDimensionChangingDelay());
        }

        // AetherPortalBlock.animateTick — a [CODE COPY] of NetherPortalBlock
        // .animateTick: the 1-in-100 ambient sound, then four motes thrown
        // out of the portal's face along the axis the portal is not
        // continuous on, as AETHER_PORTAL particles.
        void AetherPortalAnimateTick(EntityLevel& level, const glm::ivec3& pos,
                                     BlockState state, JavaRandom& random) {
            if (random.NextInt(100) == 0) {
                // level.playLocalSound(centre, AetherSoundEvents
                // .BLOCK_AETHER_PORTAL_AMBIENT, BLOCKS, 0.5, 0.8..1.2, false).
                // The mod's own sound is not shipped; the event resolves
                // through assets/sound_overlays/aether/blocks.json.
                level.PlayLocalSound(glm::dvec3(pos.x + 0.5, pos.y + 0.5, pos.z + 0.5),
                                     "aether:block.aether_portal.ambient", SoundSource::Blocks,
                                     0.5f, random.NextFloat() * 0.4f + 0.8f, false);
            }

            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            const BlockID self = state.Block();
            const bool noPortalAlongX =
                blocks->GetBlock(pos.x - 1, pos.y, pos.z) != self &&
                blocks->GetBlock(pos.x + 1, pos.y, pos.z) != self;

            for (int i = 0; i < 4; ++i) {
                // The mod's draw order, kept so a seed gives the same shimmer.
                double x  = static_cast<double>(pos.x) + random.NextDouble();
                double y  = static_cast<double>(pos.y) + random.NextDouble();
                double z  = static_cast<double>(pos.z) + random.NextDouble();
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
                level.AddParticle(ParticleKind::AetherPortal, x, y, z, xa, ya, za);
            }
        }

        // ── twilight_portal ─────────────────────────────────────────────────

        // TFPortalBlock.neighborChanged: the pool stays a portal while the
        // block under it is sturdy and each horizontal neighbour is either
        // an edge block or the same portal. Anything else reverts this cell
        // to water with UPDATE_ALL, which notifies the neighbouring portal
        // cells in turn — one broken ring block drains the whole pool.
        //
        // The decoration plants are NOT re-checked, exactly as in TF: a
        // plant sits diagonally above the pool, so its removal never
        // notifies a portal cell.
        void TwilightPortalNeighborChanged(ILevelWrite& level, const glm::ivec3& pos,
                                           BlockState state, BlockID /*sourceBlock*/,
                                           bool /*movedByPiston*/) {
            if (level.IsClientSide()) return;

            bool good = TwilightPortalBlocks::IsSturdyTop(
                level.GetBlockState(pos.x, pos.y - 1, pos.z));
            // Direction.Plane.HORIZONTAL: NORTH, EAST, SOUTH, WEST.
            constexpr glm::ivec3 kPlane[4] = {
                { 0, 0, -1}, { 1, 0,  0}, { 0, 0,  1}, {-1, 0,  0},
            };
            for (const glm::ivec3& d : kPlane) {
                if (!good) break;
                const glm::ivec3 n = pos + d;
                const BlockState neighbour = level.GetBlockState(n.x, n.y, n.z);
                good = TwilightPortalBlocks::IsEdge(neighbour.Block()) || neighbour == state;
            }

            if (!good) {
                // levelEvent(PARTICLES_DESTROY_BLOCK) has no server→client
                // channel here; the block change is what the client sees.
                level.SetBlock(pos.x, pos.y, pos.z, BlockID::Water, World::UpdateFlags::All);
            }
        }

        // TFPortalBlock.entityInside: `if (entity.canUsePortal(false))
        // entity.setAsInsidePortal(this, entity.blockPosition())` — the
        // ENTITY's block, not the portal cell (the advancement lock and the
        // one-way DISALLOW_RETURN variant are not ported).
        void TwilightPortalEntityInside(ILevelWrite& /*level*/, const glm::ivec3& /*pos*/,
                                        BlockState /*state*/, Entity& entity) {
            if (!entity.CanUsePortal(false)) return;
            entity.portal.SetAsInsidePortal(BlockID::TwilightPortal, entity.BlockPosition(),
                                            entity.GetDimensionChangingDelay());
        }

        // TFPortalBlock.animateTick ("Full [VanillaCopy] of NetherPortalBlock
        // .animateTick", reshaped for a pool): the 1-in-100 whoosh, then four
        // purple PORTAL motes a tick rising off the pool's surface.
        void TwilightPortalAnimateTick(EntityLevel& level, const glm::ivec3& pos,
                                       BlockState /*state*/, JavaRandom& random) {
            if (random.NextInt(100) == 0) {
                // level.playLocalSound(centre, TFSounds.PORTAL_WHOOSH,
                // BLOCKS, 0.5, 0.8..1.2, false) — resolved through
                // assets/sound_overlays/twilightforest/blocks.json.
                level.PlayLocalSound(glm::dvec3(pos.x + 0.5, pos.y + 0.5, pos.z + 0.5),
                                     "twilightforest:block.twilightforest.portal.whoosh", SoundSource::Blocks,
                                     0.5f, random.NextFloat() * 0.4f + 0.8f, false);
            }

            for (int i = 0; i < 4; ++i) {
                const double x  = static_cast<double>(pos.x) + static_cast<double>(random.NextFloat());
                const double y  = static_cast<double>(pos.y) + 1.0;
                const double z  = static_cast<double>(pos.z) + static_cast<double>(random.NextFloat());
                const double xa = (static_cast<double>(random.NextFloat()) - 0.5) * 0.5;
                const double ya = static_cast<double>(random.NextFloat());
                const double za = (static_cast<double>(random.NextFloat()) - 0.5) * 0.5;
                level.AddParticle(ParticleKind::Portal, x, y, z, xa, ya, za);
            }
        }

    } // namespace

    // ── TwilightPortalBlocks ────────────────────────────────────────────────

    // TFPortalBlock.isGrassOrDirt — state.is(TFBlockTags.PORTAL_EDGE).
    bool TwilightPortalBlocks::IsEdge(BlockID id) {
        const size_t i = static_cast<size_t>(id);
        return i < BlockRegistry::Size && TagSets().edge[i] != 0;
    }

    // TFPortalBlock.isNatureBlock — state.is(TFBlockTags.PORTAL_DECO).
    bool TwilightPortalBlocks::IsDecoration(BlockID id) {
        const size_t i = static_cast<size_t>(id);
        return i < BlockRegistry::Size && TagSets().decoration[i] != 0;
    }

    // TFTeleporter.randNatureBlock.
    BlockID TwilightPortalBlocks::RandomGeneratedDecoration(JavaRandom& random) {
        const std::vector<BlockID>& generated = TagSets().generated;
        if (generated.empty()) {
            const BlockID grass = BlockOfSlug("short_grass");
            return grass;   // Air when even short grass is missing: nothing is placed
        }
        return generated[static_cast<size_t>(random.NextInt(static_cast<int>(generated.size())))];
    }

    // MC BlockBehaviour.isFaceSturdy(level, pos, UP) — as BlockPlacement's
    // IsFaceSturdyUp: a colliding block whose shape union has one box
    // covering the whole top face.
    bool TwilightPortalBlocks::IsSturdyTop(BlockState state) {
        if (!BlockRegistry::HasCollision(state.Block())) return false;
        return BlockRegistry::GetBlockShapeSet(state).IsFaceSturdyUp();
    }

    // TFPortalBlock.canFormPortal — #twilightforest:portal/fluid (water),
    // narrowed to a source.
    bool TwilightPortalBlocks::IsPoolBlock(BlockState state) {
        return state.Block() == BlockID::Water && FluidStateOf(state).IsSource();
    }

    // ── TwilightPortalShape ─────────────────────────────────────────────────

    // TFPortalBlock.tryToCreatePortal's checks: a pool block with a sturdy
    // floor, a recursive validation that closes, and at least
    // MIN_PORTAL_SIZE cells.
    std::optional<TwilightPortalShape> TwilightPortalShape::Find(const IBlockAccess& level,
                                                                 const glm::ivec3& start) {
        const BlockState state = level.GetBlockState(start.x, start.y, start.z);
        if (!TwilightPortalBlocks::IsPoolBlock(state)) return std::nullopt;
        if (!TwilightPortalBlocks::IsSturdyTop(level.GetBlockState(start.x, start.y - 1, start.z))) {
            return std::nullopt;
        }

        CheckedMap checked;
        checked[start] = true;
        int size = 0;
        if (!RecursivelyValidatePortal(level, start, checked, size, state)) return std::nullopt;
        if (size < kMinCells) return std::nullopt;

        TwilightPortalShape shape;
        for (const auto& [pos, isPool] : checked) {
            if (isPool) shape.m_cells.push_back(pos);
        }
        // Deterministic order (the map's is not): bottom-to-top, then z, x.
        std::sort(shape.m_cells.begin(), shape.m_cells.end(),
                  [](const glm::ivec3& a, const glm::ivec3& b) {
                      if (a.y != b.y) return a.y < b.y;
                      if (a.z != b.z) return a.z < b.z;
                      return a.x < b.x;
                  });
        return shape;
    }

    // TFPortalBlock.tryToCreatePortal: setBlock(pos, TWILIGHT_PORTAL,
    // Block.UPDATE_CLIENTS) for every pool cell.
    void TwilightPortalShape::CreatePortalBlocks(ILevelWrite& level) const {
        const BlockState portal = BlockStates::Default(BlockID::TwilightPortal);
        for (const glm::ivec3& c : m_cells) {
            level.SetBlock(c.x, c.y, c.z, portal, World::UpdateFlags::UpdateClients);
        }
    }

    // ── AetherPortalIgnition ────────────────────────────────────────────────

    // DimensionHooks.createPortal (and, with the immersive feature compiled
    // in — always, not per the rule — the handler the fire and the echo
    // shard use).
    bool AetherPortalIgnition::TryLight(ILevelWrite& level, const glm::ivec3& seedPos) {
        // `level.dimension() == returnDimension() || == destinationDimension()`.
        if (!DimensionAllowsAetherPortal(level.GetDimension())) return false;
        // /gamerule aether off: the frame will not light.
        if (!ModDimensions::Enabled(DimensionId::Aether)) return false;
        const PortalFamily& family = AetherFamily();

        // Never immersive today — the Aether does not follow /gamerule
        // immersive_portals and is always lit with aether_portal blocks
        // (Portals::FamilyIsImmersive), so a glowstone frame lights the same
        // way whichever way the rule is set. The branch stays so the family
        // can be switched in one place.
        if (Portals::FamilyIsImmersive(family.id)) {
#if ENABLE_IMMERSIVE_PORTALS
            if (level.IsClientSide()) {
                // Prediction only: the server's handler decides.
                return Immersive::FrameShape::Find(level, seedPos, family.id).has_value();
            }
            if (auto handler = Portals::GetImmersiveFrameLitHandler()) {
                return handler(level, seedPos, family.id);
            }
#endif
            return false;
        }

        // AetherPortalShape.findEmptyAetherPortalShape(level, relativePos,
        // Direction.Axis.X) — X first, then Z.
        auto shape = PortalShape::FindEmptyPortalShape(level, seedPos, Axis::X, family);
        if (!shape) return false;
        if (!level.IsClientSide()) shape->CreatePortalBlocks(level);
        return true;
    }

    // ── Registration ────────────────────────────────────────────────────────

    void RegisterModPortalBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        // aether_portal — AetherPortalBlock. Lit by a water bucket
        // (ItemBehaviors' filled bucket → AetherPortalIgnition::TryLight),
        // never by fire. No randomTick: the mod's portal spawns nothing.
        {
            Block& portal = blocks[static_cast<size_t>(BlockID::AetherPortal)];
            portal.updateShape  = &AetherPortalUpdateShape;
            portal.entityInside = &AetherPortalEntityInside;
            portal.animateTick  = &AetherPortalAnimateTick;
        }

        // twilight_portal — TFPortalBlock. Lit by a diamond thrown into the
        // pool (the server's item-entity tick → TwilightTeleporter).
        {
            Block& portal = blocks[static_cast<size_t>(BlockID::TwilightPortal)];
            portal.neighborChanged = &TwilightPortalNeighborChanged;
            portal.entityInside    = &TwilightPortalEntityInside;
            portal.animateTick     = &TwilightPortalAnimateTick;
        }

        Log::Info("[ModPortalBehaviors] aether_portal and twilight_portal wired");
    }

} // namespace Game
