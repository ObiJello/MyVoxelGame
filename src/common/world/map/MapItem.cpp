// File: src/common/world/map/MapItem.cpp
#include "MapItem.hpp"

#include "common/data/DataComponents.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/tags/DataTags.hpp"

#include <atomic>

namespace Game {

    namespace {

        // Items.java: every registerItem(..., MapItem::new, mapProperties()).
        constexpr ItemID kMapItems[] = {
            Items::FilledMap,
            Items::OceanMonumentMap, Items::WoodlandMansionMap, Items::BuriedTrialChambersMap,
            Items::JunglePyramidMap, Items::SwampHutMap, Items::DesertVillageMap,
            Items::PlainsVillageMap, Items::SavannaVillageMap, Items::SnowyVillageMap,
            Items::TaigaVillageMap, Items::BuriedTreasureMap, Items::BuriedAncientCityMap,
            Items::BuriedMineshaftMap, Items::DesertPyramidMap, Items::AbandonedCampMap,
            Items::WarmOceanRuinsMap,
        };

        // EmptyMapItem.use.
        UseResult Use_EmptyMap(ILevelWrite* world, IUsePlayer* player, uint32_t hand, ItemStack& stack) {
            if (!world || !player) return UseResult::Pass;
            // The client swings and waits for the server's inventory update:
            // the map id only exists on the server.
            if (world->IsClientSide()) return UseResult::Success;

            ItemStack map = MapItemBridge::CreateMapAt(player->getDimensionId(), player->getPosition());
            if (map.IsEmpty()) return UseResult::Pass;

            // itemStack.consume(1, player): creative keeps it.
            if (!player->isCreative()) {
                stack.count -= 1;
                if (stack.count <= 0) stack.Clear();
            }
            // serverLevel.playSound(null, player, UI_CARTOGRAPHY_TABLE_TAKE_RESULT,
            // player.getSoundSource(), 1, 1) — to everyone, the user included.
            world->PlaySound(nullptr, player->getPosition(), SoundEvents::UI_CARTOGRAPHY_TABLE_TAKE_RESULT,
                             SoundSource::Players, 1.0f, 1.0f);

            if (stack.IsEmpty()) {
                // InteractionResult.SUCCESS.heldItemTransformedTo(map).
                stack = map;
            } else {
                // if (!player.getInventory().add(map.copy())) player.drop(map, false).
                MapItemBridge::GiveOrDrop(*player, map);
            }
            player->markSlotDirty(player->handSlotIndex(hand));
            return UseResult::Success;
        }

        // MapItem.useOn: a banner toggles its marker; anything else passes.
        UseResult UseOn_Map(const UseOnContext& ctx, ItemStack& stack) {
            if (!ctx.world) return UseResult::Pass;
            const glm::ivec3 pos = ctx.hitResult.blockPos;
            const BlockState clicked = ctx.world->GetBlockState(pos.x, pos.y, pos.z);
            const std::string_view slug = BlockRegistry::Get(clicked.Block()).registrySlug;
            if (slug.empty() || !DataTags::HasTag(DataTags::Registry::Block, slug, "minecraft:banners")) {
                return UseResult::Pass;
            }
            if (!ctx.world->IsClientSide() && ctx.player) {
                if (MapItem::GetMapId(stack) &&
                    !MapItemBridge::ToggleBanner(ctx.player->getDimensionId(), stack, pos)) {
                    return UseResult::Fail;
                }
            }
            return UseResult::Success;
        }

        std::atomic<MapItemBridge::ClientMapInfoFn> g_clientMapInfo{nullptr};
    }

    bool MapItem::IsMapItem(ItemID id) {
        for (const ItemID m : kMapItems) {
            if (m == id) return true;
        }
        return false;
    }

    std::optional<int32_t> MapItem::GetMapId(const ItemStack& stack) {
        if (stack.IsEmpty()) return std::nullopt;
        return stack.get(DataComponents::MAP_ID);
    }

    void MapItem::RegisterBehaviors(std::unordered_map<ItemID, Item>& pureItems) {
        if (auto it = pureItems.find(Items::Map); it != pureItems.end()) it->second.use = &Use_EmptyMap;
        for (const ItemID id : kMapItems) {
            if (auto it = pureItems.find(id); it != pureItems.end()) it->second.useOn = &UseOn_Map;
        }
    }

    void MapItemBridge::SetClientMapInfoSource(ClientMapInfoFn fn) { g_clientMapInfo.store(fn); }
    MapItemBridge::ClientMapInfoFn MapItemBridge::detail::ClientMapInfoSource() { return g_clientMapInfo.load(); }

} // namespace Game
