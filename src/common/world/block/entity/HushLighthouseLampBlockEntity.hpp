// File: src/common/world/block/entity/HushLighthouseLampBlockEntity.hpp
//
// The Hush lighthouse lamp's block entity (BlockEntityTypeIds::
// HUSH_LIGHTHOUSE_LAMP). The sweep itself is still a pure function of the
// level's game time and the lamp's position (HushLighthouseRenderer); what
// this entity adds is the one fact the client cannot work out for itself:
// where the nearest Aurelith is ("Lighthouses that guide", docs/the-hush.md).
//
//   guideChecked  the server has looked (so it never looks again for this
//                 lamp — the answer is fixed by the world seed)
//   hasGuide      an Aurelith's Heart lies within Server::LighthouseGuide::
//                 kGuideRange blocks (horizontally) of the lamp
//   guideX/Z      that Heart's block x/z
//
// The server fills it in once, the first time the lamp's chunk arrives
// (Server::LighthouseGuide, server/level/LighthouseGuide), and marks it
// changed: World::BlockEntityChanged sends it to every watcher and dirties
// the chunk for saving. After that it rides every chunk send (Save/Load
// below) and the chunk's NBT (BlockEntityNbt.cpp: GuideChecked, GuideX,
// GuideZ), so the lookup runs once per lamp per world, not per session.
//
// A lamp with no entry in the template NBT (every generated lighthouse and
// every lamp a player places) starts unchecked.
//
// Wire (Save/Load): byte flags (bit 0 checked, bit 1 has a guide), then
// Int guideX, Int guideZ when bit 1 is set. Trailing-field extension.
#pragma once

#include "BlockEntity.hpp"
#include "common/network/PacketRegistry.hpp"

#include <cstdint>

namespace Game {

    class HushLighthouseLampBlockEntity : public BlockEntity {
    public:
        HushLighthouseLampBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        bool GuideChecked() const { return m_guideChecked; }
        bool HasGuide() const { return m_hasGuide; }
        int32_t GuideX() const { return m_guideX; }
        int32_t GuideZ() const { return m_guideZ; }

        // The server's answer: checked, and the Heart's x/z if one was found.
        void SetGuide(bool found, int32_t x, int32_t z) {
            m_guideChecked = true;
            m_hasGuide = found;
            m_guideX = found ? x : 0;
            m_guideZ = found ? z : 0;
            MarkDirty();
        }
        // The chunk-NBT form (BlockEntityNbt.cpp).
        void LoadGuide(bool checked, bool found, int32_t x, int32_t z) {
            m_guideChecked = checked;
            m_hasGuide = checked && found;
            m_guideX = m_hasGuide ? x : 0;
            m_guideZ = m_hasGuide ? z : 0;
        }

        void Save(Network::PacketBuffer& out) const override {
            out.WriteByte(static_cast<uint8_t>((m_guideChecked ? 1 : 0) | (m_hasGuide ? 2 : 0)));
            if (m_hasGuide) {
                out.WriteInt(static_cast<uint32_t>(m_guideX));
                out.WriteInt(static_cast<uint32_t>(m_guideZ));
            }
        }

        void Load(Network::PacketReader& in) override {
            if (!in.HasMore()) return;
            const uint8_t flags = in.ReadByte();
            m_guideChecked = (flags & 1) != 0;
            m_hasGuide = false;
            if ((flags & 2) != 0 && in.HasMore()) {
                m_guideX = static_cast<int32_t>(in.ReadInt());
                m_guideZ = static_cast<int32_t>(in.ReadInt());
                m_hasGuide = true;
            }
        }

    private:
        bool    m_guideChecked = false;
        bool    m_hasGuide = false;
        int32_t m_guideX = 0;
        int32_t m_guideZ = 0;
    };

} // namespace Game
