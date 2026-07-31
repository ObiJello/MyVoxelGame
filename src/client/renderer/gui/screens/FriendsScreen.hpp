// File: src/client/renderer/gui/screens/FriendsScreen.hpp
//
// The in-game Friends screens, MC-styled via the shared Screen framework.
// Reachable from the title screen (the old Realms slot) and the pause menu;
// works over both backgrounds (ScreenManager::IsInWorld handles that).
//
//   FriendsScreen    — sectioned list: pending invite banner, incoming
//                      friend requests (inline Accept/Decline), friends with
//                      live presence lines. Footer: Add Friend / Invite /
//                      Join and Remove / Done, gated by selection + state.
//   AddFriendScreen  — one edit box, sends a friend request by username.
//
// All data comes from Client::g_friendsClient's roster snapshots (generation
// counter drives rebuilds in Tick, 20Hz). Join posts a Multiplayer
// TitleAction — the host loop routes it from either context.
#pragma once

#include "Screen.hpp"
#include "../../../network/FriendsClient.hpp"

namespace Render {

    // Sectioned, selectable list — the WorldListWidget pattern adapted to
    // heterogeneous rows (headers / invite banner / requests / friends).
    class FriendListWidget : public AbstractWidget {
    public:
        static constexpr int ROW_H = 36;
        static constexpr int ROW_W = 270;

        FriendListWidget(int x, int y, int width, int height)
            : AbstractWidget(x, y, width, height, "") {}

        struct Row {
            enum class Kind { Header, Invite, Request, Friend };
            Kind kind = Kind::Header;
            std::string title;      // header text / player name
            std::string line2;      // status / detail (grey)
            std::string line3;
            uint32_t line2Color = 0xFF808080;
            int64_t accountId = 0;                    // Request/Friend rows
            Client::FriendPresence presence;          // Friend rows
            Client::FriendInvite invite;              // Invite rows
        };

        void SetRows(std::vector<Row> rows);
        const Row* Selected() const;

        // Inline row actions (Accept/Decline on request rows, Join on the
        // invite banner) — fired with the row.
        std::function<void(const Row&)> onAccept;
        std::function<void(const Row&)> onDecline;
        std::function<void(const Row&)> onInviteJoin;
        std::function<void()> onSelectionChanged;

        void OnClick(double mouseX, double mouseY) override;
        bool OnScroll(double deltaY) override;

    protected:
        void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;

    private:
        int RowAt(double mouseX, double mouseY) const;
        int RowTop(int index) const;
        double MaxScroll() const;

        std::vector<Row> m_rows;
        int m_selected = -1;
        double m_scroll = 0.0;
    };

    class FriendsScreen : public Screen {
    public:
        FriendsScreen() : Screen("Friends") {}

        void Init() override;
        void Tick() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;

    private:
        void RebuildRows();
        void UpdateButtonStates();
        void JoinSelected();
        void InviteSelected();
        void JoinResolved(const Client::JoinInfoResult& info);

        FriendListWidget* m_list = nullptr;
        Button* m_inviteButton = nullptr;
        Button* m_joinButton = nullptr;
        Button* m_removeButton = nullptr;
        uint64_t m_builtGeneration = 0;
        std::string m_status;          // transient status/error line
        int m_statusTicks = 0;         // fades after a few seconds
    };

    class AddFriendScreen : public Screen {
    public:
        AddFriendScreen() : Screen("Add Friend") {}

        void Init() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        bool KeyPressed(int glfwKey, int glfwMods) override;   // Enter sends

    private:
        void Send();
        EditBox* m_nameBox = nullptr;
        Button* m_sendButton = nullptr;
    };

} // namespace Render
