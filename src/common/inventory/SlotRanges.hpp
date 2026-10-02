// File: src/common/inventory/SlotRanges.hpp
//
// MC net.minecraft.world.inventory.SlotRanges — the named slot sets
// `/item`'s SlotSourceArgument (and the `slots` predicates) read: a name
// ("container.5", "armor.*", "weapon.offhand") and the MC slot ids it
// covers. The ids are vanilla's SlotAccess indices (Entity.getSlot): 0-53
// a container's / the player's inventory, 98/99 the hands, 100-103 the
// humanoid armour (feet..head), 105 the body, 106 the saddle, 200+ the
// ender chest, 300+ a mob's inventory, 499 the cursor or a horse's chest
// flag, 500+ the player's crafting grid or a horse's inventory.
//
// Shared by the server (resolving a range) and the client (completing one).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace Game::SlotRanges {

    struct Range {
        std::string      name;
        std::vector<int> ids;
    };

    inline constexpr int kMainHand = 98;
    inline constexpr int kOffHand  = 99;
    inline constexpr int kFeet     = 100;
    inline constexpr int kLegs     = 101;
    inline constexpr int kChest    = 102;
    inline constexpr int kHead     = 103;
    inline constexpr int kBody     = 105;
    inline constexpr int kSaddle   = 106;
    inline constexpr int kEnderChestOffset   = 200;
    inline constexpr int kMobInventoryOffset = 300;
    inline constexpr int kCursorOrHorseChest = 499;
    inline constexpr int kCraftingOrHorse    = 500;

    // SlotRanges.SLOTS, in MC's registration order.
    inline const std::vector<Range>& All() {
        static const std::vector<Range> slots = [] {
            std::vector<Range> out;
            const auto single = [&out](std::string name, int id) { out.push_back({std::move(name), {id}}); };
            const auto range = [&out](const std::string& prefix, int offset, int size) {
                std::vector<int> all;
                for (int i = 0; i < size; ++i) {
                    out.push_back({prefix + std::to_string(i), {offset + i}});
                    all.push_back(offset + i);
                }
                out.push_back({prefix + "*", all});
            };
            single("contents", 0);
            range("container.", 0, 54);
            range("hotbar.", 0, 9);
            range("inventory.", 9, 27);
            range("enderchest.", kEnderChestOffset, 27);
            range("mob.inventory.", kMobInventoryOffset, 8);
            range("horse.", kCraftingOrHorse, 15);
            single("weapon", kMainHand);
            single("weapon.mainhand", kMainHand);
            single("weapon.offhand", kOffHand);
            out.push_back({"weapon.*", {kMainHand, kOffHand}});
            single("armor.head", kHead);
            single("armor.chest", kChest);
            single("armor.legs", kLegs);
            single("armor.feet", kFeet);
            single("armor.body", kBody);
            out.push_back({"armor.*", {kHead, kChest, kLegs, kFeet, kBody}});
            single("saddle", kSaddle);
            single("horse.chest", kCursorOrHorseChest);
            single("player.cursor", kCursorOrHorseChest);
            range("player.crafting.", kCraftingOrHorse, 4);
            return out;
        }();
        return slots;
    }

    // SlotRanges.nameToIds; null for an unknown name.
    inline const Range* Find(std::string_view name) {
        for (const Range& r : All()) {
            if (r.name == name) return &r;
        }
        return nullptr;
    }

} // namespace Game::SlotRanges
