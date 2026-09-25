// File: src/client/renderer/gui/AnvilScreen.hpp
//
// Mirrors net.minecraft.client.gui.screens.inventory.AnvilScreen (over
// ItemCombinerScreen) — the "Repair & Name" panel: two inputs, the result, a
// name box, the level cost, and the error cross over the arrow when the
// inputs make nothing.
//
// The name box is MC's EditBox as AnvilScreen configures it: unbordered,
// white text, 50 characters, never loses focus, editable only while the left
// slot holds an item. While it is editable it takes EVERY key — E types an
// "e" instead of closing, Q types instead of dropping, the digits type
// instead of swapping (AnvilScreen.keyPressed); ESC still closes.
//
// Each accepted change runs the client's AnvilMenu.setItemName (the result
// and cost update at once) and queues a RenameItemC2S for the server's copy
// (ConsumeRenameItem, drained by PlatformMain).
#pragma once

#include "AbstractContainerScreen.hpp"
#include "common/entity/Item.hpp"
#include "common/network/PacketTypes.hpp"
#include "../backend/RenderTypes.hpp"
#include <string>

namespace Game { class AnvilMenu; }

namespace Render {

    class AnvilScreen : public AbstractContainerScreen {
    public:
        static constexpr int IMAGE_W = 176;
        static constexpr int IMAGE_H = 166;
        // MC AnvilScreen: titleLabelX = 60; the rest is AbstractContainerScreen's.
        static constexpr int TITLE_X = 60;
        static constexpr int TITLE_Y = 6;
        static constexpr int INV_LABEL_X = 8;
        static constexpr int INV_LABEL_Y = IMAGE_H - 94;
        static constexpr uint32_t LABEL_COLOR = 0xFF404040;

        // MC AnvilScreen.subInit / extractBackground geometry.
        static constexpr int NAME_X = 62, NAME_Y = 24, NAME_W = 103, NAME_H = 12;
        static constexpr int FIELD_X = 59, FIELD_Y = 20, FIELD_W = 110, FIELD_H = 16;
        static constexpr int ERROR_X = 99, ERROR_Y = 45, ERROR_W = 28, ERROR_H = 21;
        // extractLabels' cost line: right-aligned against imageWidth - 8, on a
        // translucent strip from y 67 to 79, text at y 69.
        static constexpr int COST_STRIP_Y0 = 67, COST_STRIP_Y1 = 79, COST_TEXT_Y = 69;
        static constexpr uint32_t COST_COLOR       = 0xFF80FF20;   // -8323296
        static constexpr uint32_t COST_ERROR_COLOR = 0xFFFF6060;   // -40864
        static constexpr uint32_t COST_STRIP_COLOR = 0x4F000000;   // 1325400064

        static constexpr int HIT_NAME_FIELD = -20;

        void Configure(const std::string& title);

    protected:
        int ImageWidth()  const override { return IMAGE_W; }
        int ImageHeight() const override { return IMAGE_H; }

        void RenderBg(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderLabels(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderExtras(GuiGraphics& g, int leftPos, int topPos) override;

        int  HitTestExtras(int lx, int ly) override;
        bool HandleExtraClick(int hit, int glfwButton, bool shift) override;
        bool HandleExtraKey(int glfwKey, int glfwMods) override;
        bool HandleExtraCharInput(unsigned int codepoint) override;

        void OnOpen() override;
        void ContainerTick() override;

    private:
        Game::AnvilMenu* Anvil() const;
        TextureHandle EnsureBackground();
        bool NameEditable() const;

        // MC AnvilScreen.slotChanged(0): the box shows the new item's name.
        void SyncNameToInput();
        // MC AnvilScreen.onNameChanged — the EditBox responder.
        void OnNameChanged();

        // ── MC EditBox, as the anvil uses it ─────────────────────────────
        void SetValue(const std::string& value);
        void MoveCursorTo(int pos, bool extendSelection);
        void InsertText(const std::string& text);
        void DeleteChars(int dir);
        void SelectAll();
        std::string SelectedText() const;
        // MC EditBox.scrollTo: keep the cursor inside the visible window.
        void ScrollTo(GuiGraphics* g, int pos);
        void RenderNameBox(GuiGraphics& g, int leftPos, int topPos);

        TextureHandle m_background = INVALID_TEXTURE;
        bool          m_backgroundTried = false;
        std::string   m_title = "Repair & Name";

        std::string m_value;
        int         m_cursorPos = 0;
        int         m_highlightPos = 0;
        int         m_displayPos = 0;
        long long   m_focusedAtMillis = 0;
        // What slot 0 held when the box last synced to it — the stand-in for
        // MC's ContainerListener.slotChanged, which fires when the slot's
        // stack changes.
        Game::ItemStack m_lastInput{};
        bool            m_inputSeen = false;
        // A click in the box, waiting for the draw that can measure glyphs.
        bool m_nameClickPending = false;
        bool m_nameClickShift = false;
        int  m_nameClickX = 0;
    };

    AnvilScreen& GetAnvilScreen();

    // The names the screen accepted since the last drain (PlatformMain sends
    // each as a RenameItemC2S).
    bool ConsumeRenameItem(Network::RenameItemC2SPacket& out);

} // namespace Render
