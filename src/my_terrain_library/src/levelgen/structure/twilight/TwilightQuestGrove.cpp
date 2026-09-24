#include "levelgen/structure/twilight/TwilightQuestGrove.h"

#include "levelgen/structure/twilight/TwilightTemplatePieces.h"
#include "levelgen/structure/StructurePieceBehavior.h"
#include "levelgen/structure/StructureStartData.h"
#include "levelgen/structure/Structures.h"
#include "levelgen/structure/TemplateEngine.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "core/BlockPos.h"
#include "core/Direction.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Twilight Forest 4.9 — type/QuestGroveStructure.java, QuestGrove.java.

namespace minecraft {
namespace levelgen {
namespace structure {
namespace twilight_pieces {

namespace {

namespace tt = twilight_template;
using core::BlockPos;
using core::Direction;
using world::level::block::Blocks;

// QuestGroveStructure.LENGTH.
constexpr int kLength = 27;

// QuestGrove (TwilightTemplateStructurePiece): MOSSY_BRICK_DECAY
// (TargetedRotProcessor{mossy_stone_bricks}, 0.5) + StoneBricksVariants;
// placed two blocks lower (placePieceAdjusted(-2)).
class QuestGroveBehavior final : public tt::TemplatePieceBehavior {
public:
    explicit QuestGroveBehavior(Config config) : TemplatePieceBehavior(std::move(config)) {}

protected:
    // QuestGrove.handleDataMarker.
    void handleDataMarker(const std::string& label, const BlockPos& pos, WorldGenLevel* level,
                          WorldgenRandom& random, const BoundingBox& chunkBB,
                          ChunkGenerator* generator, int rotation) override {
        (void)generator;
        (void)rotation;
        if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) return;
        if (label == "quest_ram") {
            // FeaturePlacers.placeEntity(QUEST_RAM): the engine has no quest
            // ram; the grove is generated without it.
            return;
        }
        if (label == "dispenser") {
            // TFLootTables.generateLootContainer(level, pos, DROPPER facing
            // rotation.rotate(NORTH), 16 | 4 | 2, random.nextLong(),
            // QUEST_GROVE) - the seed is drawn as the call's argument.
            const int64_t seed = random.nextLong();
            BlockState* dropper = Blocks::getDefaultState("minecraft:dropper");
            if (dropper == nullptr) return;
            const Direction facing = tt::rotate(m_config.rotation, Direction::NORTH);
            const char* facingName = core::getName(facing);
            dropper = tt::withProperty(dropper, "facing", facingName);
            level->setBlock(pos, dropper, 16 | 4 | 2);
            tt::setBlockEntity(level, pos,
                tt::lootContainerPayload("minecraft:dropper", "twilightforest:quest_grove_dropper", seed));
        }
    }
};

} // namespace

bool buildQuestGrove(const StructureInfo& info, GenerationContext& ctx,
                     LegacyRandomSource& firstPieceRandom,
                     int32_t x, int32_t y, int32_t z, StructureStartData& out) {
    (void)info;
    (void)ctx;
    (void)firstPieceRandom;
    // getFirstPiece: new QuestGrove(manager, BlockPos(x - LENGTH / 2 + 1,
    // y + 2, z - LENGTH / 2 + 1)) - the + 1 offsets center the structure.
    tt::TemplatePieceBehavior::Config config;
    config.templateId = "twilightforest:quest_grove";
    config.rotation = tt::ROT_NONE;
    config.templatePosition = BlockPos(x - kLength / 2 + 1, y + 2, z - kLength / 2 + 1);
    config.adjustY = -2;
    config.processors = [](std::vector<tt::Processor>& chain, WorldGenLevel*) {
        static BlockState* const kMossyStoneBricks = Blocks::getDefaultState("minecraft:mossy_stone_bricks");
        chain.push_back(tt::targetedRot({kMossyStoneBricks}, 0.5f));
        chain.push_back(tt::stoneBricksVariants());
    };
    const BoundingBox box = tt::templateBoundingBox(config.templateId, TemplatePlaceSettings{},
                                                    config.templatePosition);
    out.pieces.push_back(tt::makePiece("twilightforest:tfquest1", box, tt::ROT_NONE, 0, config.templateId));
    out.behaviors.push_back(std::make_shared<QuestGroveBehavior>(std::move(config)));
    // generateFromStartingPiece: QuestGrove.addChildren is the no-op default;
    // the stub's sort keeps the single piece.
    return true;
}

} // namespace twilight_pieces
} // namespace structure
} // namespace levelgen
} // namespace minecraft
