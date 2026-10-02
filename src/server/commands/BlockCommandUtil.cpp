// File: src/server/commands/BlockCommandUtil.cpp
#include "BlockCommandUtil.hpp"
#include "CommandCoords.hpp"
#include "NbtPath.hpp"
#include "SnbtParser.hpp"
#include "../level/ServerLevel.hpp"
#include "../network/ServerConnection.hpp"
#include "../world/storage/anvil/BlockEntityNbt.hpp"

#include "common/nbt/NbtWrite.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/block/piston/PistonBaseBlock.hpp"   // UpdateFromNeighbourShapes
#include "common/world/level/World.hpp"
#include "common/world/math/WorldCoordinates.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace Server {

    namespace {
        using Compound = ::World::NBTTagCompound;

        // MC Level.isInSpawnableBounds (the horizontal half) and the vertical
        // ±20 000 000 BlockPosArgument.getSpawnablePos adds.
        constexpr int kMaxLevelSize = 30000000;
        constexpr int kMaxVertical  = 20000000;
    } // namespace

    void SendCommandFailure(ServerConnection& connection, const std::string& text) {
        Network::ChatMessageS2CPacket packet;
        packet.senderId = 0;
        packet.position = 1;
        packet.segments.push_back(Network::ChatSegmentData{
            text, 0xFFFF5555u, Network::ChatClickAction::None, "", ""});
        connection.SendChatMessage(packet);
    }

    std::string JavaFloatString(float value) {
        if (value == 0.0f) return std::signbit(value) ? "-0.0" : "0.0";
        // The shortest decimal that reads back as the same float, with a
        // ".0" on a whole number — Float.toString in the 1e-3..1e7 range.
        char buf[48];
        for (int precision = 1; precision <= 9; ++precision) {
            std::snprintf(buf, sizeof(buf), "%.*g", precision, static_cast<double>(value));
            if (std::strtof(buf, nullptr) == value) break;
        }
        std::string text = buf;
        if (text.find_first_of(".eEn") == std::string::npos) text += ".0";
        return text;
    }

    bool IsInWorldBounds(const Game::World& world, const glm::ivec3& pos) {
        return world.IsValidPosition(pos.x, pos.y, pos.z) &&
               pos.x >= -kMaxLevelSize && pos.z >= -kMaxLevelSize &&
               pos.x < kMaxLevelSize && pos.z < kMaxLevelSize;
    }

    bool GetLoadedBlockPos(const Game::World& world, const std::string& ax, const std::string& ay,
                           const std::string& az, const CommandSourceStack& source,
                           glm::ivec3& out, std::string& error) {
        if (!ParseBlockPos(ax, ay, az, source, source.rotation, out, error)) return false;
        if (!world.IsChunkLoaded(out.x >> 4, out.z >> 4)) {
            error = "That position is not loaded";            // argument.pos.unloaded
            return false;
        }
        if (!IsInWorldBounds(world, out)) {
            error = "That position is out of this world!";    // argument.pos.outofworld
            return false;
        }
        return true;
    }

    bool GetSpawnablePos(const std::string& ax, const std::string& ay, const std::string& az,
                         const CommandSourceStack& source, glm::ivec3& out, std::string& error) {
        if (!ParseBlockPos(ax, ay, az, source, source.rotation, out, error)) return false;
        // Level.isInSpawnableBounds: |x|, |z| < 30 000 000 and the vertical
        // band BlockPosArgument checks beside it.
        if (out.x < -kMaxLevelSize || out.x >= kMaxLevelSize || out.z < -kMaxLevelSize ||
            out.z >= kMaxLevelSize || out.y < -kMaxVertical || out.y >= kMaxVertical) {
            error = "That position is outside the allowed boundaries.";   // argument.pos.outofbounds
            return false;
        }
        return true;
    }

    bool ParseRotationArgument(const std::string& yaw, const std::string& pitch,
                               const CommandSourceStack& source, CommandRotation& out, std::string& error) {
        if ((!yaw.empty() && yaw[0] == '^') || (!pitch.empty() && pitch[0] == '^')) {
            error = "Local coordinates are not allowed here";   // argument.rotation... ERROR_NOT_LOCAL
            return false;
        }
        double y = 0.0, x = 0.0;
        if (!ParseCoord(yaw, source.rotation.yRot, false, y) ||
            !ParseCoord(pitch, source.rotation.xRot, false, x)) {
            error = "Invalid rotation: " + yaw + " " + pitch;
            return false;
        }
        out.yRot = static_cast<float>(y);
        out.xRot = static_cast<float>(x);
        return true;
    }

    bool EnsureChunksLoaded(ServerLevel& level, const glm::ivec3& min, const glm::ivec3& max) {
        Game::World* world = level.World();
        if (!world) return false;
        for (int cx = min.x >> 4; cx <= (max.x >> 4); ++cx) {
            for (int cz = min.z >> 4; cz <= (max.z >> 4); ++cz) {
                // A chunk already resident still gets its short ticket: the
                // command is about to write it, and an unload between the
                // write and the next save pass would drop the edit.
                if (!level.GetChunkBlocking(Game::Math::ChunkPos(cx, cz))) return false;
            }
        }
        return true;
    }

    void UpdateNeighboursOnBlockSet(Game::World& world, const glm::ivec3& pos, Game::BlockState oldState) {
        const Game::BlockState newState = world.GetBlockState(pos.x, pos.y, pos.z);
        const Game::BlockID newBlock = newState.Block();
        if (oldState.Block() != newBlock) {
            const Game::Block& oldDef = Game::BlockRegistry::Get(oldState.Block());
            if (oldDef.affectNeighborsAfterRemoval) oldDef.affectNeighborsAfterRemoval(world, pos, oldState, false);
        }
        world.UpdateNeighborsAt(pos, newBlock);
        if (Game::BlockRegistry::Get(newBlock).hasAnalogOutputSignal) {
            world.UpdateNeighbourForOutputSignal(pos, newBlock);
        }
    }

    namespace {
        // `{...}` after the id and properties is the block entity's compound
        // (BlockStateParser.readNbt). Splits it off; null tag when absent.
        bool SplitNbt(const std::string& text, std::string& stateText, std::shared_ptr<Compound>& tag,
                      std::string& error) {
            stateText = text;
            tag.reset();
            size_t brace = std::string::npos;
            int depth = 0;
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '[') ++depth;
                else if (text[i] == ']' && depth > 0) --depth;
                else if (text[i] == '{' && depth == 0) { brace = i; break; }
            }
            if (brace == std::string::npos) return true;
            stateText = text.substr(0, brace);
            tag = Snbt::ParseCompound(text.substr(brace), error);
            return tag != nullptr;
        }
    } // namespace

    bool ParseBlockFilter(const std::string& text, BlockFilter& out, std::string& error) {
        std::string stateText;
        if (!SplitNbt(text, stateText, out.nbt, error)) return false;
        return ParseBlockPredicate(stateText, out.predicate, error);
    }

    bool BlockFilter::Test(Game::World& world, const glm::ivec3& pos) const {
        if (!predicate.Test(world.GetBlockState(pos.x, pos.y, pos.z))) return false;
        if (!nbt) return true;
        // BlockPredicateArgument.TagPredicate: a block entity whose saved
        // data contains the compound.
        std::shared_ptr<Compound> saved = SaveBlockEntityTag(world, pos);
        return saved && Nbt::Compare(nbt.get(), saved.get());
    }

    bool ParseBlockInput(const std::string& text, BlockInput& out, std::string& error) {
        std::string stateText;
        std::shared_ptr<Compound> tag;
        if (!SplitNbt(text, stateText, tag, error)) return false;
        if (!stateText.empty() && stateText[0] == '#') {
            error = "Tags aren't allowed here, only actual blocks";
            return false;
        }
        // The predicate parser validates the named properties against the
        // block and keeps the list of the ones the text named — exactly the
        // two things BlockInput needs.
        BlockPredicate parsed;
        if (!ParseBlockPredicate(stateText, parsed, error)) return false;
        out.state = parsed.state;
        out.definedProperties = std::move(parsed.properties);
        out.tag = std::move(tag);
        return true;
    }

    bool BlockInput::Place(Game::World& world, const glm::ivec3& pos, uint32_t flags) const {
        // MC: `(update & 16) != 0 ? this.state : Block.updateFromNeighbourShapes(...)`,
        // falling back to the parsed state when the shapes reduce it to air.
        Game::BlockState placed = (flags & Game::World::UpdateFlags::KnownShape)
            ? state : Game::UpdateFromNeighbourShapes(world, state, pos);
        if (placed.Block() == Game::BlockID::Air) placed = state;
        // overwriteWithDefinedProperties: what the text named wins over the
        // shape pass.
        if (placed.Block() == state.Block()) {
            const Game::BlockID block = placed.Block();
            const uint16_t count = Game::BlockStates::PropertyCount(block);
            for (const auto& [name, value] : definedProperties) {
                for (uint16_t slot = 0; slot < count; ++slot) {
                    const Game::PropertyId prop = Game::BlockStates::PropertyAt(block, slot);
                    if (Game::BlockStates::PropertyName(prop) == name) {
                        placed = placed.SetName(prop, value);
                        break;
                    }
                }
            }
        }
        bool affected = world.SetBlock(pos, placed, flags, Game::World::kUpdateLimit);
        if (tag) {
            // The compound merged over what the block entity saves now
            // (BlockEntity.loadWithComponents of the given tag on top of the
            // fresh entity); a change there counts as affected even when the
            // state was already right.
            if (std::shared_ptr<Compound> before = SaveBlockEntityTag(world, pos)) {
                std::shared_ptr<Compound> merged = Nbt::CopyCompound(*before);
                Nbt::Merge(*merged, *tag);
                if (!Nbt::Equals(before.get(), merged.get()) && LoadBlockEntityTag(world, pos, *merged)) {
                    affected = true;
                }
            }
        }
        return affected;
    }

    std::shared_ptr<::World::NBTTagCompound> SaveBlockEntityTag(Game::World& world, const glm::ivec3& pos) {
        Game::BlockEntity* be = world.GetBlockEntity(pos);
        if (!be) return nullptr;
        const auto chunk = Game::Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
        Game::Nbt::Writer w;
        w.BeginRootCompound();
        auto list = w.BeginList("E", Game::Nbt::TagType::Compound);
        const bool ok = Game::Anvil::WriteBlockEntity(w, list, *be, chunk);
        w.EndList(list);
        w.EndRootCompound();
        if (!ok || !w.ok()) return nullptr;
        std::shared_ptr<Compound> root;
        try {
            root = std::dynamic_pointer_cast<Compound>(::World::NBTParser::Parse(w.Bytes()));
        } catch (const std::exception&) {
            return nullptr;
        }
        auto elements = root ? std::dynamic_pointer_cast<::World::NBTTagList>(root->GetTag("E")) : nullptr;
        if (!elements || elements->value.empty()) return nullptr;
        auto out = std::dynamic_pointer_cast<Compound>(elements->value.front());
        if (out) out->value.erase("keepPacked");
        return out;
    }

    bool LoadBlockEntityTag(Game::World& world, const glm::ivec3& pos, const ::World::NBTTagCompound& data) {
        std::shared_ptr<Compound> current = SaveBlockEntityTag(world, pos);
        if (!current) return false;
        auto merged = Nbt::CopyCompound(data);
        for (const char* key : {"x", "y", "z", "id"}) {
            if (Nbt::TagPtr v = current->GetTag(key)) merged->value[key] = v;
            else merged->value.erase(key);
        }
        const auto chunk = Game::Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
        glm::ivec3 local{};
        const Game::BlockID blockAt = world.GetBlockState(pos.x, pos.y, pos.z).Block();
        std::unique_ptr<Game::BlockEntity> rebuilt = Game::Anvil::ReadBlockEntity(*merged, chunk, blockAt, local);
        if (!rebuilt) return false;
        world.SetBlockEntity(pos, std::move(rebuilt));
        return true;
    }

} // namespace Server
