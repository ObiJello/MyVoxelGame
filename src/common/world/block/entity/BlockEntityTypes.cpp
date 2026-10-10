// File: src/common/world/block/entity/BlockEntityTypes.cpp
#include "BlockEntityTypes.hpp"
#include "SignBlockEntity.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/loot/ChestLootTables.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/data/DataComponents.hpp"
#include "common/core/Log.hpp"
#include <initializer_list>
#include <string_view>
#include <tuple>
#include "BaseContainerBlockEntity.hpp"
#include "ChestBlockEntity.hpp"
#include "FurnaceBlockEntity.hpp"
#include "BrewingStandBlockEntity.hpp"
#include "CampfireBlockEntity.hpp"
#include "EndGatewayBlockEntity.hpp"
#include "ComparatorBlockEntity.hpp"
#include "DaylightDetectorBlockEntity.hpp"
#include "PistonMovingBlockEntity.hpp"
#include "HopperBlockEntity.hpp"
#include "DispenserBlockEntity.hpp"
#include "LecternBlockEntity.hpp"
#include "JukeboxBlockEntity.hpp"
#include "SpawnerBlockEntity.hpp"
#include "TrialSpawnerBlockEntity.hpp"
#include "VaultBlockEntity.hpp"
#include "BrushableBlockEntity.hpp"
#include "DecoratedPotBlockEntity.hpp"
#include "BannerBlockEntity.hpp"
#include "BellBlockEntity.hpp"
#include "CopperGolemStatueBlockEntity.hpp"
#include "ChiseledBookShelfBlockEntity.hpp"
#include "BeehiveBlockEntity.hpp"
#include "PotentSulfurBlockEntity.hpp"
#include "SculkBlockEntities.hpp"
#include "AurelithBlockEntities.hpp"
#include "HushLighthouseLampBlockEntity.hpp"
#include "CraftingTableBlockEntity.hpp"
#include "SkyFacesBlockEntity.hpp"
#include "common/world/level/ILevelWrite.hpp"
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
        // Per-BlockID lazy types (LazyForBlock): never created by SetBlock.
        std::vector<const BlockEntityType*> g_lazyByBlockId;

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

        // CHEST / TRAPPED_CHEST / ENDER_CHEST — three types, not one.
        //
        // These used to share the single "chest" type. That was harmless while
        // block entities lived only in memory, but it is not once they are
        // written to disk: vanilla validates a block entity's id against the
        // block it sits on (BlockEntity.loadStatic / the type's validBlocks),
        // so a trapped chest saved as `minecraft:chest` is rejected outright
        // and the chest — contents and all — is DROPPED on load.
        {
            auto registerChest = [](uint16_t typeId, const char* stringId,
                                    std::initializer_list<BlockID> validBlocks) {
                std::unordered_set<BlockID> blocks(validBlocks);
                const auto* type = RegisterType(
                    typeId, stringId,
                    [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                        return std::make_unique<ChestBlockEntity>(t, pos, id);
                    },
                    blocks);
                s_byId[typeId] = type;
                g_byStringId[type->StringId()] = type;
                for (BlockID block : validBlocks) {
                    const auto idx = static_cast<size_t>(block);
                    if (idx < s_byBlockId.size()) s_byBlockId[idx] = type;
                }
            };
            // MC BlockEntityTypes.CHEST's valid blocks are the chest AND every
            // copper chest (CopperChestBlock hands ChestBlock BlockEntityTypes
            // .CHEST), so a copper chest saves as `minecraft:chest`.
            registerChest(BlockEntityTypeIds::CHEST, "chest",
                          { BlockID::Chest,
                            BlockID::CopperChest, BlockID::ExposedCopperChest,
                            BlockID::WeatheredCopperChest, BlockID::OxidizedCopperChest,
                            BlockID::WaxedCopperChest, BlockID::WaxedExposedCopperChest,
                            BlockID::WaxedWeatheredCopperChest, BlockID::WaxedOxidizedCopperChest });
            registerChest(BlockEntityTypeIds::TRAPPED_CHEST, "trapped_chest", { BlockID::TrappedChest });
            // An ender chest's block entity carries NO Items in vanilla — the
            // contents live in the player's EnderItems. Ours still holds slots
            // so the UI works; the serialiser deliberately omits them rather
            // than writing an Items list vanilla would discard.
            registerChest(BlockEntityTypeIds::ENDER_CHEST,   "ender_chest",   { BlockID::EnderChest });
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

        // Dispenser / dropper — 9 slots in a 3x3, plus the random-slot pick
        // and the insert used by dispense behaviours (DispenserBlockEntity).
        {
            auto registerDispenser = [](uint16_t typeId, const char* stringId, BlockID block) {
                std::unordered_set<BlockID> blocks = { block };
                const auto* type = RegisterType(
                    typeId, stringId,
                    [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                        return std::make_unique<DispenserBlockEntity>(t, pos, id);
                    },
                    blocks);
                s_byId[typeId] = type;
                g_byStringId[type->StringId()] = type;
                const auto idx = static_cast<size_t>(block);
                if (idx < s_byBlockId.size()) s_byBlockId[idx] = type;
            };
            registerDispenser(BlockEntityTypeIds::DISPENSER, "dispenser", BlockID::Dispenser);
            registerDispenser(BlockEntityTypeIds::DROPPER,   "dropper",   BlockID::Dropper);
        }

        // Hopper — 5 slots in a row plus the transfer tick (HopperBlockEntity).
        {
            std::unordered_set<BlockID> blocks = { BlockID::Hopper };
            const auto* type = RegisterType(
                BlockEntityTypeIds::HOPPER, "hopper",
                [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                    return std::make_unique<HopperBlockEntity>(t, pos, id);
                },
                blocks);
            s_byId[BlockEntityTypeIds::HOPPER] = type;
            g_byStringId[type->StringId()] = type;
            s_byBlockId[static_cast<size_t>(BlockID::Hopper)] = type;
        }

        // Brewing stand — five slots plus the brew/fuel tick
        // (BrewingStandBlockEntity, MC BrewingStandBlockEntity.serverTick).
        {
            std::unordered_set<BlockID> blocks = { BlockID::BrewingStand };
            const auto* type = RegisterType(
                BlockEntityTypeIds::BREWING_STAND, "brewing_stand",
                [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                    return std::make_unique<BrewingStandBlockEntity>(t, pos, id);
                },
                blocks);
            s_byId[BlockEntityTypeIds::BREWING_STAND] = type;
            g_byStringId[type->StringId()] = type;
            s_byBlockId[static_cast<size_t>(BlockID::BrewingStand)] = type;
        }
        // Crafter (9) stores items; its behaviour lives in its menu.
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

        // ── Skulls / mob heads ────────────────────────────────────────────
        // MC BlockEntityTypes.SKULL: one type, valid for every skull and head
        // block, floor and wall variants alike. The base BlockEntity carries
        // everything needed — MC's SkullBlockEntity adds only the player-head
        // owner profile and the note-block-powered animation ticker, neither
        // of which this port models yet. Orientation is NOT block-entity
        // state: the renderer reads `rotation`/`facing` off the block state,
        // the same way the chest renderer reads its facing.
        {
            std::unordered_set<BlockID> blocks = {
                BlockID::SkeletonSkull,       BlockID::SkeletonWallSkull,
                BlockID::WitherSkeletonSkull, BlockID::WitherSkeletonWallSkull,
                BlockID::ZombieHead,          BlockID::ZombieWallHead,
                BlockID::CreeperHead,         BlockID::CreeperWallHead,
                BlockID::PlayerHead,          BlockID::PlayerWallHead,
                BlockID::PiglinHead,          BlockID::PiglinWallHead,
                BlockID::DragonHead,          BlockID::DragonWallHead,
            };
            const auto* type = RegisterType(
                BlockEntityTypeIds::SKULL, "skull",
                [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                    return std::make_unique<BlockEntity>(t, pos, id);
                },
                blocks);
            s_byId[BlockEntityTypeIds::SKULL] = type;
            g_byStringId[type->StringId()] = type;
            for (BlockID id : blocks) {
                const auto idx = static_cast<size_t>(id);
                if (idx < s_byBlockId.size()) s_byBlockId[idx] = type;
            }
        }

        // ── Signs ─────────────────────────────────────────────────────────
        // Every "*_sign" block. MC registers SIGN against the 26 standing +
        // wall signs and HANGING_SIGN against the 26 ceiling + wall hanging
        // signs; the block's slug is the membership test, so a new wood type
        // joins by existing.
        {
            std::unordered_set<BlockID> plain, hanging;
            for (size_t i = 1; i < static_cast<size_t>(BlockID::Count); ++i) {
                const std::string& slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
                constexpr std::string_view kSuffix = "_sign";
                if (slug.size() <= kSuffix.size() ||
                    slug.compare(slug.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) continue;
                if (slug.find("hanging_sign") != std::string::npos) hanging.insert(static_cast<BlockID>(i));
                else                                                 plain.insert(static_cast<BlockID>(i));
            }
            for (auto& [typeId, stringId, blocks] :
                 { std::tuple<uint16_t, const char*, std::unordered_set<BlockID>*>{BlockEntityTypeIds::SIGN, "sign", &plain},
                   std::tuple<uint16_t, const char*, std::unordered_set<BlockID>*>{BlockEntityTypeIds::HANGING_SIGN, "hanging_sign", &hanging} }) {
                const auto* type = RegisterType(
                    typeId, stringId,
                    [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                        return std::make_unique<SignBlockEntity>(t, pos, id);
                    },
                    *blocks);
                s_byId[typeId] = type;
                g_byStringId[type->StringId()] = type;
                for (BlockID id : *blocks) {
                    const auto idx = static_cast<size_t>(id);
                    if (idx < s_byBlockId.size()) s_byBlockId[idx] = type;
                }
            }
        }

        // ── End gateway ───────────────────────────────────────────────────
        // MC BlockEntityType.END_GATEWAY. The fight's post-kill gateways and
        // the return gateways placed at teleport time get theirs through
        // World::SetBlock; worldgen gateways get one lazily on first entry
        // (PortalTravel's EndGateway branch).
        {
            std::unordered_set<BlockID> blocks = { BlockID::EndGateway };
            const auto* type = RegisterType(
                BlockEntityTypeIds::END_GATEWAY, "end_gateway",
                [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                    return std::make_unique<EndGatewayBlockEntity>(t, pos, id);
                },
                blocks);
            s_byId[BlockEntityTypeIds::END_GATEWAY] = type;
            g_byStringId[type->StringId()] = type;
            s_byBlockId[static_cast<size_t>(BlockID::EndGateway)] = type;
        }

        // ── Redstone ──────────────────────────────────────────────────────
        auto registerSimple = [](uint16_t typeId, const char* stringId, BlockID block,
                                 BlockEntityType::Factory factory) {
            std::unordered_set<BlockID> blocks = { block };
            const auto* type = RegisterType(typeId, stringId, std::move(factory), blocks);
            s_byId[typeId] = type;
            g_byStringId[type->StringId()] = type;
            const auto idx = static_cast<size_t>(block);
            if (idx < s_byBlockId.size()) s_byBlockId[idx] = type;
        };
        registerSimple(BlockEntityTypeIds::COMPARATOR, "comparator", BlockID::Comparator,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<ComparatorBlockEntity>(t, pos, id);
            });
        registerSimple(BlockEntityTypeIds::DAYLIGHT_DETECTOR, "daylight_detector", BlockID::DaylightDetector,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<DaylightDetectorBlockEntity>(t, pos, id);
            });
        // MC MovingPistonBlock.newBlockEntity returns null: the piston hands
        // the cell a fully-formed entity through World::SetBlockEntity. The
        // factory here only serves the client, which rebuilds one from the
        // wire, and a chunk load, which rebuilds one from NBT.
        registerSimple(BlockEntityTypeIds::PISTON, "piston", BlockID::MovingPiston,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<PistonMovingBlockEntity>(t, pos, id);
            });

        // ── Lectern ───────────────────────────────────────────────────────
        // MC BlockEntityTypes.LECTERN: the book lying on it and its page.
        registerSimple(BlockEntityTypeIds::LECTERN, "lectern", BlockID::Lectern,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<LecternBlockEntity>(t, pos, id);
            });

        // ── Jukebox ───────────────────────────────────────────────────────
        // MC BlockEntityTypes.JUKEBOX: the disc and the song it is playing.
        registerSimple(BlockEntityTypeIds::JUKEBOX, "jukebox", BlockID::Jukebox,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<JukeboxBlockEntity>(t, pos, id);
            });

        // ── Monster spawner ───────────────────────────────────────────────
        // MC BlockEntityTypes.MOB_SPAWNER ("minecraft:mob_spawner", the id
        // every generated spawner's payload carries): BaseSpawner's state
        // and its server/client tickers (SpawnerBlockEntity).
        registerSimple(BlockEntityTypeIds::MOB_SPAWNER, "mob_spawner", BlockID::Spawner,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<SpawnerBlockEntity>(t, pos, id);
            });

        // ── Trial chambers ────────────────────────────────────────────────
        // MC BlockEntityTypes.TRIAL_SPAWNER ("minecraft:trial_spawner") and
        // VAULT ("minecraft:vault"): the wave spawner with its normal and
        // ominous configs (TrialSpawnerBlockEntity) and the key-locked reward
        // vault (VaultBlockEntity). Generated ones come from the trial
        // chambers' template nbt (AttachGeneratedBlockEntities).
        registerSimple(BlockEntityTypeIds::TRIAL_SPAWNER, "trial_spawner", BlockID::TrialSpawner,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<TrialSpawnerBlockEntity>(t, pos, id);
            });
        registerSimple(BlockEntityTypeIds::VAULT, "vault", BlockID::Vault,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<VaultBlockEntity>(t, pos, id);
            });

        // ── Archaeology ───────────────────────────────────────────────────
        // MC BlockEntityTypes.BRUSHABLE_BLOCK ("minecraft:brushable_block"),
        // valid for both suspicious blocks: the buried find
        // (BrushableBlockEntity). Generated ones carry the structure's
        // archaeology loot table (AttachGeneratedBlockEntities).
        {
            std::unordered_set<BlockID> blocks = { BlockID::SuspiciousSand, BlockID::SuspiciousGravel };
            const auto* type = RegisterType(
                BlockEntityTypeIds::BRUSHABLE_BLOCK, "brushable_block",
                [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                    return std::make_unique<BrushableBlockEntity>(t, pos, id);
                },
                blocks);
            s_byId[BlockEntityTypeIds::BRUSHABLE_BLOCK] = type;
            g_byStringId[type->StringId()] = type;
            for (BlockID b : blocks) {
                const auto idx = static_cast<size_t>(b);
                if (idx < s_byBlockId.size()) s_byBlockId[idx] = type;
            }
        }
        // MC BlockEntityTypes.DECORATED_POT ("minecraft:decorated_pot"): the
        // pot's sherds and the one stack it holds, rolled from a loot table
        // in the trial chambers (DecoratedPotBlockEntity).
        registerSimple(BlockEntityTypeIds::DECORATED_POT, "decorated_pot", BlockID::DecoratedPot,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<DecoratedPotBlockEntity>(t, pos, id);
            });

        // MC BlockEntityTypes.BANNER ("minecraft:banner"): the pattern layers
        // and name of every standing and wall banner (BannerBlockEntity).
        // BELL ("minecraft:bell"): the swing and the raid alarm
        // (BellBlockEntity). COPPER_GOLEM_STATUE: the statue's golem name.
        auto registerMany = [](uint16_t typeId, const char* stringId,
                               const std::unordered_set<BlockID>& blocks,
                               BlockEntityType::Factory factory) {
            if (blocks.empty()) return;
            const auto* type = RegisterType(typeId, stringId, std::move(factory), blocks);
            s_byId[typeId] = type;
            g_byStringId[type->StringId()] = type;
            for (BlockID b : blocks) {
                const auto idx = static_cast<size_t>(b);
                if (idx < s_byBlockId.size()) s_byBlockId[idx] = type;
            }
        };
        {
            std::unordered_set<BlockID> banners;
            for (int i = 1; i < static_cast<int>(BlockID::Count); ++i) {
                const std::string& slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
                constexpr std::string_view kSuffix = "_banner";
                if (slug.size() > kSuffix.size() &&
                    std::string_view(slug).substr(slug.size() - kSuffix.size()) == kSuffix) {
                    banners.insert(static_cast<BlockID>(i));
                }
            }
            registerMany(BlockEntityTypeIds::BANNER, "banner", banners,
                [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                    return std::make_unique<BannerBlockEntity>(t, pos, id);
                });
        }
        registerMany(BlockEntityTypeIds::BELL, "bell", { BlockID::Bell },
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<BellBlockEntity>(t, pos, id);
            });
        registerMany(BlockEntityTypeIds::COPPER_GOLEM_STATUE, "copper_golem_statue",
            { BlockID::CopperGolemStatue, BlockID::ExposedCopperGolemStatue,
              BlockID::WeatheredCopperGolemStatue, BlockID::OxidizedCopperGolemStatue,
              BlockID::WaxedCopperGolemStatue, BlockID::WaxedExposedCopperGolemStatue,
              BlockID::WaxedWeatheredCopperGolemStatue, BlockID::WaxedOxidizedCopperGolemStatue },
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<CopperGolemStatueBlockEntity>(t, pos, id);
            });

        registerMany(BlockEntityTypeIds::CHISELED_BOOKSHELF, "chiseled_bookshelf", { BlockID::ChiseledBookshelf },
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<ChiseledBookShelfBlockEntity>(t, pos, id);
            });
        registerMany(BlockEntityTypeIds::BEEHIVE, "beehive", { BlockID::BeeNest, BlockID::Beehive },
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<BeehiveBlockEntity>(t, pos, id);
            });

        // ── Potent sulfur ─────────────────────────────────────────────────
        // MC BlockEntityTypes.POTENT_SULFUR ("minecraft:potent_sulfur"): the
        // geyser (PotentSulfurBlockEntity) — nausea over the pool, the
        // dormant/erupting countdown, the plume and the launch.
        registerSimple(BlockEntityTypeIds::POTENT_SULFUR, "potent_sulfur", BlockID::PotentSulfur,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<PotentSulfurBlockEntity>(t, pos, id);
            });

        // ── The sculk family ──────────────────────────────────────────────
        // MC BlockEntityTypes.SCULK_SENSOR / CALIBRATED_SCULK_SENSOR /
        // SCULK_CATALYST / SCULK_SHRIEKER — the vibration listeners and the
        // catalyst's spreader (SculkBlockEntities.hpp).
        registerSimple(BlockEntityTypeIds::SCULK_SENSOR, "sculk_sensor", BlockID::SculkSensor,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<SculkSensorBlockEntity>(t, pos, id);
            });
        registerSimple(BlockEntityTypeIds::CALIBRATED_SCULK_SENSOR, "calibrated_sculk_sensor",
                       BlockID::CalibratedSculkSensor,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<CalibratedSculkSensorBlockEntity>(t, pos, id);
            });
        registerSimple(BlockEntityTypeIds::SCULK_CATALYST, "sculk_catalyst", BlockID::SculkCatalyst,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<SculkCatalystBlockEntity>(t, pos, id);
            });
        registerSimple(BlockEntityTypeIds::SCULK_SHRIEKER, "sculk_shrieker", BlockID::SculkShrieker,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<SculkShriekerBlockEntity>(t, pos, id);
            });

        // ── The Hush lighthouse lamp ──────────────────────────────────────
        // The beam's sweep is a pure function of the level's game time and
        // the lamp's position (HushLighthouseRenderer), so every client sees
        // the same sweep; the entity only carries where the nearest Aurelith
        // is, filled in once by the server (HushLighthouseLampBlockEntity).
        // Generated lamps get theirs from the Hush Lighthouse template's {id}
        // nbt (AttachGeneratedBlockEntities), unchecked.
        registerSimple(BlockEntityTypeIds::HUSH_LIGHTHOUSE_LAMP, "hush_lighthouse_lamp",
                       BlockID::HushLighthouseLamp,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                // The sweep needs nothing saved; the lamp's one datum is the
                // nearest Aurelith it points travellers to (Lighthouses that
                // guide — HushLighthouseLampBlockEntity, LighthouseGuide).
                return std::make_unique<HushLighthouseLampBlockEntity>(t, pos, id);
            });

        // ── Aurelith: the Heart and the gate beacons ─────────────────────
        // Plain BlockEntities for the same reason as the lamp: nothing to
        // save or sync, the renderers derive everything from game time and
        // the block. Generated ones come from the Aurelith templates' {id}
        // nbt (AttachGeneratedBlockEntities).
        // The engine records the city's rotation (ResonanceEngineBlockEntity:
        // the quest finds every landmark from the Heart through it).
        registerSimple(BlockEntityTypeIds::RESONANCE_ENGINE, "resonance_engine",
                       BlockID::ResonanceEngine,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<ResonanceEngineBlockEntity>(t, pos, id);
            });
        registerSimple(BlockEntityTypeIds::VOICE_BEACON, "voice_beacon", BlockID::VoiceBeacon,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<BlockEntity>(t, pos, id);
            });

        // ── Aurelith: reawakening the Heart ──────────────────────────────
        // The Podium's sockets, the pedestals and the Hall of Instruments'
        // cabinet (AurelithBlockEntities.hpp). Generated ones come from the
        // templates' nbt (TemplateEngine::blockEntityPayloadFor →
        // AttachGeneratedBlockEntities).
        registerSimple(BlockEntityTypeIds::CHORD_SOCKET, "chord_socket", BlockID::ChordSocket,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<ChordSocketBlockEntity>(t, pos, id);
            });
        registerSimple(BlockEntityTypeIds::VOICE_PEDESTAL, "voice_pedestal", BlockID::VoicePedestal,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<VoicePedestalBlockEntity>(t, pos, id);
            });
        registerSimple(BlockEntityTypeIds::CHOIR_CABINET, "choir_cabinet", BlockID::ChoirCabinet,
            [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                return std::make_unique<ChoirCabinetBlockEntity>(t, pos, id);
            });

        // ── Engine: shared crafting tables ────────────────────────────────
        // The stored grid of a crafting table under the shared_crafting_tables
        // rule (CraftingTableBlockEntity). Valid for the crafting table (the
        // Anvil loader's IsValidFor check) but LAZY: kept out of s_byBlockId
        // so SetBlock never creates one and nothing that asks HasBlockEntity
        // (the mesher, pistons, placement) sees a vanilla table differently.
        {
            const auto* type = RegisterType(
                BlockEntityTypeIds::OBEY_CRAFTING_TABLE, "obeycraft:crafting_table",
                [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                    return std::make_unique<CraftingTableBlockEntity>(t, pos, id);
                },
                { BlockID::CraftingTable });
            s_byId[BlockEntityTypeIds::OBEY_CRAFTING_TABLE] = type;
            g_byStringId[type->StringId()] = type;
            g_lazyByBlockId.assign(static_cast<size_t>(BlockID::Count), nullptr);
            g_lazyByBlockId[static_cast<size_t>(BlockID::CraftingTable)] = type;
        }

        // ── Engine: sky faces ─────────────────────────────────────────────
        // A door's faces opened onto the sky (SkyFacesBlockEntity). Valid
        // for every door and LAZY for the crafting table's reasons: a door
        // carries one only while a face of it shows the sky.
        {
            std::unordered_set<BlockID> doors;
            for (size_t i = 0; i < static_cast<size_t>(BlockID::Count); ++i) {
                if (SkyFaces::AppliesTo(static_cast<BlockID>(i))) doors.insert(static_cast<BlockID>(i));
            }
            const auto* type = RegisterType(
                BlockEntityTypeIds::OBEY_SKY_FACES, "obeycraft:sky_faces",
                [](const BlockEntityType* t, glm::ivec3 pos, BlockID id) {
                    return std::make_unique<SkyFacesBlockEntity>(t, pos, id);
                },
                doors);
            s_byId[BlockEntityTypeIds::OBEY_SKY_FACES] = type;
            g_byStringId[type->StringId()] = type;
            for (BlockID door : doors) g_lazyByBlockId[static_cast<size_t>(door)] = type;
        }

        Log::Info("[BlockEntityTypes] initialised with %zu type(s)", g_typeStorage.size());
    }

    // MC BlockEntity.setChanged → Level.blockEntityChanged: mark for saving
    // and, when the block has an analog output, tell the comparators reading
    // it. Menus never have to remember to do this — Slot calls SetChanged
    // through the container on every mutation.
    void BaseContainerBlockEntity::SetChanged() {
        MarkDirty();
        if (ILevelWrite* level = GetLevel()) {
            if (BlockRegistry::Get(GetBlockId()).hasAnalogOutputSignal) {
                level->UpdateNeighbourForOutputSignal(GetWorldPos(), GetBlockId());
            }
        }
    }

    void BaseContainerBlockEntity::PreRemoveSideEffects(ILevelWrite& level, const glm::ivec3& pos,
                                                        BlockState /*oldState*/) {
        if (level.IsClientSide()) return;
        if (const BlockEntityType* type = GetType()) {
            const std::string& id = type->StringId();
            if (id == "shulker_box" || id == "ender_chest") return;
        }
        if (!GetLevel()) SetLevel(&level);
        // Containers.dropContents reads each slot through getItem, which
        // unpacks: an unopened structure chest spills its rolled loot.
        for (ItemStack& stack : TakeAllContents()) {
            DropContainerItemStack(level, glm::dvec3(pos), std::move(stack));
        }
    }

    void BaseContainerBlockEntity::ApplyItemComponents(const DataComponentMap& components) {
        if (auto name = components.get(DataComponents::CUSTOM_NAME)) m_customName = *name;
        // LOCK — BaseContainerBlockEntity.applyImplicitComponents.
        if (auto lock = components.get(DataComponents::LOCK)) m_lock = lock->predicate;
        // CONTAINER_LOOT — RandomizableContainerBlockEntity.applyImplicit-
        // Components: the table (and seed) rolled when first opened.
        if (auto loot = components.get(DataComponents::CONTAINER_LOOT)) {
            SetLootTable(loot->lootTable, loot->seed);
            SetChanged();
        }
        if (auto contents = components.get(DataComponents::CONTAINER)) {
            // ItemContainerContents.copyInto: slot by slot, the rest emptied.
            for (size_t i = 0; i < m_items.size(); ++i) {
                m_items[i] = i < contents->items.size() ? contents->items[i] : ItemStack{};
            }
            SetChanged();
        }
    }

    bool BaseContainerBlockEntity::CanUnlockWith(const ItemStack& held) const {
        return m_lock.IsEmpty() || BlockData::LockUnlocksWith(LockCode{m_lock}, held);
    }

    void BaseContainerBlockEntity::CollectComponents(DataComponentMap& out) const {
        if (!m_customName.empty()) out.set(DataComponents::CUSTOM_NAME, m_customName);
        if (!m_lock.IsEmpty()) out.set(DataComponents::LOCK, LockCode{m_lock});
        if (HasLootTable()) out.set(DataComponents::CONTAINER_LOOT, SeededContainerLoot{m_lootTable, m_lootTableSeed});
        // ItemContainerContents.fromItems: trailing empties dropped; nothing
        // at all for an empty container (its patch would equal the default).
        ItemContainerContents contents;
        size_t last = 0;
        for (size_t i = 0; i < m_items.size(); ++i) if (!m_items[i].IsEmpty()) last = i + 1;
        if (last == 0) return;
        contents.items.assign(m_items.begin(), m_items.begin() + static_cast<std::ptrdiff_t>(last));
        out.set(DataComponents::CONTAINER, std::move(contents));
    }

    // MC RandomizableContainer.unpackLootTable. MC bails without a level (it
    // needs the server's loot registry); here the table is read from the data
    // pack, so a container the chunk loader has not yet handed a level to
    // (the tick walker installs it later) still rolls — with the seed, or a
    // time-seeded random when there is neither seed nor level.
    void BaseContainerBlockEntity::UnpackLootTable(float luck) {
        if (m_lootTable.empty()) return;
        ILevelWrite* level = GetLevel();
        if (level && level->IsClientSide()) return;   // the server rolls; the client is told
        const std::string key = std::move(m_lootTable);
        m_lootTable.clear();                            // re-entrancy guard, before fill
        const int64_t seed = m_lootTableSeed;
        m_lootTableSeed = 0;
        // LootParams: ORIGIN = Vec3.atCenterOf(blockPos) in this level (what
        // exploration_map and location_check read). No level, no origin.
        ChestLoot::LootLevelContext lootLevel;
        if (level) {
            lootLevel.dimensionId = DimensionToRaw(level->GetDimension());
            lootLevel.origin = glm::dvec3(GetWorldPos()) + glm::dvec3(0.5);
        }
        if (!ChestLoot::Fill(*this, key, seed, level ? level->Random() : nullptr, luck,
                             level ? &lootLevel : nullptr)) {
            Log::Warning("[BlockEntity] %s at (%d,%d,%d) named loot table '%s', which does not exist",
                         GetType() ? std::string(GetType()->StringId()).c_str() : "container",
                         GetWorldPos().x, GetWorldPos().y, GetWorldPos().z, key.c_str());
        }
        MarkDirty();
    }

    const BlockEntityType* BlockEntityTypes::LazyForBlock(BlockID id) {
        const auto idx = static_cast<size_t>(id);
        return (idx < g_lazyByBlockId.size()) ? g_lazyByBlockId[idx] : nullptr;
    }

    bool BlockEntityTypes::MayHaveBlockEntity(BlockID id) {
        return HasBlockEntity(id) || LazyForBlock(id) != nullptr;
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
