// File: src/common/entity/PlayerColors.hpp
//
// Single source of truth for the player-color palette. Used by:
//   - Launcher UI (renders the swatch grid in settings)
//   - Launcher → game CLI (`--color <name>`)
//   - Game CLI parsing (PlatformMain) → local Player.color
//   - Network handshake (server stores + broadcasts to remote clients)
//   - Renderer (PlayerRenderer + PlayerInventoryPreview look up RGB by id)
//
// Palette philosophy: most entries share HSL S=100% L=50% to match the existing
// neon-green saturation. Black/white/brown can't be neon by definition and are
// included as the closest visible representative — see the doc string in the
// launcher's settings UI.
#pragma once

#include <cstdint>
#include <string>

namespace Game {

    enum class PlayerColorId : uint8_t {
        Default = 0,   // existing neon green — used when no selection / unknown name
        Red,
        Orange,
        Yellow,
        Blue,
        Purple,
        Pink,
        White,
        Black,
        Brown,
        // Keep `Count` last — used for table-size validation.
        Count
    };

    struct PlayerColorEntry {
        PlayerColorId id;
        const char*   name;     // capital-cased, displayed in UI tooltips
        const char*   slug;     // lowercase identifier — wire format / config key
        uint8_t       r, g, b;
    };

    // Palette table — order doesn't matter to consumers; LookupPlayerColor uses id.
    inline constexpr PlayerColorEntry kPlayerColorTable[] = {
        { PlayerColorId::Default, "Default Green", "default",   0, 255,  60 },
        { PlayerColorId::Red,     "Red",           "red",     255,   0,   0 },
        { PlayerColorId::Orange,  "Orange",        "orange",  255, 128,   0 },
        { PlayerColorId::Yellow,  "Yellow",        "yellow",  255, 255,   0 },
        { PlayerColorId::Blue,    "Blue",          "blue",      0, 128, 255 },
        { PlayerColorId::Purple,  "Purple",        "purple",  128,   0, 255 },
        { PlayerColorId::Pink,    "Pink",          "pink",    255,   0, 191 },
        { PlayerColorId::White,   "White",         "white",   255, 255, 255 },
        { PlayerColorId::Black,   "Black",         "black",    40,  40,  40 },
        { PlayerColorId::Brown,   "Brown",         "brown",   160,  82,  30 },
    };
    static_assert(sizeof(kPlayerColorTable) / sizeof(kPlayerColorTable[0])
                      == static_cast<size_t>(PlayerColorId::Count),
                  "kPlayerColorTable must have one entry per PlayerColorId");

    // Returns the entry for `id`, or the Default entry if `id` is out of range.
    const PlayerColorEntry& LookupPlayerColor(PlayerColorId id);

    // Case-insensitive lookup of a slug like "pink" → PlayerColorId::Pink. Returns
    // Default for unknown / empty inputs (so empty config values map cleanly).
    PlayerColorId ParsePlayerColorName(const std::string& slug);

} // namespace Game
