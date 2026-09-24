// File: src/client/renderer/gui/screens/SignEditScreen.hpp
//
// The sign editor — MC AbstractSignEditScreen / SignEditScreen /
// HangingSignEditScreen:
//   • "Edit Sign Message" (or "Edit Hanging Sign Message") at y = 40
//   • the sign's GUI sprite (textures/gui/signs/<wood>.png, 24×26 — the
//     top 12 rows alone for a wall sign; hanging: gui/hanging_signs, 16×16)
//     blown up 3.9× (4.5×) under the four lines of text
//   • four lines, ↑/↓/Enter move between them, typing appends at the
//     end of the current line as long as the line fits the board
//     (90 px, 60 for a hanging sign), Backspace deletes
//   • Done at (w/2-100, h/4+144); ESC is Done too
//   • the lines go to the server when the screen closes (removed() →
//     ServerboundSignUpdatePacket), and the client's own copy of the sign
//     shows them as they are typed
//   • not a pause screen
#pragma once

#include "Screen.hpp"
#include "common/network/packets/game/SignUpdateC2SPacket.hpp"
#include <array>
#include <functional>
#include <glm/glm.hpp>
#include <string>

namespace Render {

    // What the server sent with the open request (OpenSignEditorS2CPacket):
    // the sign block and the face's current text.
    struct SignEditorOpen {
        glm::ivec3 pos{0};
        bool       front = true;
        uint16_t   blockId = 0;
        std::array<std::string, 4> lines{};
        uint8_t    color = 15;
        bool       glowing = false;
    };

    class SignEditScreen : public Screen {
    public:
        explicit SignEditScreen(const SignEditorOpen& open);
        ~SignEditScreen() override;

        void Init() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        bool KeyPressed(int glfwKey, int glfwMods) override;
        bool CharTyped(unsigned int codepoint) override;
        bool IsPauseScreen() const override { return false; }
        void OnClose() override;

    private:
        void PushLinesToSign();   // the live preview on the client's block entity
        void Submit();

        glm::ivec3 m_pos;
        bool       m_front;
        bool       m_hanging = false;
        bool       m_wall    = false;
        std::string m_wood   = "oak";
        int         m_lineHeight   = 10;
        int         m_maxLineWidth = 90;
        uint32_t    m_textColor    = 0xFF000000u;
        std::array<std::string, 4> m_lines{};
        int  m_line = 0;
        bool m_submitted = false;
        double m_openedAt = 0.0;

        TextureHandle m_texture = INVALID_TEXTURE;
        int m_texW = 0, m_texH = 0;
        // The GUI font's string width, captured from the first Render (the
        // font lives on GuiGraphics, which a screen only sees when drawing).
        std::function<int(const std::string&)> m_widthProbe;
    };

    // ── Host-loop hooks ───────────────────────────────────────────────────
    // Open the editor for the sign the server described (OpenSignEditorS2C).
    void ShowSignEditScreen(const SignEditorOpen& open);
    // True once when an editor closed; PlatformMain sends the packet.
    bool ConsumeSignUpdate(Network::SignUpdateC2SPacket& out);

} // namespace Render
