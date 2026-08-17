// File: src/common/world/block/entity/BlockEntityTypes.cpp
#include "BlockEntityTypes.hpp"
#include "BaseContainerBlockEntity.hpp"
#include "ChestBlockEntity.hpp"
#include "FurnaceBlockEntity.hpp"
#include "CampfireBlockEntity.hpp"
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

        // ── Plain storage containers ──────────────────────────────────────
        // Everything below is a BaseContainerBlockEntity with nothing but a
        // slot count, so one helper covers them. MC gives each its own class
        // because each carries extra behaviour we don't have yet (a barrel's
        // open state, a hopper's transfer cooldown, a dispenser's RNG); the
        // storage half is identical, and that is all these need to open.
        auto registerContainer = [](uint16_t typeId, const char* stringId,
                                    int slots, std::unordered_set<BlockID> blocks) {
            const auto* type = RegisterType(
                typeId, stringId,
                [slots](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                    return std::make_unique<BaseContainerBlockEntity>(t, pos, id, slots);
                },
                blocks);
            s_byId[typeId] = type;
            g_byStringId[type->StringId()] = type;
            for (BlockID id : blocks) {
                const auto idx = static_cast<size_t>(id);
                if (idx < s_byBlockId.size()) s_byBlockId[idx] = type;
            }
        };

        // Barrel — 27 slots, same as a chest (BarrelBlockEntity).
        registerContainer(BlockEntityTypeIds::BARREL, "barrel", 27, {BlockID::Barrel});

        // Shulker boxes — 27 slots each, one BE type across all 17 colours
        // (MC registers ShulkerBoxBlockEntity against every dyed variant too).
        registerContainer(BlockEntityTypeIds::SHULKER_BOX, "shulker_box", 27, {
            BlockID::ShulkerBox,
            BlockID::WhiteShulkerBox,     BlockID::OrangeShulkerBox,
            BlockID::MagentaShulkerBox,   BlockID::LightBlueShulkerBox,
            BlockID::YellowShulkerBox,    BlockID::LimeShulkerBox,
            BlockID::PinkShulkerBox,      BlockID::GrayShulkerBox,
            BlockID::LightGrayShulkerBox, BlockID::CyanShulkerBox,
            BlockID::PurpleShulkerBox,    BlockID::BlueShulkerBox,
            BlockID::BrownShulkerBox,     BlockID::GreenShulkerBox,
            BlockID::RedShulkerBox,       BlockID::BlackShulkerBox,
        });

        // Dispenser / dropper — 9 slots in a 3x3 (DispenserBlockEntity).
        registerContainer(BlockEntityTypeIds::DISPENSER, "dispenser", 9, {BlockID::Dispenser});
        registerContainer(BlockEntityTypeIds::DROPPER,   "dropper",   9, {BlockID::Dropper});

        // Hopper — 5 slots in a row (HopperBlockEntity).
        registerContainer(BlockEntityTypeIds::HOPPER, "hopper", 5, {BlockID::Hopper});

        // Brewing stand (5 slots) and crafter (9). Both store items, so both
        // need a block entity; their behaviour lives in their menus.
        registerContainer(BlockEntityTypeIds::BREWING_STAND, "brewing_stand", 5,
                          {BlockID::BrewingStand});
        registerContainer(BlockEntityTypeIds::CRAFTER, "crafter", 9, {BlockID::Crafter});

        // ── Furnace family ────────────────────────────────────────────────
        // One class, three registrations. The CookingKind baked into each is
        // the whole difference: MC does the same, handing
        // AbstractFurnaceBlockEntity a recipeType per block.
        auto registerFurnace = [](uint16_t typeId, const char* stringId,
                                  CookingKind kind, BlockID block) {
            std::unordered_set<BlockID> blocks = {block};
            const auto* type = RegisterType(
                typeId, stringId,
                [kind](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                    return std::make_unique<FurnaceBlockEntity>(t, pos, id, kind);
                },
                blocks);
            s_byId[typeId] = type;
            g_byStringId[type->StringId()] = type;
            const auto idx = static_cast<size_t>(block);
            if (idx < s_byBlockId.size()) s_byBlockId[idx] = type;
        };
        registerFurnace(BlockEntityTypeIds::FURNACE, "furnace",
                        CookingKind::Smelting, BlockID::Furnace);
        registerFurnace(BlockEntityTypeIds::BLAST_FURNACE, "blast_furnace",
                        CookingKind::Blasting, BlockID::BlastFurnace);
        registerFurnace(BlockEntityTypeIds::SMOKER, "smoker",
                        CookingKind::Smoking, BlockID::Smoker);

        // ── Campfires ─────────────────────────────────────────────────────
        // Four food slots with independent timers. Not registerContainer'd
        // because the campfire needs its own class for the cook/cooldown tick
        // — the container helper only stores items.
        //
        // MC gives the soul campfire its own BlockEntityType even though the
        // class is shared (BlockEntityType.CAMPFIRE vs SOUL_CAMPFIRE), so the
        // ids stay separate here too.
        auto registerCampfire = [](uint16_t typeId, const char* stringId, BlockID block) {
            std::unordered_set<BlockID> blocks = {block};
            const auto* type = RegisterType(
                typeId, stringId,
                [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                    return std::make_unique<CampfireBlockEntity>(t, pos, id);
                },
                blocks);
            s_byId[typeId] = type;
            g_byStringId[type->StringId()] = type;
            const auto idx = static_cast<size_t>(block);
            if (idx < s_byBlockId.size()) s_byBlockId[idx] = type;
        };
        registerCampfire(BlockEntityTypeIds::CAMPFIRE, "campfire", BlockID::Campfire);
        registerCampfire(BlockEntityTypeIds::SOUL_CAMPFIRE, "soul_campfire",
                         BlockID::SoulCampfire);

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
