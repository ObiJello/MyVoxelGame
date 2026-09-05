// File: src/client/renderer/gui/screens/PackSelectionScreen.hpp
//
// Options → Resource Packs: MC's PackSelectionScreen, PackSelectionModel
// and TransferableSelectionList (client/gui/screens/packs/), on this
// engine's widgets.
//
//   • Two lists side by side, "Available" and "Selected", 200 wide, rows
//     36 high: the pack icon (pack.png or unknown_pack.png), the name, and
//     two grey lines of description. A pack made for another version is
//     tinted red, with the reason on hover (PackCompatibility); it is
//     selected on click without MC's extra confirmation screen.
//   • Hovering a row's icon shows what a click there does, with the
//     transferable_list sprites: the arrow-right to select; on a selected
//     pack the left half unselects, the top-right quarter moves it up, the
//     bottom-right down. Higher in the Selected list = higher priority.
//   • A search box filters both lists by id, name and description.
//   • "Open Pack Folder" opens resourcepacks/; the folder is rescanned
//     once a second while the screen is up (MC's directory watcher + its
//     20-tick reload cooldown).
//   • Done / Esc commits: the repository takes the new order, options.txt
//     gets resourcePacks / incompatibleResourcePacks (Options
//     .updateResourcePacks), and if the list changed the host reloads
//     resources (APPLY_RESOURCE_PACKS → PlatformMain::ReloadResources).
#pragma once

#include "Screen.hpp"
#include "Widgets.hpp"
#include "../../backend/RenderTypes.hpp"
#include "client/resource/ResourcePacks.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace Render {

    class PackSelectionScreen;

    class TransferableSelectionList : public AbstractWidget {
    public:
        static constexpr int ROW_H     = 36;   // ObjectSelectionList itemHeight
        static constexpr int HEADER_H  = 13;   // (int)(9 * 1.5)
        static constexpr int ICON      = 32;
        static constexpr int PADDING   = 2;    // ENTRY_PADDING
        static constexpr int SCROLLBAR = 6;
        static constexpr int TEXT_W    = 157;  // MAX_DESCRIPTION_WIDTH_PIXELS

        TransferableSelectionList(PackSelectionScreen& screen, bool selectedList,
                                  int x, int y, int width, int height, std::string title);

        // The pack ids to show, top first.
        void SetIds(std::vector<std::string> ids);
        void SetBounds(int x, int y, int width, int height);

        void OnClick(double mouseX, double mouseY) override;
        bool OnScroll(double deltaY) override;

    protected:
        void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;

    private:
        int    RowAt(double mouseX, double mouseY) const;   // -1 = none
        int    RowTop(int index) const;
        int    ContentX() const { return m_x + 2 + PADDING; }
        int    RowWidth() const { return m_width - 4; }
        bool   Scrollable() const { return ContentHeight() > m_height; }
        int    ContentHeight() const { return HEADER_H + static_cast<int>(m_ids.size()) * ROW_H + 8; }
        double MaxScroll() const;

        PackSelectionScreen&     m_screen;
        bool                     m_selectedList;
        std::string              m_title;
        std::vector<std::string> m_ids;
        double                   m_scroll = 0.0;
    };

    class PackSelectionScreen : public Screen {
    public:
        PackSelectionScreen();
        ~PackSelectionScreen() override;

        void Init() override;
        void Tick() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        void OnClose() override;

        // ── PackSelectionModel.Entry, for the lists ────────────────────
        const Resources::Pack* PackFor(const std::string& id) const;
        bool IsSelected(const std::string& id) const;
        bool CanMoveUp(const std::string& id) const;
        bool CanMoveDown(const std::string& id) const;
        void Select(const std::string& id);     // asks first when incompatible
        void Unselect(const std::string& id);
        void MoveUp(const std::string& id);
        void MoveDown(const std::string& id);
        TextureHandle IconFor(const Resources::Pack& pack);

    private:
        static constexpr int LIST_WIDTH = 200;
        static constexpr int FOOTER_H   = 33;
        static constexpr int RELOAD_COOLDOWN = 20;

        int  HeaderHeight() const;
        void PopulateLists();
        void ReloadPacks();
        void Commit();
        void ToggleSelection(const std::string& id, bool select);
        void Move(const std::string& id, int direction);
        std::vector<std::string> Filtered(const std::vector<std::string>& ids) const;

        // The model's two lists: selected is TOP FIRST (the repository's
        // order reversed), unselected in the repository's (sorted) order.
        std::vector<std::string> m_selected;
        std::vector<std::string> m_unselected;
        std::string              m_filter;

        TransferableSelectionList* m_availableList = nullptr;
        TransferableSelectionList* m_selectedList  = nullptr;
        EditBox*                   m_search = nullptr;
        Button*                    m_doneButton = nullptr;

        std::unordered_map<std::string, TextureHandle> m_icons;   // by pack id; INVALID = default
        TextureHandle m_defaultIcon = INVALID_TEXTURE;
        bool          m_defaultIconTried = false;

        std::vector<std::string> m_knownIds;
        int  m_rescanTicks = 0;
        bool m_committed = false;
    };

} // namespace Render
