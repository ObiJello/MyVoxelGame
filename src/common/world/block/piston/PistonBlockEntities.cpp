// File: src/common/world/block/piston/PistonBlockEntities.cpp
//
// See PistonBlockEntities.hpp — the pistons_move_block_entities rule.
#include "common/world/block/piston/PistonBlockEntities.hpp"

#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/block/entity/BlockEntityType.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/level/ILevelWrite.hpp"

#include <string_view>

namespace Game::PistonBlockEntities {

    namespace {

        // The block entities whose whole state is position-free, so moving
        // the object to another cell is all a move takes. Anything not here
        // keeps vanilla's rule (immovable) — see the header for why each
        // excluded family stays put.
        constexpr std::string_view kMovableTypes[] = {
            // Containers (inventory, custom name, lock, loot table).
            "chest", "trapped_chest", "ender_chest", "barrel", "shulker_box",
            "dispenser", "dropper", "hopper", "crafter", "brewing_stand",
            "furnace", "blast_furnace", "smoker",
            // Decoration and utility.
            "sign", "hanging_sign", "banner", "skull", "lectern",
            "campfire", "soul_campfire", "chiseled_bookshelf", "beehive",
            "daylight_detector", "comparator", "decorated_pot", "bell",
            "copper_golem_statue", "brushable_block", "potent_sulfur",
            // Engine: the shared crafting table's stored grid.
            "obeycraft:crafting_table",
        };

    } // namespace

    bool IsMovableType(const BlockEntityType* type) {
        if (!type) return false;
        const std::string_view id = type->StringId();
        for (std::string_view movable : kMovableTypes) {
            if (id == movable) return true;
        }
        return false;
    }

    bool CanPushBlockWithEntity(BlockID block) {
        return Enabled() && IsMovableType(BlockEntityTypes::ForBlock(block));
    }

    bool ShouldCarry(const BlockEntity& entity) {
        // By TYPE: the moving cell's own entity is the piston type (never on
        // the whitelist). The entity's block id is not a safe test — a
        // client copy built from data that reached a cell while it was
        // still the moving piston used to carry that id (and so could never
        // be carried again; see ClientConnection::HandleBlockEntityData).
        const BlockEntityType* type = entity.GetType();
        return Enabled() && type && type->TypeId() != BlockEntityTypeIds::PISTON && IsMovableType(type);
    }

    void PlaceCarried(ILevelWrite& level, const glm::ivec3& pos, std::unique_ptr<BlockEntity> carried,
                      bool landedHere) {
        if (!carried) return;
        const BlockState landed = level.GetBlockState(pos.x, pos.y, pos.z);
        const BlockEntityType* type = carried->GetType();
        if (landedHere && type && landed.Block() != BlockID::Air && type->IsValidFor(landed.Block())) {
            carried->MoveCarriedTo(pos);
            carried->RebindBlock(landed.Block());
            // Replaces the empty entity the landing write created (World::
            // SetBlock's step 6) and broadcasts the carried state to every
            // watcher; on the client, the landed cell's own copy.
            level.SetBlockEntity(pos, std::move(carried));
            // The landing write's neighbour updates ran while the cell held
            // that empty entity: a comparator reading it saw nothing (MC
            // BlockEntity.setChanged → updateNeighbourForOutputSignal).
            if (!level.IsClientSide() && BlockRegistry::Get(landed.Block()).hasAnalogOutputSignal) {
                level.UpdateNeighbourForOutputSignal(pos, landed.Block());
            }
            return;
        }
        // The block is not there (it broke on landing, or something else took
        // the cell): MC Containers.dropContents at the cell, so a container's
        // items come out instead of vanishing. A client copy just goes.
        if (!level.IsClientSide()) {
            carried->MoveCarriedTo(pos);
            if (!carried->GetLevel()) carried->SetLevel(&level);
            carried->PreRemoveSideEffects(level, pos, landed);
        }
    }

} // namespace Game::PistonBlockEntities
