// File: src/common/world/block/BlockCloneItem.cpp
#include "common/world/block/BlockCloneItem.hpp"

#include "common/data/DataComponentMap.hpp"
#include "common/data/DataComponents.hpp"
#include "common/data/components/BlockDataComponents.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/FlowerPotBlock.hpp"
#include "common/world/block/entity/BannerBlockEntity.hpp"
#include "common/world/block/entity/CopperGolemStatueBlockEntity.hpp"
#include "common/world/block/entity/DecoratedPotBlockEntity.hpp"
#include "common/world/level/ILevelWrite.hpp"

#include <array>
#include <string>
#include <string_view>

namespace Game {

    namespace {

        bool EndsWith(std::string_view s, std::string_view suffix) {
            return s.size() >= suffix.size() &&
                   s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        // MC StandingAndWallBlockItem.registerBlocks: the torch, sign,
        // hanging-sign, banner, skull/head and coral-fan items register their
        // wall block too. Every one of those wall blocks is named
        // "<standing>" with "wall_" spliced in before its last word(s)
        // (wall_torch, oak_wall_sign, oak_wall_hanging_sign, white_wall_banner,
        // skeleton_wall_skull, zombie_wall_head, tube_coral_wall_fan) — the
        // engine's and the mods' wall torches follow the same rule. Air when
        // `slug` is not such a wall block or its standing twin is unknown.
        BlockID StandingTwinOf(std::string_view slug) {
            static constexpr std::array<std::string_view, 7> kWallSuffixes = {
                "wall_torch", "wall_sign", "wall_hanging_sign", "wall_banner",
                "wall_skull", "wall_head", "wall_fan",
            };
            for (std::string_view suffix : kWallSuffixes) {
                if (!EndsWith(slug, suffix)) continue;
                const size_t at = slug.size() - suffix.size();
                // Bare "wall_torch", or "<prefix>_wall_<...>".
                if (at != 0 && slug[at - 1] != '_' && slug[at - 1] != ':') continue;
                std::string standing(slug.substr(0, at));
                standing += suffix.substr(5);   // drop "wall_"
                return BlockStates::FromSlug(standing).Block();
            }
            return BlockID::Air;
        }

        // MC's blocks that no item is registered for (asItem() == AIR) and
        // whose getCloneItemStack does not name one either way. The ones with
        // an override (stems, kelp plant, piston head, ...) are listed too:
        // their own asItem is AIR in MC, the override answers for the pick.
        bool HasNoItem(BlockID id) {
            switch (id) {
                case BlockID::Water:
                case BlockID::Lava:
                case BlockID::BubbleColumn:
                case BlockID::Fire:
                case BlockID::SoulFire:
                case BlockID::NetherPortal:
                case BlockID::EndPortal:
                case BlockID::EndGateway:
                case BlockID::FrostedIce:
                case BlockID::MovingPiston:
                case BlockID::PistonHead:
                case BlockID::AttachedMelonStem:
                case BlockID::AttachedPumpkinStem:
                case BlockID::TallSeagrass:
                case BlockID::KelpPlant:
                case BlockID::CaveVinesPlant:
                case BlockID::TwistingVinesPlant:
                case BlockID::WeepingVinesPlant:
                case BlockID::BambooSapling:
                case BlockID::BigDripleafStem:
                // The engine's and the mods' portals, as the vanilla ones.
                case BlockID::HushPortal:
                case BlockID::TwilightPortal:
                case BlockID::AetherPortal:
                    return true;
                default:
                    return false;
            }
        }

        bool IsCandleCake(BlockID id) {
            return EndsWith(BlockRegistry::Get(id).registrySlug, "candle_cake");
        }

        struct AsItemTable {
            std::array<ItemID, static_cast<size_t>(BlockID::Count)> items{};

            AsItemTable() {
                // Every block starts as its own block item (the engine's
                // ItemID == BlockID convention covers MC's plain BlockItems).
                for (size_t i = 0; i < items.size(); ++i) {
                    items[i] = ItemRegistry::FromBlock(static_cast<BlockID>(i));
                }
                items[0] = Items::Air;

                for (size_t i = 1; i < items.size(); ++i) {
                    const BlockID id = static_cast<BlockID>(i);
                    if (HasNoItem(id) || IsCandleCake(id) || FlowerPot::ContentOf(id) != BlockID::Air) {
                        items[i] = Items::Air;
                        continue;
                    }
                    const BlockID standing = StandingTwinOf(BlockRegistry::Get(id).registrySlug);
                    if (standing != BlockID::Air && standing != id) {
                        items[i] = ItemRegistry::FromBlock(standing);
                    }
                }

                // MC Items.CAULDRON = registerBlock(CAULDRON, WATER_CAULDRON,
                // LAVA_CAULDRON, POWDER_SNOW_CAULDRON).
                const ItemID cauldron = ItemRegistry::FromBlock(BlockID::Cauldron);
                items[static_cast<size_t>(BlockID::WaterCauldron)]      = cauldron;
                items[static_cast<size_t>(BlockID::LavaCauldron)]       = cauldron;
                items[static_cast<size_t>(BlockID::PowderSnowCauldron)] = cauldron;

                // The renamed BlockItems (MC createBlockItemWithCustomItemName,
                // BlockItemIds' two-name entries): every pure item that places
                // a block is that block's item — wheat_seeds for wheat,
                // redstone for redstone_wire, string for tripwire, …
                ItemRegistry::ForEachPureItem([&](ItemID itemId, const Item& item) {
                    if (item.placesBlock != BlockID::Air) {
                        items[static_cast<size_t>(item.placesBlock)] = itemId;
                    }
                });
                // Two renamed BlockItems whose placement the engine runs
                // through its own use code instead of placesBlock.
                items[static_cast<size_t>(BlockID::CaveVines)]  = Items::GlowBerries;
                items[static_cast<size_t>(BlockID::PowderSnow)] = Items::PowderSnowBucket;
            }
        };

        ItemStack Stack(ItemID id) {
            return id == Items::Air ? ItemStack{} : ItemStack(id, 1);
        }

        // MC BlockItemStateProperties.EMPTY.with(property, state's value).
        void SetStateProperty(ItemStack& stack, BlockState state, std::string_view property) {
            const std::string_view value = state.GetValueByName(property);
            if (value.empty()) return;
            stack.components.set(DataComponents::BLOCK_STATE,
                                 BlockItemStateProperties{}.With(property, value));
        }

    } // namespace

    ItemID BlockAsItem(BlockID block) {
        static const AsItemTable table;
        const size_t i = static_cast<size_t>(block);
        return i < table.items.size() ? table.items[i] : Items::Air;
    }

    ItemStack GetCloneItemStack(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
        const BlockID id = state.Block();
        switch (id) {
            case BlockID::Air:
            // MovingPistonBlock / NetherPortalBlock / EndPortalBlock /
            // EndGatewayBlock / FrostedIceBlock: ItemStack.EMPTY.
            case BlockID::MovingPiston:
            case BlockID::NetherPortal:
            case BlockID::EndPortal:
            case BlockID::EndGateway:
            case BlockID::FrostedIce:
                return {};

            // PistonHeadBlock: the base of the head's TYPE.
            case BlockID::PistonHead:
                return Stack(ItemRegistry::FromBlock(
                    state.HasProperty(PropertyId::PISTON_TYPE) &&
                            state.GetIndex(PropertyId::PISTON_TYPE) == 1
                        ? BlockID::StickyPiston : BlockID::Piston));

            // StemBlock / AttachedStemBlock: the stem's seed item.
            case BlockID::PumpkinStem:
            case BlockID::AttachedPumpkinStem: return Stack(Items::PumpkinSeeds);
            case BlockID::MelonStem:
            case BlockID::AttachedMelonStem:   return Stack(Items::MelonSeeds);

            // CropBlock.getBaseSeedId (and TorchflowerCropBlock's override).
            case BlockID::Wheat:           return Stack(Items::WheatSeeds);
            case BlockID::Carrots:         return Stack(Items::Carrot);
            case BlockID::Potatoes:        return Stack(Items::Potato);
            case BlockID::Beetroots:       return Stack(Items::BeetrootSeeds);
            case BlockID::TorchflowerCrop: return Stack(Items::TorchflowerSeeds);

            case BlockID::NetherWart:      return Stack(Items::NetherWart);        // NetherWartBlock
            case BlockID::SweetBerryBush:  return Stack(Items::SweetBerries);      // SweetBerryBushBlock
            case BlockID::CaveVines:
            case BlockID::CaveVinesPlant:  return Stack(Items::GlowBerries);       // CaveVines(Plant)Block
            case BlockID::BambooSapling:   return Stack(ItemRegistry::FromBlock(BlockID::Bamboo));       // BambooSaplingBlock
            case BlockID::BigDripleafStem: return Stack(ItemRegistry::FromBlock(BlockID::BigDripleaf));  // BigDripleafStemBlock
            case BlockID::TallSeagrass:    return Stack(ItemRegistry::FromBlock(BlockID::Seagrass));     // TallSeagrassBlock

            // GrowingPlantBodyBlock: the head block's item.
            case BlockID::KelpPlant:          return Stack(BlockAsItem(BlockID::Kelp));
            case BlockID::TwistingVinesPlant: return Stack(BlockAsItem(BlockID::TwistingVines));
            case BlockID::WeepingVinesPlant:  return Stack(BlockAsItem(BlockID::WeepingVines));

            // LightBlock.setLightOnStack / TestBlock.setModeOnStack.
            case BlockID::Light: {
                ItemStack stack = Stack(BlockAsItem(id));
                SetStateProperty(stack, state, "level");
                return stack;
            }
            case BlockID::TestBlock: {
                ItemStack stack = Stack(BlockAsItem(id));
                SetStateProperty(stack, state, "mode");
                return stack;
            }
            default:
                break;
        }

        // CandleCakeBlock: the cake, whatever candle sits on it.
        if (IsCandleCake(id)) return Stack(ItemRegistry::FromBlock(BlockID::Cake));

        // FlowerPotBlock: the plant of a potted block, the pot when empty.
        if (const BlockID plant = FlowerPot::ContentOf(id); plant != BlockID::Air) {
            return Stack(BlockAsItem(plant));
        }

        ItemStack stack = Stack(BlockAsItem(id));
        if (stack.IsEmpty()) return stack;

        // The block-entity driven overrides.
        if (BlockEntity* be = level.GetBlockEntity(pos)) {
            if (dynamic_cast<BannerBlockEntity*>(be)) {
                // AbstractBannerBlock -> BannerBlockEntity.getItem: the
                // banner of its colour with the entity's components
                // (patterns, custom name, the ominous item_name/rarity).
                be->CollectComponents(stack.components);
            } else if (dynamic_cast<DecoratedPotBlockEntity*>(be)) {
                // DecoratedPotBlock -> createDecoratedPotInstance: the pot
                // with its sherds, never its contents.
                DataComponentMap collected;
                be->CollectComponents(collected);
                stack.components.CopyNamed(collected, "pot_decorations");
            } else if (dynamic_cast<CopperGolemStatueBlockEntity*>(be)) {
                // CopperGolemStatueBlockEntity.getItem: the entity's
                // components plus the statue's pose as BLOCK_STATE.
                be->CollectComponents(stack.components);
                SetStateProperty(stack, state, "copper_golem_pose");
            }
        }
        return stack;
    }

} // namespace Game
