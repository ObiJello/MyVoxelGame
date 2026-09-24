// File: src/client/renderer/gui/screens/BookScreens.hpp
//
// The book screens — MC client/gui/screens/inventory BookViewScreen,
// LecternScreen, BookEditScreen and BookSignScreen, with their PageButton and
// the MultiLineEditBox the editor types into:
//
//   BookViewScreen  textures/gui/book.png (192×192 of a 256 sheet) at
//                   ((w-192)/2, 2); the page's text wrapped at 114 px, at most
//                   14 lines, from (left+36, top+30); "Page n of m" right-
//                   aligned at left+148, top+16; page buttons (widget/
//                   page_forward / page_backward sprites, 23×13) at left+116 /
//                   left+43, top+157, shown only where there is a page to go
//                   to; PageUp / PageDown; Done (200 wide) below the book;
//                   clicking styled text runs its click_event (change_page,
//                   run_command, suggest_command, copy_to_clipboard).
//   LecternScreen   the same view over a LecternMenu: page turns are menu
//                   buttons the server applies (so every reader of that
//                   lectern turns together), plus Take Book beside Done.
//   BookEditScreen  a book and quill: a 122×134 multi-line editor (114 px of
//                   text, 14 lines, 1024 characters a page), pages added by
//                   paging past the last, Sign / Done.
//   BookSignScreen  "Enter Book Title:", a 15-character title, "by <you>",
//                   the finalize warning, Sign and Close / Cancel.
//
// Text metrics are MC's Font: a glyph advances its width + 1 (+1 bold) and a
// space advances 4 — so a page breaks where it breaks in Minecraft. The
// engine's font atlas covers printable ASCII; other characters take a blank
// 4-pixel cell, as they do everywhere else in this renderer.
#pragma once

#include "Screen.hpp"
#include "common/data/BookContent.hpp"
#include "common/network/packets/game/BookPackets.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Game { struct ItemStack; }

namespace Render {

    // ── Host hooks ──────────────────────────────────────────────────────────

    // MC ClientPacketListener.handleOpenBook: show the written (or
    // writable) book now in `hand`.
    void OpenBookFromHand(uint32_t hand);
    // MC LocalPlayer.openItemGui: a book and quill was used in `hand`.
    void OpenBookEditScreen(const Game::ItemStack& book, uint32_t hand);
    // The server opened a lectern menu (OpenScreenS2C, MenuType::Lectern);
    // the client's LecternMenu is already the container menu. `mayBuild` is
    // the player's Abilities.mayBuild (not adventure / spectator): only then
    // is there a Take Book button.
    void OpenLecternScreen(const std::string& title, bool mayBuild);
    // The lectern menu went away (the server closed it, or another menu
    // replaced it): bring the screen down without telling the server.
    void CloseLecternScreen();
    bool IsLecternScreenOpen();

    // Outbound packets, drained by the host loop (PlatformMain) each tick.
    bool ConsumeEditBook(Network::EditBookC2SPacket& out);
    bool ConsumeContainerButtonClick(Network::ContainerButtonClickC2SPacket& out);
    // Queue a menu button press (MC ServerboundContainerButtonClickPacket)
    // from any container screen — the merchant screen's reroll uses it — on
    // the same queue as the lectern's, so the host sends them in order.
    void QueueContainerButtonClick(uint32_t containerId, uint32_t buttonId);
    // The lectern screen closed on the player's side (MC
    // LocalPlayer.closeContainer): send InventoryCloseC2S.
    bool ConsumeBookContainerClose();

} // namespace Render
