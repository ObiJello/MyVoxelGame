// File: src/common/data/DataComponents.cpp
#include "DataComponents.hpp"

namespace Game::DataComponents {

    const DataComponentType<bool>             ENCHANTMENT_GLINT_OVERRIDE{"enchantment_glint_override"};
    const DataComponentType<ItemEnchantments> STORED_ENCHANTMENTS       {"stored_enchantments"};

#if ENABLE_PORTAL_GUN
    const DataComponentType<uint8_t>  PORTAL_GUN_NEXT_COLOR  {"portal_gun_next_color"};
    const DataComponentType<uint64_t> PORTAL_GUN_INSTANCE_ID {"portal_gun_instance_id"};
#endif

} // namespace Game::DataComponents
