// File: src/client/renderer/gui/InventoryScreen.cpp
#include "InventoryScreen.hpp"
#include "ContainerScreen.hpp"
#include "FurnaceScreen.hpp"
#include "CreativeModeInventoryScreen.hpp"
#include "CraftingScreen.hpp"
#include "GuiGraphics.hpp"
#include "items/PlayerInventoryPreview.hpp"
#include "screens/Screen.hpp"          // LoadStandaloneGuiTexture
#include "client/entity/Player.hpp"
#include "common/inventory/ChestMenu.hpp"
#include "common/inventory/FurnaceMenu.hpp"
#include "common/inventory/UtilityMenus.hpp"
#include "common/inventory/SystemMenus.hpp"
#include "common/inventory/CraftingMenu.hpp"
#include "common/core/Log.hpp"

namespace Render {

    namespace {
        // The local player, kept here so OpenInventoryScreen can read the game
        // mode without PlatformMain having to pass it in at every call site.
        Game::ClientPlayer* s_player = nullptr;

        // Every container screen, so open/close/rebind can sweep them all.
        void CloseAllContainerScreens() {
            GetSurvivalInventoryScreen().CloseSilently();
            GetCreativeInventoryScreen().CloseSilently();
            GetCraftingScreen().CloseSilently();
            GetContainerScreen().CloseSilently();
            GetFurnaceScreen().CloseSilently();
        }
    }

    InventoryScreen& GetSurvivalInventoryScreen() {
        static InventoryScreen s;
        return s;
    }

    AbstractContainerScreen& GetInventoryScreen() {
        // Whichever is open owns input; the survival screen stands in when none
        // is, because all of its handlers early-out on !IsOpen.
        //
        // EVERY container screen has to be listed here. This is what the host
        // loop renders and feeds input to, so a screen missing from this chain
        // opens, holds a live menu, and is simply never drawn — which is
        // exactly how a chest could look like "right-click does nothing" while
        // the server had already built and sent the menu.
        if (GetCraftingScreen().IsOpen())          return GetCraftingScreen();
        if (GetContainerScreen().IsOpen())         return GetContainerScreen();
        if (GetFurnaceScreen().IsOpen())           return GetFurnaceScreen();
        if (GetCreativeInventoryScreen().IsOpen()) return GetCreativeInventoryScreen();
        return GetSurvivalInventoryScreen();
    }

    void OpenInventoryScreen() {
        // The inventory key never opens a BLOCK container — if one is already
        // showing, this is the keypress that dismisses it (MC: the key closes
        // whatever screen is up).
        // Same list as GetInventoryScreen: the inventory key dismisses
        // whatever block screen is up rather than opening the player's own.
        if (GetCraftingScreen().IsOpen())  { GetCraftingScreen().Close();  return; }
        if (GetContainerScreen().IsOpen()) { GetContainerScreen().Close(); return; }
        if (GetFurnaceScreen().IsOpen())   { GetFurnaceScreen().Close();   return; }
        if (s_player && s_player->IsCreative()) GetCreativeInventoryScreen().Open();
        else                                    GetSurvivalInventoryScreen().Open();
    }

    void SetInventoryScreenPlayer(Game::ClientPlayer* player) {
        // Rebinding means a new session (or a teardown, with player == null).
        // The screens are singletons that outlive the ClientPlayer, so they are
        // closed and re-pointed here — one left open across the swap would
        // render against a menu bound to the previous player's inventory.
        CloseAllContainerScreens();

        s_player = player;
        RebuildPlayerInventoryMenu(player);
        GetSurvivalInventoryScreen().SetPlayer(player);
        GetCreativeInventoryScreen().SetPlayer(player);
        GetCraftingScreen().SetPlayer(player);
    }

    // ─── Server-driven menu changes ──────────────────────────────
    void OpenClientContainerScreen(Game::MenuType type, uint32_t containerId,
                                   const std::string& title) {
        if (!s_player) return;

        // Whatever is up belongs to the menu being replaced.
        CloseAllContainerScreens();

        switch (type) {
            case Game::MenuType::Crafting: {
                auto menu = std::make_unique<Game::CraftingMenu>(&s_player->inventory);
                menu->containerId = containerId;
                SetClientContainerMenu(std::move(menu), type);
                GetCraftingScreen().SetTitle(title.empty() ? "Crafting" : title);
                GetCraftingScreen().Open();
                break;
            }
            // ── Block containers ──────────────────────────────────────────
            // All ChestMenu, so one branch builds them: the type gives the
            // grid dimensions, the menu owns a scratch container on this side
            // (there is no block entity on the client), and the server's slot
            // sync fills it.
            case Game::MenuType::Generic9x1:
            case Game::MenuType::Generic9x2:
            case Game::MenuType::Generic9x3:
            case Game::MenuType::Generic9x4:
            case Game::MenuType::Generic9x5:
            case Game::MenuType::Generic9x6:
            case Game::MenuType::Generic3x3:
            case Game::MenuType::Hopper: {
                int rows = 3, columns = 9;
                if (type == Game::MenuType::Generic3x3)      { rows = 3; columns = 3; }
                else if (type == Game::MenuType::Hopper)     { rows = 1; columns = 5; }
                else {
                    rows = 1 + (static_cast<int>(type) -
                                static_cast<int>(Game::MenuType::Generic9x1));
                }
                auto menu = std::make_unique<Game::ChestMenu>(&s_player->inventory,
                                                              rows, columns);
                menu->containerId = containerId;
                SetClientContainerMenu(std::move(menu), type);
                GetContainerScreen().Configure(
                    ContainerScreen::LayoutFor(type, rows),
                    title.empty() ? "Container" : title);
                GetContainerScreen().Open();
                break;
            }

            case Game::MenuType::Furnace:
            case Game::MenuType::BlastFurnace:
            case Game::MenuType::Smoker: {
                const Game::CookingKind kind =
                    type == Game::MenuType::BlastFurnace ? Game::CookingKind::Blasting
                  : type == Game::MenuType::Smoker       ? Game::CookingKind::Smoking
                                                         : Game::CookingKind::Smelting;
                auto menu = std::make_unique<Game::FurnaceMenu>(&s_player->inventory, kind);
                menu->containerId = containerId;
                SetClientContainerMenu(std::move(menu), type);
                GetFurnaceScreen().Configure(type, title.empty() ? "Furnace" : title);
                GetFurnaceScreen().Open();
                break;
            }

            case Game::MenuType::Stonecutter:
            case Game::MenuType::Grindstone:
            case Game::MenuType::CartographyTable:
            case Game::MenuType::Loom:
            case Game::MenuType::Smithing:
            case Game::MenuType::Anvil: {
                std::unique_ptr<Game::AbstractContainerMenu> menu;
                switch (type) {
                    case Game::MenuType::Stonecutter:
                        menu = std::make_unique<Game::StonecutterMenu>(&s_player->inventory); break;
                    case Game::MenuType::Grindstone:
                        menu = std::make_unique<Game::GrindstoneMenu>(&s_player->inventory); break;
                    case Game::MenuType::CartographyTable:
                        menu = std::make_unique<Game::CartographyTableMenu>(&s_player->inventory); break;
                    case Game::MenuType::Loom:
                        menu = std::make_unique<Game::LoomMenu>(&s_player->inventory); break;
                    case Game::MenuType::Smithing:
                        menu = std::make_unique<Game::SmithingMenu>(&s_player->inventory); break;
                    default:
                        menu = std::make_unique<Game::AnvilMenu>(&s_player->inventory); break;
                }
                menu->containerId = containerId;
                SetClientContainerMenu(std::move(menu), type);
                GetContainerScreen().Configure(
                    ContainerScreen::LayoutFor(type, 3),
                    title.empty() ? "Container" : title);
                GetContainerScreen().Open();
                break;
            }

            case Game::MenuType::Enchantment:
            case Game::MenuType::BrewingStand:
            case Game::MenuType::Beacon:
            case Game::MenuType::Crafter3x3: {
                std::unique_ptr<Game::AbstractContainerMenu> menu;
                switch (type) {
                    case Game::MenuType::Enchantment:
                        menu = std::make_unique<Game::EnchantmentMenu>(&s_player->inventory); break;
                    case Game::MenuType::BrewingStand:
                        menu = std::make_unique<Game::BrewingStandMenu>(&s_player->inventory); break;
                    case Game::MenuType::Beacon:
                        menu = std::make_unique<Game::BeaconMenu>(&s_player->inventory); break;
                    default:
                        menu = std::make_unique<Game::CrafterMenu>(&s_player->inventory); break;
                }
                menu->containerId = containerId;
                SetClientContainerMenu(std::move(menu), type);
                GetContainerScreen().Configure(
                    ContainerScreen::LayoutFor(type, 3),
                    title.empty() ? "Container" : title);
                GetContainerScreen().Open();
                break;
            }

            case Game::MenuType::Inventory:
                // Not something the server opens — it is what you fall back to.
                SetClientContainerMenu(nullptr, Game::MenuType::Inventory);
                break;

            default:
                // Menu types whose screens land in later phases. Falling back
                // to the plain inventory beats leaving a menu open with no
                // screen drawing it.
                SetClientContainerMenu(nullptr, Game::MenuType::Inventory);
                break;
        }
    }

    void ApplyContainerFullSync(Game::MenuType menuType, uint32_t containerId,
                                const std::vector<Game::ItemStack>& slots) {
        // The snapshot's menuType is authoritative. Two cases need acting on
        // before the contents can be applied, because the slot indices only
        // mean anything against the right menu:
        //
        //  • it names a menu we do not have — the OpenScreen packet was lost or
        //    reordered behind its snapshot; build it now.
        //  • it names the plain inventory while a block screen is up — the
        //    server closed our container (we asked, or it decided), so the
        //    screen comes down.
        if (menuType != ClientContainerMenuType()) {
            if (menuType == Game::MenuType::Inventory) {
                // All of them, not just the crafting screen — the server can
                // close any block menu, and leaving another one up would keep
                // rendering against a menu that no longer exists.
                CloseAllContainerScreens();
                SetClientContainerMenu(nullptr, Game::MenuType::Inventory);
            } else {
                Log::Debug("[ContainerSync] Snapshot for menu type %u arrived before its "
                           "OpenScreen — building it now",
                           static_cast<unsigned>(menuType));
                OpenClientContainerScreen(menuType, containerId, "");
            }
        }
        ApplyContainerSlots(slots);
    }

    // ─── Background ──────────────────────────────────────────────
    TextureHandle InventoryScreen::EnsureBackground() {
        if (m_backgroundTried) return m_background;
        m_backgroundTried = true;
        int w = 0, h = 0;
        m_background = LoadStandaloneGuiTexture(
            "assets/textures/gui/container/inventory.png", w, h);
        return m_background;
    }

    // ─── Rendering ───────────────────────────────────────────────
    void InventoryScreen::RenderBg(GuiGraphics& g, int leftPos, int topPos) {
        // MC InventoryScreen.renderBg line 79: blit the panel from the 256x256
        // sheet, then draw the player into the preview box.
        const TextureHandle bg = EnsureBackground();
        if (bg == INVALID_TEXTURE) {
            g.Fill(leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H, 0xC0202020);
        } else {
            g.Blit(bg, leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H,
                   0.0f, 0.0f, (float)IMAGE_W / 256.0f, (float)IMAGE_H / 256.0f);
        }

        Game::ClientPlayer* player = Player();
        if (!player) return;

        // MC line 80: renderEntityInInventoryFollowsMouse(graphics, xo + 26,
        // yo + 8, xo + 75, yo + 78, 30, 0.0625F, xMouse, yMouse, player).
        // Bump the stratum so the figure sits above the panel background; the
        // slots drawn afterwards get their own stratum from the base class.
        g.NextStratum();
        StickFigurePose pose;
        pose.bodyYawDeg   = player->visualYaw;
        pose.headYawDeg   = player->visualYaw;
        pose.headPitchDeg = player->visualPitch;
        pose.isCrouching  = false;
        RenderStickFigureInInventory(
            g,
            leftPos + PREVIEW_X0, topPos + PREVIEW_Y0,
            leftPos + PREVIEW_X1, topPos + PREVIEW_Y1,
            PREVIEW_SIZE, 0.0625f,
            MouseGui().x, MouseGui().y,
            pose,
            player->color);
    }

    void InventoryScreen::RenderLabels(GuiGraphics& g, int leftPos, int topPos) {
        // MC InventoryScreen.renderLabels line 58 — the title only ("Crafting",
        // container.crafting), dark grey and WITHOUT a drop shadow.
        g.DrawString("Crafting", leftPos + TITLE_X, topPos + TITLE_Y, LABEL_COLOR, false);
    }

    void InventoryScreen::ContainerTick() {
        // MC InventoryScreen.containerTick line 33: a player who gains infinite
        // materials is handed straight to the creative screen. Silent on both
        // ends so the cursor stack survives the swap — MC's setScreen does not
        // close the container either.
        if (Player() && Player()->IsCreative()) {
            CloseSilently();
            GetCreativeInventoryScreen().OpenSilently();
        }
    }

} // namespace Render
