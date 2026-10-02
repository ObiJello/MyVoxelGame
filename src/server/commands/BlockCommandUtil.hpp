// File: src/server/commands/BlockCommandUtil.hpp
//
// The pieces MC's block-editing commands (/fill, /clone, /setworldspawn,
// /spawnpoint, /place) share — each one a vanilla helper that more than one
// command calls, kept in one place so they cannot drift apart:
//
//   BlockPosArgument.getLoadedBlockPos / getSpawnablePos — the two position
//   checks a block-position argument runs before a command sees it;
//   BlockInput (BlockStateArgument's result) and its place();
//   ServerLevel.updateNeighboursOnBlockSet — the deferred neighbour pass the
//   fill and clone loops run once every block is down;
//   the block-entity compound round trip /clone carries contents with.
#pragma once

#include "BlockStateArgument.hpp"
#include "CommandSourceStack.hpp"
#include "server/world/storage/NBTParser.hpp"

#include "common/world/block/BlockState.hpp"

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Game { class World; }

namespace Server {

    class ServerConnection;
    class ServerLevel;

    // MC CommandSourceStack.sendFailure: the message in red, sender only.
    void SendCommandFailure(ServerConnection& connection, const std::string& text);

    // Java's Float.toString for the plain range a command prints (an angle,
    // a coordinate): "90.0", "-12.5", "0.33333334".
    std::string JavaFloatString(float value);

    // MC Level.isInWorldBounds: inside the build height and the 30 000 000
    // horizontal limit.
    bool IsInWorldBounds(const Game::World& world, const glm::ivec3& pos);

    // MC BlockPosArgument.getLoadedBlockPos(context, level, name): parsed
    // from the stack, then "That position is not loaded" when its chunk is
    // not resident in `level`'s world, "That position is out of this world!"
    // outside the build limits.
    bool GetLoadedBlockPos(const Game::World& world, const std::string& ax, const std::string& ay,
                           const std::string& az, const CommandSourceStack& source,
                           glm::ivec3& out, std::string& error);

    // MC BlockPosArgument.getSpawnablePos: parsed, then "That position is
    // outside the allowed boundaries." outside Level.isInSpawnableBounds.
    // No loaded check.
    bool GetSpawnablePos(const std::string& ax, const std::string& ay, const std::string& az,
                         const CommandSourceStack& source, glm::ivec3& out, std::string& error);

    // MC RotationArgument (`<yaw> <pitch>`, `~` relative to the source's
    // rotation, `^` refused). Yaw first, as typed.
    bool ParseRotationArgument(const std::string& yaw, const std::string& pitch,
                               const CommandSourceStack& source, CommandRotation& out, std::string& error);

    // Make sure every chunk of the column box [minXZ, maxXZ] is resident in
    // `level` — MC's ServerLevel.getBlockState / setBlock load a chunk on
    // demand; this engine's World never does, so the commands that touch
    // chunks the player is not standing in load them first, blocking, the
    // way a respawn does (ServerLevel::GetChunkBlocking, which also holds a
    // short ticket so the work is not raced by an unload). False when one
    // could not be loaded (shutdown).
    bool EnsureChunksLoaded(ServerLevel& level, const glm::ivec3& min, const glm::ivec3& max);

    // MC ServerLevel.updateNeighboursOnBlockSet(pos, oldState): the old
    // block's affectNeighborsAfterRemoval when the block changed, the
    // neighbours' neighborChanged, and the comparator update for a block
    // with an analog output.
    void UpdateNeighboursOnBlockSet(Game::World& world, const glm::ivec3& pos, Game::BlockState oldState);

    // MC BlockInput — what BlockStateArgument parses: the state, the
    // properties the text NAMED (only those are forced back after the
    // neighbour-shape pass), and the optional block-entity compound.
    struct BlockInput {
        Game::BlockState state;
        std::vector<std::pair<std::string, std::string>> definedProperties;
        std::shared_ptr<::World::NBTTagCompound> tag;

        // MC BlockInput.place(level, pos, flags): the state reshaped against
        // its neighbours (Block.updateFromNeighbourShapes) unless the flags
        // carry UPDATE_KNOWN_SHAPE, the named properties written back over
        // that, setBlock, then the compound merged into the block entity.
        // True when the block changed.
        bool Place(Game::World& world, const glm::ivec3& pos, uint32_t flags) const;
    };

    // MC BlockStateArgument.parse: `[ns:]id[prop=value,...]{nbt}`. A tag is
    // refused ("Tags aren't allowed here, only actual blocks").
    bool ParseBlockInput(const std::string& text, BlockInput& out, std::string& error);

    // MC BlockPredicateArgument with its optional `{nbt}`: the block / tag
    // and properties (BlockPredicate), and a compound the block entity there
    // must contain (NbtUtils.compareNbt, partial lists).
    struct BlockFilter {
        BlockPredicate predicate;
        std::shared_ptr<::World::NBTTagCompound> nbt;

        bool Test(Game::World& world, const glm::ivec3& pos) const;
    };
    bool ParseBlockFilter(const std::string& text, BlockFilter& out, std::string& error);

    // A block entity's whole compound (id, x, y, z and its fields), written
    // the way the world save writes it. Null when there is none.
    std::shared_ptr<::World::NBTTagCompound> SaveBlockEntityTag(Game::World& world, const glm::ivec3& pos);

    // Load `data` into the block entity at `pos` — the fields only: its own
    // id and position are kept (MC BlockEntity.loadCustomOnly). The block
    // entity is rebuilt from the compound and resent. False when there is no
    // block entity there or the compound does not fit it.
    bool LoadBlockEntityTag(Game::World& world, const glm::ivec3& pos, const ::World::NBTTagCompound& data);

} // namespace Server
