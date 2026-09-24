// File: src/common/world/block/AurelithQuestBlocks.hpp
//
// The blocks of Aurelith's quest (docs/the-hush.md, "Reawakening the Heart";
// common/world/level/AurelithQuest.hpp):
//
//   dim_cyan/violet/amber_lumen_panel, dim_lumen_strip, dim_stave_stone,
//   dim_choir_lamp — the dormant city's lights: its lumen panels, strips,
//                    Stave stone and street lamps as they burn while the Heart
//                    is silent. Emissive like their lit twins, drawn from
//                    darker textures; the same classes and properties, so the
//                    awakening's light wave swaps a state for its twin's state
//                    at the same index (ToLit / ToDim). Mined, a dim light
//                    drops its lit twin: the crystal is the same, only the
//                    note has faded.
//   chord_socket   — one of the Conductor's Podium's four sockets. Right-click
//                    with a voice key to seat it (the key's note sounds);
//                    empty-handed, take a seated key back — until the Chord
//                    locks them. When the fourth key is seated the server
//                    judges the order (Aurelith::OnSocketSeated).
//   voice_pedestal — holds one item for show (the city's hidden keys wait on
//                    them); right-click to set something on it or lift it off.
//   choir_cabinet  — the Hall of Instruments' tuned cabinet: locked until the
//                    resonant chimes before it are struck in its song — the
//                    colour of the lumen plinth under each chime is the note
//                    (OnChimeStruck, from the chime's own use). Sung right,
//                    its doors open and it gives up what it holds.
#pragma once

#include "common/world/block/BlockRegistry.hpp"
#include "common/world/level/AurelithQuest.hpp"
#include "common/entity/GeneratedItemList.hpp"

#include <glm/glm.hpp>

#include <array>
#include <optional>
#include <string_view>

namespace Game {

    class ILevelWrite;

    // Wires the quest blocks' behaviours and marks the dim lights emissive.
    // Called from BlockRegistry::Init beside the other Aurelith registrations.
    void BlockRegistry_RegisterAurelithQuestBlocks(std::array<Block, BlockRegistry::Size>& blocks);

    namespace Aurelith {

        // ── The voice keys ────────────────────────────────────────────────
        std::optional<Voice> VoiceOfKey(ItemID id);
        ItemID KeyOf(Voice v);

        // ── The dormant city's lights ─────────────────────────────────────
        // The lit twin of a dim light (Air when `dim` is not one), and back.
        BlockID LitTwinOf(BlockID dim);
        BlockID DimTwinOf(BlockID lit);
        inline bool IsDimLight(BlockID id) { return LitTwinOf(id) != BlockID::Air; }
        inline bool IsLitLight(BlockID id) { return DimTwinOf(id) != BlockID::Air; }
        // The same state on the twin (identical property layouts), or `s`
        // unchanged when it has none.
        BlockState ToLit(BlockState s);
        BlockState ToDim(BlockState s);

        // ── The Hall of Instruments' cabinet ──────────────────────────────
        inline constexpr int kCabinetReach      = 6;     // chimes this near a cabinet sing to it
        inline constexpr int kCabinetPauseTicks = 200;   // a song left this long starts over

        // The lumen colour a plinth block sings as a note: "cyan", "violet",
        // "amber" (the lit or dim panels), "stave" (Stave stone), or "" for
        // anything else.
        std::string_view NoteColourOf(BlockID plinth);

        // A resonant chime was struck at `chime` (server; ChoirPuzzle's chime
        // use calls this after sounding it). Advances, resets or opens every
        // unsolved choir cabinet within kCabinetReach.
        void OnChimeStruck(ILevelWrite& level, const glm::ivec3& chime);

    } // namespace Aurelith

} // namespace Game
