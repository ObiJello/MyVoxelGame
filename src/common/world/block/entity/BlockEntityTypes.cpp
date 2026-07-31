// File: src/common/world/block/entity/BlockEntityTypes.cpp
#include "BlockEntityTypes.hpp"
#include "ChestBlockEntity.hpp"
#include "../../../core/Log.hpp"
#include <memory>
#include <unordered_map>

namespace Game {

    std::array<const BlockEntityType*, BlockEntityTypeIds::MAX_ID>
        BlockEntityTypes::s_byId{};
    std::vector<const BlockEntityType*>
        BlockEntityTypes::s_byBlockId;
    bool BlockEntityTypes::s_initialised = false;

    namespace {
        // Owning storage for registered types (raw pointers in the lookup
        // tables alias into here). Lifetime = whole program; the registry is
        // populated once at startup and never mutated thereafter.
        std::vector<std::unique_ptr<BlockEntityType>> g_typeStorage;
        std::unordered_map<std::string, const BlockEntityType*> g_byStringId;

        const BlockEntityType* RegisterType(
                uint16_t typeId, std::string stringId,
                BlockEntityType::Factory factory,
                std::unordered_set<BlockID> validBlocks) {
            auto type = std::make_unique<BlockEntityType>(
                typeId, std::move(stringId), std::move(factory), std::move(validBlocks));
            const BlockEntityType* raw = type.get();
            g_typeStorage.push_back(std::move(type));
            return raw;
        }
    }

    void BlockEntityTypes::Initialize() {
        if (s_initialised) return;
        s_initialised = true;

        // Size the BlockID lookup table to cover every block id; null entries
        // mean "no BE for this block".
        s_byBlockId.assign(static_cast<size_t>(BlockID::Count), nullptr);

        // CHEST (backs Chest, TrappedChest, EnderChest)
        {
            std::unordered_set<BlockID> blocks = {
                BlockID::Chest, BlockID::TrappedChest, BlockID::EnderChest,
            };
            const auto* type = RegisterType(
                BlockEntityTypeIds::CHEST, "chest",
                [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                    return std::make_unique<ChestBlockEntity>(t, pos, id);
                },
                blocks);
            s_byId[BlockEntityTypeIds::CHEST] = type;
            g_byStringId[type->StringId()] = type;
            for (BlockID id : blocks) {
                const auto idx = static_cast<size_t>(id);
                if (idx < s_byBlockId.size()) s_byBlockId[idx] = type;
            }
        }

        // TODO future stages: register Sign, Banner, Bed, ShulkerBox, …

        Log::Info("[BlockEntityTypes] initialised with %zu type(s)", g_typeStorage.size());
    }

    bool BlockEntityTypes::HasBlockEntity(BlockID id) {
        const auto idx = static_cast<size_t>(id);
        return idx < s_byBlockId.size() && s_byBlockId[idx] != nullptr;
    }

    const BlockEntityType* BlockEntityTypes::ForBlock(BlockID id) {
        const auto idx = static_cast<size_t>(id);
        return (idx < s_byBlockId.size()) ? s_byBlockId[idx] : nullptr;
    }

    const BlockEntityType* BlockEntityTypes::ForId(uint16_t typeId) {
        return (typeId < BlockEntityTypeIds::MAX_ID) ? s_byId[typeId] : nullptr;
    }

    const BlockEntityType* BlockEntityTypes::ByStringId(const std::string& stringId) {
        auto it = g_byStringId.find(stringId);
        return (it != g_byStringId.end()) ? it->second : nullptr;
    }

} // namespace Game
