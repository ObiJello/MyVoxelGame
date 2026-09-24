// File: src/client/renderer/gui/screens/SignEditScreen.cpp
#include "SignEditScreen.hpp"

#include "../GuiGraphics.hpp"
#include "../../backend/RenderBackend.hpp"
#include "client/world/ClientChunkManager.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/entity/SignBlockEntity.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/core/Log.hpp"

#include <GLFW/glfw3.h>
#include <chrono>
#include <cmath>
#include <optional>

namespace Render {

    namespace {
        std::optional<Network::SignUpdateC2SPacket> s_pendingUpdate;
        bool s_signScreenOpen = false;

        double NowSeconds() {
            using namespace std::chrono;
            return duration<double>(steady_clock::now().time_since_epoch()).count();
        }

        Game::SignBlockEntity* ClientSignAt(const glm::ivec3& pos) {
            if (!Client::g_clientChunkManager) return nullptr;
            const auto cp = Game::Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
            Client::ClientChunk* chunk = Client::g_clientChunkManager->GetChunk(cp);
            if (!chunk || !chunk->chunkData) return nullptr;
            return dynamic_cast<Game::SignBlockEntity*>(
                chunk->chunkData->GetBlockEntity(pos.x - cp.x * 16, pos.y, pos.z - cp.z * 16));
        }

        // MC ARGB.scaleRGB(color, 0.4F) / AbstractSignRenderer.getDarkColor.
        uint32_t DarkColor(const Game::SignText& text) {
            const uint32_t colour = Game::DyeTextColor(text.color);
            if (colour == Game::DyeTextColor(Game::DyeColor::Black) && text.glowing) return 0xFFF0EBCCu;
            auto ch = [&](int shift) {
                return static_cast<uint32_t>(std::min(255.0f, static_cast<float>((colour >> shift) & 0xFF) * 0.4f)) << shift;
            };
            return 0xFF000000u | ch(16) | ch(8) | ch(0);
        }
    } // namespace

    SignEditScreen::SignEditScreen(const SignEditorOpen& open)
        : Screen("Edit Sign Message"), m_pos(open.pos), m_front(open.front) {
        s_signScreenOpen = true;
        m_openedAt = NowSeconds();

        // The block — from the server's packet, not the client's chunk copy,
        // which can lag the click by a packet or two: wood, kind, and
        // therefore the sprite, the line height and the width the text must
        // fit.
        Game::BlockID id = static_cast<Game::BlockID>(open.blockId);
        if (!Game::IsSignBlock(id) && Client::g_clientBlockAccess) {
            id = Client::g_clientBlockAccess->GetBlockState(open.pos.x, open.pos.y, open.pos.z).Block();
        }
        const std::string& slug = Game::BlockRegistry::Get(id).registrySlug;
        m_hanging = Game::IsCeilingHangingSignBlock(id) || Game::IsWallHangingSignBlock(id);
        m_wall    = Game::IsWallSignBlock(id) || Game::IsWallHangingSignBlock(id);
        for (const char* suffix : { "_wall_hanging_sign", "_hanging_sign", "_wall_sign", "_sign" }) {
            const std::string s(suffix);
            if (slug.size() > s.size() && slug.compare(slug.size() - s.size(), s.size(), s) == 0) {
                m_wood = slug.substr(0, slug.size() - s.size());
                break;
            }
        }
        if (m_hanging) m_title = "Edit Hanging Sign Message";
        m_lineHeight   = m_hanging ? Game::SignBlockEntity::kHangingLineHeight   : Game::SignBlockEntity::kPlainLineHeight;
        m_maxLineWidth = m_hanging ? Game::SignBlockEntity::kHangingMaxLineWidth : Game::SignBlockEntity::kPlainMaxLineWidth;

        // The current text of the face being edited, and its ink — again
        // the server's, which is what the editor is editing.
        {
            Game::SignText text;
            text.lines   = open.lines;
            text.color   = open.color < Game::kDyeColorCount ? static_cast<Game::DyeColor>(open.color)
                                                              : Game::DyeColor::Black;
            text.glowing = open.glowing;
            m_lines = text.lines;
            m_textColor = text.glowing ? Game::DyeTextColor(text.color) : DarkColor(text);
            // And the client's copy is brought in step, so the preview and
            // the world agree from the first frame.
            if (Game::SignBlockEntity* sign = ClientSignAt(open.pos)) {
                sign->SetText(open.front ? Game::SignTextSlot::Front : Game::SignTextSlot::Back, text);
            }
        }

        const std::string rel = (m_hanging ? "assets/textures/gui/hanging_signs/" : "assets/textures/gui/signs/") + m_wood + ".png";
        m_texture = LoadStandaloneGuiTexture(rel.c_str(), m_texW, m_texH);
    }

    SignEditScreen::~SignEditScreen() {
        Submit();
        // Deferred: the frame that closed the screen may still be drawing
        // the board on the GPU.
        if (g_renderBackend && m_texture != INVALID_TEXTURE) g_renderBackend->DeferredDestroyTexture(m_texture);
        s_signScreenOpen = false;
    }

    void SignEditScreen::Init() {
        // AbstractSignEditScreen.init: Done at (width/2 - 100, height/4 + 144).
        AddWidget(new Button(m_width / 2 - 100, m_height / 4 + 144,
                             WidgetDims::BUTTON_WIDTH, WidgetDims::BUTTON_HEIGHT,
                             "Done", [this] { Screen::OnClose(); }));
    }

    void SignEditScreen::PushLinesToSign() {
        // AbstractSignEditScreen.setMessage → sign.setText: the client's
        // copy shows the line as it is typed.
        if (Game::SignBlockEntity* sign = ClientSignAt(m_pos)) {
            const Game::SignTextSlot slot = m_front ? Game::SignTextSlot::Front : Game::SignTextSlot::Back;
            Game::SignText text = sign->GetText(slot);
            text.lines = m_lines;
            sign->SetText(slot, text);
        }
    }

    void SignEditScreen::Submit() {
        // AbstractSignEditScreen.removed: the lines go out once, when the
        // screen goes away, whichever way it went.
        if (m_submitted) return;
        m_submitted = true;
        Network::SignUpdateC2SPacket packet;
        packet.pos   = m_pos;
        packet.front = m_front;
        packet.lines = m_lines;
        s_pendingUpdate = packet;
    }

    void SignEditScreen::OnClose() {
        Screen::OnClose();   // pops; the destructor submits
    }

    bool SignEditScreen::KeyPressed(int glfwKey, int glfwMods) {
        // AbstractSignEditScreen.keyPressed: up → previous line, down or
        // confirmation → next line (wrapping), the cursor at the line's end.
        if (glfwKey == GLFW_KEY_UP) {
            m_line = (m_line - 1) & 3;
            return true;
        }
        if (glfwKey == GLFW_KEY_DOWN || glfwKey == GLFW_KEY_ENTER || glfwKey == GLFW_KEY_KP_ENTER) {
            m_line = (m_line + 1) & 3;
            return true;
        }
        if (glfwKey == GLFW_KEY_BACKSPACE) {
            if (!m_lines[m_line].empty()) {
                m_lines[m_line].pop_back();
                PushLinesToSign();
            }
            return true;
        }
        if (glfwKey == GLFW_KEY_ESCAPE) {
            OnClose();
            return true;
        }
        return Screen::KeyPressed(glfwKey, glfwMods);
    }

    bool SignEditScreen::CharTyped(unsigned int codepoint) {
        // TextFieldHelper with the width filter: a character is taken only
        // while the line still fits the board (font.width <= maxTextLineWidth).
        if (codepoint < 32 || codepoint > 126) return true;   // the 8×8 sheet is ASCII
        std::string next = m_lines[m_line];
        next += static_cast<char>(codepoint);
        if (m_widthProbe && m_widthProbe(next) > m_maxLineWidth) return true;
        m_lines[m_line] = next;
        PushLinesToSign();
        return true;
    }

    void SignEditScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        // The width check needs the GUI font; it becomes available here.
        if (!m_widthProbe) {
            m_widthProbe = [&g](const std::string& s) { return g.GetStringWidth(s); };
        }

        Screen::Render(g, mouseX, mouseY, partialTick);   // background + Done

        g.DrawCenteredString(m_title, m_width / 2, 40, 0xFFFFFFFFu);

        // SignEditScreen.extractSign: translate(width/2, yOffset); the sprite
        // (translate(0,27), scale 3.9, blit(-12,-13, 24×26 — 12 rows for a
        // wall sign)) then the text at scale 0.9765628. Hanging:
        // yOffset 125, translate(0,-13), scale 4.5, blit(-8,-8, 16×16), text 1.0.
        const float cx = static_cast<float>(m_width) / 2.0f;
        const float yOffset = m_hanging ? 125.0f : 90.0f;
        if (m_texture != INVALID_TEXTURE) {
            if (!m_hanging) {
                const int shownH = m_wall ? 12 : 26;
                const float scale = 3.9f;
                const float x0 = cx - 12.0f * scale;
                const float y0 = yOffset + 27.0f - 13.0f * scale;
                g.Blit(m_texture,
                       static_cast<int>(std::lround(x0)), static_cast<int>(std::lround(y0)),
                       static_cast<int>(std::lround(x0 + 24.0f * scale)),
                       static_cast<int>(std::lround(y0 + shownH * scale)),
                       0.0f, 0.0f, 1.0f, static_cast<float>(shownH) / 26.0f);
            } else {
                const float scale = 4.5f;
                const float x0 = cx - 8.0f * scale;
                const float y0 = yOffset - 13.0f - 8.0f * scale;
                g.Blit(m_texture,
                       static_cast<int>(std::lround(x0)), static_cast<int>(std::lround(y0)),
                       static_cast<int>(std::lround(x0 + 16.0f * scale)),
                       static_cast<int>(std::lround(y0 + 16.0f * scale)),
                       0.0f, 0.0f, 1.0f, 1.0f);
            }
        }

        const float textScale = m_hanging ? 1.0f : 0.9765628f;
        const int signMidpoint = 4 * m_lineHeight / 2;
        // TextCursorUtils.isCursorVisible: (ms since open / 300) even.
        const bool cursorVisible = (static_cast<long long>((NowSeconds() - m_openedAt) * 1000.0) / 300) % 2 == 0;

        g.PushMatrix();
        g.Translate(cx, yOffset);
        g.Scale(textScale, textScale);
        for (int i = 0; i < 4; ++i) {
            const std::string& line = m_lines[i];
            const int w = g.GetStringWidth(line);
            const int x = -w / 2;
            const int y = i * m_lineHeight - signMidpoint;
            if (!line.empty()) g.DrawString(line, x, y, m_textColor, /*dropShadow=*/false);
            if (i == m_line && cursorVisible) {
                // The cursor sits at the end of the line: the "_" glyph
                // (extractAppendCursor).
                g.DrawString("_", x + w, y, m_textColor, /*dropShadow=*/false);
            }
        }
        g.PopMatrix();
    }

    // ── Host-loop hooks ───────────────────────────────────────────────────

    void ShowSignEditScreen(const SignEditorOpen& open) {
        if (s_signScreenOpen) return;
        GetScreenManager().Push(std::make_unique<SignEditScreen>(open));
        s_signScreenOpen = true;
    }

    bool ConsumeSignUpdate(Network::SignUpdateC2SPacket& out) {
        if (!s_pendingUpdate) return false;
        out = *s_pendingUpdate;
        s_pendingUpdate.reset();
        return true;
    }

} // namespace Render
