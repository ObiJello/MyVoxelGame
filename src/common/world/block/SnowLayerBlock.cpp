// File: src/common/world/block/SnowLayerBlock.cpp
//
// SnowLayerBlock and SnowyBlock (MC 26.3) — see SnowLayerBlock.hpp for the
// split between this file and the model-derived shapes.
#include "common/world/block/SnowLayerBlock.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/block/ShapeOcclusion.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/lighting/ChunkLight.hpp"   // Lighting::LightLayer
#include "common/core/Log.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Game {

    namespace {

        // ── Block tags, resolved once per BlockID ───────────────────────────
        //
        // MC `state.is(BlockTags.X)` against the data pack's tag files
        // (data/minecraft/tags/block/*.json), so a data pack that edits the
        // lists is honoured. Resolved lazily, not at BlockRegistry::Init:
        // survival is first asked long after the data pack is reachable, and
        // the answer is then a byte read for every later neighbour update.
        struct SnowTagTables {
            std::array<bool, BlockRegistry::Size> snow{};              // #minecraft:snow
            std::array<bool, BlockRegistry::Size> cannotSupport{};     // #cannot_support_snow_layer
            std::array<bool, BlockRegistry::Size> supportOverride{};   // #support_override_snow_layer
        };

        const SnowTagTables& Tags() {
            static SnowTagTables tables;
            static std::once_flag once;
            std::call_once(once, [] {
                for (size_t i = 1; i < BlockRegistry::Size; ++i) {
                    const std::string& slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
                    if (slug.empty()) continue;
                    const std::vector<std::string>& tags =
                        DataTags::TagsFor(DataTags::Registry::Block, slug);
                    auto has = [&](const char* tag) {
                        return std::binary_search(tags.begin(), tags.end(), std::string(tag));
                    };
                    tables.snow[i]            = has("#minecraft:snow");
                    tables.cannotSupport[i]   = has("#minecraft:cannot_support_snow_layer");
                    tables.supportOverride[i] = has("#minecraft:support_override_snow_layer");
                }
                // The data pack is the authority, but a missing or unreadable
                // one must not leave snow floating on ice or grass that never
                // turns white under it: fall back to the vanilla lists.
                const bool anyTag = std::find(tables.snow.begin(), tables.snow.end(), true) !=
                                    tables.snow.end();
                if (!anyTag) {
                    for (BlockID id : { BlockID::SnowLayer, BlockID::Snow, BlockID::PowderSnow }) {
                        tables.snow[static_cast<size_t>(id)] = true;
                    }
                    for (BlockID id : { BlockID::Ice, BlockID::PackedIce, BlockID::Barrier }) {
                        tables.cannotSupport[static_cast<size_t>(id)] = true;
                    }
                    for (BlockID id : { BlockID::HoneyBlock, BlockID::SoulSand, BlockID::Mud }) {
                        tables.supportOverride[static_cast<size_t>(id)] = true;
                    }
                }
            });
            return tables;
        }

        bool Tagged(const std::array<bool, BlockRegistry::Size>& table, BlockID id) {
            const size_t i = static_cast<size_t>(id);
            return i < table.size() && table[i];
        }

        // MC SnowLayerBlock.SHAPES[1..7] as collision boxes: Block.column(16,
        // 0, height * 2). SHAPES[0] (a zero-height box) is Shapes.empty().
        const std::array<BlockRegistry::BlockShape, SnowLayer::kMaxHeight>& CollisionBoxes() {
            static const std::array<BlockRegistry::BlockShape, SnowLayer::kMaxHeight> kBoxes = [] {
                std::array<BlockRegistry::BlockShape, SnowLayer::kMaxHeight> boxes{};
                for (int height = 0; height < SnowLayer::kMaxHeight; ++height) {
                    boxes[static_cast<size_t>(height)].min = glm::vec3(0.0f);
                    boxes[static_cast<size_t>(height)].max =
                        glm::vec3(1.0f, static_cast<float>(height * 2) / 16.0f, 1.0f);
                }
                return boxes;
            }();
            return kBoxes;
        }

        // ── SnowLayerBlock hooks ───────────────────────────────────────────

        // MC SnowLayerBlock.updateShape:
        //   !state.canSurvive(level, pos) ? AIR : super.updateShape(...)
        // from ANY direction — only the block below decides survival, but a
        // change on another side still re-asks, exactly as vanilla does.
        // AIR from updateShape is a destroy (World::UpdateOrDestroy), whose
        // entity-less dropResources yields nothing for snow.
        bool SnowLayerUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                  BlockState /*state*/, Direction /*toNeighbour*/,
                                  BlockID /*neighbourId*/, BlockState& outState,
                                  ScheduledTickAccess* /*ticks*/) {
            if (SnowLayer::CanSurvive(level, pos)) return false;
            outState = BlockState{};
            return true;
        }

        // MC Properties.randomTicks(): every layer count ticks.
        bool SnowLayerIsRandomlyTicking(BlockState /*state*/) { return true; }

        // MC SnowLayerBlock.randomTick:
        //   if (level.getBrightness(LightLayer.BLOCK, pos) > 11) {
        //       dropResources(state, level, pos);
        //       level.removeBlock(pos, false);
        //       level.gameEvent(GameEvent.BLOCK_DESTROY, pos, Context.of(state));
        //   }
        // BLOCK light only — sunlight never melts snow, a torch or glowstone
        // beside it does. dropResources has no entity, so the snow table's
        // entity_properties pool fails and the melt drops nothing.
        void SnowLayerRandomTick(ILevelWrite& level, const glm::ivec3& pos,
                                 BlockState state, JavaRandom& /*random*/) {
            if (level.GetBrightness(Lighting::LightLayer::Block, pos.x, pos.y, pos.z) <= 11) return;
            DropBlockLoot(level, pos, state);
            // Level.removeBlock(pos, false): the cell's fluid's legacy block
            // (a snow layer holds none, so air), flag 3.
            level.SetBlock(pos.x, pos.y, pos.z, BlockState{}, World::UpdateFlags::All);
            level.GameEvent(GameEventId::BlockDestroy, pos, GameEventContext::Of(state));
        }

        // ── SnowyBlock hook ────────────────────────────────────────────────

        // MC SnowyBlock.updateShape:
        //   directionToNeighbour == UP
        //       ? state.setValue(SNOWY, isSnowySetting(neighbourState))
        //       : super.updateShape(...)
        // This is what turns a grass block white the moment snow lands on
        // it — a player's layer, a snow golem's trail, accumulating snowfall —
        // and green again when the snow is dug off or melts.
        bool SnowyUpdateShape(const IBlockAccess& /*level*/, const glm::ivec3& /*pos*/,
                              BlockState state, Direction toNeighbour, BlockID neighbourId,
                              BlockState& outState, ScheduledTickAccess* /*ticks*/) {
            if (toNeighbour != Direction::Up) return false;
            const BlockState next =
                state.SetName(PropertyId::SNOWY, SnowLayer::IsInSnowTag(neighbourId) ? "true" : "false");
            if (next == state) return false;
            outState = next;
            return true;
        }

    } // namespace

    namespace SnowLayer {

        int Layers(BlockState state) {
            if (!state.Is(BlockID::SnowLayer)) return 0;
            // LAYERS lists 1..8, so the value index is layers - 1.
            return state.GetIndex(PropertyId::LAYERS) + 1;
        }

        bool IsInSnowTag(BlockID id) {
            return Tagged(Tags().snow, id);
        }

        bool CanSurvive(const IBlockAccess& level, const glm::ivec3& pos) {
            const BlockState below = level.GetBlockState(pos.x, pos.y - 1, pos.z);
            const BlockID belowId = below.Block();
            if (Tagged(Tags().cannotSupport, belowId)) return false;
            if (Tagged(Tags().supportOverride, belowId)) return true;
            // `belowState.is(this) && layers == 8`. Tested before the face
            // rule because a full pile's COLLISION shape is SHAPES[7] — 14
            // pixels — so the face rule alone would refuse to stack on it.
            if (belowId == BlockID::SnowLayer) return Layers(below) == kMaxHeight;
            // Block.isFaceFull(belowState.getCollisionShape(level, below), UP).
            // A `.noCollision()` block's collision shape is empty; the shape
            // set would otherwise fall back to its outline.
            if (belowId == BlockID::Air || !BlockRegistry::HasCollision(belowId)) return false;
            const BlockRegistry::BlockShapeSet collision =
                BlockRegistry::GetBlockCollisionShapeSet(below);
            if (collision.count == 0) return false;
            if (collision.IsFullCube()) return true;
            return Shapes::FaceCovers(collision, Direction::Up, Shapes::FaceRect{0.0f, 0.0f, 1.0f, 1.0f});
        }

        BlockState GrownState(BlockState existing) {
            const int layers = Layers(existing);
            if (layers == 0 || layers >= kMaxHeight) return existing;
            // `state.setValue(LAYERS, Math.min(8, layers + 1))` — setValue on
            // the state already there.
            return existing.SetIndex(PropertyId::LAYERS, layers);   // index of layers + 1
        }

        const BlockRegistry::BlockShape* CollisionBox(BlockState state) {
            const int layers = Layers(state);
            if (layers <= 1) return nullptr;
            return &CollisionBoxes()[static_cast<size_t>(layers - 1)];
        }

        bool IsPathfindableLand(BlockState state) {
            return Layers(state) < kHeightImpassable;
        }

    } // namespace SnowLayer

    bool IsSnowyBlock(BlockID id) {
        if (id == BlockID::Air) return false;
        return BlockStates::Default(id).HasProperty(PropertyId::SNOWY);
    }

    BlockState SnowyPlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                   BlockState state) {
        if (!IsSnowyBlock(state.Block())) return state;
        const BlockID above = level.GetBlock(pos.x, pos.y + 1, pos.z);
        return state.SetName(PropertyId::SNOWY, SnowLayer::IsInSnowTag(above) ? "true" : "false");
    }

    void BlockRegistry_RegisterSnow(std::array<Block, BlockRegistry::Size>& blocks) {
        Block& snow = blocks[static_cast<size_t>(BlockID::SnowLayer)];
        snow.updateShape       = &SnowLayerUpdateShape;
        snow.isRandomlyTicking = &SnowLayerIsRandomlyTicking;
        snow.randomTick        = &SnowLayerRandomTick;

        // Every block declaring `snowy` is a SnowyBlock in MC's class chain
        // (GrassBlock and MyceliumBlock via SpreadingSnowyBlock, PODZOL
        // directly, the Aether's AetherGrassBlock via GrassBlock). None of
        // them has an updateShape of its own; a hook some other port
        // installed is left in place and reported rather than silently lost.
        for (size_t i = 1; i < blocks.size(); ++i) {
            const BlockID id = static_cast<BlockID>(i);
            if (!IsSnowyBlock(id)) continue;
            if (blocks[i].updateShape && blocks[i].updateShape != &SnowyUpdateShape) {
                Log::Warning("[Snow] %s already has an updateShape hook; its `snowy` "
                             "flag will not follow the block above",
                             blocks[i].registrySlug.c_str());
                continue;
            }
            blocks[i].updateShape = &SnowyUpdateShape;
        }
    }

} // namespace Game
