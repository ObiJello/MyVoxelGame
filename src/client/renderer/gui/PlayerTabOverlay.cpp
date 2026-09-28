// File: src/client/renderer/gui/PlayerTabOverlay.cpp
#include "PlayerTabOverlay.hpp"

#include "SpectatorGui.hpp"   // DrawPlayerFace
#include "client/network/ClientPlayerInfo.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace Render {

    namespace {

        constexpr int kMaxRowsPerCol = 20;   // PlayerTabOverlay.MAX_ROWS_PER_COL
        constexpr size_t kMaxPlayers = 80;   // getPlayerInfos().limit(80)

        // String::compareToIgnoreCase.
        int CompareIgnoreCase(const std::string& a, const std::string& b) {
            const size_t n = std::min(a.size(), b.size());
            for (size_t i = 0; i < n; ++i) {
                const int ca = std::tolower(static_cast<unsigned char>(a[i]));
                const int cb = std::tolower(static_cast<unsigned char>(b[i]));
                if (ca != cb) return ca - cb;
            }
            return static_cast<int>(a.size()) - static_cast<int>(b.size());
        }

        // decorateName: a spectator's name is ITALIC.
        std::string DisplayName(const Client::PlayerInfo& info) {
            return info.IsSpectator() ? std::string("\xC2\xA7" "o") + info.name : info.name;
        }

        // extractPingIcon's thresholds.
        const char* PingSprite(int32_t latency) {
            if (latency < 0)    return "icon/ping_unknown";
            if (latency < 150)  return "icon/ping_5";
            if (latency < 300)  return "icon/ping_4";
            if (latency < 600)  return "icon/ping_3";
            if (latency < 1000) return "icon/ping_2";
            return "icon/ping_1";
        }

    } // namespace

    PlayerTabOverlay& GetPlayerTabOverlay() {
        static PlayerTabOverlay s_overlay;
        return s_overlay;
    }

    void PlayerTabOverlay::Render(GuiGraphics& g) {
        if (!m_visible) return;

        // getPlayerInfos: sorted by PLAYER_COMPARATOR, at most 80.
        std::vector<const Client::PlayerInfo*> infos;
        for (const auto& [id, info] : Client::PlayerInfoMap()) infos.push_back(&info);
        std::stable_sort(infos.begin(), infos.end(),
                         [](const Client::PlayerInfo* a, const Client::PlayerInfo* b) {
                             const int sa = a->IsSpectator() ? 1 : 0;
                             const int sb = b->IsSpectator() ? 1 : 0;
                             if (sa != sb) return sa < sb;
                             return CompareIgnoreCase(a->name, b->name) < 0;
                         });
        if (infos.size() > kMaxPlayers) infos.resize(kMaxPlayers);
        if (infos.empty()) return;

        const int screenWidth = g.GuiWidth();
        int maxNameWidth = 0;
        for (const Client::PlayerInfo* info : infos) {
            maxNameWidth = std::max(maxNameWidth, g.GetStringWidth(DisplayName(*info)));
        }

        const int slots = static_cast<int>(infos.size());
        int rows = slots;
        int cols = 1;
        while (rows > kMaxRowsPerCol) {
            ++cols;
            rows = (slots + cols - 1) / cols;
        }

        // showHead: MC shows faces on an online-mode connection; every player
        // here has one (the stick figure's head).
        constexpr bool showHead = true;
        constexpr int widthForScore = 0;   // no display objective
        const int slotWidth = std::min(cols * ((showHead ? 9 : 0) + maxNameWidth + widthForScore + 13),
                                       screenWidth - 50) / cols;
        const int xxo = screenWidth / 2 - (slotWidth * cols + (cols - 1) * 5) / 2;
        const int yyo = 10;
        const int maxLineWidth = slotWidth * cols + (cols - 1) * 5;

        g.Fill(screenWidth / 2 - maxLineWidth / 2 - 1, yyo - 1,
               screenWidth / 2 + maxLineWidth / 2 + 1, yyo + rows * 9,
               0x80000000u);   // Integer.MIN_VALUE
        // options.getBackgroundColor(553648127): 0x20FFFFFF.
        constexpr uint32_t kSlotBackground = 0x20FFFFFFu;

        for (int i = 0; i < slots; ++i) {
            const int col  = i / rows;
            const int line = i % rows;
            int xo = xxo + col * slotWidth + col * 5;
            const int yo = yyo + line * 9;
            g.Fill(xo, yo, xo + slotWidth, yo + 8, kSlotBackground);
        }
        g.NextStratum();
        for (int i = 0; i < slots; ++i) {
            const Client::PlayerInfo& info = *infos[static_cast<size_t>(i)];
            const int col  = i / rows;
            const int line = i % rows;
            int xo = xxo + col * slotWidth + col * 5;
            const int yo = yyo + line * 9;
            if (showHead) {
                DrawPlayerFace(g, xo, yo, 8, info.colorId);
                xo += 9;
            }
            // A spectator's name at 0x90 alpha (-1862270977), everyone
            // else's white.
            g.DrawString(DisplayName(info), xo, yo, info.IsSpectator() ? 0x90FFFFFFu : 0xFFFFFFFFu);
            // extractPingIcon(graphics, slotWidth, xo - (showHead ? 9 : 0), yo, info).
            g.BlitSprite(PingSprite(info.latency), xo - (showHead ? 9 : 0) + slotWidth - 11, yo, 10, 8);
        }
    }

} // namespace Render
