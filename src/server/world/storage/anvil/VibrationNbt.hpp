// File: src/server/world/storage/anvil/VibrationNbt.hpp
//
// The NBT codecs of MC 26.3's vibration and sculk state, shared by the block
// entities (BlockEntityNbt) and the warden (EntityNbt):
//
//   VibrationSystem.Data.CODEC   "listener": { event?: VibrationInfo,
//                                  selector: { event?: VibrationInfo, tick },
//                                  event_delay }
//   VibrationInfo.CODEC          { game_event, distance, pos: [x,y,z],
//                                  source?: uuid, projectile_owner?: uuid }
//   SculkSpreader.ChargeCursor   "cursors": [{ pos: [x,y,z], charge,
//                                  decay_delay, update_delay, facings? }]
#pragma once

#include "common/nbt/NbtWrite.hpp"
#include "common/world/block/SculkSpreader.hpp"
#include "common/world/level/gameevent/VibrationSystem.hpp"
#include "server/world/storage/NBTParser.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace Game::Anvil {

    // Writes `name` (MC's key is "listener").
    void WriteVibrationData(Nbt::Writer& w, std::string_view name, const VibrationData& data);

    // MC reads with orElseGet(Data::new): a missing or unreadable compound
    // is a fresh Data.
    VibrationData ReadVibrationData(const ::World::NBTTagCompound& parent, const std::string& name);

    // SculkSpreader.save: the "cursors" list.
    void WriteSculkCursors(Nbt::Writer& w, const SculkSpreader& spreader);
    // SculkSpreader.load: at most 32, the rest dropped.
    std::vector<SculkSpreader::ChargeCursor> ReadSculkCursors(const ::World::NBTTagCompound& parent);

} // namespace Game::Anvil
