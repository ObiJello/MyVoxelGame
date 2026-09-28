// File: src/common/network/packets/game/MapItemDataS2CPacket.hpp
//
// MC ClientboundMapItemDataPacket: one map's changes for one carrier —
// the scale and locked flag, then optionally the full decoration list and
// optionally a dirty rectangle of colour bytes (MapItemSavedData.MapPatch).
// Built per player by MapItemSavedData.HoldingPlayer.nextUpdatePacket, so a
// map nobody changed costs nothing on the wire.
//
// Wire (MC's STREAM_CODEC order): VarInt map id, byte scale, bool locked,
// optional decoration list (bool present, VarInt count, per decoration:
// VarInt type registry index, byte x, byte y, byte rot, optional name), then
// the patch (byte width — 0 means none — height, startX, startY, VarInt
// length + colour bytes).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/world/map/MapItemSavedData.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace Network {

    struct MapItemDataS2CPacket {
        int32_t mapId = 0;
        int8_t  scale = 0;
        bool    locked = false;
        std::optional<std::vector<Game::Maps::MapDecoration>> decorations;
        std::optional<Game::Maps::MapPatch> colorPatch;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const MapItemDataS2CPacket& p) {
            PacketBuffer b;
            b.WriteVarInt(static_cast<uint32_t>(p.mapId));
            b.WriteByte(static_cast<uint8_t>(p.scale));
            b.WriteByte(p.locked ? 1 : 0);
            b.WriteByte(p.decorations ? 1 : 0);
            if (p.decorations) {
                b.WriteVarInt(static_cast<uint32_t>(p.decorations->size()));
                for (const auto& d : *p.decorations) {
                    b.WriteVarInt(static_cast<uint32_t>(d.type));
                    b.WriteByte(static_cast<uint8_t>(d.x));
                    b.WriteByte(static_cast<uint8_t>(d.y));
                    b.WriteByte(static_cast<uint8_t>(d.rot & 15));
                    b.WriteByte(d.name ? 1 : 0);
                    if (d.name) b.WriteString(*d.name);
                }
            }
            if (p.colorPatch && p.colorPatch->width > 0) {
                const auto& patch = *p.colorPatch;
                b.WriteByte(static_cast<uint8_t>(patch.width));
                b.WriteByte(static_cast<uint8_t>(patch.height));
                b.WriteByte(static_cast<uint8_t>(patch.startX));
                b.WriteByte(static_cast<uint8_t>(patch.startY));
                b.WriteVarInt(static_cast<uint32_t>(patch.colors.size()));
                b.WriteBytes(patch.colors);
            } else {
                b.WriteByte(0);
            }
            return b.GetData();
        }

        inline MapItemDataS2CPacket DeserializeMapItemDataS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            MapItemDataS2CPacket p;
            p.mapId  = static_cast<int32_t>(r.ReadVarInt());
            p.scale  = static_cast<int8_t>(r.ReadByte());
            p.locked = r.ReadByte() != 0;
            if (r.ReadByte() != 0) {
                const uint32_t count = r.ReadVarInt();
                if (count > 4096) throw std::runtime_error("MapItemDataS2C: too many decorations");
                std::vector<Game::Maps::MapDecoration> list;
                list.reserve(count);
                for (uint32_t i = 0; i < count; ++i) {
                    Game::Maps::MapDecoration d;
                    const uint32_t type = r.ReadVarInt();
                    if (type >= static_cast<uint32_t>(Game::Maps::DecorationType::Count)) {
                        throw std::runtime_error("MapItemDataS2C: decoration type out of range");
                    }
                    d.type = static_cast<Game::Maps::DecorationType>(type);
                    d.x    = static_cast<int8_t>(r.ReadByte());
                    d.y    = static_cast<int8_t>(r.ReadByte());
                    d.rot  = static_cast<int8_t>(r.ReadByte() & 15);
                    if (r.ReadByte() != 0) d.name = r.ReadString(32767);
                    list.push_back(std::move(d));
                }
                p.decorations = std::move(list);
            }
            const int width = r.ReadByte();
            if (width > 0) {
                Game::Maps::MapPatch patch;
                patch.width  = width;
                patch.height = r.ReadByte();
                patch.startX = r.ReadByte();
                patch.startY = r.ReadByte();
                const uint32_t length = r.ReadVarInt();
                if (length > static_cast<uint32_t>(Game::Maps::kMapSize * Game::Maps::kMapSize)) {
                    throw std::runtime_error("MapItemDataS2C: colour patch too large");
                }
                patch.colors = r.ReadBytes(length);
                p.colorPatch = std::move(patch);
            }
            return p;
        }

    } // namespace Serialization

} // namespace Network
