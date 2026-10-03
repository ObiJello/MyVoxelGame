// File: src/common/network/packets/game/PlayerAppearancePackets.hpp
//
// How a player looks (Game::PlayerAppearance — docs/player-appearance.md).
//
//   PlayerAppearanceC2S (0xC8)  client → server, once after LoginSuccess:
//                               this client's own look.
//   PlayerAppearanceS2C (0xC9)  server → client: one player's look — relayed
//                               to everyone else when a player sends theirs,
//                               and every present player's to a newcomer.
//
// MC carries the same thing as the profile's "textures" property (skin and
// cape URLs on textures.minecraft.net, plus the model) in
// ClientboundPlayerInfoUpdatePacket, and each client downloads the images.
// Players here have no profile server, so the images themselves travel:
// the server validates them (PlayerAppearance::Sanitize — PNG, 64x64 / 64x32
// skin, 64x32 cape, size caps) and relays the bytes as sent.
//
// Wire (both directions; S2C prefixes VarInt playerId):
//   byte   version            (4; 1 = ends after the paint, 2 = no drawing,
//                              3 = the drawing field holds the old pixel
//                              canvas — Decode converts it either way)
//   byte   mode               (AppearanceMode)
//   byte   model              (SkinModel)
//   byte   modelParts         (ModelPartBits)
//   VarInt skinLength, bytes  (PNG; 0 = the model's default skin)
//   VarInt capeLength, bytes  (PNG; 0 = no cape)
//   byte   hasPaint
//   [hasPaint] byte cellCount, cellCount bytes (PlayerColorId per cell)
//   VarInt sculptLength, bytes — version 2's 3D voxels, retired: written as
//          0, and a version-2 sender's bytes are skipped.
//   VarInt drawingLength, bytes (StickFigureDrawing::Encode — its strokes,
//          or a version-3 sender's pixel canvas; 0 = none — a drawing
//          replaces the stick figure) — trailing, version 3: absent from
//          older senders. A blob over StickFigureDrawing::
//          kMaxEncodedBytes, or one that does not decode, is skipped and the
//          look arrives without it.
// Fields may be appended after these (wire-compat: trailing additions).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/entity/PlayerAppearance.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace Network {

    struct PlayerAppearanceC2SPacket {
        Game::PlayerAppearance appearance;
    };

    struct PlayerAppearanceS2CPacket {
        uint32_t playerId = 0;
        Game::PlayerAppearance appearance;
    };

    namespace Serialization {

        inline constexpr uint8_t kPlayerAppearanceVersion = 4;

        inline void WritePlayerAppearance(PacketBuffer& buffer, const Game::PlayerAppearance& a) {
            buffer.WriteByte(kPlayerAppearanceVersion);
            buffer.WriteByte(static_cast<uint8_t>(a.mode));
            buffer.WriteByte(static_cast<uint8_t>(a.model));
            buffer.WriteByte(a.modelParts);
            buffer.WriteVarInt(static_cast<uint32_t>(a.skinPng.size()));
            if (!a.skinPng.empty()) buffer.WriteBytes(a.skinPng);
            buffer.WriteVarInt(static_cast<uint32_t>(a.capePng.size()));
            if (!a.capePng.empty()) buffer.WriteBytes(a.capePng);
            buffer.WriteByte(a.hasPaint ? 1 : 0);
            if (a.hasPaint) {
                buffer.WriteByte(static_cast<uint8_t>(Game::StickFigurePaint::kCellCount));
                buffer.WriteBytes(a.paint.cells.data(), a.paint.cells.size());
            }
            // Version 2's voxel slot, retired: always empty.
            buffer.WriteVarInt(0);
            // Version 3 on: the drawn figure (its strokes since version 4).
            const std::vector<uint8_t> drawing = a.drawing.Encode();
            buffer.WriteVarInt(static_cast<uint32_t>(drawing.size()));
            if (!drawing.empty()) buffer.WriteBytes(drawing);
        }

        // Throws (PacketReader's runtime_error) on a truncated or oversized
        // record; the caller's decode guard drops the packet. Values are NOT
        // validated here — PlayerAppearance::Sanitize does that.
        inline Game::PlayerAppearance ReadPlayerAppearance(PacketReader& reader) {
            Game::PlayerAppearance a;
            const uint8_t version = reader.ReadByte();
            if (version == 0) throw std::runtime_error("PlayerAppearance: bad version");
            a.mode       = static_cast<Game::AppearanceMode>(reader.ReadByte());
            a.model      = static_cast<Game::SkinModel>(reader.ReadByte());
            a.modelParts = reader.ReadByte();
            const uint32_t skinLength = reader.ReadVarInt();
            if (skinLength > Game::kMaxSkinPngBytes) throw std::runtime_error("PlayerAppearance: skin too large");
            if (skinLength > 0) a.skinPng = reader.ReadBytes(skinLength);
            const uint32_t capeLength = reader.ReadVarInt();
            if (capeLength > Game::kMaxCapePngBytes) throw std::runtime_error("PlayerAppearance: cape too large");
            if (capeLength > 0) a.capePng = reader.ReadBytes(capeLength);
            a.hasPaint = reader.ReadByte() != 0;
            if (a.hasPaint) {
                const uint8_t cellCount = reader.ReadByte();
                // A newer peer may paint more cells: keep the ones this
                // layout has, skip the rest.
                const size_t keep = std::min<size_t>(cellCount, a.paint.cells.size());
                for (size_t i = 0; i < cellCount; ++i) {
                    const uint8_t v = reader.ReadByte();
                    if (i < keep) a.paint.cells[i] = v;
                }
            }
            // Version 2's voxel slot (a version-1 sender stops before it):
            // retired, its bytes skipped.
            if (reader.Remaining() > 0) {
                const uint32_t sculptLength = reader.ReadVarInt();
                if (sculptLength > reader.Remaining()) throw std::runtime_error("PlayerAppearance: truncated sculpt");
                if (sculptLength > 0) (void)reader.ReadBytes(sculptLength);
            }
            // Version 3's drawing (older senders stop before it). The
            // structure is checked here, the caps by Decode; a bad blob costs
            // the drawing, never the rest of the look.
            if (reader.Remaining() > 0) {
                const uint32_t drawingLength = reader.ReadVarInt();
                if (drawingLength > reader.Remaining()) throw std::runtime_error("PlayerAppearance: truncated drawing");
                if (drawingLength > 0) {
                    const std::vector<uint8_t> bytes = reader.ReadBytes(drawingLength);
                    if (drawingLength <= Game::StickFigureDrawing::kMaxEncodedBytes) {
                        if (auto drawing = Game::StickFigureDrawing::Decode(bytes)) a.drawing = std::move(*drawing);
                    }
                }
            }
            return a;
        }

        inline std::vector<uint8_t> Serialize(const PlayerAppearanceC2SPacket& packet) {
            PacketBuffer buffer(512 + packet.appearance.skinPng.size() + packet.appearance.capePng.size() +
                                Game::StickFigureDrawing::kMaxEncodedBytes);
            WritePlayerAppearance(buffer, packet.appearance);
            return buffer.GetData();
        }

        inline PlayerAppearanceC2SPacket DeserializePlayerAppearanceC2S(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            PlayerAppearanceC2SPacket packet;
            packet.appearance = ReadPlayerAppearance(reader);
            return packet;
        }

        inline std::vector<uint8_t> Serialize(const PlayerAppearanceS2CPacket& packet) {
            PacketBuffer buffer(512 + packet.appearance.skinPng.size() + packet.appearance.capePng.size() +
                                Game::StickFigureDrawing::kMaxEncodedBytes);
            buffer.WriteVarInt(packet.playerId);
            WritePlayerAppearance(buffer, packet.appearance);
            return buffer.GetData();
        }

        inline PlayerAppearanceS2CPacket DeserializePlayerAppearanceS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            PlayerAppearanceS2CPacket packet;
            packet.playerId = reader.ReadVarInt();
            packet.appearance = ReadPlayerAppearance(reader);
            return packet;
        }

    } // namespace Serialization

} // namespace Network
