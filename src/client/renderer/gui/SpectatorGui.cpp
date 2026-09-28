// File: src/client/renderer/gui/SpectatorGui.cpp
#include "SpectatorGui.hpp"

#include "client/input/KeyMapping.hpp"
#include "client/network/ClientPlayerInfo.hpp"
#include "common/entity/PlayerColors.hpp"
#include "common/text/Language.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Render {

    namespace {

        int64_t NowMs() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // MC ARGB.white(alpha).
        uint32_t White(float alpha) {
            const uint32_t a = static_cast<uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
            return (a << 24) | 0x00FFFFFFu;
        }

        // MC ARGB.colorFromFloat(alpha, b, b, b).
        uint32_t Grey(float alpha, float brightness) {
            const auto c = [](float v) {
                return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            const uint32_t g = c(brightness);
            return (c(alpha) << 24) | (g << 16) | (g << 8) | g;
        }

        constexpr int64_t kFadeOutDelayMs = 5000;   // SpectatorGui.FADE_OUT_DELAY
        constexpr float   kFadeOutTimeMs  = 2000.0f; // FADE_OUT_TIME
        constexpr int     kClose          = 8;
        constexpr int     kScrollRight    = 7;

    } // namespace

    void DrawPlayerFace(GuiGraphics& g, int x, int y, int size, uint8_t colorId,
                        float alpha, float brightness) {
        // The stick figure's head on an 8×8 grid: the player's colour, two
        // eyes and the smile — the same face the world draws.
        const Game::PlayerColorEntry& entry =
            Game::LookupPlayerColor(static_cast<Game::PlayerColorId>(colorId));
        const auto channel = [&](uint8_t v, float scale) {
            return static_cast<uint32_t>(std::clamp(v * brightness * scale, 0.0f, 255.0f));
        };
        const uint32_t a = static_cast<uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f) << 24;
        const uint32_t skin = a | (channel(entry.r, 1.0f) << 16) | (channel(entry.g, 1.0f) << 8) |
                              channel(entry.b, 1.0f);
        const uint32_t dark = a | (channel(entry.r, 0.2f) << 16) | (channel(entry.g, 0.2f) << 8) |
                              channel(entry.b, 0.2f);
        g.PushMatrix();
        g.Translate(static_cast<float>(x), static_cast<float>(y));
        g.Scale(static_cast<float>(size) / 8.0f, static_cast<float>(size) / 8.0f);
        g.Fill(0, 0, 8, 8, skin);
        g.Fill(2, 2, 3, 4, dark);   // eyes
        g.Fill(5, 2, 6, 4, dark);
        g.Fill(1, 5, 2, 6, dark);   // smile
        g.Fill(6, 5, 7, 6, dark);
        g.Fill(2, 6, 6, 7, dark);
        g.PopMatrix();
    }

    SpectatorGui& GetSpectatorGui() {
        static SpectatorGui s_gui;
        return s_gui;
    }

    // ── Menu (SpectatorMenu) ──────────────────────────────────────────────

    std::vector<SpectatorGui::Item> SpectatorGui::ListedPlayers() const {
        // TeleportToPlayerMenuCategory(getListedOnlinePlayers()): everyone who
        // is not a spectator, in profile order (join order here).
        std::vector<Item> out;
        for (const auto& [id, info] : Client::PlayerInfoMap()) {
            if (info.IsSpectator()) continue;
            Item item;
            item.kind     = ItemKind::Player;
            item.playerId = id;
            item.name     = info.name;
            item.colorId  = info.colorId;
            out.push_back(std::move(item));
        }
        return out;
    }

    void SpectatorGui::OpenMenu() {
        m_open         = true;
        m_selectedSlot = -1;
        m_page         = 0;
        SelectCategory(Category::Root);
    }

    void SpectatorGui::CloseMenu() {
        // onSpectatorMenuClosed: menu = null, lastSelectionTime = 0.
        m_open            = false;
        m_items.clear();
        m_selectedSlot    = -1;
        m_page            = 0;
        m_lastSelectionMs = 0;
    }

    void SpectatorGui::Reset() { CloseMenu(); }

    void SpectatorGui::SelectCategory(Category category) {
        m_category = category;
        m_items.clear();
        switch (category) {
            case Category::Root: {
                // RootSpectatorMenuCategory: teleport to player, teleport to
                // team. The player page's list is taken now, as MC builds the
                // category (and its list) with the menu.
                Item toPlayer; toPlayer.kind = ItemKind::TeleportToPlayer;
                Item toTeam;   toTeam.kind   = ItemKind::TeleportToTeam;
                m_items.push_back(toPlayer);
                m_items.push_back(toTeam);
                break;
            }
            case Category::TeleportToPlayer:
                m_items = ListedPlayers();
                break;
            case Category::TeleportToTeam:
                // No scoreboard teams in this port: no team entries.
                break;
        }
        m_selectedSlot = -1;
        m_page         = 0;
    }

    SpectatorGui::Item SpectatorGui::GetItem(int slot) const {
        Item item;
        const int index = slot + m_page * 6;
        if (m_page > 0 && slot == 0) {
            item.kind = ItemKind::ScrollLeft;
        } else if (slot == kScrollRight) {
            item.kind = index < static_cast<int>(m_items.size()) ? ItemKind::ScrollRightEnabled
                                                                 : ItemKind::ScrollRightDisabled;
        } else if (slot == kClose) {
            item.kind = ItemKind::Close;
        } else if (index >= 0 && index < static_cast<int>(m_items.size())) {
            item = m_items[static_cast<size_t>(index)];
        }
        return item;
    }

    bool SpectatorGui::IsEnabled(const Item& item) const {
        switch (item.kind) {
            case ItemKind::Empty:               return false;
            case ItemKind::Close:               return true;
            case ItemKind::ScrollLeft:          return true;
            case ItemKind::ScrollRightEnabled:  return true;
            case ItemKind::ScrollRightDisabled: return false;
            // TeleportToPlayerMenuCategory.isEnabled: its list is not empty.
            case ItemKind::TeleportToPlayer:    return !ListedPlayers().empty();
            // TeleportToTeamMenuCategory.isEnabled: there is a team.
            case ItemKind::TeleportToTeam:      return false;
            case ItemKind::Player: {
                // PlayerMenuItem.isEnabled: that player is not (now) a spectator.
                const auto& infos = Client::PlayerInfoMap();
                const auto it = infos.find(item.playerId);
                return it != infos.end() && !it->second.IsSpectator();
            }
        }
        return false;
    }

    std::string SpectatorGui::NameOf(const Item& item) const {
        using Game::Language::GetOrDefault;
        switch (item.kind) {
            case ItemKind::Empty:               return {};
            case ItemKind::Close:               return GetOrDefault("spectatorMenu.close", "Close Menu");
            case ItemKind::ScrollLeft:          return GetOrDefault("spectatorMenu.previous_page", "Previous Page");
            case ItemKind::ScrollRightEnabled:
            case ItemKind::ScrollRightDisabled: return GetOrDefault("spectatorMenu.next_page", "Next Page");
            case ItemKind::TeleportToPlayer:    return GetOrDefault("spectatorMenu.teleport", "Teleport to Player");
            case ItemKind::TeleportToTeam:      return GetOrDefault("spectatorMenu.team_teleport", "Teleport to Team Member");
            case ItemKind::Player:              return item.name;
        }
        return {};
    }

    std::string SpectatorGui::Prompt() const {
        using Game::Language::GetOrDefault;
        switch (m_category) {
            case Category::Root:
                return GetOrDefault("spectatorMenu.root.prompt",
                                    "Press a key to select a command, and again to use it.");
            case Category::TeleportToPlayer:
                return GetOrDefault("spectatorMenu.teleport.prompt", "Select a player to teleport to");
            case Category::TeleportToTeam:
                return GetOrDefault("spectatorMenu.team_teleport.prompt", "Select a team to teleport to");
        }
        return {};
    }

    void SpectatorGui::SelectSlot(int slot) {
        const Item item = GetItem(slot);
        if (item.kind == ItemKind::Empty) return;
        if (m_selectedSlot == slot && IsEnabled(item)) {
            UseItem(item);
        } else {
            m_selectedSlot = slot;
        }
    }

    void SpectatorGui::UseItem(const Item& item) {
        switch (item.kind) {
            case ItemKind::Empty:
            case ItemKind::ScrollRightDisabled:
                break;
            case ItemKind::Close:
                CloseMenu();
                break;
            case ItemKind::ScrollLeft:
                --m_page;
                break;
            case ItemKind::ScrollRightEnabled:
                ++m_page;
                break;
            case ItemKind::TeleportToPlayer:
                SelectCategory(Category::TeleportToPlayer);
                break;
            case ItemKind::TeleportToTeam:
                SelectCategory(Category::TeleportToTeam);
                break;
            case ItemKind::Player:
                // PlayerMenuItem.selectItem: ServerboundTeleportToEntityPacket.
                // The menu stays as it is, as in MC.
                if (IsEnabled(item) && m_teleport) m_teleport(item.playerId);
                break;
        }
    }

    // ── Input (SpectatorGui) ──────────────────────────────────────────────

    void SpectatorGui::OnHotbarSelected(int slot) {
        m_lastSelectionMs = NowMs();
        if (m_open) SelectSlot(slot);
        else        OpenMenu();
    }

    void SpectatorGui::OnMouseScrolled(int wheel) {
        if (!m_open || wheel == 0) return;
        int newSlot = m_selectedSlot + wheel;
        while (newSlot >= 0 && newSlot <= 8) {
            const Item item = GetItem(newSlot);
            if (item.kind != ItemKind::Empty && IsEnabled(item)) break;
            newSlot += wheel;
        }
        if (newSlot >= 0 && newSlot <= 8) {
            SelectSlot(newSlot);
            m_lastSelectionMs = NowMs();
        }
    }

    void SpectatorGui::OnHotbarActionKeyPressed() {
        m_lastSelectionMs = NowMs();
        if (m_open) {
            if (m_selectedSlot != -1) SelectSlot(m_selectedSlot);
        } else {
            OpenMenu();
        }
    }

    // ── Drawing ───────────────────────────────────────────────────────────

    float SpectatorGui::HotbarAlpha() const {
        const int64_t delta = m_lastSelectionMs - NowMs() + kFadeOutDelayMs;
        return std::clamp(static_cast<float>(delta) / kFadeOutTimeMs, 0.0f, 1.0f);
    }

    void SpectatorGui::RenderHotbar(GuiGraphics& g) {
        if (!m_open) return;
        const float alpha = HotbarAlpha();
        if (alpha <= 0.0f) {
            CloseMenu();   // menu.exit()
            return;
        }
        const int screenCenter = g.GuiWidth() / 2;
        const int y = static_cast<int>(std::floor(static_cast<float>(g.GuiHeight()) - 22.0f * alpha));
        const uint32_t color = White(alpha);
        g.BlitSprite("hud/hotbar", screenCenter - 91, y, 182, 22, color);
        if (m_selectedSlot >= 0) {
            g.BlitSprite("hud/hotbar_selection", screenCenter - 91 - 1 + m_selectedSlot * 20, y - 1, 24, 23, color);
        }
        g.NextStratum();
        for (int slot = 0; slot < 9; ++slot) {
            RenderSlot(g, slot, g.GuiWidth() / 2 - 90 + slot * 20 + 2, y + 3, alpha, GetItem(slot));
        }
    }

    void SpectatorGui::RenderSlot(GuiGraphics& g, int slot, int x, int y, float alpha, const Item& item) {
        if (item.kind == ItemKind::Empty) return;
        const bool  enabled    = IsEnabled(item);
        const float brightness = enabled ? 1.0f : 0.25f;
        const uint32_t tint = Grey(alpha, brightness);
        switch (item.kind) {
            case ItemKind::Close:
                g.BlitSprite("spectator/close", x, y, 16, 16, tint);
                break;
            case ItemKind::ScrollLeft:
                g.BlitSprite("spectator/scroll_left", x, y, 16, 16, tint);
                break;
            case ItemKind::ScrollRightEnabled:
            case ItemKind::ScrollRightDisabled:
                g.BlitSprite("spectator/scroll_right", x, y, 16, 16, tint);
                break;
            case ItemKind::TeleportToPlayer:
                g.BlitSprite("spectator/teleport_to_player", x, y, 16, 16, tint);
                break;
            case ItemKind::TeleportToTeam:
                g.BlitSprite("spectator/teleport_to_team", x, y, 16, 16, tint);
                break;
            case ItemKind::Player:
                // PlayerMenuItem.extractIcon: the face at (2, 2), 12 square,
                // white(alpha) — never dimmed.
                DrawPlayerFace(g, x + 2, y + 2, 12, item.colorId, alpha, 1.0f);
                break;
            case ItemKind::Empty:
                break;
        }
        if (alpha > 0.0f && enabled) {
            // The slot's key, right-aligned in the slot's corner.
            std::string key;
            if (Input::Binds::Hotbar[slot]) key = Input::Binds::Hotbar[slot]->key.DisplayName();
            if (!key.empty()) {
                g.NextStratum();
                g.DrawString(key, x + 19 - 2 - g.GetStringWidth(key), y + 6 + 3, White(alpha));
            }
        }
    }

    void SpectatorGui::RenderAction(GuiGraphics& g) {
        const float alpha = HotbarAlpha();
        if (alpha <= 0.0f || !m_open) return;
        const Item item = GetItem(m_selectedSlot);
        const std::string action = item.kind == ItemKind::Empty ? Prompt() : NameOf(item);
        if (action.empty()) return;
        const int width = g.GetStringWidth(action);
        const int x = (g.GuiWidth() - width) / 2;
        const int y = g.GuiHeight() - 35;
        g.DrawStringWithBackdrop(action, x, y, width, White(alpha));
    }

} // namespace Render
