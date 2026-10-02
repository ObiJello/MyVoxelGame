// File: src/common/network/packets/game/AdvancementPackets.hpp
//
// The advancement packets (MC 26.3 protocol shapes, this engine's ids):
//
//   UpdateAdvancementsS2C     MC ClientboundUpdateAdvancementsPacket —
//                             reset flag; the advancements that became
//                             visible (id, parent, display with its laid-out
//                             x/y, requirements, telemetry flag); the ids that
//                             stopped being visible; progress per id; and
//                             showAdvancements (false while /advancement
//                             grants in bulk, so no toast storm).
//                             Wire: bool reset; VarInt n + n advancements;
//                             VarInt n + n ids; VarInt n + n (id, progress);
//                             bool showAdvancements.
//                             An advancement: string id; bool hasParent +
//                             string; bool hasDisplay + DisplayInfo; VarInt
//                             sets + per set VarInt n + n strings; bool
//                             sendsTelemetryEvent.
//                             DisplayInfo (DisplayInfo.STREAM_CODEC): title,
//                             description (Text components), icon (item
//                             stack), VarInt frame, int flags (1 background,
//                             2 show toast, 4 hidden, 8 announce — the last is
//                             this engine's, so the screen can tell), the
//                             background id when flag 1, float x, float y.
//
//   SelectAdvancementsTabS2C  MC ClientboundSelectAdvancementsTabPacket —
//                             the tab the server last saw opened (or none).
//                             Wire: bool has + string id.
//
//   SeenAdvancementsC2S       MC ServerboundSeenAdvancementsPacket — the
//                             screen opened a tab (action 0 + id) or closed
//                             (action 1). Wire: VarInt action [+ string id].
#pragma once

#include "common/advancements/Advancement.hpp"
#include "common/network/ItemStackSerialization.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/text/TextComponent.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Network {

    struct AdvancementEntryData {
        std::string id;
        std::optional<std::string> parent;
        std::optional<Game::Advancements::DisplayInfo> display;
        Game::Advancements::Requirements requirements;
        bool sendsTelemetryEvent = false;
    };

    struct UpdateAdvancementsS2CPacket {
        bool reset = false;
        std::vector<AdvancementEntryData> added;
        std::vector<std::string> removed;
        std::vector<std::pair<std::string, Game::Advancements::AdvancementProgress>> progress;
        bool showAdvancements = true;
    };

    struct SelectAdvancementsTabS2CPacket {
        std::optional<std::string> tab;
    };

    struct SeenAdvancementsC2SPacket {
        enum class Action : uint8_t { OpenedTab = 0, ClosedScreen = 1 };
        Action action = Action::ClosedScreen;
        std::string tab;   // OpenedTab only
    };

    namespace Serialization {

        namespace AdvancementWire {
            constexpr size_t kMaxId = 32767;
            constexpr int32_t kFlagBackground = 1;
            constexpr int32_t kFlagShowToast  = 2;
            constexpr int32_t kFlagHidden     = 4;
            constexpr int32_t kFlagAnnounce   = 8;
            constexpr uint32_t kMaxEntries    = 1u << 16;

            inline uint32_t CheckedCount(PacketReader& reader, const char* what) {
                const uint32_t n = reader.ReadVarInt();
                if (n > kMaxEntries) throw std::runtime_error(std::string("UpdateAdvancementsS2C: too many ") + what);
                return n;
            }

            inline void WriteDisplay(PacketBuffer& buffer, const Game::Advancements::DisplayInfo& d) {
                Game::Text::Write(buffer, d.title);
                Game::Text::Write(buffer, d.description);
                WriteItemStack(buffer, d.icon);
                buffer.WriteVarInt(static_cast<uint32_t>(d.type));
                int32_t flags = 0;
                if (d.background)     flags |= kFlagBackground;
                if (d.showToast)      flags |= kFlagShowToast;
                if (d.hidden)         flags |= kFlagHidden;
                if (d.announceToChat) flags |= kFlagAnnounce;
                buffer.WriteInt(static_cast<uint32_t>(flags));
                if (d.background) buffer.WriteString(*d.background);
                buffer.WriteFloat(d.x);
                buffer.WriteFloat(d.y);
            }

            inline Game::Advancements::DisplayInfo ReadDisplay(PacketReader& reader) {
                Game::Advancements::DisplayInfo d;
                d.title = Game::Text::Read(reader);
                d.description = Game::Text::Read(reader);
                d.icon = ReadItemStack(reader);
                const uint32_t frame = reader.ReadVarInt();
                d.type = frame <= 2 ? static_cast<Game::Advancements::FrameType>(frame)
                                    : Game::Advancements::FrameType::Task;   // ByIdMap ZERO
                const int32_t flags = static_cast<int32_t>(reader.ReadInt());
                if (flags & kFlagBackground) d.background = reader.ReadString(kMaxId);
                d.showToast      = (flags & kFlagShowToast) != 0;
                d.hidden         = (flags & kFlagHidden) != 0;
                d.announceToChat = (flags & kFlagAnnounce) != 0;
                d.x = reader.ReadFloat();
                d.y = reader.ReadFloat();
                return d;
            }
        } // namespace AdvancementWire

        inline std::vector<uint8_t> Serialize(const UpdateAdvancementsS2CPacket& packet) {
            using namespace AdvancementWire;
            PacketBuffer buffer;
            buffer.WriteByte(packet.reset ? 1 : 0);
            buffer.WriteVarInt(static_cast<uint32_t>(packet.added.size()));
            for (const AdvancementEntryData& a : packet.added) {
                buffer.WriteString(a.id);
                buffer.WriteByte(a.parent ? 1 : 0);
                if (a.parent) buffer.WriteString(*a.parent);
                buffer.WriteByte(a.display ? 1 : 0);
                if (a.display) WriteDisplay(buffer, *a.display);
                buffer.WriteVarInt(static_cast<uint32_t>(a.requirements.sets.size()));
                for (const auto& set : a.requirements.sets) {
                    buffer.WriteVarInt(static_cast<uint32_t>(set.size()));
                    for (const auto& name : set) buffer.WriteString(name);
                }
                buffer.WriteByte(a.sendsTelemetryEvent ? 1 : 0);
            }
            buffer.WriteVarInt(static_cast<uint32_t>(packet.removed.size()));
            for (const std::string& id : packet.removed) buffer.WriteString(id);
            buffer.WriteVarInt(static_cast<uint32_t>(packet.progress.size()));
            for (const auto& [id, progress] : packet.progress) {
                buffer.WriteString(id);
                progress.Write(buffer);
            }
            buffer.WriteByte(packet.showAdvancements ? 1 : 0);
            return buffer.GetData();
        }

        inline UpdateAdvancementsS2CPacket DeserializeUpdateAdvancementsS2C(const std::vector<uint8_t>& data) {
            using namespace AdvancementWire;
            PacketReader reader(data);
            UpdateAdvancementsS2CPacket packet;
            packet.reset = reader.ReadByte() != 0;
            const uint32_t added = CheckedCount(reader, "advancements");
            packet.added.reserve(added);
            for (uint32_t i = 0; i < added; ++i) {
                AdvancementEntryData a;
                a.id = reader.ReadString(kMaxId);
                if (reader.ReadByte() != 0) a.parent = reader.ReadString(kMaxId);
                if (reader.ReadByte() != 0) a.display = ReadDisplay(reader);
                const uint32_t sets = CheckedCount(reader, "requirement sets");
                for (uint32_t s = 0; s < sets; ++s) {
                    const uint32_t n = CheckedCount(reader, "criteria");
                    std::vector<std::string> names;
                    names.reserve(n);
                    for (uint32_t k = 0; k < n; ++k) names.push_back(reader.ReadString(kMaxId));
                    a.requirements.sets.push_back(std::move(names));
                }
                a.sendsTelemetryEvent = reader.ReadByte() != 0;
                packet.added.push_back(std::move(a));
            }
            const uint32_t removed = CheckedCount(reader, "removals");
            for (uint32_t i = 0; i < removed; ++i) packet.removed.push_back(reader.ReadString(kMaxId));
            const uint32_t progress = CheckedCount(reader, "progress entries");
            for (uint32_t i = 0; i < progress; ++i) {
                std::string id = reader.ReadString(kMaxId);
                packet.progress.emplace_back(std::move(id), Game::Advancements::AdvancementProgress::Read(reader));
            }
            packet.showAdvancements = reader.ReadByte() != 0;
            return packet;
        }

        inline std::vector<uint8_t> Serialize(const SelectAdvancementsTabS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteByte(packet.tab ? 1 : 0);
            if (packet.tab) buffer.WriteString(*packet.tab);
            return buffer.GetData();
        }

        inline SelectAdvancementsTabS2CPacket DeserializeSelectAdvancementsTabS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            SelectAdvancementsTabS2CPacket packet;
            if (reader.ReadByte() != 0) packet.tab = reader.ReadString(AdvancementWire::kMaxId);
            return packet;
        }

        inline std::vector<uint8_t> Serialize(const SeenAdvancementsC2SPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteVarInt(static_cast<uint32_t>(packet.action));
            if (packet.action == SeenAdvancementsC2SPacket::Action::OpenedTab) buffer.WriteString(packet.tab);
            return buffer.GetData();
        }

        inline SeenAdvancementsC2SPacket DeserializeSeenAdvancementsC2S(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            SeenAdvancementsC2SPacket packet;
            const uint32_t action = reader.ReadVarInt();
            if (action > 1) throw std::runtime_error("SeenAdvancementsC2S: unknown action " + std::to_string(action));
            packet.action = static_cast<SeenAdvancementsC2SPacket::Action>(action);
            if (packet.action == SeenAdvancementsC2SPacket::Action::OpenedTab) {
                packet.tab = reader.ReadString(AdvancementWire::kMaxId);
            }
            return packet;
        }

    } // namespace Serialization

} // namespace Network
