// File: src/client/renderer/gui/screens/PackSelectionScreen.cpp
#include "PackSelectionScreen.hpp"

#include "WorldSelectScreens.hpp"
#include "OptionsScreens.hpp"          // WorldSettingsContext
#include "server/world/storage/anvil/WorldFolder.hpp"
#include "server/world/storage/anvil/WorldSidecar.hpp"
#include "../GuiGraphics.hpp"
#include "../FontRenderer.hpp"
#include "../../backend/RenderBackend.hpp"
#include "platform/GameDirectory.hpp"
#include "common/core/Log.hpp"
#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    namespace {
        std::string Lower(std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        std::string Ellipsize(GuiGraphics& g, const std::string& text, int maxWidth) {
            if (g.GetStringWidth(text) <= maxWidth) return text;
            std::string s = text;
            while (!s.empty() && g.GetStringWidth(s + "...") > maxWidth) s.pop_back();
            return s + "...";
        }

        // MultiLineTextWidget with setMaxRows(2): word-wrap to the width,
        // the last row ellipsized when there is more.
        std::vector<std::string> WrapTwoLines(GuiGraphics& g, const std::string& text, int maxWidth) {
            std::vector<std::string> lines;
            std::string word, line;
            auto flush = [&](bool last) {
                if (lines.size() == 2) return;
                lines.push_back(last ? Ellipsize(g, line, maxWidth) : line);
                line.clear();
            };
            size_t i = 0;
            while (i <= text.size() && lines.size() < 2) {
                const char c = i < text.size() ? text[i] : ' ';
                if (c == ' ' || c == '\n') {
                    if (!word.empty()) {
                        const std::string candidate = line.empty() ? word : line + " " + word;
                        if (g.GetStringWidth(candidate) <= maxWidth || line.empty()) {
                            line = candidate;
                        } else {
                            flush(false);
                            line = word;
                        }
                        word.clear();
                    }
                    if (c == '\n' && !line.empty()) flush(false);
                } else {
                    word += c;
                }
                ++i;
            }
            if (lines.size() < 2 && !line.empty()) {
                // Anything left over past the second line is ellipsized.
                const bool more = i < text.size();
                lines.push_back(more || g.GetStringWidth(line) > maxWidth ? Ellipsize(g, line + (more ? " ..." : ""), maxWidth) : line);
            }
            return lines;
        }
    }

    // ═══════════════════════ TransferableSelectionList ═════════════════════

    TransferableSelectionList::TransferableSelectionList(PackSelectionScreen& screen, bool selectedList,
                                                         int x, int y, int width, int height, std::string title)
        : AbstractWidget(x, y, width, height, ""),
          m_screen(screen), m_selectedList(selectedList), m_title(std::move(title)) {}

    void TransferableSelectionList::SetIds(std::vector<std::string> ids) {
        m_ids = std::move(ids);
        m_scroll = std::clamp(m_scroll, 0.0, MaxScroll());
    }

    void TransferableSelectionList::SetBounds(int x, int y, int width, int height) {
        m_x = x; m_y = y; m_width = width; m_height = height;
    }

    double TransferableSelectionList::MaxScroll() const {
        const double max = ContentHeight() - m_height;
        return max > 0.0 ? max : 0.0;
    }

    int TransferableSelectionList::RowTop(int index) const {
        return m_y + 4 + HEADER_H + index * ROW_H - static_cast<int>(m_scroll);
    }

    int TransferableSelectionList::RowAt(double mouseX, double mouseY) const {
        if (!ContainsPoint(mouseX, mouseY)) return -1;
        if (mouseX < m_x + 2 || mouseX >= m_x + 2 + RowWidth()) return -1;
        const double rel = mouseY - (m_y + 4 + HEADER_H) + m_scroll;
        if (rel < 0) return -1;
        const int idx = static_cast<int>(rel / ROW_H);
        return idx < static_cast<int>(m_ids.size()) ? idx : -1;
    }

    bool TransferableSelectionList::OnScroll(double deltaY) {
        if (MaxScroll() <= 0.0) return false;
        m_scroll = std::clamp(m_scroll - deltaY * (ROW_H / 2.0), 0.0, MaxScroll());
        return true;
    }

    // PackEntry.mouseClicked: the icon square is the control surface.
    void TransferableSelectionList::OnClick(double mouseX, double mouseY) {
        const int row = RowAt(mouseX, mouseY);
        if (row < 0) return;
        const std::string id = m_ids[row];
        const int relX = static_cast<int>(mouseX) - ContentX();
        const int relY = static_cast<int>(mouseY) - (RowTop(row) + PADDING);
        if (relX < 0 || relY < 0 || relX >= ICON || relY >= ICON) return;
        if (!m_selectedList) {
            m_screen.Select(id);                    // mouseOverIcon → select
            return;
        }
        if (relX < ICON / 2) {                      // left half → unselect
            if (m_screen.IsSelected(id)) m_screen.Unselect(id);
        } else if (relY < ICON / 2) {               // top-right quarter → up
            if (m_screen.CanMoveUp(id)) m_screen.MoveUp(id);
        } else {                                    // bottom-right quarter → down
            if (m_screen.CanMoveDown(id)) m_screen.MoveDown(id);
        }
    }

    void TransferableSelectionList::RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float) {
        m_scroll = std::clamp(m_scroll, 0.0, MaxScroll());
        g.EnableScissor(m_x, m_y, m_x + m_width, m_y + m_height);

        // HeaderEntry: the title, centred, bold + underlined in MC.
        {
            const int y = m_y + 4 - static_cast<int>(m_scroll);
            const int textW = g.GetStringWidth(m_title);
            const int textX = m_x + m_width / 2 - textW / 2;
            const int textY = y + HEADER_H / 2 - FontRenderer::LINE_HEIGHT / 2;
            g.DrawString(m_title, textX, textY, 0xFFFFFFFF);
            g.DrawString(m_title, textX + 1, textY, 0xFFFFFFFF);            // bold: MC's one-pixel offset
            g.Fill(textX, textY + FontRenderer::LINE_HEIGHT, textX + textW + 1, textY + FontRenderer::LINE_HEIGHT + 1, 0xFFFFFFFF);
        }

        const int hoveredRow = RowAt(mouseX, mouseY);
        const int textW = TEXT_W - (Scrollable() ? SCROLLBAR : 0);
        for (int i = 0; i < static_cast<int>(m_ids.size()); ++i) {
            const int top = RowTop(i);
            if (top + ROW_H < m_y || top > m_y + m_height) continue;
            const Resources::Pack* pack = m_screen.PackFor(m_ids[i]);
            if (!pack) continue;
            const int cx = ContentX();
            const int cy = top + PADDING;
            const int contentRight = m_x + 2 + RowWidth() - PADDING - (Scrollable() ? SCROLLBAR : 0);

            // Incompatible: the red plate behind the whole row.
            const bool compatible = Resources::IsCompatible(pack->compatibility);
            if (!compatible) g.Fill(cx - 1, cy - 1, contentRight + 1, cy + ICON + 1, 0xFF770000);

            // Icon.
            const TextureHandle icon = m_screen.IconFor(*pack);
            if (icon != INVALID_TEXTURE) g.Blit(icon, cx, cy, cx + ICON, cy + ICON, 0.0f, 0.0f, 1.0f, 1.0f);
            else g.Fill(cx, cy, cx + ICON, cy + ICON, 0xFF303030);

            std::string name = pack->title;
            std::string description = pack->ExtendedDescription();

            // Hover overlay on the icon, with the controls that apply
            // (PackEntry.renderContent). MC shows it for every pack that is
            // not fixed-and-required; vanilla is required but movable.
            const bool showOverlay = !(pack->fixedPosition && pack->required);
            const bool hovered = hoveredRow == i;
            if (showOverlay && hovered) {
                g.Fill(cx, cy, cx + ICON, cy + ICON, 0xA0909090);
                const int relX = mouseX - cx, relY = mouseY - cy;
                const bool overIcon = relX >= 0 && relX < ICON && relY >= 0 && relY < ICON;
                if (!compatible) {
                    name = "Incompatible";
                    description = Resources::CompatibilityDescription(pack->compatibility);
                }
                if (!m_selectedList) {
                    g.BlitSprite(overIcon ? "transferable_list/select_highlighted" : "transferable_list/select", cx, cy, ICON, ICON);
                } else {
                    if (!pack->required) {   // canUnselect
                        const bool leftHalf = overIcon && relX < ICON / 2;
                        g.BlitSprite(leftHalf ? "transferable_list/unselect_highlighted" : "transferable_list/unselect", cx, cy, ICON, ICON);
                    }
                    if (m_screen.CanMoveUp(pack->id)) {
                        const bool tr = overIcon && relX >= ICON / 2 && relY < ICON / 2;
                        g.BlitSprite(tr ? "transferable_list/move_up_highlighted" : "transferable_list/move_up", cx, cy, ICON, ICON);
                    }
                    if (m_screen.CanMoveDown(pack->id)) {
                        const bool br = overIcon && relX >= ICON / 2 && relY >= ICON / 2;
                        g.BlitSprite(br ? "transferable_list/move_down_highlighted" : "transferable_list/move_down", cx, cy, ICON, ICON);
                    }
                }
            }

            // Name and the two grey description rows.
            const int textX = cx + ICON + 2;
            g.DrawString(Ellipsize(g, name, textW), textX, cy + 1, 0xFFFFFFFF);
            int ly = cy + 12;
            for (const std::string& line : WrapTwoLines(g, description, textW)) {
                g.DrawString(line, textX, ly, 0xFF808080);
                ly += FontRenderer::LINE_HEIGHT;
            }
        }
        g.DisableScissor();

        if (Scrollable()) {
            const int sx = m_x + m_width - SCROLLBAR;
            g.BlitSprite("widget/scroller_background", sx, m_y, SCROLLBAR, m_height);
            const double thumbH = std::max(32.0, static_cast<double>(m_height) * m_height / ContentHeight());
            const double frac = MaxScroll() > 0.0 ? m_scroll / MaxScroll() : 0.0;
            const int thumbY = m_y + static_cast<int>(frac * (m_height - thumbH));
            g.BlitSprite("widget/scroller", sx, thumbY, SCROLLBAR, static_cast<int>(thumbH));
        }
    }

    // ═══════════════════════════ PackSelectionScreen ═══════════════════════

    PackSelectionScreen::PackSelectionScreen() : Screen("Select Resource Packs") {
        // PackSelectionModel: selected = repository order reversed (top =
        // highest priority); unselected = everything else, sorted.
        Resources::PackRepository& repo = Resources::Repository();
        m_selected = repo.SelectedIds();
        std::reverse(m_selected.begin(), m_selected.end());
        for (const Resources::Pack& p : repo.Available()) {
            if (std::find(m_selected.begin(), m_selected.end(), p.id) == m_selected.end()) m_unselected.push_back(p.id);
        }
        for (const Resources::Pack& p : repo.Available()) m_knownIds.push_back(p.id);
    }

    PackSelectionScreen::~PackSelectionScreen() {
        if (!g_renderBackend) return;
        for (auto& [id, tex] : m_icons) if (tex != INVALID_TEXTURE) g_renderBackend->DestroyTexture(tex);
        if (m_defaultIcon != INVALID_TEXTURE) g_renderBackend->DestroyTexture(m_defaultIcon);
    }

    int PackSelectionScreen::HeaderHeight() const {
        // 4 + title(9) + 4 + hint(9) + 4 + search(15) + 4.
        return 4 + FontRenderer::LINE_HEIGHT + 4 + FontRenderer::LINE_HEIGHT + 4 + 15 + 4;
    }

    void PackSelectionScreen::Init() {
        const int headerH = HeaderHeight();
        const int contentH = m_height - headerH - FOOTER_H;

        m_search = AddWidget(new EditBox(m_width / 2 - 100, headerH - 15 - 4, 200, 15, ""));
        m_search->SetHint("Search...");
        m_search->SetText(m_filter);
        m_search->SetResponder([this](const std::string& v) { m_filter = v; PopulateLists(); });

        m_availableList = AddWidget(new TransferableSelectionList(*this, false,
            m_width / 2 - 15 - LIST_WIDTH, headerH, LIST_WIDTH, contentH, "Available"));
        m_selectedList = AddWidget(new TransferableSelectionList(*this, true,
            m_width / 2 + 15, headerH, LIST_WIDTH, contentH, "Selected"));

        // Footer: LinearLayout.horizontal().spacing(8), two 150-wide buttons.
        const int footerY = m_height - FOOTER_H / 2 - 10;
        const int left = m_width / 2 - (150 * 2 + 8) / 2;
        auto* folder = AddWidget(new Button(left, footerY, 150, 20, "Open Pack Folder", [] {
            const std::string dir = Resources::PacksDirectory();
            if (!Platform::GameDirectory::OpenInFileBrowser(dir)) {
                Log::Warning("[ResourcePacks] Could not open %s in the file browser", dir.c_str());
            }
        }));
        folder->SetTooltip({"(Place pack files here)"});
        m_doneButton = AddWidget(new Button(left + 150 + 8, footerY, 150, 20, "Done", [this] { OnClose(); }));

        PopulateLists();
    }

    std::vector<std::string> PackSelectionScreen::Filtered(const std::vector<std::string>& ids) const {
        if (m_filter.empty()) return ids;
        const std::string needle = Lower(m_filter);
        std::vector<std::string> out;
        for (const std::string& id : ids) {
            const Resources::Pack* p = PackFor(id);
            if (!p) continue;
            if (Lower(p->id).find(needle) != std::string::npos ||
                Lower(p->title).find(needle) != std::string::npos ||
                Lower(p->description).find(needle) != std::string::npos) {
                out.push_back(id);
            }
        }
        return out;
    }

    void PackSelectionScreen::PopulateLists() {
        if (m_selectedList)  m_selectedList->SetIds(Filtered(m_selected));
        if (m_availableList) m_availableList->SetIds(Filtered(m_unselected));
        if (m_doneButton)    m_doneButton->active = !m_selected.empty();
    }

    // PackSelectionModel.findNewPacks + the screen's reload().
    void PackSelectionScreen::ReloadPacks() {
        Resources::PackRepository& repo = Resources::Repository();
        repo.Reload();
        std::vector<std::string> ids;
        for (const Resources::Pack& p : repo.Available()) ids.push_back(p.id);
        m_selected.erase(std::remove_if(m_selected.begin(), m_selected.end(),
            [&](const std::string& id) { return !repo.Get(id); }), m_selected.end());
        m_unselected.clear();
        for (const std::string& id : ids) {
            if (std::find(m_selected.begin(), m_selected.end(), id) == m_selected.end()) m_unselected.push_back(id);
        }
        // A pack that changed on disk gets its icon read again.
        if (g_renderBackend) for (auto& [id, tex] : m_icons) if (tex != INVALID_TEXTURE) g_renderBackend->DestroyTexture(tex);
        m_icons.clear();
        m_knownIds = std::move(ids);
        PopulateLists();
    }

    void PackSelectionScreen::Tick() {
        if (++m_rescanTicks < RELOAD_COOLDOWN) return;
        m_rescanTicks = 0;
        // MC watches the folder and reloads 20 ticks after a change; the
        // same cadence by comparing the folder's pack ids once a second.
        std::vector<std::string> ids;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(Resources::PacksDirectory(), ec)) {
            const std::string name = e.path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            if (e.is_directory(ec) || (e.is_regular_file(ec) && e.path().extension() == ".zip")) ids.push_back("file/" + name);
        }
        ids.push_back("vanilla");
        std::sort(ids.begin(), ids.end());
        std::vector<std::string> known = m_knownIds;
        std::sort(known.begin(), known.end());
        // Non-pack entries never make it into known, so compare presence of
        // the known set rather than equality.
        bool changed = false;
        for (const std::string& id : known) if (std::find(ids.begin(), ids.end(), id) == ids.end()) { changed = true; break; }
        if (!changed) {
            for (const std::string& id : ids) {
                if (std::find(known.begin(), known.end(), id) == known.end()) {
                    // New entry: only a change if it is really a pack.
                    Resources::Repository().Reload();
                    if (Resources::Repository().Get(id)) { changed = true; break; }
                }
            }
        }
        if (changed) ReloadPacks();
    }

    void PackSelectionScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        Screen::Render(g, mouseX, mouseY, partialTick);
        g.DrawCenteredString(m_title, m_width / 2, 4, 0xFFFFFFFF);
        // pack.dropInfo in MC; this engine has no window drop, so say where
        // packs go instead.
        g.DrawCenteredString("Put pack folders or .zip files in the pack folder", m_width / 2,
                             4 + FontRenderer::LINE_HEIGHT + 4, 0xFF808080);
        RenderMenuSeparators(g, m_width, HeaderHeight() - 2, m_height - FOOTER_H);
        if (Resources::AnyEnabledPackHasStartupOnlyContent()) {
            g.DrawCenteredString("Block models, item models and language files from packs apply after a restart",
                                 m_width / 2, m_height - FOOTER_H - FontRenderer::LINE_HEIGHT - 2, 0xFFA0A0A0);
        }
    }

    // ── model ────────────────────────────────────────────────────────────

    const Resources::Pack* PackSelectionScreen::PackFor(const std::string& id) const {
        return Resources::Repository().Get(id);
    }

    bool PackSelectionScreen::IsSelected(const std::string& id) const {
        return std::find(m_selected.begin(), m_selected.end(), id) != m_selected.end();
    }

    bool PackSelectionScreen::CanMoveUp(const std::string& id) const {
        const auto it = std::find(m_selected.begin(), m_selected.end(), id);
        if (it == m_selected.end() || it == m_selected.begin()) return false;
        const Resources::Pack* above = PackFor(*(it - 1));
        return above && !above->fixedPosition;
    }

    bool PackSelectionScreen::CanMoveDown(const std::string& id) const {
        const auto it = std::find(m_selected.begin(), m_selected.end(), id);
        if (it == m_selected.end() || it + 1 == m_selected.end()) return false;
        const Resources::Pack* below = PackFor(*(it + 1));
        return below && !below->fixedPosition;
    }

    // EntryBase.toggleSelection: out of one list, into the other at the
    // pack's default position (reversed, since the lists are top-first).
    void PackSelectionScreen::ToggleSelection(const std::string& id, bool select) {
        const Resources::Pack* pack = PackFor(id);
        if (!pack) return;
        std::vector<std::string>& from = select ? m_unselected : m_selected;
        std::vector<std::string>& to   = select ? m_selected : m_unselected;
        from.erase(std::remove(from.begin(), from.end(), id), from.end());
        const int at = Resources::PackRepository::InsertPosition(to, Resources::Repository(), pack->defaultPosition, true);
        to.insert(to.begin() + at, id);
        PopulateLists();
    }

    // PackEntry.handlePackSelection. MC puts a confirmation screen in front
    // of an incompatible pack; here the red row and its hover text ("Made
    // for an older version of Minecraft") are the warning, and the click
    // selects at once — the pack still records as accepted-incompatible in
    // the saved lists, exactly as after MC's "Yes".
    void PackSelectionScreen::Select(const std::string& id) {
        const Resources::Pack* pack = PackFor(id);
        if (!pack || IsSelected(id)) return;
        ToggleSelection(id, true);
    }

    void PackSelectionScreen::Unselect(const std::string& id) {
        const Resources::Pack* pack = PackFor(id);
        if (!pack || !IsSelected(id) || pack->required) return;
        ToggleSelection(id, false);
    }

    void PackSelectionScreen::Move(const std::string& id, int direction) {
        const auto it = std::find(m_selected.begin(), m_selected.end(), id);
        if (it == m_selected.end()) return;
        const int pos = static_cast<int>(it - m_selected.begin());
        const int to = pos + direction;
        if (to < 0 || to >= static_cast<int>(m_selected.size())) return;
        std::swap(m_selected[pos], m_selected[to]);
        PopulateLists();
    }

    void PackSelectionScreen::MoveUp(const std::string& id)   { if (CanMoveUp(id))   Move(id, -1); }
    void PackSelectionScreen::MoveDown(const std::string& id) { if (CanMoveDown(id)) Move(id, +1); }

    TextureHandle PackSelectionScreen::IconFor(const Resources::Pack& pack) {
        if (!g_renderBackend) return INVALID_TEXTURE;
        auto load = [](const std::string& path) -> TextureHandle {
            if (path.empty()) return INVALID_TEXTURE;
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!px) return INVALID_TEXTURE;
            TextureHandle t = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, px);
            stbi_image_free(px);
            if (t != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(t, TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap(t, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            return t;
        };
        auto it = m_icons.find(pack.id);
        if (it == m_icons.end()) it = m_icons.emplace(pack.id, load(pack.iconPath)).first;
        if (it->second != INVALID_TEXTURE) return it->second;
        if (!m_defaultIconTried) {
            m_defaultIconTried = true;
            m_defaultIcon = load(PlatformMain::GetAssetPath("assets/textures/misc/unknown_pack.png"));
        }
        return m_defaultIcon;
    }

    // PackSelectionModel.commit + Options.updateResourcePacks.
    void PackSelectionScreen::Commit() {
        if (m_committed) return;
        m_committed = true;
        std::vector<std::string> repoOrder = m_selected;
        std::reverse(repoOrder.begin(), repoOrder.end());
        Resources::Repository().SetSelected(repoOrder);

        const Resources::OptionLists lists = Resources::CurrentOptionLists();

        // Chosen while in one of our own worlds: the selection belongs to
        // THAT world (data/obeycraft.json) and comes back with it; the
        // global list in options.txt is what every other world and the
        // title screen use. Elsewhere it is the global list that changes.
        bool savedToWorld = false;
        if (WorldSettingsContext::Active() && WorldSettingsContext::CanPersist()) {
            std::string reason;
            if (auto root = Game::Anvil::RootForWorldName(WorldSettingsContext::WorldName(), reason)) {
                if (Game::Anvil::LooksLikeWorld(*root)) {
                    const std::string dir = root->Root().string();
                    Game::Anvil::WorldSidecar sidecar = Game::Anvil::ReadWorldSidecar(dir);
                    sidecar.hasResourcePacks          = true;
                    sidecar.resourcePacks             = lists.selected;
                    sidecar.incompatibleResourcePacks = lists.incompatible;
                    if (Game::Anvil::WriteWorldSidecar(dir, sidecar)) savedToWorld = true;
                    else Log::Warning("[ResourcePacks] could not save the world's pack selection");
                }
            }
        }
        if (!savedToWorld) {
            Platform::g_gameSettings.SetResourcePacks(Resources::SerializePackList(lists.selected));
            Platform::g_gameSettings.SetIncompatibleResourcePacks(Resources::SerializePackList(lists.incompatible));
            Platform::g_gameSettings.Save();
        }
        // Reload whenever the LIVE selection changed (Options.updateResourcePacks
        // compares the lists; the host's ReloadResources is a no-op when the
        // effective layers are the same).
        GetScreenManager().MarkSettingApplied(ScreenManager::APPLY_RESOURCE_PACKS);
    }

    void PackSelectionScreen::OnClose() {
        Commit();
        Screen::OnClose();
    }

} // namespace Render
