// File: src/client/renderer/gui/MerchantScreen.hpp
//
// Mirrors net.minecraft.client.gui.screens.inventory.MerchantScreen — the
// villager / wandering trader trading screen over a client MerchantMenu.
//
//   left      the trade list: seven 88×20 buttons (widget/button), each
//             showing cost A (struck through with the discounted price beside
//             it when a special price applies), cost B, the arrow (crossed
//             out when out of stock) and the result; a scroller when there
//             are more than seven
//   right     the two payment squares and the result, the villager's XP bar
//             (container/villager/experience_bar_*: current XP, plus the
//             selected trade's XP in green), the red cross over the result
//             when the selected trade is out of stock
//   title     "<name> - <level name>" (merchant.title / merchant.level.N)
//             while the bar is shown, else the plain name
//   reroll    (engine addition) a 20×20 icon button left of the payment
//             slots, on villagers only (the XP bar shown, a level): the
//             refresh glyph re-rolls the level-1 trades as breaking and
//             replacing the job site would; once the villager has traded
//             (XP > 0, or past Novice) it shows the slashed glyph and does
//             nothing — MC's ResetProfession lock. The press is a menu
//             button (ContainerButtonClickC2S, MerchantMenu::
//             BUTTON_REROLL_TRADES); the new offers arrive as MerchantOffersS2C
//             and the selection returns to the first trade.
//
// Clicking a trade sets the menu's selection hint and moves the payment in
// from the inventory — predicted here (MerchantMenu.tryMoveItems) and sent
// to the server as SelectTradeC2S, which does the same authoritatively.
#pragma once

#include "AbstractContainerScreen.hpp"
#include "../backend/RenderTypes.hpp"

#include <string>
#include <utility>
#include <vector>

namespace Game { class MerchantMenu; }

namespace Render {

    class MerchantScreen : public AbstractContainerScreen {
    public:
        static constexpr int IMAGE_W = 276;
        static constexpr int IMAGE_H = 166;
        static constexpr uint32_t LABEL_COLOR = 0xFF404040;   // -12566464

        // MC MerchantScreen constants.
        static constexpr int NUMBER_OF_OFFER_BUTTONS = 7;
        static constexpr int TRADE_BUTTON_X = 5;
        static constexpr int TRADE_BUTTON_W = 88;
        static constexpr int TRADE_BUTTON_H = 20;
        static constexpr int SCROLLER_W = 6;
        static constexpr int SCROLLER_H = 27;
        static constexpr int SCROLL_BAR_H = 139;
        static constexpr int SCROLL_BAR_TOP_Y = 18;
        static constexpr int SCROLL_BAR_START_X = 94;
        static constexpr int INVENTORY_LABEL_X = 107;
        // The reroll button: MC's small icon-button size (widget/
        // locked_button is 20×20), left of the trade boxes — in the open
        // strip between the offer list's scrollbar (ends at 100) and the
        // first payment slot's frame (135..153, 36..54), 5 px clear of it
        // and centred on the slot row.
        static constexpr int REROLL_BUTTON_X = 110;
        static constexpr int REROLL_BUTTON_Y = 35;
        static constexpr int REROLL_BUTTON_SIZE = 20;

        void Configure(const std::string& title);
        // A reroll's offers arrived: back to the first trade, scrolled to
        // the top (MerchantMenu::ClickMenuButton re-selects 0 server-side).
        void ResetTradeSelection();

    protected:
        int ImageWidth()  const override { return IMAGE_W; }
        int ImageHeight() const override { return IMAGE_H; }

        void RenderBg(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderExtraSlots(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderLabels(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderExtras(GuiGraphics& g, int leftPos, int topPos) override;

        int  HitTestExtras(int lx, int ly) override;
        bool HandleExtraClick(int hit, int glfwButton, bool shift) override;
        void HandleExtraRelease() override { m_isDragging = false; }
        bool HandleExtraScroll(double dy) override;
        void OnExtraMouseMove(int leftPos, int topPos) override;
        const Game::ItemStack* HoveredExtraStack() const override;

        void OnOpen() override;

    private:
        static constexpr int HIT_TRADE_BUTTON_0 = -20;   // -20 .. -26
        static constexpr int HIT_SCROLLER       = -30;
        static constexpr int HIT_REROLL         = -31;

        enum class RerollState { Hidden, Available, Locked };

        Game::MerchantMenu* Merchant() const;
        bool CanScroll(int numberOfOffers) const { return numberOfOffers > NUMBER_OF_OFFER_BUTTONS; }
        int  VisibleButtonCount() const;
        void PostButtonClick();
        RerollState GetRerollState() const;
        bool RerollHovered(int leftPos, int topPos) const;
        TextureHandle EnsureBackground();

        void RenderProgressBar(GuiGraphics& g, int xo, int yo);
        void RenderScroller(GuiGraphics& g, int xo, int yo);
        void RenderOfferExtras(GuiGraphics& g, int leftPos, int topPos);
        void RenderRerollIcon(GuiGraphics& g, int leftPos, int topPos);
        void RenderRerollTooltip(GuiGraphics& g, int leftPos, int topPos);
        void RenderTextTooltip(GuiGraphics& g, const std::string& text, int mx, int my);
        // Lines of (text, colour); a gap under the first when there are more
        // (MC ClientTextTooltip spacing), flipped left of the mouse when it
        // would run off the screen (DefaultTooltipPositioner).
        void RenderTextTooltip(GuiGraphics& g, const std::vector<std::pair<std::string, uint32_t>>& lines,
                               int mx, int my);

        std::string   m_title = "Villager";
        TextureHandle m_background = INVALID_TEXTURE;
        bool          m_backgroundTried = false;

        int  m_shopItem = 0;
        int  m_scrollOff = 0;
        bool m_isDragging = false;
        int  m_lastMouseLx = 0;   // panel-relative mouse x, for the tooltips
        mutable Game::ItemStack m_tooltipStack{};
    };

    MerchantScreen& GetMerchantScreen();

    // MC ClientPacketListener.handleMerchantOffers.
    void ApplyMerchantOffers(const Network::MerchantOffersS2CPacket& packet);
    // The trades clicked since the last drain (ServerboundSelectTradePacket),
    // oldest first — PlatformMain sends them.
    bool ConsumeSelectTrade(Network::SelectTradeC2SPacket& out);

} // namespace Render
