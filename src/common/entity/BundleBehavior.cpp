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
// Weight is an exact fraction of a full bundle, as MC's apache Fraction:
// each item weighs 1/getMaxStackSize() — a stack's own limit, so a
// `[max_stack_size=3]` stick weighs a third — a nested bundle 1/16 plus its
// contents. Full at 1.
//
// NOT wired: BundleItem.use's hold-to-dump (BundleItem.java:127-130, :211-219)
// — dumping spawns ItemEntities, which don't exist. The click ops are the
// full in-inventory behaviour.
#include "Item.hpp"
#include "GeneratedItemList.hpp"
#include "Inventory.hpp"
#include "../data/DataComponents.hpp"
#include "../core/Log.hpp"
#include "../inventory/AbstractContainerMenu.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_map>

namespace Game {

    namespace {

        // org.apache.commons.lang3.math.Fraction, as far as the bundle uses
        // it: reduced, positive denominator; an overflow (ArithmeticException
        // in MC) marks the result invalid ("Excessive total bundle weight").
        struct Fraction {
            int64_t num = 0;
            int64_t den = 1;
            bool    valid = true;

            static int64_t Gcd(int64_t a, int64_t b) {
                a = a < 0 ? -a : a;
                b = b < 0 ? -b : b;
                while (b != 0) { const int64_t t = a % b; a = b; b = t; }
                return a == 0 ? 1 : a;
            }
            static Fraction Of(int64_t n, int64_t d) {
                Fraction f;
                if (d == 0) { f.valid = false; return f; }
                if (d < 0) { n = -n; d = -d; }
                const int64_t g = Gcd(n, d);
                f.num = n / g;
                f.den = d / g;
                return f;
            }
            // Denominators stay far below this for any real contents (lcm of
            // stack limits up to 99, nesting adds 16); past it is overflow.
            static constexpr int64_t kLimit = int64_t{1} << 40;
            // a * b, or false when it would overflow.
            static bool Mul(int64_t a, int64_t b, int64_t& out) {
                const int64_t aa = a < 0 ? -a : a;
                const int64_t bb = b < 0 ? -b : b;
                if (aa != 0 && bb > (INT64_MAX / 4) / aa) return false;
                out = a * b;
                return true;
            }
            Fraction Add(const Fraction& o) const {
                if (!valid || !o.valid) return Invalid();
                const int64_t g = Gcd(den, o.den);
                int64_t d = 0, a = 0, b = 0;
                if (!Mul(den / g, o.den, d) || d > kLimit) return Invalid();
                if (!Mul(num, d / den, a) || !Mul(o.num, d / o.den, b)) return Invalid();
                return Of(a + b, d);
            }
            Fraction Times(int64_t k) const {
                if (!valid) return Invalid();
                int64_t n = 0;
                if (!Mul(num, k, n)) return Invalid();
                return Of(n, den);
            }
            static Fraction Invalid() { Fraction f; f.valid = false; return f; }
        };

        const Fraction kBundleInBundleWeight = Fraction::Of(1, 16);   // BUNDLE_IN_BUNDLE_WEIGHT

        Fraction WeightOf(const ItemStack& stack);

        // BundleContents.computeContentWeight: Σ weight(stack) × count.
        Fraction ContentsWeight(const BundleContents& contents) {
            Fraction total = Fraction::Of(0, 1);
            for (const auto& s : contents.items) {
                total = total.Add(WeightOf(s).Times(s.count));
                if (!total.valid) break;
            }
            return total;
        }

        // BundleContents.getWeight: a nested bundle costs 1/16 + its own
        // weight; anything else 1/getMaxStackSize(). (Occupied beehives weigh
        // a whole bundle in MC; this engine's hive items carry no bees.)
        Fraction WeightOf(const ItemStack& stack) {
            if (auto inner = stack.get(DataComponents::BUNDLE_CONTENTS)) {
                return ContentsWeight(*inner).Add(kBundleInBundleWeight);
            }
            return Fraction::Of(1, std::max(1, GetMaxStackSize(stack)));
        }

        // BundleContents.canItemBeInBundle (:71-73).
        bool CanItemBeInBundle(const ItemStack& stack) {
            return !stack.IsEmpty()
                && ItemRegistry::Get(stack.itemId).canFitInsideContainerItems;
        }

        // Mutable.getMaxAmountToAdd: (1 - weight) / itemWeight, truncated.
        int MaxAmountToAdd(const BundleContents& contents, const ItemStack& item) {
            const Fraction current = ContentsWeight(contents);
            const Fraction perItem = WeightOf(item);
            if (!current.valid || !perItem.valid || perItem.num <= 0) return 0;
            const Fraction remaining = Fraction::Of(1, 1).Add(Fraction::Of(-current.num, current.den));
            if (!remaining.valid || remaining.num <= 0) return 0;
            // (rn/rd) / (pn/pd) = rn*pd / (rd*pn), Fraction.intValue truncates.
            const long double q = (static_cast<long double>(remaining.num) * static_cast<long double>(perItem.den)) /
                                  (static_cast<long double>(remaining.den) * static_cast<long double>(perItem.num));
            return q >= 2147483647.0L ? 2147483647 : std::max(static_cast<int>(q), 0);
        }

        // Mutable.tryInsert (:181-203) — moves up to the weight-limited count
        // from `toAdd` into `contents`. Returns the amount moved.
        int TryInsert(BundleContents& contents, ItemStack& toAdd) {
            if (!CanItemBeInBundle(toAdd)) return 0;
            const int amount = std::min(toAdd.count, MaxAmountToAdd(contents, toAdd));
            if (amount == 0) return 0;

            // findStackIndex: a stackable item merges into the first stack
            // of the same item AND components.
            int stackIndex = -1;
            if (IsStackable(toAdd)) {
                for (size_t i = 0; i < contents.items.size(); ++i) {
                    if (IsSameItemSameComponents(contents.items[i], toAdd)) {
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
        bool Bundle_StackedOnOther(ItemStack& carried, AbstractContainerMenu& menu,
                                   int slotIndex, ClickAction action,
                                   ContainerClickResult& result) {
            auto initial = carried.get(DataComponents::BUNDLE_CONTENTS);
            if (!initial) return false;                    // :52-53
            BundleContents contents = *initial;            // Mutable copy (:56)
            Slot& slot = menu.GetSlot(slotIndex);
            ItemStack& other = slot.GetItemMut();          // slot.getItem (:55)

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
                    slot.SetByPlayer(removed);             // :70 (slot was empty)
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
                                     AbstractContainerMenu& menu, int slotIndex,
                                     ClickAction action,
                                     ContainerClickResult& result) {
            (void)menu;
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
