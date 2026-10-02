// File: src/common/entity/SelectableEntityTypes.hpp
//
// The entity type ids a selector's `type=` knows besides the engine's
// EntityTypeId table (GeneratedEntityTypes): MC 26.3's EntityType registry
// entries this engine keeps in managers of their own — players, dropped
// items, experience orbs, which ARE selected — and the few it has no entity
// for at all, which are valid ids that simply match nothing (as a type with
// no live entity does in MC). Shared by the server's selector parser and the
// client's `type=` completion so the two accept the same list.
#pragma once

#include <array>
#include <string_view>

namespace Game {

    inline constexpr std::array<std::string_view, 12> kSelectorOnlyEntityTypes = {
        "player", "item", "experience_orb",
        // In MC's registry, not modelled by this engine.
        "block_display", "experience_bottle", "interaction", "item_display",
        "lingering_potion", "mannequin", "marker", "spectral_arrow", "text_display",
    };

} // namespace Game
