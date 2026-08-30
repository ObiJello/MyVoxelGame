// File: src/server/world/storage/anvil/ChunkSerializer.hpp
//
// Game::Chunk <-> vanilla chunk NBT.
//
// Emission order is SerializableChunkData.write() verbatim, so our output can
// be compared field-by-field against a chunk Minecraft wrote.
//
// Runs on the CALLING thread, not the I/O thread. That is what makes block
// entities reachable at all — the async path used to snapshot with
// Chunk::Clone(), which copies sections and heightmaps but not the block
// entity map, so a chest's contents were dropped before the saver ever saw
// them. Serialising where the chunk is still live also makes the snapshot
// atomic with respect to the eviction that triggered it. The bytes ARE the
// snapshot, and they are smaller than the chunk.
#pragma once

#include "common/world/chunk/Chunk.hpp"
#include "common/world/math/WorldMath.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Game::Anvil {

    // MC ChunkStatus. Only two matter to us: a chunk we generated and finished
    // is `full`; anything else we would not be writing.
    inline constexpr const char* kStatusFull = "minecraft:full";

    // The lowest section index's Y, written as `yPos`. MIN_WORLD_Y / 16 = -4.
    // This is a SECTION index, not a block coordinate — a mistake that costs
    // 64 blocks of vertical offset and is invisible until you open the world.
    inline constexpr int kMinSectionY = Chunk::MIN_WORLD_Y / Chunk::SECTION_HEIGHT;

    // Uncompressed chunk NBT. The caller compresses (on its own thread) and
    // hands the result to AnvilRegion::Write.
    //
    // `gameTime` is the world clock at the moment of the save. Only the
    // scheduled-tick list needs it: MC stores each appointment as a DELAY
    // rather than an absolute trigger tick, so that reopening the save later
    // does not make every pending tick fire on the first frame.
    bool SerialiseChunk(const Chunk& chunk, int dataVersion, int64_t gameTime,
                        std::vector<uint8_t>& out, std::string& error);

    // How strictly to read a chunk.
    enum class ReadMode {
        // OUR OWN save. A position mismatch means the region index and the
        // payload disagree, which should never happen and is worth refusing.
        Strict,
        // An IMPORTED Minecraft world. Vanilla relocates a misplaced chunk
        // with a warning rather than dropping it, and refusing chunks real
        // Minecraft accepts would make imported worlds look broken.
        Lenient,
    };

    // `gameTime` rebases the saved tick delays onto the current clock — the
    // read half of the note on SerialiseChunk above.
    bool DeserialiseChunk(const std::vector<uint8_t>& nbt, Math::ChunkPos expected,
                          Chunk& out, std::string& error,
                          ReadMode mode = ReadMode::Strict,
                          int64_t gameTime = 0);

    inline constexpr ReadMode ChunkSerializer_ReadMode_Lenient = ReadMode::Lenient;

    // True when a parsed chunk root looks like the 1.18+ layout this
    // serialiser understands: a root-level `sections` list whose entries carry
    // `block_states`. Pre-1.18 worlds nest sections under `Level` and pre-1.13
    // ones use Blocks/Data byte arrays; both stay on the legacy reader.
    bool IsModernChunkLayout(const std::vector<uint8_t>& nbt);

    // Debug-build guard for the palette repack (design risk R1).
    //
    // Re-parses bytes we just produced and compares all 24x4096 block states
    // and 24x64 biomes against the chunk they came from. A repack bug is
    // otherwise silent until Minecraft refuses one chunk in a thousand; this
    // turns it into a caught mismatch on the developer's machine. Costs a full
    // deserialise, so it is compiled out of release builds.
    bool VerifyRoundTrip(const Chunk& source, const std::vector<uint8_t>& nbt,
                         std::string& mismatch);

} // namespace Game::Anvil
