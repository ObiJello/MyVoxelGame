// File: src/common/data/components/StackComponents.cpp
#include "StackComponents.hpp"

#include <string>

namespace Game {

    int GetItemDefaultMaxStackSize(ItemID itemId) {
        const Item& item = ItemRegistry::Get(itemId);
        if (auto v = item.defaultComponents.get(DataComponents::MAX_STACK_SIZE)) return *v;
        return item.maxStackSize;
    }

    int GetMaxStackSize(const ItemStack& stack) {
        if (auto v = stack.components.get(DataComponents::MAX_STACK_SIZE)) return *v;
        // getOrDefault(MAX_STACK_SIZE, 1): a patch that removes the item's
        // limit (`stick[!max_stack_size]`) leaves one per stack.
        if (stack.components.isRemoved(DataComponents::MAX_STACK_SIZE)) return 1;
        return GetItemDefaultMaxStackSize(stack.itemId);
    }

    bool IsStackable(const ItemStack& stack) {
        return GetMaxStackSize(stack) > 1 && (!IsDamageableItem(stack) || !IsDamaged(stack));
    }

    namespace {
        // ItemStack.validateContainedItemSizes.
        std::string ValidateContainedItemSizes(const std::vector<ItemStack>& items) {
            for (const ItemStack& item : items) {
                if (item.IsEmpty()) continue;
                const int max = GetMaxStackSize(item);
                if (item.count > max) {
                    return "Item stack with count of " + std::to_string(item.count) +
                           " was larger than maximum: " + std::to_string(max);
                }
            }
            return {};
        }
    }

    std::string ValidateItemStack(const ItemStack& stack) {
        if (stack.IsEmpty()) return {};
        // validateComponents over the stack's whole (patched) map.
        if (stack.get(DataComponents::MAX_DAMAGE).has_value() && GetMaxStackSize(stack) > 1) {
            return "Item cannot be both damageable and stackable";
        }
        if (auto container = stack.get(DataComponents::CONTAINER)) {
            if (std::string e = ValidateContainedItemSizes(container->items); !e.empty()) return e;
        }
        if (auto bundle = stack.get(DataComponents::BUNDLE_CONTENTS)) {
            if (std::string e = ValidateContainedItemSizes(bundle->items); !e.empty()) return e;
        }
        if (auto charged = stack.get(DataComponents::CHARGED_PROJECTILES)) {
            if (std::string e = ValidateContainedItemSizes(charged->items); !e.empty()) return e;
        }
        // validateStrict's count check.
        const int max = GetMaxStackSize(stack);
        if (stack.count > max) {
            return "Item stack with stack size of " + std::to_string(stack.count) +
                   " was larger than maximum: " + std::to_string(max);
        }
        return {};
    }

} // namespace Game

namespace Game::DataComponents {

    namespace {
        void SerVarIntStack(Network::PacketBuffer& b, const int32_t& v) { b.WriteVarInt(static_cast<uint32_t>(v)); }
        int32_t DeVarIntStack(Network::PacketReader& r) {
            // MAX_STACK_SIZE's range is the codec's; a desynced peer's value is
            // clamped rather than trusted (a 0 would make every slot full).
            const int32_t v = static_cast<int32_t>(r.ReadVarInt());
            return v < 1 ? 1 : (v > kMaxStackSizeLimit ? kMaxStackSizeLimit : v);
        }
        // Unit.STREAM_CODEC: nothing on the wire.
        void SerUnitLock(Network::PacketBuffer&, const bool&) {}
        bool DeUnitLock(Network::PacketReader&) { return true; }
    }

    const DataComponentType<NbtCompoundValue> CUSTOM_DATA{"custom_data", 210,
                                                          &SerNbtCompoundValue, &DeNbtCompoundValue};
    const DataComponentType<int32_t> MAX_STACK_SIZE{"max_stack_size", 211, &SerVarIntStack, &DeVarIntStack};
    const DataComponentType<bool> CREATIVE_SLOT_LOCK{"creative_slot_lock", 212, &SerUnitLock, &DeUnitLock};

} // namespace Game::DataComponents
