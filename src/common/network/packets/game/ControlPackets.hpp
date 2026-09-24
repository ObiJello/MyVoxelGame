// File: src/common/network/packets/game/ControlPackets.hpp
//
// /control <player>: one player's client drives another player's client.
//
// The model is possession by INPUT, not by state. The controlled client keeps
// running its own game exactly as before — physics, block breaking, the
// inventory screen, chat — but its Input layer is fed by the controller's
// raw key, mouse-button, character, scroll and mouse-motion events instead of
// its own window's. Everything the controlled player "does" is therefore done
// by their own client and reaches the server through the packets it already
// sends, so the world and every other player see them as themselves.
//
// The controller sees what the controlled player sees: their client streams
// its final camera pose back (ControlView), the server tees the controlled
// player's inventory, container and stat packets to the controller
// (ServerConnection::SendPacket's mirror), and the controller's client runs
// the same screen code on the same input in step.
//
// Three packets:
//   ControlS2C          — role assignment (start / stop) to both sides.
//   ControlInput        — the controller's input for one frame, C2S from the
//                         controller and relayed S2C to the controlled client
//                         unchanged (one struct, both directions).
//   ControlView         — the controlled client's camera and screen state for
//                         one frame, C2S from it and relayed S2C to the
//                         controller unchanged.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace Network {

    enum class ControlRole : uint8_t {
        None       = 0,   // control ended (or never started)
        Controller = 1,   // this client drives `otherId`
        Controlled = 2,   // this client is driven by `otherId`
        // A player carried as an item (/morph item) sees the holder's HUD:
        // the Watched side streams its view frames, the HudMirror side takes
        // the hotbar selection, hand and stats from them (its camera stays
        // its own), and the server tees the holder's inventory and stats to
        // it exactly as for a controller.
        HudMirror  = 3,
        Watched    = 4,
    };

    struct ControlS2CPacket {
        ControlRole role = ControlRole::None;
        uint32_t    otherId = 0;
        std::string otherName;
    };

    // One frame of the controller's input. Events are GLFW-level so the
    // controlled client can push them through the same code its own window
    // callbacks use; the key codes are interpreted with the CONTROLLED
    // client's key bindings.
    struct ControlInputPacket {
        struct KeyEvent    { int16_t key = 0;    uint8_t action = 0; uint8_t mods = 0; };
        struct ButtonEvent { uint8_t button = 0; uint8_t action = 0; uint8_t mods = 0; };

        // Mouse motion this frame, in the controller's window pixels, dy
        // already flipped so up is positive (Input::GetMouseDelta's form).
        float mouseDx = 0.0f;
        float mouseDy = 0.0f;
        // Raw scroll offsets (the controlled client applies its own scroll
        // settings, as its callback would).
        float scrollX = 0.0f;
        float scrollY = 0.0f;
        // The shared GUI cursor, in GUI pixels. `cursorRef` says relative to
        // what: 0 = the screen centre (chat), 1 = the open container panel's
        // top-left corner (LeftPos/TopPos), so the same slot is under it on
        // both clients whatever their window size and GUI scale.
        bool    cursorValid = false;
        uint8_t cursorRef = 0;
        float   cursorX = 0.0f;
        float   cursorY = 0.0f;
        // Every key / button down is released (the controller opened its own
        // menu, or stopped controlling).
        bool releaseAll = false;

        std::vector<KeyEvent>    keys;
        std::vector<ButtonEvent> buttons;
        std::vector<uint32_t>    chars;
    };

    // Which client-local screen the controlled client has up. Container
    // screens opened BY THE SERVER (chest, furnace, crafting table) reach the
    // controller through the teed OpenScreenS2C; these are the ones the
    // client opens on its own.
    enum class ControlScreen : uint8_t {
        None      = 0,
        Survival  = 1,   // InventoryScreen (E)
        Creative  = 2,   // CreativeModeInventoryScreen (E in creative)
        Container = 3,   // a server-opened container screen
        Chat      = 4,
    };

    struct ControlViewPacket {
        glm::dvec3 cameraPos{0.0};      // the FINAL camera (after F5 pull-in)
        float      yaw = 0.0f;
        float      pitch = 0.0f;
        float      fov = 70.0f;
        uint8_t    perspective = 0;     // Render::Perspective
        glm::dvec3 playerPos{0.0};      // feet, for mesh scheduling
        int8_t     dimension = 0;
        ControlScreen screen = ControlScreen::None;
        std::string chatText;           // the chat input line while Chat
        bool       statsHidden = false; // creative / spectator HUD
        bool       eyeInWater = false;
        // The hotbar selection is client-authoritative (HeldItemChangeC2S is
        // never echoed), so the controller's HUD and hand follow it from here.
        uint8_t    selectedSlot = 0;
        // The sender's steady clock, ms (low 32 bits): the controller
        // plays frames back on THIS clock, a little behind, so network
        // jitter and rate mismatch never reach the picture.
        uint32_t   sendTimeMs = 0;
        // What the crosshair is on: the block outline and the crack overlay
        // are drawn from these on the controller (its own aim is elsewhere).
        bool       hitValid = false;
        glm::ivec3 hitPos{0};
        bool       entityPicked = false;   // a mob under the crosshair: no outline
        int8_t     breakStage = -1;        // -1 = not breaking
        glm::ivec3 breakPos{0};
        // The first-person hand: a swing since the last frame, the
        // hold-to-use pose, and the walk distance the bob runs on.
        bool       swing = false;
        bool       usingItem = false;
        uint8_t    usingHand = 0;
        uint8_t    useAnim = 0;
        int32_t    useItemRemaining = 0;
        int32_t    useItemDuration = 0;
        float      walkDist = 0.0f;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const ControlS2CPacket& p) {
            PacketBuffer b;
            b.WriteByte(static_cast<uint8_t>(p.role));
            b.WriteVarInt(p.otherId);
            b.WriteString(p.otherName);
            return b.GetData();
        }
        inline ControlS2CPacket DeserializeControlS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            ControlS2CPacket p;
            p.role      = static_cast<ControlRole>(r.ReadByte());
            p.otherId   = r.ReadVarInt();
            p.otherName = r.ReadString();
            return p;
        }

        inline std::vector<uint8_t> Serialize(const ControlInputPacket& p) {
            PacketBuffer b;
            b.WriteFloat(p.mouseDx);
            b.WriteFloat(p.mouseDy);
            b.WriteFloat(p.scrollX);
            b.WriteFloat(p.scrollY);
            b.WriteByte(static_cast<uint8_t>((p.cursorValid ? 1 : 0) | (p.releaseAll ? 2 : 0)));
            b.WriteFloat(p.cursorX);
            b.WriteFloat(p.cursorY);
            b.WriteByte(p.cursorRef);
            b.WriteVarInt(static_cast<uint32_t>(p.keys.size()));
            for (const auto& k : p.keys) {
                b.WriteShort(static_cast<uint16_t>(k.key));
                b.WriteByte(k.action);
                b.WriteByte(k.mods);
            }
            b.WriteVarInt(static_cast<uint32_t>(p.buttons.size()));
            for (const auto& m : p.buttons) {
                b.WriteByte(m.button);
                b.WriteByte(m.action);
                b.WriteByte(m.mods);
            }
            b.WriteVarInt(static_cast<uint32_t>(p.chars.size()));
            for (uint32_t c : p.chars) b.WriteVarInt(c);
            return b.GetData();
        }
        inline ControlInputPacket DeserializeControlInput(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            ControlInputPacket p;
            p.mouseDx = r.ReadFloat();
            p.mouseDy = r.ReadFloat();
            p.scrollX = r.ReadFloat();
            p.scrollY = r.ReadFloat();
            const uint8_t flags = r.ReadByte();
            p.cursorValid = (flags & 1) != 0;
            p.releaseAll  = (flags & 2) != 0;
            p.cursorX = r.ReadFloat();
            p.cursorY = r.ReadFloat();
            p.cursorRef = r.ReadByte();
            const uint32_t nk = r.ReadVarInt();
            p.keys.reserve(std::min<uint32_t>(nk, 256));
            for (uint32_t i = 0; i < nk && i < 256; ++i) {
                ControlInputPacket::KeyEvent k;
                k.key    = static_cast<int16_t>(r.ReadShort());
                k.action = r.ReadByte();
                k.mods   = r.ReadByte();
                p.keys.push_back(k);
            }
            const uint32_t nb = r.ReadVarInt();
            p.buttons.reserve(std::min<uint32_t>(nb, 256));
            for (uint32_t i = 0; i < nb && i < 256; ++i) {
                ControlInputPacket::ButtonEvent m;
                m.button = r.ReadByte();
                m.action = r.ReadByte();
                m.mods   = r.ReadByte();
                p.buttons.push_back(m);
            }
            const uint32_t nc = r.ReadVarInt();
            p.chars.reserve(std::min<uint32_t>(nc, 256));
            for (uint32_t i = 0; i < nc && i < 256; ++i) p.chars.push_back(r.ReadVarInt());
            return p;
        }

        inline std::vector<uint8_t> Serialize(const ControlViewPacket& p) {
            PacketBuffer b;
            b.WriteDouble(p.cameraPos.x);
            b.WriteDouble(p.cameraPos.y);
            b.WriteDouble(p.cameraPos.z);
            b.WriteFloat(p.yaw);
            b.WriteFloat(p.pitch);
            b.WriteFloat(p.fov);
            b.WriteByte(p.perspective);
            b.WriteDouble(p.playerPos.x);
            b.WriteDouble(p.playerPos.y);
            b.WriteDouble(p.playerPos.z);
            b.WriteByte(static_cast<uint8_t>(p.dimension));
            b.WriteByte(static_cast<uint8_t>(p.screen));
            b.WriteString(p.chatText);
            b.WriteByte(static_cast<uint8_t>((p.statsHidden ? 1 : 0) | (p.eyeInWater ? 2 : 0)));
            b.WriteByte(p.selectedSlot);
            b.WriteInt(p.sendTimeMs);
            b.WriteByte(static_cast<uint8_t>((p.hitValid ? 1 : 0) | (p.entityPicked ? 2 : 0)));
            b.WriteInt(static_cast<uint32_t>(p.hitPos.x));
            b.WriteInt(static_cast<uint32_t>(p.hitPos.y));
            b.WriteInt(static_cast<uint32_t>(p.hitPos.z));
            b.WriteByte(static_cast<uint8_t>(p.breakStage));
            b.WriteInt(static_cast<uint32_t>(p.breakPos.x));
            b.WriteInt(static_cast<uint32_t>(p.breakPos.y));
            b.WriteInt(static_cast<uint32_t>(p.breakPos.z));
            b.WriteByte(static_cast<uint8_t>((p.swing ? 1 : 0) | (p.usingItem ? 2 : 0)));
            b.WriteByte(p.usingHand);
            b.WriteByte(p.useAnim);
            b.WriteInt(static_cast<uint32_t>(p.useItemRemaining));
            b.WriteInt(static_cast<uint32_t>(p.useItemDuration));
            b.WriteFloat(p.walkDist);
            return b.GetData();
        }
        inline ControlViewPacket DeserializeControlView(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            ControlViewPacket p;
            p.cameraPos.x = r.ReadDouble();
            p.cameraPos.y = r.ReadDouble();
            p.cameraPos.z = r.ReadDouble();
            p.yaw   = r.ReadFloat();
            p.pitch = r.ReadFloat();
            p.fov   = r.ReadFloat();
            p.perspective = r.ReadByte();
            p.playerPos.x = r.ReadDouble();
            p.playerPos.y = r.ReadDouble();
            p.playerPos.z = r.ReadDouble();
            p.dimension = static_cast<int8_t>(r.ReadByte());
            p.screen    = static_cast<ControlScreen>(r.ReadByte());
            p.chatText  = r.ReadString();
            const uint8_t flags = r.ReadByte();
            p.statsHidden = (flags & 1) != 0;
            p.eyeInWater  = (flags & 2) != 0;
            p.selectedSlot = r.ReadByte();
            p.sendTimeMs   = r.ReadInt();
            const uint8_t hitFlags = r.ReadByte();
            p.hitValid     = (hitFlags & 1) != 0;
            p.entityPicked = (hitFlags & 2) != 0;
            p.hitPos.x = static_cast<int32_t>(r.ReadInt());
            p.hitPos.y = static_cast<int32_t>(r.ReadInt());
            p.hitPos.z = static_cast<int32_t>(r.ReadInt());
            p.breakStage = static_cast<int8_t>(r.ReadByte());
            p.breakPos.x = static_cast<int32_t>(r.ReadInt());
            p.breakPos.y = static_cast<int32_t>(r.ReadInt());
            p.breakPos.z = static_cast<int32_t>(r.ReadInt());
            const uint8_t handFlags = r.ReadByte();
            p.swing     = (handFlags & 1) != 0;
            p.usingItem = (handFlags & 2) != 0;
            p.usingHand = r.ReadByte();
            p.useAnim   = r.ReadByte();
            p.useItemRemaining = static_cast<int32_t>(r.ReadInt());
            p.useItemDuration  = static_cast<int32_t>(r.ReadInt());
            p.walkDist  = r.ReadFloat();
            return p;
        }

    } // namespace Serialization
} // namespace Network
