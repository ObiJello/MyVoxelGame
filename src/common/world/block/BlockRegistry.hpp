// File: src/common/world/block/BlockRegistry.hpp
#pragma once

#include "Blocks.hpp"
#include "BlockStateIds.hpp"
#include "BlockModel.hpp"
#include "Direction.hpp"
#include "../../entity/MiningTier.hpp"
#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace Server { class ServerPlayer; }

namespace Game {

    struct IBlockAccess;

    // Forward declarations only — including BlockInteraction.hpp here would
    // also expose its `class World;` forward decl (Game::World) to every TU
    // that pulls BlockRegistry.hpp transitively. That breaks files like
    // SectionDataUnpacker.cpp which write unqualified `World::NBTTagPtr`
    // expecting it to resolve to the global `::World` NBT namespace —
    // making Game::World suddenly visible shadows the lookup. Function
    // pointer typedefs only need declarations, so fwd-decl is enough.
    enum class UseResult : int;
    struct BlockHitResult;
    struct ItemStack;
    class World;
    class ILevelWrite;
    class IUsePlayer;
    class JavaRandom;

    // Render layer classification — determines which rendering pass a block uses
    enum class RenderLayer : uint8_t {
        Opaque = 0,      // Solid blocks (stone, dirt, wood)
        Cutout = 1,      // Alpha-test blocks (leaves, grass, flowers)
        Translucent = 2  // Blended blocks (glass, water, ice)
    };

    // Empty-hand right-click on this block. Mirrors MC's
    // `BlockBehaviour.useWithoutItem(level, player, hit)` (called from
    // `ServerPlayerGameMode.useItemOn` line 354 when the held item returned
    // TryEmptyHandInteraction). Examples: open door, flip lever, press button,
    // open chest, sit in boat. Default nullptr → Pass.
    using BlockUseWithoutItemFn = UseResult (*)(ILevelWrite* world, const glm::ivec3& pos,
                                                IUsePlayer* player,
                                                const BlockHitResult& hit);

    // Right-click WITH an item on this block. Mirrors MC's
    // `BlockBehaviour.useItemOn(stack, level, player, hand, hit)` — called
    // BEFORE Item.useOn (so blocks can claim certain item interactions, like
    // fitting a banner into a pot). Default nullptr → Pass. Returning
    // TryEmptyHandInteraction from here makes the dispatch fall through to
    // useWithoutItem (i.e., "I'd react to an empty-hand click here, ignore
    // the item").
    using BlockUseItemOnFn = UseResult (*)(struct ItemStack& stack,
                                           ILevelWrite* world, const glm::ivec3& pos,
                                           IUsePlayer* player, uint32_t hand,
                                           const BlockHitResult& hit);

    // ── Growth (MC BlockBehaviour random ticking + BonemealableBlock) ───────
    //
    // Whether a state wants random ticks at all. Mirrors
    // `BlockBehaviour.isRandomlyTicking(state)` — note MC asks the STATE, not
    // the block: a fully grown crop stops ticking (`!isMaxAge(state)`), which
    // is what keeps a harvested field from costing anything once it matures.
    // A block whose fn is null never random-ticks.
    using BlockIsRandomlyTickingFn = bool (*)(uint8_t stateIndex);

    // One random tick. Mirrors `BlockBehaviour.randomTick(state, serverLevel,
    // pos, random)`. `id` is passed explicitly because one function backs
    // several blocks (wheat/carrots/potatoes all share CropBlock's tick) and
    // the growth-speed rule needs to know which block it is comparing
    // neighbours against.
    //
    // Takes ILevelWrite rather than World even though random ticks are
    // server-only, so the growth helpers can be shared verbatim with the bone
    // meal path below — which DOES run on both sides.
    using BlockRandomTickFn = void (*)(ILevelWrite& level, const glm::ivec3& pos,
                                       BlockID id, uint8_t stateIndex,
                                       JavaRandom& random);

    // MC BonemealableBlock. `isValidBonemealTarget` is the "would bone meal do
    // anything here" test (crops: not yet max age); `performBonemeal` applies
    // the growth. MC's third method, `isBonemealSuccess`, returns true for
    // every block modelled here, so it is folded into the target test rather
    // than carried as a third pointer. A block with a null pair is not
    // bonemealable.
    using BlockIsValidBonemealTargetFn = bool (*)(const IBlockAccess& level,
                                                  const glm::ivec3& pos,
                                                  uint8_t stateIndex);
    using BlockPerformBonemealFn = void (*)(ILevelWrite& level, const glm::ivec3& pos,
                                            BlockID id, uint8_t stateIndex,
                                            JavaRandom& random);

    // A neighbour of this block changed. Narrowed port of MC's
    // `BlockBehaviour.updateShape(state, …, direction, neighbourPos,
    // neighbourState, …)`, which returns the state this block BECOMES —
    // possibly AIR, which is how vanilla destroys a block whose support went.
    //
    // `toNeighbour` is the direction from THIS block toward the one that
    // changed, and `neighbourId` is what now sits there. Return true and fill
    // outBlock/outState to transform; return false to leave the block alone.
    //
    // This is the hook the existing `needsSupportBelow` rule in
    // World::NotifyNeighborBlocks could not express: that rule can only
    // DESTROY, and attached stems need to turn back into a growable stem when
    // their fruit is picked.
    using BlockNeighborChangedFn = bool (*)(const IBlockAccess& level, const glm::ivec3& pos,
                                            BlockID id, uint8_t stateIndex,
                                            Direction toNeighbour, BlockID neighbourId,
                                            BlockID& outBlock, uint8_t& outState);

    struct Block {
        std::string name;
        bool opaque;
        std::string modelName;  // Reference to BlockModel instead of texture indices

        // The vanilla registry name ("stone", "oak_slab") WITHOUT the
        // "minecraft:" prefix — column 2 of BlockDefs.inc, and what MC keys
        // its data files on (loot tables are `blocks/<registrySlug>.json`).
        //
        // NOT the same as modelName: Init() deliberately re-registers some
        // blocks with a different model (Water→"water_still", every Infested*
        // →its host block's model, double-plants→"<name>_bottom"), so
        // modelName is a rendering detail that several blocks SHARE. Slugs are
        // unique per BlockDefs.inc row; promoted state variants (SnowGrass,
        // *SlabTop, *Top halves) inherit their base block's slug, exactly as
        // they share one vanilla block.
        std::string registrySlug;

        // Optional override for blocks that don't use standard models
        std::array<uint16_t, 6> legacyTexIdx{0, 0, 0, 0, 0, 0};
        bool useLegacyTextures = false;

        // Rendering hints
        bool enableBiomeTinting = false;  // Whether this block uses biome coloring
        bool isTransparent = false;       // Whether this block has transparent parts
        RenderLayer renderLayer = RenderLayer::Opaque;

        // Per-block interaction callbacks. Default nullptr → Pass.
        BlockUseWithoutItemFn useWithoutItem = nullptr;
        BlockUseItemOnFn      useItemOn      = nullptr;

        // Growth callbacks (MC randomTick / BonemealableBlock). All default to
        // nullptr, which means "does not grow" — the state every block but the
        // farming set is in. `randomTick` is only ever reached when
        // `isRandomlyTicking` says yes, so the two are wired as a pair.
        BlockIsRandomlyTickingFn     isRandomlyTicking     = nullptr;
        BlockRandomTickFn            randomTick            = nullptr;
        BlockIsValidBonemealTargetFn isValidBonemealTarget = nullptr;
        BlockPerformBonemealFn       performBonemeal       = nullptr;
        BlockNeighborChangedFn       neighborChanged       = nullptr;

        // ── Mining data (MC parity) ────────────────────────────────────────
        // destroyTime: MC's `strength(destroyTime, ...)` first arg from
        // Blocks.java. Units are MC seconds at the player's base mining speed
        // (×30 ticks for the correct tool). -1 = unbreakable (bedrock).
        // 0 = instant break (grass plant, flower, leaves).
        float       destroyTime         = 1.0f;
        bool        requiresCorrectTool = false;
        ToolType    preferredTool       = ToolType::None;
        MiningTier  minTier             = MiningTier::Wood;

        // MC's BlockBehaviour.Properties.noCollision(): when false, the block
        // is non-colliding (the player walks straight through it). Mirrors
        // Blocks.java's `.noCollision()` on flowers, grasses, leaf litter,
        // torches, redstone wire, vines, etc. Default true — every full-cube
        // block keeps its existing solid collision.
        bool        hasCollision        = true;

        // MC's canSurvive, narrowed to the one rule ground vegetation uses:
        // the block below must present a solid top face
        // (LeafLitterBlock.canSurvive → isFaceSturdy(below, UP);
        // VegetationBlock.canSurvive → mayPlaceOn(below)). When it stops being
        // true the block breaks, which is what stops a flower or a clump of
        // leaf litter floating after you mine out the dirt under it.
        //
        // Deliberately NOT set on side- or ceiling-attached blocks (vines,
        // glow lichen, hanging roots, wall torches): those survive on a
        // different face, so treating them as ground vegetation would delete
        // them the moment anything below changed.
        bool        needsSupportBelow   = false;

        // MC's `BlockBehaviour.Properties.replaceable()` — the flag behind
        // `BlockState.canBeReplaced()`. A replaceable block is overwritten by a
        // block you place into its cell rather than blocking the placement:
        // water, lava, tall grass, snow layers, fire, vines and friends.
        //
        // Read by CanBeReplacedByPlacement, which is the single gate both the
        // server's placement path and the client's prediction consult.
        bool        replaceable         = false;
    };

    class BlockRegistry {
    public:
        static constexpr size_t Size = static_cast<size_t>(BlockID::Count);

        // Initialize the block registry
        static void Init();

        // Get block definition by ID
        static const Block& Get(BlockID id);

        // Check if a block uses model-based rendering
        static bool UsesModelRendering(BlockID id);

        // Get model for a block (returns default if not found)
        static const BlockModel& GetBlockModel(BlockID id);
        // Model for a specific block STATE — the blockstate JSON may map each
        // state to a different (possibly rotated) model. MC's equivalent is
        // BlockModelShaper.getBlockModel(BlockState). Falls back to the plain
        // model when the block has no blockstate file or no states.
        static const BlockModel& GetBlockModel(BlockID id, uint8_t stateIndex);

        // Combined outline / selection shape for a block, in [0,1] block-local
        // coordinates. Computed once per BlockID by unioning the AABB of every
        // element in the block's model (mirrors MC's collapsing of multi-box
        // VoxelShapes into a single outline AABB for raycasting + highlight).
        // Degenerate axes (zero thickness, e.g. leaf litter's flat plane) are
        // expanded by a sub-pixel epsilon so the wireframe shader doesn't
        // produce NaN edges. Returns (0,0,0)-(1,1,1) for full cubes and for
        // blocks whose model couldn't be resolved.
        struct BlockShape { glm::vec3 min{0.0f}; glm::vec3 max{1.0f}; };

        // Shape for a specific block STATE. Rotation lives in the model, so a
        // block whose blockstate maps facing to a y-rotated model has a
        // DIFFERENT shape per state — a leaf litter clump occupies a different
        // quarter of its cell depending on which way it faces. Deriving the
        // shape from the state's own model is exactly MC's
        // LeafLitterBlock.getShape → getShapeForEachState(getShapeCalculator(
        // FACING, AMOUNT)), which builds one VoxelShape per state.
        //
        // Querying the state-0 overload for a rotated block gives the shape of
        // the block as authored (north-facing), which is why the outline and
        // the raycast used to sit in the wrong corner.
        static const BlockShape& GetBlockShape(BlockID id, uint8_t stateIndex);

        // World-aware shape. Identical to GetBlockShape except for blocks whose
        // real extent depends on a NEIGHBOUR — today just a paired chest, which
        // reaches the cell edge toward its partner so the two halves' hitboxes
        // meet (MC ChestBlock.HALF_SHAPES). MC can key that off a blockstate
        // property (ChestBlock.TYPE) and keep it in the static table; we derive
        // the pairing from the world, so this needs the world and returns by
        // value rather than caching.
        static BlockShape GetBlockShapeAt(const IBlockAccess& world, const glm::ivec3& pos,
                                          BlockID id, uint8_t stateIndex);

        // Default-state shape. Correct for the ~99% of blocks that have no
        // state properties at all; prefer the two-argument form anywhere a
        // state index is available.
        static const BlockShape& GetBlockShape(BlockID id);

        // ── MC BlockBehaviour.OffsetType ────────────────────────────────────
        //
        // Flowers, grasses and similar decoration are drawn nudged off the grid
        // by a hash of their position (BlockBehaviour.Properties.offsetType).
        // Without it every plant sits dead centre in its cell and a field of
        // grass reads as a visible lattice rather than scatter.
        //
        // NONE = no offset. XZ = horizontal jitter. XYZ = the same plus a
        // downward sink, which is what stops short grass and ferns from all
        // standing at exactly the same height.
        enum class OffsetType : uint8_t { None, XZ, XYZ };
        static OffsetType GetOffsetType(BlockID id);

        // The offset itself, in block units. Pure function of (x, z) — MC seeds
        // with y = 0 so a double plant's two halves shift together.
        static glm::vec3 GetBlockOffset(BlockID id, int worldX, int worldZ);

        // Convenience wrapper around Block::hasCollision so call sites in
        // physics / world helpers don't have to fetch the whole Block.
        static bool HasCollision(BlockID id);

        // Slab orientation helpers. MC stores slab `type` (BOTTOM/TOP/DOUBLE)
        // as a blockstate; we promote each top-half variant to its own
        // BlockID. SlabTopVariant(bottom) returns the matching `*SlabTop`
        // BlockID, SlabBottomVariant(top) returns the canonical bottom form
        // (the one items resolve to). Both return BlockID::Air for non-slabs.
        static BlockID SlabTopVariant(BlockID bottom);
        static BlockID SlabBottomVariant(BlockID top);
        static bool    IsSlabTop(BlockID id);

        // ── Block states ────────────────────────────────────────────────────
        // Port of MC's StateDefinition (createBlockStateDefinition). Each block
        // declares an ordered list of properties; the cartesian product of
        // their values is the block's state list, and a voxel stores an index
        // into it (MC's BlockState.getId()).
        //
        // INVARIANT: every property lists its DEFAULT value FIRST, so state
        // index 0 is always the block's default state (MC defaultBlockState()).
        // The whole storage layer leans on this — ChunkSection keeps no state
        // plane at all until something writes a non-zero index, which is only
        // sound because zero means "default" for every block.
        //
        // The index is mixed-radix with the FIRST property most significant.
        struct BlockStateDefinition {
            struct Property {
                std::string              name;    // e.g. "facing"
                std::vector<std::string> values;  // default first, e.g. north,east,south,west
            };
            std::vector<Property> properties;      // empty => the block has exactly one state

            using PropertyMap = std::unordered_map<std::string, std::string>;

            uint16_t StateCount() const {
                uint16_t n = 1;
                for (const auto& p : properties) n = static_cast<uint16_t>(n * p.values.size());
                return n;
            }

            // Properties not present in `props`, or carrying an unrecognised
            // value, fall back to that property's default (index 0) — which is
            // what a caller who doesn't model a property means, and what an
            // older save that predates it should decode as.
            uint8_t IndexOf(const PropertyMap& props) const;
            PropertyMap PropertiesOf(uint8_t stateIndex) const;

            // Value of one property in a given state, or empty if this block
            // doesn't declare that property.
            std::string_view ValueOf(uint8_t stateIndex, std::string_view propName) const;

            // Build a state index from a single property, leaving every other
            // property at its default. The common case for placement rules.
            uint8_t IndexOfSingle(std::string_view propName, std::string_view value) const;
        };

        static const BlockStateDefinition& GetStateDefinition(BlockID id);
        // Populates the per-BlockID state table. Called from Init() after every
        // block is registered (it classifies from model names).
        static void InitBlockStates();
        static uint16_t GetStateCount(BlockID id) { return GetStateDefinition(id).StateCount(); }
        static bool     HasStates(BlockID id)     { return !GetStateDefinition(id).properties.empty(); }

        // Backing storage for all blocks
        static std::array<Block, Size> blockDefinitions;

    private:
        BlockRegistry() = delete;

        // Helper to register a block with model-based rendering
        static void RegisterModelBlock(BlockID id, const std::string& name, RenderLayer layer,
                                     const std::string& modelName);

        // Helper to register a block with legacy texture indices
        static void RegisterLegacyBlock(BlockID id, const std::string& name, bool opaque,
                                      const std::array<uint16_t, 6>& texIndices);
    };

    // Flat block-state id space — see BlockStateIds.hpp, included above.


} // namespace Game