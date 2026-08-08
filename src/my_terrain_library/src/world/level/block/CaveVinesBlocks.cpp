#include "world/level/block/Blocks.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

const Block* CaveVinesBlock::getBodyBlock() const {
    return Blocks::CAVE_VINES_PLANT;
}

const Block* CaveVinesPlantBlock::getHeadBlock() const {
    return Blocks::CAVE_VINES;
}

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
