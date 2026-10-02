// File: src/common/data/components/ComponentTooltips.hpp
//
// MC TooltipProvider, as a registry. ItemStack.addDetailsToTooltip calls each
// component's addToTooltip in one fixed order; the engine's tooltip builder
// (AbstractContainerScreen::BuildTooltipLines) walks the same order as the
// Slot enum below. A component whose lines are produced in common code
// registers its provider against its slot (a static Registrar in the
// component's own .cpp); the builder calls every provider of a slot in turn,
// skipping it when the stack does not carry the component or TOOLTIP_DISPLAY
// hides it (MC TooltipDisplay.shows).
#pragma once

#include "common/text/TextComponent.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Game {
    struct ItemStack;
    class DataComponentTypeBase;
}

namespace Game::ComponentTooltips {

    // ItemStack.addDetailsToTooltip's order (MC 26.3), one entry per call.
    enum class Slot : uint8_t {
        AppendHoverText = 0,      // Item.appendHoverText
        TropicalFishPattern,
        Instrument,
        MapId,
        Bees,
        ContainerLoot,
        Container,
        BannerPatterns,
        PotDecorations,
        WrittenBookContent,
        ChargedProjectiles,
        Fireworks,
        FireworkExplosion,
        PotionContents,
        JukeboxPlayable,
        Trim,
        StoredEnchantments,
        Enchantments,
        DyedColor,
        Profile,
        Lore,
        SulfurCubeContent,
        AttributeModifiers,       // addAttributeTooltips
        IntangibleProjectile,     // addUnitComponentToTooltip
        Unbreakable,              // addUnitComponentToTooltip
        SignTextFront,
        SignTextBack,
        OminousBottleAmplifier,
        SuspiciousStewEffects,
        BlockState,
        EntityData,
        SpawnerBlockEntityData,   // Spawner.appendHoverText (spawner / trial spawner)
        CanBreak,
        CanPlaceOn,
        Count
    };

    // One tooltip line. `text` is the plain string (search, width, the
    // single-colour draw); `rich`, when set, is drawn instead with its own
    // styles (colours, hex colours, bold …) — a custom name or a lore line
    // given as a styled text component. `argb` colours the plain draw and is
    // the base colour under `rich`.
    struct Line {
        std::string                    text;
        uint32_t                       argb = 0xFFFFFFFFu;
        std::optional<Text::Component> rich;
    };

    // MC Item.TooltipContext + TooltipFlag.
    struct Context {
        bool advanced = false;   // TooltipFlag.isAdvanced (F3+H)
        bool creative = false;   // TooltipFlag.isCreative
    };

    using Provider = void (*)(const ItemStack& stack, const Context& context, std::vector<Line>& out);

    struct Registration {
        Slot                         slot;
        // The component the provider answers for — the builder calls the
        // provider only when the stack has it and TOOLTIP_DISPLAY shows it.
        // Null: the provider decides for itself (called for every stack).
        const DataComponentTypeBase* type = nullptr;
        Provider                     provider = nullptr;
    };

    void Register(Slot slot, const DataComponentTypeBase* type, Provider provider);
    // Every provider registered for `slot`, in registration order.
    const std::vector<Registration>& ForSlot(Slot slot);

    struct Registrar {
        Registrar(Slot slot, const DataComponentTypeBase* type, Provider provider) {
            Register(slot, type, provider);
        }
    };

    // Common colours (ChatFormatting ARGB).
    inline constexpr uint32_t kWhite       = 0xFFFFFFFFu;
    inline constexpr uint32_t kGray        = 0xFFAAAAAAu;
    inline constexpr uint32_t kDarkGray    = 0xFF555555u;
    inline constexpr uint32_t kBlue        = 0xFF5555FFu;
    inline constexpr uint32_t kRed         = 0xFFFF5555u;
    inline constexpr uint32_t kDarkGreen   = 0xFF00AA00u;
    inline constexpr uint32_t kDarkPurple  = 0xFFAA00AAu;
    inline constexpr uint32_t kGold        = 0xFFFFAA00u;
    inline constexpr uint32_t kYellow      = 0xFFFFFF55u;

} // namespace Game::ComponentTooltips
