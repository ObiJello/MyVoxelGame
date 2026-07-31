// File: src/client/renderer/gui/screens/FriendsScreen.cpp
#include "FriendsScreen.hpp"
#include "TitleScreen.hpp"   // TitleAction (Join posts Multiplayer)
#include "../GuiGraphics.hpp"
#include "../FontRenderer.hpp"
#include "common/core/Log.hpp"
#include <GLFW/glfw3.h>
#include <algorithm>

namespace Render {

    namespace {
        // Inline mini-button geometry inside 36px rows.
        constexpr int MINI_W = 52;
        constexpr int MINI_H = 16;

        // Status-line colors.
        constexpr uint32_t kGreen  = 0xFF55FF55;
        constexpr uint32_t kGray   = 0xFF808080;
        constexpr uint32_t kDarker = 0xFF505050;
        constexpr uint32_t kAqua   = 0xFF55FFFF;
    }

    // ═══════════════════════════ FriendListWidget ═══════════════════════════

    void FriendListWidget::SetRows(std::vector<Row> rows) {
        // Preserve selection across rebuilds by account id.
        int64_t selectedId = 0;
        if (const Row* sel = Selected()) selectedId = sel->accountId;
        m_rows = std::move(rows);
        m_selected = -1;
        if (selectedId != 0) {
            for (size_t i = 0; i < m_rows.size(); ++i) {
                if (m_rows[i].kind == Row::Kind::Friend &&
                    m_rows[i].accountId == selectedId) {
                    m_selected = static_cast<int>(i);
                    break;
                }
            }
        }
    }

    const FriendListWidget::Row* FriendListWidget::Selected() const {
        if (m_selected < 0 || m_selected >= static_cast<int>(m_rows.size()))
            return nullptr;
        const Row& row = m_rows[m_selected];
        return row.kind == Row::Kind::Friend ? &row : nullptr;
    }

    double FriendListWidget::MaxScroll() const {
        double content = static_cast<double>(m_rows.size()) * ROW_H;
        double max = content - m_height + 4.0;
        return max > 0.0 ? max : 0.0;
    }

    int FriendListWidget::RowTop(int index) const {
        return m_y + 2 + index * ROW_H - static_cast<int>(m_scroll);
    }

    int FriendListWidget::RowAt(double mouseX, double mouseY) const {
        if (!ContainsPoint(mouseX, mouseY)) return -1;
        const int rowX = m_x + (m_width - ROW_W) / 2;
        if (mouseX < rowX || mouseX >= rowX + ROW_W) return -1;
        int idx = static_cast<int>((mouseY - m_y - 2 + m_scroll) / ROW_H);
        return (idx >= 0 && idx < static_cast<int>(m_rows.size())) ? idx : -1;
    }

    void FriendListWidget::OnClick(double mouseX, double mouseY) {
        const int idx = RowAt(mouseX, mouseY);
        if (idx < 0) return;
        Row& row = m_rows[idx];
        const int rowX = m_x + (m_width - ROW_W) / 2;
        const int top = RowTop(idx);
        const int miniY = top + (ROW_H - 4 - MINI_H) / 2;

        // Inline buttons take priority over row selection.
        auto inMini = [&](int miniX) {
            return mouseX >= miniX && mouseX < miniX + MINI_W &&
                   mouseY >= miniY && mouseY < miniY + MINI_H;
        };
        const int rightBtnX = rowX + ROW_W - MINI_W - 4;
        const int leftBtnX  = rightBtnX - MINI_W - 4;

        if (row.kind == Row::Kind::Request) {
            if (inMini(leftBtnX))  { if (onAccept)  onAccept(row);  return; }
            if (inMini(rightBtnX)) { if (onDecline) onDecline(row); return; }
        } else if (row.kind == Row::Kind::Invite) {
            if (inMini(rightBtnX)) { if (onInviteJoin) onInviteJoin(row); return; }
        } else if (row.kind == Row::Kind::Friend) {
            m_selected = idx;
            if (onSelectionChanged) onSelectionChanged();
        }
    }

    bool FriendListWidget::OnScroll(double deltaY) {
        if (MaxScroll() <= 0.0) return false;
        m_scroll = std::clamp(m_scroll - deltaY * ROW_H, 0.0, MaxScroll());
        return true;
    }

    void FriendListWidget::RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float) {
        m_scroll = std::clamp(m_scroll, 0.0, MaxScroll());
        g.Fill(m_x, m_y, m_x + m_width, m_y + m_height, 0x77000000);

        const int rowX = m_x + (m_width - ROW_W) / 2;
        g.EnableScissor(m_x, m_y, m_x + m_width, m_y + m_height);

        for (size_t i = 0; i < m_rows.size(); ++i) {
            const int top = RowTop(static_cast<int>(i));
            if (top + ROW_H < m_y || top > m_y + m_height) continue;
            const Row& row = m_rows[i];

            if (row.kind == Row::Kind::Header) {
                g.DrawCenteredString(row.title, m_x + m_width / 2,
                                     top + (ROW_H - FontRenderer::LINE_HEIGHT) / 2,
                                     0xFFFFFFFF);
                continue;
            }

            // Selection / hover chrome (friend rows only select).
            if (static_cast<int>(i) == m_selected &&
                row.kind == Row::Kind::Friend) {
                g.Fill(rowX - 2, top - 2, rowX + ROW_W + 2, top + ROW_H - 2, 0xFF808080);
                g.Fill(rowX - 1, top - 1, rowX + ROW_W + 1, top + ROW_H - 3, 0xFF000000);
            } else if (RowAt(mouseX, mouseY) == static_cast<int>(i) &&
                       row.kind == Row::Kind::Friend) {
                g.Fill(rowX - 1, top - 1, rowX + ROW_W + 1, top + ROW_H - 3, 0x30FFFFFF);
            }

            g.DrawString(row.title, rowX + 3, top + 1, 0xFFFFFFFF);
            if (!row.line2.empty()) {
                g.DrawString(row.line2, rowX + 3,
                             top + 1 + FontRenderer::LINE_HEIGHT + 2, row.line2Color);
            }
            if (!row.line3.empty()) {
                g.DrawString(row.line3, rowX + 3,
                             top + 1 + 2 * (FontRenderer::LINE_HEIGHT + 2), kGray);
            }

            // Inline mini buttons.
            const int miniY = top + (ROW_H - 4 - MINI_H) / 2;
            const int rightBtnX = rowX + ROW_W - MINI_W - 4;
            const int leftBtnX  = rightBtnX - MINI_W - 4;
            auto miniButton = [&](int x, const char* label, bool hovered) {
                g.BlitSprite(hovered ? "widget/button_highlighted" : "widget/button",
                             x, miniY, MINI_W, MINI_H);
                g.DrawCenteredString(label, x + MINI_W / 2,
                                     miniY + (MINI_H - FontRenderer::LINE_HEIGHT) / 2 + 1,
                                     0xFFFFFFFF);
            };
            auto hoverMini = [&](int x) {
                return mouseX >= x && mouseX < x + MINI_W &&
                       mouseY >= miniY && mouseY < miniY + MINI_H;
            };
            if (row.kind == Row::Kind::Request) {
                miniButton(leftBtnX, "Accept", hoverMini(leftBtnX));
                miniButton(rightBtnX, "Decline", hoverMini(rightBtnX));
            } else if (row.kind == Row::Kind::Invite) {
                miniButton(rightBtnX, "Join", hoverMini(rightBtnX));
            }
        }
        g.DisableScissor();

        if (MaxScroll() > 0.0) {
            const int sx = rowX + ROW_W + 4;
            g.BlitSprite("widget/scroller_background", sx, m_y, 6, m_height);
            double thumbH = std::max(32.0, static_cast<double>(m_height) * m_height /
                                     (static_cast<double>(m_rows.size()) * ROW_H));
            double frac = m_scroll / MaxScroll();
            int thumbY = m_y + static_cast<int>(frac * (m_height - thumbH));
            g.BlitSprite("widget/scroller", sx, thumbY, 6, static_cast<int>(thumbH));
        }
    }

    // ═══════════════════════════ FriendsScreen ══════════════════════════════

    void FriendsScreen::Init() {
        m_list = AddWidget(new FriendListWidget(0, 32, m_width, m_height - 32 - 64));
        m_list->onSelectionChanged = [this] { UpdateButtonStates(); };
        m_list->onAccept = [](const FriendListWidget::Row& row) {
            if (Client::g_friendsClient)
                Client::g_friendsClient->AcceptRequest(row.accountId);
        };
        m_list->onDecline = [](const FriendListWidget::Row& row) {
            if (Client::g_friendsClient)
                Client::g_friendsClient->DeclineRequest(row.accountId);
        };
        m_list->onInviteJoin = [this](const FriendListWidget::Row& row) {
            if (!Client::g_friendsClient) return;
            Client::g_friendsClient->ClearLatestInvite();
            // Resolve the address now (direct or relay) — same path as
            // clicking Join on the friend's row.
            Client::g_friendsClient->RequestJoinInfo(row.invite.fromId);
            m_status = "Connecting to " + row.invite.fromName + "'s world...";
            m_statusTicks = 100;
        };

        const int cx = m_width / 2;
        // Row 1: Add Friend | Invite | Join  (three 98-wide, SelectWorld style)
        AddWidget(new Button(cx - 154, m_height - 52, 98, 20, "Add Friend", [this] {
            m_manager->Push(std::make_unique<AddFriendScreen>());
        }));
        m_inviteButton = AddWidget(new Button(cx - 49, m_height - 52, 98, 20,
            "Invite", [this] { InviteSelected(); }));
        m_joinButton = AddWidget(new Button(cx + 56, m_height - 52, 98, 20,
            "Join", [this] { JoinSelected(); }));
        // Row 2: Remove | Done
        m_removeButton = AddWidget(new Button(cx - 154, m_height - 28, 150, 20,
            "Remove", [this] {
                if (const auto* sel = m_list->Selected()) {
                    if (Client::g_friendsClient)
                        Client::g_friendsClient->RemoveFriend(sel->accountId);
                }
            }));
        AddWidget(new Button(cx + 4, m_height - 28, 150, 20,
            "Done", [this] { OnClose(); }));

        m_builtGeneration = 0;   // force rebuild
        RebuildRows();
        UpdateButtonStates();
    }

    void FriendsScreen::RebuildRows() {
        std::vector<FriendListWidget::Row> rows;
        if (!Client::g_friendsClient) {
            m_list->SetRows(std::move(rows));
            return;
        }
        const auto roster = Client::g_friendsClient->GetRoster();
        m_builtGeneration = roster.generation;

        using Row = FriendListWidget::Row;

        Client::FriendInvite invite;
        if (Client::g_friendsClient->LatestInvite(invite)) {
            Row banner;
            banner.kind = Row::Kind::Invite;
            banner.title = invite.fromName + " invited you";
            banner.line2 = "to '" + invite.world + "'";
            banner.line2Color = kAqua;
            banner.invite = invite;
            rows.push_back(std::move(banner));
        }

        if (!roster.incoming.empty()) {
            Row header; header.title = "Friend Requests";
            rows.push_back(std::move(header));
            for (const auto& e : roster.incoming) {
                Row row;
                row.kind = Row::Kind::Request;
                row.title = e.name;
                row.line2 = "wants to be your friend";
                row.accountId = e.id;
                rows.push_back(std::move(row));
            }
        }

        Row header; header.title = "Friends";
        rows.push_back(std::move(header));
        for (const auto& e : roster.friends) {
            Row row;
            row.kind = Row::Kind::Friend;
            row.title = e.name;
            row.accountId = e.id;
            row.presence = e.presence;
            switch (e.presence.state) {
                case Client::FriendPresence::State::Hosting:
                    row.line2 = "Hosting '" + e.presence.world + "'";
                    row.line2Color = kGreen;
                    break;
                case Client::FriendPresence::State::Playing:
                    row.line2 = "Playing on " + e.presence.world;
                    row.line2Color = kGreen;
                    break;
                case Client::FriendPresence::State::Menu:
                    row.line2 = "Online";
                    row.line2Color = kGreen;
                    break;
                default:
                    row.line2 = "Offline";
                    row.line2Color = kDarker;
                    break;
            }
            rows.push_back(std::move(row));
        }
        for (const auto& e : roster.outgoing) {
            Row row;
            row.kind = Row::Kind::Friend;   // selectable so it can be removed
            row.title = e.name;
            row.accountId = e.id;
            row.line2 = "Request sent";
            row.line2Color = kGray;
            rows.push_back(std::move(row));
        }

        m_list->SetRows(std::move(rows));
        UpdateButtonStates();
    }

    void FriendsScreen::UpdateButtonStates() {
        const auto* sel = m_list ? m_list->Selected() : nullptr;
        const bool connected = Client::g_friendsClient &&
                               Client::g_friendsClient->IsConnected();
        const bool hosting = connected &&
            Client::g_friendsClient->CurrentPresence() ==
                Client::FriendPresence::State::Hosting;

        if (m_joinButton) {
            m_joinButton->active = connected && sel &&
                sel->presence.state == Client::FriendPresence::State::Hosting;
        }
        if (m_inviteButton) {
            m_inviteButton->active = connected && hosting && sel &&
                sel->presence.state != Client::FriendPresence::State::Offline;
            if (!hosting) {
                m_inviteButton->SetTooltip({"Host a world to invite friends."});
            } else {
                m_inviteButton->SetTooltip({});
            }
        }
        if (m_removeButton) m_removeButton->active = sel != nullptr;
    }

    void FriendsScreen::InviteSelected() {
        const auto* sel = m_list ? m_list->Selected() : nullptr;
        if (!sel || !Client::g_friendsClient) return;
        Client::g_friendsClient->SendInvite(sel->accountId);
        m_status = "Invite sent to " + sel->title;
        m_statusTicks = 60;   // ~3s at 20Hz
    }

    void FriendsScreen::JoinSelected() {
        const auto* sel = m_list ? m_list->Selected() : nullptr;
        if (!sel || !Client::g_friendsClient) return;
        Client::g_friendsClient->RequestJoinInfo(sel->accountId);
        m_status = "Fetching " + sel->title + "'s address...";
        m_statusTicks = 100;
    }

    void FriendsScreen::JoinResolved(const Client::JoinInfoResult& info) {
        if (info.host.empty() || info.port == 0) return;
        TitleAction action;
        action.kind = TitleAction::Kind::Multiplayer;
        action.host = info.host;
        action.port = info.port;
        // Relay joins dial the friends service and present this ticket
        // before the game handshake; empty for direct connections.
        action.relayTicket = info.ticket;
        SetTitleAction(std::move(action));
        // From the title phase, RunTitleScreenPhase consumes this and starts
        // the session; in-game, the pause branch routes it through
        // quit-to-title with a pending auto-join. Either way this screen is
        // about to be torn down — nothing more to do here.
    }

    void FriendsScreen::Tick() {
        if (m_statusTicks > 0 && --m_statusTicks == 0) m_status.clear();
        if (!Client::g_friendsClient) return;

        // Roster changed → rebuild rows in place.
        if (Client::g_friendsClient->GetRoster().generation != m_builtGeneration) {
            RebuildRows();
        }
        // Op errors → status line.
        std::string err = Client::g_friendsClient->ConsumeLastError();
        if (!err.empty()) {
            m_status = std::move(err);
            m_statusTicks = 80;
        }
        // Join lookups resolving.
        Client::JoinInfoResult join;
        while (Client::g_friendsClient->PollJoinResult(join)) {
            if (join.ok) {
                Log::Info("[Friends] joining %s:%u (%s)", join.host.c_str(),
                          static_cast<unsigned>(join.port),
                          join.relay ? "relayed" : "direct");
                JoinResolved(join);
            } else {
                m_status = join.error.empty() ? "Couldn't get the address."
                                              : join.error;
                m_statusTicks = 80;
            }
        }
        UpdateButtonStates();
    }

    void FriendsScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        Screen::Render(g, mouseX, mouseY, partialTick);
        g.DrawCenteredString(m_title, m_width / 2, 12, 0xFFFFFFFF);
        RenderMenuSeparators(g, m_width, 30, m_height - 64);

        if (!Client::g_friendsClient) {
            g.DrawCenteredString("Log in from the ObeyCraft launcher to use friends.",
                                 m_width / 2, m_height / 2 - 4, kGray);
        } else if (!Client::g_friendsClient->IsConnected()) {
            g.DrawCenteredString("Connecting to the friends server...",
                                 m_width / 2, 22, kGray);
        }

        if (!m_status.empty()) {
            g.DrawCenteredString(m_status, m_width / 2, m_height - 62, kAqua);
        }
    }

    // ═══════════════════════════ AddFriendScreen ════════════════════════════

    void AddFriendScreen::Init() {
        const int cx = m_width / 2;

        m_nameBox = AddWidget(new EditBox(cx - 100, m_height / 4 + 24, 200, 20,
                                          "Username"));
        m_nameBox->SetMaxLength(16);
        m_nameBox->SetHint("Friend's username");
        m_nameBox->SetResponder([this](const std::string& text) {
            if (m_sendButton) m_sendButton->active = !text.empty();
        });
        SetFocus(m_nameBox);

        m_sendButton = AddWidget(new Button(cx - 100, m_height / 4 + 72, 200, 20,
            "Send Request", [this] { Send(); }));
        m_sendButton->active = false;

        AddWidget(new Button(cx - 100, m_height / 4 + 96, 200, 20,
            "Cancel", [this] { OnClose(); }));
    }

    void AddFriendScreen::Send() {
        const std::string name = m_nameBox ? m_nameBox->GetText() : "";
        if (name.empty() || !Client::g_friendsClient) return;
        Client::g_friendsClient->SendFriendRequest(name);
        OnClose();   // back to FriendsScreen; result shows via roster/status
    }

    bool AddFriendScreen::KeyPressed(int glfwKey, int glfwMods) {
        if (glfwKey == GLFW_KEY_ENTER || glfwKey == GLFW_KEY_KP_ENTER) {
            Send();
            return true;
        }
        return Screen::KeyPressed(glfwKey, glfwMods);
    }

    void AddFriendScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        Screen::Render(g, mouseX, mouseY, partialTick);
        g.DrawCenteredString(m_title, m_width / 2, 20, 0xFFFFFFFF);
        g.DrawString("Username", m_width / 2 - 100, m_height / 4 + 12, 0xFFA0A0A0);
    }

} // namespace Render
