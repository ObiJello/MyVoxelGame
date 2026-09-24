// File: src/common/world/block/entity/SignBlockEntity.cpp
#include "SignBlockEntity.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/core/Mth.hpp"

#include <cmath>
#include <string_view>

namespace Game {

    namespace {
        // MC DyeColor(..., textColor): the sign-text colour of each dye.
        constexpr uint32_t kTextColors[kDyeColorCount] = {
            0xFFFFFFFFu, // white
            0xFFFF681Fu, // orange
            0xFFFF00FFu, // magenta
            0xFF9AC0CDu, // light_blue
            0xFFFFFF00u, // yellow
            0xFFBFFF00u, // lime
            0xFFFF69B4u, // pink
            0xFF808080u, // gray
            0xFFD0D0D0u, // light_gray
            0xFF00FFFFu, // cyan
            0xFFA020F0u, // purple
            0xFF0000FFu, // blue
            0xFF8B4513u, // brown
            0xFF00FF00u, // green
            0xFFFF0000u, // red
            0xFF000000u, // black
        };
        constexpr const char* kNames[kDyeColorCount] = {
            "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
            "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black",
        };
    } // namespace

    uint32_t DyeTextColor(DyeColor c) {
        const int i = static_cast<int>(c);
        return (i >= 0 && i < kDyeColorCount) ? kTextColors[i] : kTextColors[kDyeColorCount - 1];
    }

    const char* DyeColorName(DyeColor c) {
        const int i = static_cast<int>(c);
        return (i >= 0 && i < kDyeColorCount) ? kNames[i] : "black";
    }

    bool DyeColorFromName(std::string_view name, DyeColor& out) {
        if (name.rfind("minecraft:", 0) == 0) name.remove_prefix(10);
        for (int i = 0; i < kDyeColorCount; ++i) {
            if (name == kNames[i]) { out = static_cast<DyeColor>(i); return true; }
        }
        return false;
    }

    SignBlockEntity::SignBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
        : BlockEntity(type, worldPos, blockId) {}

    bool SignBlockEntity::IsHanging() const {
        const std::string& slug = BlockRegistry::Get(GetBlockId()).registrySlug;
        return slug.find("hanging_sign") != std::string::npos;
    }

    void SignBlockEntity::Save(Network::PacketBuffer& out) const {
        for (const SignText* text : { &m_front, &m_back }) {
            for (const std::string& line : text->lines) out.WriteString(line);
            out.WriteByte(static_cast<uint8_t>(text->color));
            out.WriteByte(text->glowing ? 1 : 0);
        }
        out.WriteByte(m_waxed ? 1 : 0);
    }

    void SignBlockEntity::Load(Network::PacketReader& in) {
        for (SignText* text : { &m_front, &m_back }) {
            for (std::string& line : text->lines) {
                if (!in.HasMore()) return;
                line = in.ReadString();
            }
            if (!in.HasMore()) return;
            const uint8_t colour = in.ReadByte();
            text->color = colour < kDyeColorCount ? static_cast<DyeColor>(colour) : DyeColor::Black;
            if (!in.HasMore()) return;
            text->glowing = in.ReadByte() != 0;
        }
        if (!in.HasMore()) return;
        m_waxed = in.ReadByte() != 0;
    }

    SignTextSlot SignSlotFacing(float signYawDeg, const glm::ivec3& pos,
                                const glm::vec3& hitboxCentre,
                                double playerX, double playerZ) {
        // MC SignBlockEntity.getSlotPlayerIsFacing:
        //   xd = player.x - (pos.x + centre.x); zd = player.z - (pos.z + centre.z)
        //   playerYRot = atan2(zd, xd) * 180/π - 90
        //   FRONT when |wrap(signYRot - playerYRot)| <= 90
        const double xd = playerX - (static_cast<double>(pos.x) + hitboxCentre.x);
        const double zd = playerZ - (static_cast<double>(pos.z) + hitboxCentre.z);
        const float playerYRot = static_cast<float>(std::atan2(zd, xd) * 57.2957763671875) - 90.0f;
        const float diff = std::abs(Mth::WrapDegrees(signYawDeg - playerYRot));
        return diff <= 90.0f ? SignTextSlot::Front : SignTextSlot::Back;
    }

} // namespace Game
