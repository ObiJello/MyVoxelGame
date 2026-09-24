// File: src/server/world/storage/anvil/LibraryChunkStorage.cpp
#include "server/world/storage/anvil/LibraryChunkStorage.hpp"

#include "common/core/Log.hpp"
#include "common/nbt/NbtWrite.hpp"

#include "nbt/NbtIo.h"

#include <sstream>
#include <stdexcept>
#include <string>

namespace Game::Anvil {

    namespace {
        std::unique_ptr<minecraft::nbt::CompoundTag> Decode(const std::vector<uint8_t>& nbt) {
            std::istringstream input(std::string(reinterpret_cast<const char*>(nbt.data()), nbt.size()));
            auto tag = minecraft::nbt::NbtIo::read(input);
            if (!tag) throw std::runtime_error("chunk NBT is not a compound");
            return tag;
        }
    }

    LibraryChunkStorage::LibraryChunkStorage(std::shared_ptr<AnvilChunkIo> io, DimensionId dimension,
                                             std::weak_ptr<AnvilChunkStorage> gameSaves)
        : m_io(std::move(io)), m_dimension(dimension), m_gameSaves(std::move(gameSaves)) {}

    std::unique_ptr<minecraft::nbt::CompoundTag> LibraryChunkStorage::read(
            const minecraft::world::ChunkPos& libPos) {
        const Math::ChunkPos pos{libPos.x(), libPos.z()};
        std::vector<uint8_t> nbt;
        std::string error;
        if (auto saves = m_gameSaves.lock()) {
            if (saves->TryReadPending(pos, nbt, error)) return Decode(nbt);
            if (!error.empty()) throw std::runtime_error(error);
        }
        if (!m_io->ReadChunkNbt(m_dimension, RegionKind::Chunks, pos, nbt, error)) {
            if (error.empty()) return nullptr;      // never saved
            throw std::runtime_error(error);
        }
        return Decode(nbt);
    }

    void LibraryChunkStorage::write(const minecraft::world::ChunkPos& libPos,
                                    const minecraft::nbt::CompoundTag* tag) {
        if (tag == nullptr) return;
        const Math::ChunkPos pos{libPos.x(), libPos.z()};
        // The game's own save of this position is queued: its FULL chunk
        // lands after us whatever we write, so writing is wasted work.
        if (auto saves = m_gameSaves.lock()) {
            if (saves->HasPendingWrite(pos)) return;
        }

        std::ostringstream output;
        minecraft::nbt::NbtIo::write(*tag, output);
        const std::string bytes = output.str();
        std::vector<uint8_t> payload;
        if (!Nbt::ZlibCompress(std::vector<uint8_t>(bytes.begin(), bytes.end()), payload)) {
            throw std::runtime_error("zlib compression failed");
        }
        bool skipped = false;
        std::string error;
        if (!m_io->WriteProtoChunkNbt(m_dimension, pos, payload, skipped, error)) {
            throw std::runtime_error(error.empty() ? std::string("region write failed") : error);
        }
    }

} // namespace Game::Anvil
