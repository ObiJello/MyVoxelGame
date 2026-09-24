// File: src/server/world/storage/anvil/LibraryChunkStorage.hpp
//
// The terrain library's chunk storage, over the game's region files.
//
// Minecraft keeps every chunk it unloads, finished or not: a proto chunk (a
// chunk still being generated) is saved with its blocks, structure starts,
// pending block entities and the decoration its neighbours spilled into it,
// and is read back when it is needed again (ChunkMap.processUnloads ->
// save). The library does the same through this class, so a world grows
// without seams across unloads and restarts.
//
// ONE WRITER PER FILE. The library's writes go through the same AnvilChunkIo
// as the game's saves and loads, under the same mutex (see
// AnvilChunkStorage.hpp: a second handle on a region file keeps a stale
// sector table and corrupts it).
//
// FULL CHUNKS ARE THE GAME'S. The library writes proto chunks only, and a
// proto never replaces a finished chunk: a position the game has saved FULL,
// or has a FULL save queued for, is left alone (AnvilChunkIo::
// WriteProtoChunkNbt checks the saved Status under the lock).
//
// Reads see what the game will write: a chunk evicted moments ago is served
// from the game's pending save queue before the region file.
#pragma once

#include "server/world/storage/anvil/AnvilChunkStorage.hpp"

#include "common/world/level/DimensionId.hpp"

#include "world/level/chunk/storage/ChunkStorageBackend.h"

#include <memory>

namespace Game::Anvil {

    class LibraryChunkStorage final : public minecraft::world::level::chunk::storage::ChunkStorageBackend {
    public:
        LibraryChunkStorage(std::shared_ptr<AnvilChunkIo> io, DimensionId dimension,
                            std::weak_ptr<AnvilChunkStorage> gameSaves);

        // Null when the chunk was never saved; throws on a read or decode
        // failure (the library then never writes over that position).
        std::unique_ptr<minecraft::nbt::CompoundTag> read(const minecraft::world::ChunkPos& pos) override;

        // Throws on failure; returns quietly when the game owns the position.
        void write(const minecraft::world::ChunkPos& pos, const minecraft::nbt::CompoundTag* tag) override;

    private:
        std::shared_ptr<AnvilChunkIo>    m_io;
        DimensionId                      m_dimension;
        std::weak_ptr<AnvilChunkStorage> m_gameSaves;
    };

} // namespace Game::Anvil
