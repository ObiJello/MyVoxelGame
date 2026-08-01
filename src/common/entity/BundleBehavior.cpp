// File: src/common/entity/BundleBehavior.cpp
//
// Bundle click-to-insert / click-to-extract. Mirrors BundleItem.java's
// overrideStackedOnOther (:50-85) / overrideOtherStackedOnMe (:87-125) and
// the BundleContents.Mutable insert/remove math (BundleContents.java:145-230).
//
// COW rule: DataComponentMap values are shared across stack copies, so the
// contents are ALWAYS get → copy → mutate → set (matching MC's
// Mutable → toImmutable → set flow, BundleItem.java:56-64) — never mutate a
// fetched BundleContents in place.
//
// Weight is tracked in 64ths of a full bundle (MC uses exact Fractions with
// per-item weight 1/maxStackSize; 64ths represents 1/64, 1/16 and 1/1 stacks
// exactly). Full at 64 units. Bundle-in-bundle costs 4 units (= MC's 1/16,
// BundleContents.java:141) plus its own contents.
//
// NOT wired: BundleItem.use's hold-to-dump (BundleItem.java:127-130, :211-219)
// — dumping spawns ItemEntities, which don't exist. The click ops are the
// full in-inventory behaviour.
#include "Item.hpp"
#include "GeneratedItemList.hpp"
#include "Inventory.hpp"
#include "../data/DataComponents.hpp"
#include "../core/Log.hpp"
#include "server/inventory/InventoryClickHandler.hpp"
#include "server/player/ServerPlayer.hpp"

#include <algorithm>
#include <unordered_map>

namespace Game {

    namespace {

        constexpr int kFullBundleUnits     = 64;  // Fraction.ONE
        constexpr int kBundleInBundleUnits = 4;   // 1/16 (BundleContents.java:141)

        int WeightUnitsOf(const ItemStack& stack);

        // Total weight of a contents list, in 64ths (BundleContents.computeContentWeight).
        int ContentsWeightUnits(const BundleContents& contents) {
            int units = 0;
            for (const auto& s : contents.items) {
                units += WeightUnitsOf(s) * s.count;
            }
            return units;
        }

        // Per-item weight — BundleContents.getWeight (:61-69): a nested
        // bundle costs 1/16 + its own weight; anything else 1/maxStackSize.
        // (The BEES branch is omitted — no bee data.)
        int WeightUnitsOf(const ItemStack& stack) {
            if (auto inner = stack.get(DataComponents::BUNDLE_CONTENTS)) {
                return kBundleInBundleUnits + ContentsWeightUnits(*inner);
            }
            const int maxStack = std::max(1, ItemRegistry::Get(stack.itemId).maxStackSize);
            return kFullBundleUnits / maxStack;
        }

        // BundleContents.canItemBeInBundle (:71-73).
        bool CanItemBeInBundle(const ItemStack& stack) {
            return !stack.IsEmpty()
                && ItemRegistry::Get(stack.itemId).canFitInsideContainerItems;
        }

        // Mutable.getMaxAmountToAdd (:176-179).
        int MaxAmountToAdd(const BundleContents& contents, const ItemStack& item) {
            const int remaining = kFullBundleUnits - ContentsWeightUnits(contents);
            const int perItem   = WeightUnitsOf(item);
            return perItem > 0 ? std::max(remaining / perItem, 0) : 0;
        }

        // Mutable.tryInsert (:181-203) — moves up to the weight-limited count
        // from `toAdd` into `contents`. Returns the amount moved.
        int TryInsert(BundleContents& contents, ItemStack& toAdd) {
            if (!CanItemBeInBundle(toAdd)) return 0;
            const int amount = std::min(toAdd.count, MaxAmountToAdd(contents, toAdd));
            if (amount == 0) return 0;

            // findStackIndex (:162-174) — merge into an existing same-item
            // stack (MC also requires same COMPONENTS; approximated by id +
            // empty patches, the common case — nested bundles never merge
            // because their weight differs anyway).
            int stackIndex = -1;
            if (ItemRegistry::Get(toAdd.itemId).maxStackSize > 1) {
                for (size_t i = 0; i < contents.items.size(); ++i) {
                    if (contents.items[i].itemId == toAdd.itemId
                        && contents.items[i].components.empty()
                        && toAdd.components.empty()) {
                        stackIndex = static_cast<int>(i);
                        break;
                    }
                }
            }
            if (stackIndex != -1) {
                ItemStack merged = contents.items[stackIndex];
                merged.count += amount;
                contents.items.erase(contents.items.begin() + stackIndex);
                contents.items.insert(contents.items.begin(), merged);  // newest first (:196)
            } else {
                ItemStack split = toAdd;                                 // :198 split
                split.count = amount;
                contents.items.insert(contents.items.begin(), split);
            }
            toAdd.count -= amount;
            if (toAdd.count <= 0) toAdd.Clear();
            return amount;
        }

        // Mutable.removeOne (:220-230) — pops the selected (or newest) stack.
        // Returns an empty stack when the bundle is empty.
        ItemStack RemoveOne(BundleContents& contents) {
            if (contents.items.empty()) return {};
            const bool selValid = contents.selectedItem >= 0
                && contents.selectedItem < static_cast<int>(contents.items.size());
            const int index = selValid ? contents.selectedItem : 0;
            ItemStack removed = contents.items[index];
            contents.items.erase(contents.items.begin() + index);
            contents.selectedItem = -1;
            return removed;
        }

        // ── The two click overrides ─────────────────────────────────────────

        // Mirrors BundleItem.overrideStackedOnOther (BundleItem.java:50-85):
        // the CARRIED bundle was clicked onto a slot.
        bool Bundle_StackedOnOther(ItemStack& carried, Inventory& inv, int slotIndex,
                                   ClickAction action, Server::ServerPlayer& player,
                                   Server::InventoryClickResult& result) {
            (void)player;
            auto initial = carried.get(DataComponents::BUNDLE_CONTENTS);
            if (!initial) return false;                    // :52-53
            BundleContents contents = *initial;            // Mutable copy (:56)
            ItemStack& other = inv.MutableSlot(slotIndex); // slot.getItem (:55)

            if (action == ClickAction::PRIMARY && !other.IsEmpty()) {
                // :57-66 tryTransfer — absorb the clicked slot's stack.
                if (TryInsert(contents, other) > 0) {
                    Log::Debug("[Bundle] insert — TODO: wire sound system"); // :59
                } else {
                    Log::Debug("[Bundle] insert fail — TODO: wire sound system"); // :61
                }
                carried.components.set(DataComponents::BUNDLE_CONTENTS, contents); // :64
                result.changedSlots.push_back(static_cast<uint8_t>(slotIndex));
                result.carriedChanged = true;
                return true;
            }
            if (action == ClickAction::SECONDARY && other.IsEmpty()) {
                // :67-80 removeOne → into the empty slot (safeInsert).
                ItemStack removed = RemoveOne(contents);
                if (!removed.IsEmpty()) {
                    other = removed;                       // :70 (slot was empty)
                    Log::Debug("[Bundle] remove one — TODO: wire sound system"); // :74
                }
                carried.components.set(DataComponents::BUNDLE_CONTENTS, contents); // :78
                result.changedSlots.push_back(static_cast<uint8_t>(slotIndex));
                result.carriedChanged = true;
                return true;
            }
            return false;                                  // :82
        }

        // Mirrors BundleItem.overrideOtherStackedOnMe (BundleItem.java:87-125):
        // another stack (the cursor) was clicked onto the bundle in a slot.
        bool Bundle_OtherStackedOnMe(ItemStack& slotStack, ItemStack& carried,
                                     Inventory& inv, int slotIndex,
                                     ClickAction action, Server::ServerPlayer& player,
                                     Server::InventoryClickResult& result) {
            (void)inv; (void)player;
            if (action == ClickAction::PRIMARY && carried.IsEmpty()) {
                // :88-90 toggleSelectedItem(-1) — selection is client-side
                // only here; nothing to do server-side. Fall through to the
                // normal pickup.
                return false;
            }
            auto initial = slotStack.get(DataComponents::BUNDLE_CONTENTS);
            if (!initial) return false;                    // :92-94
            BundleContents contents = *initial;            // Mutable copy (:96)

            if (action == ClickAction::PRIMARY && !carried.IsEmpty()) {
                // :97-106 tryInsert the cursor stack into the bundle.
                if (TryInsert(contents, carried) > 0) {
                    Log::Debug("[Bundle] insert — TODO: wire sound system");
                } else {
                    Log::Debug("[Bundle] insert fail — TODO: wire sound system");
                }
                slotStack.components.set(DataComponents::BUNDLE_CONTENTS, contents);
                result.changedSlots.push_back(static_cast<uint8_t>(slotIndex));
                result.carriedChanged = true;
                return true;
            }
            if (action == ClickAction::SECONDARY && carried.IsEmpty()) {
                // :107-118 removeOne → onto the cursor.
                ItemStack removed = RemoveOne(contents);
                if (!removed.IsEmpty()) {
                    Log::Debug("[Bundle] remove one — TODO: wire sound system");
                    carried = removed;                     // :112 carriedItem.set
                }
                slotStack.components.set(DataComponents::BUNDLE_CONTENTS, contents);
                result.changedSlots.push_back(static_cast<uint8_t>(slotIndex));
                result.carriedChanged = true;
                return true;
            }
            return false;                                  // :120-121
        }

    } // namespace

    // Registration — bundle + the 16 dyed bundles get the click overrides,
    // an empty BUNDLE_CONTENTS default (Items.java bundle rows) and
    // stacksTo(1); shulker-box items get canFitInsideContainerItems = false
    // (the BlockItem override MC gives shulker boxes) so they can't nest.
    // Crafting remainders (Items.java `.craftRemainder(...)` rows) ride along
    // here too — data-ready, no crafting system yet.
    void ItemRegistry_RegisterBundles(std::unordered_map<ItemID, Item>& pureItems) {
        auto setBundle = [&](ItemID id) {
            auto it = pureItems.find(id);
            if (it == pureItems.end()) return;
            it->second.defaultComponents.set(DataComponents::BUNDLE_CONTENTS,
                                             BundleContents{});
            it->second.overrideStackedOnOther   = &Bundle_StackedOnOther;
            it->second.overrideOtherStackedOnMe = &Bundle_OtherStackedOnMe;
            it->second.maxStackSize = 1;
        };
        for (ItemID id : {
                Items::Bundle,
                Items::WhiteBundle,     Items::OrangeBundle, Items::MagentaBundle,
                Items::LightBlueBundle, Items::YellowBundle, Items::LimeBundle,
                Items::PinkBundle,      Items::GrayBundle,   Items::LightGrayBundle,
                Items::CyanBundle,      Items::PurpleBundle, Items::BlueBundle,
                Items::BrownBundle,     Items::GreenBundle,  Items::RedBundle,
                Items::BlackBundle }) {
            setBundle(id);
        }

        // Shulker boxes never fit inside container items (bundles). The
        // shulker box ITEMS are block items (not in pureItems) in this
        // engine, and block items can't currently reach a bundle anyway —
        // guarded here for the pure-item shulker entries if they ever exist.
        for (auto& [id, item] : pureItems) {
            if (item.name.find("Shulker Box") != std::string::npos) {
                item.canFitInsideContainerItems = false;
            }
        }

        // Crafting remainders — Items.java `.craftRemainder(...)` rows.
        auto setRemainder = [&](ItemID id, ItemID remainder) {
            auto it = pureItems.find(id);
            if (it != pureItems.end()) it->second.craftingRemainder = remainder;
        };
        setRemainder(Items::MilkBucket,   Items::Bucket);       // milk_bucket row
        setRemainder(Items::DragonBreath, Items::GlassBottle);  // Items.java:2900
        setRemainder(Items::HoneyBottle,  Items::GlassBottle);  // honey_bottle row

        Log::Info("[ItemRegistry] Registered bundle click-behaviours on 17 bundles");
    }

} // namespace Game
