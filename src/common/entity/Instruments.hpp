// File: src/common/entity/Instruments.hpp
//
// MC net.minecraft.world.item.Instrument + the INSTRUMENT registry, read
// from the data pack (data/<ns>/instrument/<path>.json: sound_event,
// use_duration in seconds, range in blocks, description), and the
// instrument tags (data/<ns>/tags/instrument/*.json) the set_instrument loot
// function and the creative tab's generateInstrumentTypes resolve.
//
// A goat horn's DataComponents.INSTRUMENT holds the instrument's id;
// InstrumentItem.use plays it (server/items/ArchaeologyItems.cpp).
#pragma once

#include <string>
#include <vector>

namespace Game::Instruments {

    // Items.GOAT_HORN's default INSTRUMENT (Instruments.PONDER_GOAT_HORN).
    inline constexpr const char* kDefaultGoatHorn = "minecraft:ponder_goat_horn";

    struct Instrument {
        std::string id;              // "minecraft:ponder_goat_horn"
        std::string soundEvent;      // "item.goat_horn.sound.0" (namespace stripped)
        float       useDuration = 0.0f;   // seconds
        float       range = 0.0f;         // blocks; the sound's volume is range / 16
        std::string descriptionKey;  // "instrument.minecraft.ponder_goat_horn"
    };

    // The registered instrument, read on first use and cached; nullptr for
    // an id the data pack does not define. Any thread.
    const Instrument* Get(const std::string& id);

    // A HolderSet spec as the data pack writes it: "#minecraft:goat_horns"
    // (a tag, nested tags expanded, order kept) or one id. Unknown entries
    // are dropped.
    std::vector<std::string> Resolve(const std::string& spec);

    // InstrumentComponent.addToTooltip's line: the description text.
    std::string DescriptionOf(const std::string& id);

    // Forget every parsed definition (a data reload).
    void Reload();

} // namespace Game::Instruments
