// File: src/common/entity/npc/VillagerTrades.hpp
//
// MC 26.3's DATA-DRIVEN trades — net.minecraft.world.item.trading.{TradeSet,
// VillagerTrade, TradeCost} over the shipped data pack:
//
//   data/minecraft/trade_set/<profession>/level_<n>.json
//       { amount, trades: "#minecraft:<profession>/level_<n>",
//         allow_duplicates?, random_sequence? }
//   data/minecraft/tags/villager_trade/<profession>/level_<n>.json
//       { values: [ "minecraft:<profession>/<n>/<trade>", ... ] }
//   data/minecraft/villager_trade/<profession>/<n>/<trade>.json
//       { wants, additional_wants?, gives, max_uses?, xp?,
//         reputation_discount?, merchant_predicate?,
//         given_item_modifiers?, double_trade_price_enchantments? }
//
// The 1.14-era hard-coded VillagerTrades tables are gone in 26.x; this reads
// the same files MC's registries are built from, on first use, and caches
// them. A data pack that edits a trade therefore edits it here too.
//
// Supported item modifiers are every function the vanilla trades use:
// enchant_randomly, enchant_with_levels, filtered (+discard), set_name,
// set_potion, set_random_potion, set_stew_effect, set_random_dyes, and
// exploration_map — which, with no filled-map system to write into, leaves the
// blank map, so the trade's own "filtered: map_id" guard discards the offer
// exactly as MC does when no structure is found. Enchanted GEAR has no
// ENCHANTMENTS component in this engine yet (only books store
// enchantments); those trades' "filtered: enchantments" guard discards them
// the same way. Both are named in the villager docs as the gaps they are.
#pragma once

#include "common/entity/npc/MerchantOffer.hpp"
#include "common/entity/npc/VillagerData.hpp"

#include <string>

namespace Game {

    class JavaRandom;

    namespace VillagerTrades {

        // MC AbstractVillager.addOffersFromTradeSet: roll the set's amount and
        // draw that many offers from its trades (without duplicates unless
        // the set allows them), each through VillagerTrade.getOffer — which
        // may decline (its merchant_predicate fails, a filter discards the
        // item, a cost rounds to zero), in which case another is drawn.
        //
        // `merchantType` answers the trades' `minecraft:villager/variant`
        // predicate (the cartographer's biome maps); the wandering trader has
        // none, so pass nullopt and any such predicate fails, as it would on
        // a non-villager in MC.
        void AddOffersFromTradeSet(const std::string& tradeSetKey, MerchantOffers& offers,
                                   JavaRandom& random, std::optional<VillagerType> merchantType);

        // True when data/<ns>/trade_set/<path>.json exists and parsed.
        bool TradeSetExists(const std::string& tradeSetKey);

        // Forget every parsed file; the next roll re-reads the data pack.
        void Reload();

    } // namespace VillagerTrades
} // namespace Game
