// File: src/common/world/block/entity/SignBlockEntity.hpp
//
// MC SignBlockEntity / HangingSignBlockEntity + SignText. Both sign kinds
// share the class (MC subclasses only to change two numbers — the line
// height and the maximum line width — which are answered off the block
// here). The board and post are ordinary block-model geometry meshed with
// the chunk (26.3 template_sign_rot_N / template_wall_sign / the hanging
// templates); this entity carries only what the renderer draws on top of
// it and what the editor changes.
#pragma once

#include "BlockEntity.hpp"
#include <array>
#include <cstdint>
#include <string>

namespace Game {

    // MC DyeColor ordinals, and DyeColor.getTextColor() per entry.
    enum class DyeColor : uint8_t {
        White = 0, Orange, Magenta, LightBlue, Yellow, Lime, Pink, Gray,
        LightGray, Cyan, Purple, Blue, Brown, Green, Red, Black,
    };
    constexpr int kDyeColorCount = 16;
    uint32_t DyeTextColor(DyeColor c);           // 0xFFRRGGBB
    const char* DyeColorName(DyeColor c);         // "light_blue"
    bool DyeColorFromName(std::string_view name, DyeColor& out);

    // MC SignText: four lines, a dye colour, and whether the text glows.
    struct SignText {
        static constexpr int kLines = 4;
        std::array<std::string, kLines> lines{};
        DyeColor color   = DyeColor::Black;
        bool     glowing = false;

        bool HasMessage() const {
            for (const auto& l : lines) if (!l.empty()) return true;
            return false;
        }
    };

    // MC SignTextSlot: which face of the board.
    enum class SignTextSlot : uint8_t { Back = 0, Front = 1 };

    class SignBlockEntity : public BlockEntity {
    public:
        static constexpr int kPlainMaxLineWidth   = 90;   // SignBlockEntity.MAX_TEXT_LINE_WIDTH
        static constexpr int kPlainLineHeight     = 10;   // SignBlockEntity.TEXT_LINE_HEIGHT
        static constexpr int kHangingMaxLineWidth = 60;   // HangingSignBlockEntity
        static constexpr int kHangingLineHeight   = 9;

        SignBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId);

        const SignText& GetText(SignTextSlot slot) const {
            return slot == SignTextSlot::Front ? m_front : m_back;
        }
        SignText& MutableText(SignTextSlot slot) {
            return slot == SignTextSlot::Front ? m_front : m_back;
        }
        void SetText(SignTextSlot slot, const SignText& text) { MutableText(slot) = text; MarkDirty(); }

        bool IsWaxed() const { return m_waxed; }
        void SetWaxed(bool waxed) { m_waxed = waxed; MarkDirty(); }

        // A hanging sign (ceiling or wall) — smaller board, tighter text.
        bool IsHanging() const;
        int  MaxLineWidth() const { return IsHanging() ? kHangingMaxLineWidth : kPlainMaxLineWidth; }
        int  LineHeight()   const { return IsHanging() ? kHangingLineHeight   : kPlainLineHeight; }

        // MC SignBlockEntity.playerWhoMayEdit: the one player the editor was
        // opened for. Server-side only; not on the wire, not saved.
        uint32_t GetAllowedEditor() const { return m_allowedEditor; }
        void     SetAllowedEditor(uint32_t playerId) { m_allowedEditor = playerId; }

        // MC SignText.hasEditableText: nothing here has a click command or
        // other non-plain content, so every text is editable.
        bool HasEditableText(SignTextSlot) const { return true; }

        // Wire + snapshot: 4 lines, colour, glow per face; waxed.
        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;

    private:
        SignText m_front;
        SignText m_back;
        bool     m_waxed = false;
        uint32_t m_allowedEditor = 0;
    };

    // Which face the player stands in front of — MC SignBlockEntity
    // .getSlotPlayerIsFacing, from the sign's yaw and the player's position.
    // `signYawDeg` is the block's getYRotationDegrees; `hitboxCentre` the
    // block-local centre of the sign's shape (0.5,0.5,0.5 for most).
    SignTextSlot SignSlotFacing(float signYawDeg, const glm::ivec3& pos,
                                const glm::vec3& hitboxCentre,
                                double playerX, double playerZ);

} // namespace Game
