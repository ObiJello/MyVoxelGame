// File: src/server/commands/DataCommand.cpp
#include "DataCommand.hpp"
#include "CommandCoords.hpp"
#include "CommandStorage.hpp"
#include "ContextNumberProviders.hpp"
#include "EntitySelector.hpp"
#include "NbtPath.hpp"
#include "SnbtParser.hpp"
#include "../IntegratedServer.hpp"
#include "../entity/ExperienceOrbManager.hpp"
#include "../entity/ItemEntityManager.hpp"
#include "../level/ServerLevel.hpp"
#include "../entity/ServerLevelBridge.hpp"
#include "../network/ServerConnection.hpp"
#include "../world/storage/anvil/BlockEntityNbt.hpp"
#include "../world/storage/anvil/EntityNbt.hpp"

#include "common/core/Log.hpp"
#include "common/entity/ExperienceOrb.hpp"
#include "common/entity/ItemEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/level/World.hpp"
#include "common/world/math/WorldCoordinates.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace Server {

    namespace {

        using Nbt::TagPtr;
        using Compound = ::World::NBTTagCompound;

        // ── Output ──────────────────────────────────────────────────────────

        void Fail(ServerConnection& connection, const std::string& text) {
            Network::ChatMessageS2CPacket packet;
            packet.senderId = 0;
            packet.position = 1;
            Network::ChatSegmentData seg;
            seg.text = text;
            seg.color = 0xFFFF5555u;   // MC sendFailure: red
            packet.segments.push_back(std::move(seg));
            connection.SendChatMessage(packet);
        }

        // "<before><pretty NBT><after>" as one chat line.
        // A query's success line (MC sendSuccess(..., false)).
        void SendWithTag(const CommandSourceStack& source, ServerConnection& connection,
                         const std::string& before, const ::World::NBTTag& tag,
                         const std::string& after = {}) {
            Network::ChatMessageS2CPacket packet;
            packet.senderId = 0;
            packet.position = 1;
            Network::ChatSegmentData head;
            head.text = before;
            packet.segments.push_back(std::move(head));
            Nbt::AppendPretty(tag, packet.segments);
            if (!after.empty()) {
                Network::ChatSegmentData tail;
                tail.text = after;
                packet.segments.push_back(std::move(tail));
            }
            source.SendSuccess(connection, packet, false);
        }

        std::string Scale2(double scale) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.2f", scale);
            return buf;
        }

        // ── Accessors (MC DataAccessor) ─────────────────────────────────────

        struct Accessor {
            enum class Kind : uint8_t { Entity, Block, Storage } kind = Kind::Entity;
            SelectedEntity entity;                 // Entity
            glm::ivec3 pos{0};                     // Block
            Game::World* world = nullptr;          // Block
            std::string storageId;                 // Storage

            // MC getData: a fresh compound to read or edit.
            bool GetData(std::shared_ptr<Compound>& out, std::string& error) const {
                switch (kind) {
                    case Kind::Entity:
                        out = SavedEntityNbt(entity);
                        if (!out) { error = "That entity no longer exists"; return false; }
                        return true;
                    case Kind::Block: {
                        Game::BlockEntity* be = world ? world->GetBlockEntity(pos) : nullptr;
                        if (!be) { error = "The target block is not a block entity"; return false; }
                        const auto chunk = Game::Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
                        Game::Nbt::Writer w;
                        w.BeginRootCompound();
                        auto list = w.BeginList("E", Game::Nbt::TagType::Compound);
                        const bool ok = Game::Anvil::WriteBlockEntity(w, list, *be, chunk);
                        w.EndList(list);
                        w.EndRootCompound();
                        std::shared_ptr<Compound> root;
                        if (ok && w.ok()) {
                            try {
                                root = std::dynamic_pointer_cast<Compound>(::World::NBTParser::Parse(w.Bytes()));
                            } catch (const std::exception&) {}
                        }
                        auto elements = root ? std::dynamic_pointer_cast<::World::NBTTagList>(root->GetTag("E")) : nullptr;
                        if (!elements || elements->value.empty()) { error = "The target block is not a block entity"; return false; }
                        out = std::dynamic_pointer_cast<Compound>(elements->value.front());
                        if (!out) { error = "The target block is not a block entity"; return false; }
                        out->value.erase("keepPacked");
                        return true;
                    }
                    case Kind::Storage:
                        out = CommandStorage::Get(storageId);
                        return true;
                }
                return false;
            }

            // MC setData.
            bool SetData(const Compound& tag, std::string& error) const {
                switch (kind) {
                    case Kind::Entity: return SetEntity(tag, error);
                    case Kind::Block:  return SetBlock(tag, error);
                    case Kind::Storage:
                        CommandStorage::Set(storageId, tag);
                        return true;
                }
                return false;
            }

            // MC EntityDataAccessor.setData: never a player; load, then put the
            // entity's own UUID back.
            bool SetEntity(const Compound& tag, std::string& error) const {
                if (!g_integratedServer) return false;
                ServerLevel* level = g_integratedServer->GetLevel(entity.dimension);
                switch (entity.kind) {
                    case SelectedEntity::Kind::Player:
                        error = "Unable to modify player data";
                        return false;
                    case SelectedEntity::Kind::Mob: {
                        Game::Mob* mob = entity.mob;
                        if (!mob || mob->IsRemoved()) { error = "That entity no longer exists"; return false; }
                        const Game::Uuid uuid = mob->GetUuid();
                        Game::Anvil::ApplyMobNbt(tag, *mob);
                        mob->SetUuid(uuid);
                        mob->needsSync = true;
                        return true;
                    }
                    case SelectedEntity::Kind::Item: {
                        ItemEntityManager* items = level ? level->Items() : nullptr;
                        Game::ItemEntity* live = items ? items->Find(entity.id) : nullptr;
                        if (!live) { error = "That entity no longer exists"; return false; }
                        Game::ItemEntity loaded = *live;
                        if (!Game::Anvil::ReadItem(tag, loaded)) {
                            // An emptied stack: MC's ItemEntity with an empty
                            // stack discards itself.
                            live->stack.Clear();
                            return true;
                        }
                        loaded.id = live->id;
                        loaded.uuid = live->uuid;
                        loaded.pendingSpawn = true;   // a full resend: the stack may differ
                        loaded.needsSync = true;
                        *live = std::move(loaded);
                        return true;
                    }
                    case SelectedEntity::Kind::Orb: {
                        ExperienceOrbManager* orbs = level ? level->Orbs() : nullptr;
                        if (!orbs) { error = "That entity no longer exists"; return false; }
                        auto it = orbs->AllMutable().find(entity.id);
                        if (it == orbs->AllMutable().end()) { error = "That entity no longer exists"; return false; }
                        Game::ExperienceOrb loaded = it->second;
                        if (!Game::Anvil::ReadOrb(tag, loaded)) { it->second.age = Game::ExperienceOrb::kLifetimeTicks; return true; }
                        loaded.id = it->second.id;
                        loaded.uuid = it->second.uuid;
                        loaded.pendingSpawn = true;
                        loaded.needsSync = true;
                        it->second = std::move(loaded);
                        return true;
                    }
                }
                return false;
            }

            // MC BlockDataAccessor.setData: the block entity loads the compound
            // (its own position and type kept) and is resent.
            bool SetBlock(const Compound& tag, std::string& error) const {
                Game::BlockEntity* be = world ? world->GetBlockEntity(pos) : nullptr;
                if (!be) { error = "The target block is not a block entity"; return false; }
                std::shared_ptr<Compound> current;
                if (!GetData(current, error)) return false;
                auto data = Nbt::CopyCompound(tag);
                for (const char* key : {"x", "y", "z", "id"}) {
                    if (TagPtr v = current->GetTag(key)) data->value[key] = v;
                    else data->value.erase(key);
                }
                const auto chunk = Game::Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
                glm::ivec3 local{};
                const Game::BlockID blockAt = world->GetBlockState(pos.x, pos.y, pos.z).Block();
                std::unique_ptr<Game::BlockEntity> rebuilt = Game::Anvil::ReadBlockEntity(*data, chunk, blockAt, local);
                if (!rebuilt) { error = "The target block is not a block entity"; return false; }
                world->SetBlockEntity(pos, std::move(rebuilt));
                return true;
            }

            // MC getModifiedSuccess / getPrintSuccess texts.
            std::string Modified() const {
                switch (kind) {
                    case Kind::Entity: return "Modified entity data of " + entity.name;
                    case Kind::Block:
                        return "Modified block data of " + std::to_string(pos.x) + ", " + std::to_string(pos.y) + ", " +
                               std::to_string(pos.z);
                    case Kind::Storage: return "Modified storage " + storageId;
                }
                return {};
            }
            std::string QueryPrefix() const {
                switch (kind) {
                    case Kind::Entity: return entity.name + " has the following entity data: ";
                    case Kind::Block:
                        return std::to_string(pos.x) + ", " + std::to_string(pos.y) + ", " + std::to_string(pos.z) +
                               " has the following block data: ";
                    case Kind::Storage: return "Storage " + storageId + " has the following contents: ";
                }
                return {};
            }
            std::string Scaled(const std::string& path, double scale, int value) const {
                switch (kind) {
                    case Kind::Entity:
                        return path + " on " + entity.name + " after scale factor of " + Scale2(scale) + " is " + std::to_string(value);
                    case Kind::Block:
                        return path + " on block " + std::to_string(pos.x) + ", " + std::to_string(pos.y) + ", " +
                               std::to_string(pos.z) + " after scale factor of " + Scale2(scale) + " is " + std::to_string(value);
                    case Kind::Storage:
                        return path + " in storage " + storageId + " after scale factor of " + Scale2(scale) + " is " +
                               std::to_string(value);
                }
                return {};
            }
        };

        // `block <pos>` | `entity <target>` | `storage <id>` from args[i…].
        bool ParseAccessor(const std::vector<std::string>& args, size_t& i, const CommandSourceStack& source,
                           Accessor& out, std::string& error) {
            if (i >= args.size()) { error = "Unknown or incomplete command, see below for error"; return false; }
            const std::string kind = args[i];
            if (kind == "entity") {
                if (i + 1 >= args.size()) { error = "Expected an entity"; return false; }
                std::vector<SelectedEntity> found;
                if (!ResolveSelector(args[i + 1], SelectorKind::Entity, source, found, error)) return false;
                out.kind = Accessor::Kind::Entity;
                out.entity = found.front();
                i += 2;
                return true;
            }
            if (kind == "block") {
                if (i + 3 >= args.size()) { error = "Expected a block position"; return false; }
                glm::ivec3 pos;
                if (!ParseBlockPos(args[i + 1], args[i + 2], args[i + 3], source, source.rotation, pos, error)) return false;
                ServerLevel* level = g_integratedServer ? g_integratedServer->GetLevel(source.dimension) : nullptr;
                Game::World* world = level ? level->World() : nullptr;
                // MC BlockPosArgument.getLoadedBlockPos.
                if (!world || !world->IsChunkLoaded(pos.x >> 4, pos.z >> 4)) { error = "That position is not loaded"; return false; }
                if (!world->IsValidPosition(pos.x, pos.y, pos.z)) { error = "That position is out of this world!"; return false; }
                if (!world->GetBlockEntity(pos)) { error = "The target block is not a block entity"; return false; }
                out.kind = Accessor::Kind::Block;
                out.pos = pos;
                out.world = world;
                i += 4;
                return true;
            }
            if (kind == "storage") {
                if (i + 1 >= args.size()) { error = "Expected a storage id"; return false; }
                if (!CommandStorage::NormalizeId(args[i + 1], out.storageId)) {
                    error = "Invalid ID: " + args[i + 1];
                    return false;
                }
                out.kind = Accessor::Kind::Storage;
                i += 2;
                return true;
            }
            error = "Incorrect argument for command: expected block, entity or storage";
            return false;
        }

        bool ParseInt(const std::string& text, int& out) {
            if (text.empty()) return false;
            char* end = nullptr;
            const long v = std::strtol(text.c_str(), &end, 10);
            if (!end || *end != '\0') return false;
            out = static_cast<int>(v);
            return true;
        }

        // DataCommands.getAsText: a string's value, a number's SNBT.
        bool AsText(const ::World::NBTTag& tag, std::string& out, std::string& error) {
            if (tag.type == ::World::NBTTagType::TAG_String) {
                out = static_cast<const ::World::NBTTagString&>(tag).value;
                return true;
            }
            if (Nbt::IsNumeric(tag)) {
                out = Nbt::ToSnbt(tag);
                return true;
            }
            error = "Expected a value: got " + Nbt::ToSnbt(tag);
            return false;
        }

        // DataCommands.substring with getOffset's negative indices.
        bool Substring(const std::string& input, std::optional<int> start, std::optional<int> end, std::string& out,
                       std::string& error) {
            const int length = static_cast<int>(input.size());
            const auto offset = [&](int index) { return index >= 0 ? index : length + index; };
            const int s = start ? offset(*start) : 0;
            const int e = end ? offset(*end) : length;
            if (s < 0 || e > length || s > e) {
                error = "Invalid substring indices: " + std::to_string(s) + " to " + std::to_string(e);
                return false;
            }
            out = input.substr(static_cast<size_t>(s), static_cast<size_t>(e - s));
            return true;
        }

        // LootContextSources: `default` | `block <computePos>` | `entity
        // <computeTarget>` from args[i…], into the providers' LootContext
        // (THIS_ENTITY = the source's entity, ORIGIN = its position).
        Game::Entity* LiveEntity(const SelectedEntity& e) {
            if (e.kind == SelectedEntity::Kind::Mob) return e.mob;
            if (e.kind == SelectedEntity::Kind::Player && g_integratedServer) {
                return g_integratedServer->GetPlayerEntityView(static_cast<uint32_t>(e.id));
            }
            return nullptr;   // items and orbs are not Entities here
        }

        bool ComputeContext(const CommandSourceStack& source, const std::vector<std::string>& args, size_t& i,
                            NumberProviders::Context& ctx, std::string& error) {
            ServerLevel* level = g_integratedServer ? g_integratedServer->GetLevel(source.dimension) : nullptr;
            if (!level || !level->MobLevel()) { error = "That dimension is not loaded"; return false; }
            ctx.level = level->MobLevel();
            ctx.blocks = level->MobLevel()->Blocks();
            ctx.random = &level->MobLevel()->Random();
            ctx.origin = source.position;
            if (source.entity) ctx.thisEntity = LiveEntity(*source.entity);
            if (i >= args.size()) { error = "Unknown or incomplete command, see below for error"; return false; }
            const std::string kind = args[i];
            if (kind == "default") {
                ++i;
                return true;
            }
            if (kind == "block") {
                if (i + 3 >= args.size()) { error = "Expected a block position"; return false; }
                glm::ivec3 pos;
                if (!ParseBlockPos(args[i + 1], args[i + 2], args[i + 3], source, source.rotation, pos, error)) return false;
                Game::World* world = level->World();
                if (!world || !world->IsChunkLoaded(pos.x >> 4, pos.z >> 4)) { error = "That position is not loaded"; return false; }
                if (!world->IsValidPosition(pos.x, pos.y, pos.z)) { error = "That position is out of this world!"; return false; }
                ctx.hasBlockState = true;
                ctx.blockState = world->GetBlockState(pos.x, pos.y, pos.z);
                i += 4;
                return true;
            }
            if (kind == "entity") {
                if (i + 1 >= args.size()) { error = "Expected an entity"; return false; }
                std::vector<SelectedEntity> found;
                if (!ResolveSelector(args[i + 1], SelectorKind::Entity, source, found, error)) return false;
                ctx.targetEntity = LiveEntity(found.front());
                i += 2;
                return true;
            }
            error = "Incorrect argument for command: expected default, block or entity";
            return false;
        }

        constexpr const char* kUnchanged = "Nothing changed. The specified properties already have these values";

        // ── The subcommands ─────────────────────────────────────────────────

        void Get(const CommandSourceStack& source, const std::vector<std::string>& args, ServerConnection& connection) {
            size_t i = 1;
            Accessor target;
            std::string error;
            if (!ParseAccessor(args, i, source, target, error)) { Fail(connection, error); return; }
            std::shared_ptr<Compound> data;
            if (!target.GetData(data, error)) { Fail(connection, error); return; }
            if (i >= args.size()) {
                SendWithTag(source, connection, target.QueryPrefix(), *data);
                return;
            }
            Nbt::Path path;
            if (!Nbt::Path::Parse(args[i], path, error)) { Fail(connection, error); return; }
            std::vector<TagPtr> tags;
            if (!path.Get(data, tags, error)) { Fail(connection, error); return; }
            if (tags.size() > 1) { Fail(connection, "This argument accepts a single NBT value"); return; }
            const TagPtr& tag = tags.front();
            if (i + 1 >= args.size()) {
                SendWithTag(source, connection, target.QueryPrefix(), *tag);
                return;
            }
            char* end = nullptr;
            const double scale = std::strtod(args[i + 1].c_str(), &end);
            if (!end || *end != '\0' || args[i + 1].empty()) { Fail(connection, "Invalid double '" + args[i + 1] + "'"); return; }
            if (i + 2 < args.size()) { Fail(connection, "Incorrect argument for command"); return; }
            if (!Nbt::IsNumeric(*tag)) {
                Fail(connection, "Can't get " + path.Text() + "; only numeric tags are allowed");
                return;
            }
            const int value = static_cast<int>(std::floor(Nbt::NumericValue(*tag) * scale));
            source.SendSuccess(connection, target.Scaled(path.Text(), scale, value), false);
        }

        void MergeCmd(const CommandSourceStack& source, const std::vector<std::string>& args, ServerConnection& connection) {
            size_t i = 1;
            Accessor target;
            std::string error;
            if (!ParseAccessor(args, i, source, target, error)) { Fail(connection, error); return; }
            if (i >= args.size()) { Fail(connection, "Expected compound tag"); return; }
            std::string text = args[i];
            for (size_t k = i + 1; k < args.size(); ++k) text += " " + args[k];
            auto nbt = Snbt::ParseCompound(text, error);
            if (!nbt) { Fail(connection, error); return; }
            std::shared_ptr<Compound> old;
            if (!target.GetData(old, error)) { Fail(connection, error); return; }
            if (Nbt::IsTooDeep(*nbt, 0)) { Fail(connection, "Resulting NBT too deeply nested"); return; }
            auto result = Nbt::CopyCompound(*old);
            Nbt::Merge(*result, *nbt);
            if (Nbt::Equals(old.get(), result.get())) { Fail(connection, kUnchanged); return; }
            if (!target.SetData(*result, error)) { Fail(connection, error); return; }
            source.SendSuccess(connection, target.Modified(), true);
        }

        void RemoveCmd(const CommandSourceStack& source, const std::vector<std::string>& args, ServerConnection& connection) {
            size_t i = 1;
            Accessor target;
            std::string error;
            if (!ParseAccessor(args, i, source, target, error)) { Fail(connection, error); return; }
            if (i >= args.size()) { Fail(connection, "Expected an NBT path"); return; }
            Nbt::Path path;
            if (!Nbt::Path::Parse(args[i], path, error)) { Fail(connection, error); return; }
            if (i + 1 < args.size()) { Fail(connection, "Incorrect argument for command"); return; }
            std::shared_ptr<Compound> data;
            if (!target.GetData(data, error)) { Fail(connection, error); return; }
            if (path.Remove(data) == 0) { Fail(connection, kUnchanged); return; }
            if (!target.SetData(*data, error)) { Fail(connection, error); return; }
            source.SendSuccess(connection, target.Modified(), true);
        }

        void ModifyCmd(const CommandSourceStack& source, const std::vector<std::string>& args, ServerConnection& connection) {
            size_t i = 1;
            Accessor target;
            std::string error;
            if (!ParseAccessor(args, i, source, target, error)) { Fail(connection, error); return; }
            if (i >= args.size()) { Fail(connection, "Expected an NBT path"); return; }
            Nbt::Path targetPath;
            if (!Nbt::Path::Parse(args[i], targetPath, error)) { Fail(connection, error); return; }
            ++i;
            if (i >= args.size()) { Fail(connection, "Unknown or incomplete command, see below for error"); return; }
            const std::string op = args[i++];
            int insertIndex = 0;
            if (op == "insert") {
                if (i >= args.size() || !ParseInt(args[i], insertIndex)) { Fail(connection, "Expected integer"); return; }
                ++i;
            } else if (op != "append" && op != "prepend" && op != "set" && op != "merge") {
                Fail(connection, "Incorrect argument for command: expected append, insert, merge, prepend or set");
                return;
            }

            // The source values (MC: from / string / value).
            if (i >= args.size()) { Fail(connection, "Unknown or incomplete command, see below for error"); return; }
            const std::string how = args[i++];
            std::vector<TagPtr> sourceTags;
            if (how == "value") {
                if (i >= args.size()) { Fail(connection, "Expected value"); return; }
                std::string text = args[i];
                for (size_t k = i + 1; k < args.size(); ++k) text += " " + args[k];
                TagPtr value = Snbt::ParseValue(text, error);
                if (!value) { Fail(connection, error); return; }
                sourceTags.push_back(std::move(value));
            } else if (how == "from" || how == "string") {
                Accessor src;
                if (!ParseAccessor(args, i, source, src, error)) { Fail(connection, error); return; }
                std::shared_ptr<Compound> srcData;
                if (!src.GetData(srcData, error)) { Fail(connection, error); return; }
                if (i < args.size()) {
                    Nbt::Path sourcePath;
                    if (!Nbt::Path::Parse(args[i], sourcePath, error)) { Fail(connection, error); return; }
                    ++i;
                    if (!sourcePath.Get(srcData, sourceTags, error)) { Fail(connection, error); return; }
                } else {
                    sourceTags.push_back(srcData);
                }
                if (how == "string") {
                    std::optional<int> start, end;
                    if (i < args.size()) {
                        int v = 0;
                        if (!ParseInt(args[i++], v)) { Fail(connection, "Expected integer"); return; }
                        start = v;
                    }
                    if (i < args.size()) {
                        int v = 0;
                        if (!ParseInt(args[i++], v)) { Fail(connection, "Expected integer"); return; }
                        end = v;
                    }
                    std::vector<TagPtr> strings;
                    for (const TagPtr& t : sourceTags) {
                        std::string text, cut;
                        if (!AsText(*t, text, error)) { Fail(connection, error); return; }
                        if (start || end) {
                            if (!Substring(text, start, end, cut, error)) { Fail(connection, error); return; }
                        } else {
                            cut = text;
                        }
                        strings.push_back(std::make_shared<::World::NBTTagString>(cut));
                    }
                    sourceTags = std::move(strings);
                }
                if (i < args.size()) { Fail(connection, "Incorrect argument for command"); return; }
            } else if (how == "compute") {
                // MC LootContextSources.addContextSources: the LootContext a
                // context number provider reads — the source's entity and
                // position, plus a block (computePos) or an entity
                // (computeTarget) — then `float <provider>` / `integer
                // <provider>` (ResourceOrIdArgument over the provider
                // registries).
                NumberProviders::Context ctx;
                if (!ComputeContext(source, args, i, ctx, error)) { Fail(connection, error); return; }
                if (i + 1 >= args.size() || (args[i] != "float" && args[i] != "integer")) {
                    Fail(connection, "Incorrect argument for command: expected float or integer");
                    return;
                }
                const bool integer = args[i] == "integer";
                std::string text = args[i + 1];
                for (size_t k = i + 2; k < args.size(); ++k) text += " " + args[k];
                i = args.size();
                if (integer) {
                    NumberProviders::Provider provider = NumberProviders::ParseIntArgument(text, error);
                    if (!provider) { Fail(connection, error); return; }
                    sourceTags.push_back(std::make_shared<::World::NBTTagInt>(NumberProviders::GetInt(provider, ctx)));
                } else {
                    NumberProviders::Provider provider = NumberProviders::ParseFloatArgument(text, error);
                    if (!provider) { Fail(connection, error); return; }
                    sourceTags.push_back(std::make_shared<::World::NBTTagFloat>(NumberProviders::GetFloat(provider, ctx)));
                }
            } else {
                Fail(connection, "Incorrect argument for command: expected compute, from, string or value");
                return;
            }

            std::shared_ptr<Compound> data;
            if (!target.GetData(data, error)) { Fail(connection, error); return; }
            int changed = 0;
            if (op == "set") {
                if (sourceTags.empty()) { Fail(connection, kUnchanged); return; }
                if (!targetPath.Set(data, *sourceTags.back(), changed, error)) { Fail(connection, error); return; }
            } else if (op == "merge") {
                auto combined = std::make_shared<Compound>();
                for (const TagPtr& t : sourceTags) {
                    if (Nbt::IsTooDeep(*t, 0)) { Fail(connection, "Resulting NBT too deeply nested"); return; }
                    if (t->type != ::World::NBTTagType::TAG_Compound) {
                        Fail(connection, "Expected an object: got " + Nbt::ToSnbt(*t));
                        return;
                    }
                    Nbt::Merge(*combined, static_cast<const Compound&>(*t));
                }
                std::vector<TagPtr> targets;
                if (!targetPath.GetOrCreate(data, [] { return std::make_shared<Compound>(); }, targets, error)) {
                    Fail(connection, error);
                    return;
                }
                for (const TagPtr& t : targets) {
                    if (!t || t->type != ::World::NBTTagType::TAG_Compound) {
                        Fail(connection, "Expected an object: got " + (t ? Nbt::ToSnbt(*t) : std::string()));
                        return;
                    }
                    auto& object = static_cast<Compound&>(*t);
                    auto original = Nbt::CopyCompound(object);
                    Nbt::Merge(object, *combined);
                    if (!Nbt::Equals(original.get(), &object)) ++changed;
                }
            } else {
                const int index = op == "prepend" ? 0 : op == "append" ? -1 : insertIndex;
                if (!targetPath.Insert(index, data, sourceTags, changed, error)) { Fail(connection, error); return; }
            }
            if (changed == 0) { Fail(connection, kUnchanged); return; }
            if (!target.SetData(*data, error)) { Fail(connection, error); return; }
            source.SendSuccess(connection, target.Modified(), true);
        }

    } // namespace

    void DataCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        using Cmd::Arg;
        // MC DataCommands' tree: each subcommand over the three accessors
        // (ArgProvider: block <targetPos> | entity <target> | storage <target>).
        const auto accessors = [](const std::string& prefix, const std::function<void(Cmd::Node&)>& decorate) {
            Cmd::Node block = Cmd::Argument(prefix + "Pos", Arg::BlockPos);
            Cmd::Node entity = Cmd::Argument(prefix, Arg::Entity);
            Cmd::Node storage = Cmd::Argument(prefix, Arg::StorageId);
            decorate(block);
            decorate(entity);
            decorate(storage);
            return std::vector<Cmd::Node>{
                Cmd::Literal("block").Then(std::move(block)),
                Cmd::Literal("entity").Then(std::move(entity)),
                Cmd::Literal("storage").Then(std::move(storage)),
            };
        };
        Cmd::Node root = Cmd::Root();
        root.Then(Cmd::Literal("get").Then(accessors("target", [](Cmd::Node& n) {
            n.Executes().Then(Cmd::Argument("path", Arg::NbtPath).Executes()
                .Then(Cmd::Argument("scale", Arg::Float).Suggests({"1", "0.5", "100"}).Executes()));
        })));
        root.Then(Cmd::Literal("merge").Then(accessors("target", [](Cmd::Node& n) {
            n.Then(Cmd::Argument("nbt", Arg::Nbt).Executes());
        })));
        root.Then(Cmd::Literal("remove").Then(accessors("target", [](Cmd::Node& n) {
            n.Then(Cmd::Argument("path", Arg::NbtPath).Executes());
        })));
        // modify: <target> <targetPath> <operation> <source>
        const auto sources = [&]() {
            std::vector<Cmd::Node> out;
            out.push_back(Cmd::Literal("from").Then(accessors("source", [](Cmd::Node& n) {
                n.Executes().Then(Cmd::Argument("sourcePath", Arg::NbtPath).Executes());
            })));
            out.push_back(Cmd::Literal("string").Then(accessors("source", [](Cmd::Node& n) {
                n.Executes().Then(Cmd::Argument("sourcePath", Arg::NbtPath).Executes()
                    .Then(Cmd::Argument("start", Arg::Integer).Suggests({"0", "1", "-1"}).Executes()
                        .Then(Cmd::Argument("end", Arg::Integer).Suggests({"-1", "5"}).Executes())));
            })));
            out.push_back(Cmd::Literal("value").Then(Cmd::Argument("value", Arg::NbtTag).Executes()));
            // compute (default | block <computePos> | entity <computeTarget>)
            //         (float <provider> | integer <provider>)
            const auto numbers = [] {
                return std::vector<Cmd::Node>{
                    Cmd::Literal("float").Then(Cmd::Argument("provider", Arg::FloatProvider).Executes()),
                    Cmd::Literal("integer").Then(Cmd::Argument("provider", Arg::IntProvider).Executes()),
                };
            };
            out.push_back(Cmd::Literal("compute")
                .Then(Cmd::Literal("default").Then(numbers()))
                .Then(Cmd::Literal("block").Then(Cmd::Argument("computePos", Arg::BlockPos).Then(numbers())))
                .Then(Cmd::Literal("entity").Then(Cmd::Argument("computeTarget", Arg::Entity).Then(numbers()))));
            return out;
        };
        root.Then(Cmd::Literal("modify").Then(accessors("target", [&](Cmd::Node& n) {
            Cmd::Node path = Cmd::Argument("targetPath", Arg::NbtPath);
            path.Then(Cmd::Literal("append").Then(sources()));
            path.Then(Cmd::Literal("insert").Then(Cmd::Argument("index", Arg::Integer).Suggests({"0", "-1"}).Then(sources())));
            path.Then(Cmd::Literal("merge").Then(sources()));
            path.Then(Cmd::Literal("prepend").Then(sources()));
            path.Then(Cmd::Literal("set").Then(sources()));
            n.Then(std::move(path));
        })));
        dispatcher.RegisterCommand("data", DataCommand::Execute, std::move(root));
    }

    void DataCommand::Execute(const CommandSourceStack& source,
                              const std::vector<std::string>& args,
                              ServerConnection& connection,
                              PlayerSessionManager& /*sessionManager*/) {
        const std::string sub = args.empty() ? std::string() : args[0];
        if (sub == "get")    { Get(source, args, connection); return; }
        if (sub == "merge")  { MergeCmd(source, args, connection); return; }
        if (sub == "remove") { RemoveCmd(source, args, connection); return; }
        if (sub == "modify") { ModifyCmd(source, args, connection); return; }
        Fail(connection, "Unknown or incomplete command, see below for error");
        connection.SendChatMessage("/data (get|merge|modify|remove) (block <pos>|entity <target>|storage <id>) ...", 1);
    }

} // namespace Server
