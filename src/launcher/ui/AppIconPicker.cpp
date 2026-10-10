// File: src/launcher/ui/AppIconPicker.cpp
//
// See AppIconPicker.hpp. Drawn in the design's terms (LauncherWidgets /
// LauncherTheme): a Settings row like Renderer's, and a popup styled like
// Widgets::Dropdown's menu.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "AppIconPicker.hpp"
#include "LauncherTheme.hpp"
#include "LauncherWidgets.hpp"

#include <glad/glad.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <string>

namespace Launcher {

    using namespace Palette;
    using namespace Widgets;
    namespace Icon = Render::BlockIcon;

    namespace {

        constexpr float kTile = 46.0f;      // a grid tile, design px
        constexpr float kTileGap = 6.0f;
        constexpr float kTileInset = 4.0f;  // thumbnail inset inside its tile
        constexpr std::chrono::microseconds kThumbBudget{4000};   // thumbnail drawing per frame
        constexpr double kRandomCycle = 0.9;   // seconds per block on the Random tile

        bool ContainsNoCase(const std::string& haystack, const char* needle) {
            if (!needle[0]) return true;
            auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
            std::string h(haystack), n(needle);
            std::transform(h.begin(), h.end(), h.begin(), lower);
            std::transform(n.begin(), n.end(), n.begin(), lower);
            return h.find(n) != std::string::npos;
        }

        ImTextureID Tex(GLuint id) { return static_cast<ImTextureID>(static_cast<uintptr_t>(id)); }

    } // namespace

    AppIconPicker::~AppIconPicker() {
        if (m_loader.joinable()) m_loader.join();
    }

    void AppIconPicker::SetAssetsDir(std::filesystem::path assetsDir) {
        m_assetsDir = std::move(assetsDir);
    }

    void AppIconPicker::StartLoading() {
        if (m_loadStarted || m_assetsDir.empty()) return;
        // The game may be installed while the launcher is open: look again
        // once a second until its assets are there.
        const double now = ImGui::GetTime();
        if (now - m_lastAssetsCheck < 1.0) return;
        m_lastAssetsCheck = now;
        std::error_code ec;
        if (!std::filesystem::is_directory(m_assetsDir / "items", ec)) return;
        m_loadStarted = true;
        m_loader = std::thread([this] {
            m_blocks = Icon::LoadAllFullBlocks(m_assetsDir);
            m_loaded.store(true, std::memory_order_release);
        });
    }

    const Icon::Model* AppIconPicker::Find(const std::string& id) const {
        const auto it = m_index.find(id);
        return it == m_index.end() ? nullptr : &m_blocks[it->second];
    }

    std::string AppIconPicker::LabelFor(const std::string& choice) const {
        if (choice == Icon::kRandom) return "Random";
        if (const Icon::Model* m = Find(choice)) return m->name;
        if (choice.empty() || choice == Icon::kDefaultBlock) return "TNT";
        return choice;
    }

    GLuint AppIconPicker::ThumbFor(const std::string& id, int pixels) {
        // Keyed by size too: the field and the grid draw the same block at
        // different sizes, and must not take turns redrawing it.
        Thumb& thumb = m_thumbs[id + "@" + std::to_string(pixels)];
        if (thumb.texture != 0) return thumb.texture;
        const Icon::Model* model = Find(id);
        if (!model) return 0;
        // At least one a frame, then only inside the frame's budget.
        const auto now = std::chrono::steady_clock::now();
        if (m_thumbsDrawn > 0 && now >= m_thumbDeadline) return 0;
        if (m_thumbsDrawn++ == 0) m_thumbDeadline = now + kThumbBudget;

        const Icon::Image image = Icon::Render(*model, pixels);
        GLint previous = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
        if (thumb.texture == 0) glGenTextures(1, &thumb.texture);
        glBindTexture(GL_TEXTURE_2D, thumb.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
        return thumb.texture;
    }

    void AppIconPicker::ReleaseGpu() {
        for (auto& [id, thumb] : m_thumbs) {
            if (thumb.texture != 0) glDeleteTextures(1, &thumb.texture);
        }
        m_thumbs.clear();
    }

    void AppIconPicker::DrawRow(std::string& choice, float width) {
        StartLoading();
        if (!m_ready && m_loaded.load(std::memory_order_acquire)) {
            if (m_loader.joinable()) m_loader.join();
            for (size_t i = 0; i < m_blocks.size(); ++i) m_index.emplace(m_blocks[i].id, i);
            m_ready = true;
        }
        m_thumbsDrawn = 0;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float rowH = 52.0f;
        // Pickable once the installed game's blocks are (being) read.
        const bool available = m_loadStarted && (!m_ready || !m_blocks.empty());
        const bool isDefault = choice.empty() || choice == Icon::kDefaultBlock;

        Txt(dl, g_fontSmall, p + ImVec2(0, 10), TextBody, "App icon");
        std::string sub;
        if (!available) sub = "INSTALL THE GAME TO PICK ANOTHER BLOCK";
        else if (isDefault) sub = "TNT - THE DEFAULT";
        else if (choice == Icon::kRandom) sub = "A NEW FULL BLOCK EVERY LAUNCH";
        else sub = "--icon " + choice;
        Txt(dl, g_fontMono10, p + ImVec2(0, 29), TextFaint, sub.c_str());

        // The field: the choice's thumbnail and name, a chevron; opens the grid.
        const ImVec2 size(190.0f, 34.0f);
        const ImVec2 pos(p.x + width - size.x, p.y + (rowH - size.y) * 0.5f);
        ImGui::PushID("##appIcon");
        const bool open = ImGui::IsPopupOpen("##grid");
        ImGui::SetCursorScreenPos(pos);
        if (ImGui::InvisibleButton("##field", size) && available) {
            m_search[0] = '\0';
            ImGui::OpenPopup("##grid");
        }
        const bool hovered = available && ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        dl->AddRectFilled(pos, pos + size, hovered || open ? BgHover : Rail, 9.0f);
        dl->AddRect(pos, pos + size, open ? Accent : hovered ? BorderHover : Border, 9.0f);
        {
            const float thumb = 24.0f;
            const ImVec2 t0(pos.x + 7.0f, pos.y + (size.y - thumb) * 0.5f);
            const float scale = ImGui::GetIO().DisplayFramebufferScale.y;
            const int px = static_cast<int>(std::lround(thumb * (scale > 0.0f ? scale : 1.0f)));
            std::string shown = isDefault ? std::string(Icon::kDefaultBlock) : choice;
            if (choice == Icon::kRandom && m_ready && !m_blocks.empty()) {
                shown = m_blocks[static_cast<size_t>(ImGui::GetTime() / kRandomCycle) % m_blocks.size()].id;
            }
            if (GLuint tex = available ? ThumbFor(shown, px) : 0) dl->AddImage(Tex(tex), t0, t0 + ImVec2(thumb, thumb));
            const std::string label = Ellipsize(g_fontSmallMed, LabelFor(choice), size.x - 70.0f);
            const ImVec2 ts = Measure(g_fontSmallMed, label.c_str());
            Txt(dl, g_fontSmallMed, ImVec2(t0.x + thumb + 8.0f, pos.y + (size.y - ts.y) * 0.5f),
                available ? TextPrimary : TextGhost, label.c_str());
            const ImVec2 c(pos.x + size.x - 15.0f, pos.y + size.y * 0.5f);
            const float dy = open ? -2.0f : 2.0f;
            dl->AddLine(ImVec2(c.x - 4.0f, c.y - dy), ImVec2(c.x, c.y + dy), TextMuted, 1.5f);
            dl->AddLine(ImVec2(c.x, c.y + dy), ImVec2(c.x + 4.0f, c.y - dy), TextMuted, 1.5f);
        }

        // The grid popup, under the field or over it when there is no room —
        // and when neither side holds it whole, on the roomier side, cut to
        // fit the window (the grid scrolls).
        constexpr float kPad = 8.0f;
        constexpr float kEdge = 4.0f;   // gap to the field and to the window edge
        const float menuW = 8.0f * (kTile + kTileGap) - kTileGap + kPad * 2.0f + 12.0f;   // 8 columns + scrollbar
        const float below = pos.y + size.y + kEdge;
        const float roomBelow = ImGui::GetIO().DisplaySize.y - kEdge - below;
        const float roomAbove = pos.y - kEdge - kEdge;
        const bool openBelow = roomBelow >= 330.0f || roomBelow >= roomAbove;
        const float menuH = std::min(330.0f, openBelow ? roomBelow : roomAbove);
        ImGui::SetNextWindowPos(ImVec2(pos.x + size.x - menuW, openBelow ? below : pos.y - kEdge - menuH));
        ImGui::SetNextWindowSize(ImVec2(menuW, menuH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPad, kPad));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(Rail));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(Border));
        if (ImGui::BeginPopup("##grid", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
            DrawGrid(choice);
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(4);
        ImGui::PopID();

        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
        dl->AddLine(ImVec2(p.x, p.y + rowH), ImVec2(p.x + width, p.y + rowH), BorderSoft);
        ImGui::Dummy(ImVec2(0, 0));
    }

    void AppIconPicker::DrawTile(ImDrawList* dl, const char* key, const std::string& id, const std::string& label,
                                 ImVec2 pos, float size, bool selected, bool& clicked) {
        ImGui::SetCursorScreenPos(pos);
        clicked = ImGui::InvisibleButton(key, ImVec2(size, size));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            Tooltip(label.c_str());
        }
        if (selected) dl->AddRectFilled(pos, pos + ImVec2(size, size), BgActive, 7.0f);
        else if (hovered) dl->AddRectFilled(pos, pos + ImVec2(size, size), BgHover, 7.0f);
        if (ImGui::IsRectVisible(pos, pos + ImVec2(size, size))) {
            const float inner = size - kTileInset * 2.0f;
            const float scale = ImGui::GetIO().DisplayFramebufferScale.y;
            const int px = static_cast<int>(std::lround(inner * (scale > 0.0f ? scale : 1.0f)));
            if (GLuint tex = ThumbFor(id, px)) {
                const ImVec2 t0 = pos + ImVec2(kTileInset, kTileInset);
                dl->AddImage(Tex(tex), t0, t0 + ImVec2(inner, inner));
            }
        }
        if (selected) dl->AddRect(pos, pos + ImVec2(size, size), Accent, 7.0f, 0, 1.5f);
    }

    void AppIconPicker::DrawGrid(std::string& choice) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = ImGui::GetContentRegionAvail().x;

        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        Input("##appIconSearch", "search blocks", m_search, sizeof(m_search), w, 32.0f, g_fontInput13,
              0, nullptr, 8.0f, 11.0f);
        ImGui::Dummy(ImVec2(0, 8));

        if (!m_ready) {
            Txt(dl, g_fontMono10, ImGui::GetCursorScreenPos(), TextFaint, "LOADING BLOCKS...");
            return;
        }

        // TNT (the default) and Random lead; then every other full block.
        struct Entry { std::string value, thumb, label; };
        std::vector<Entry> entries;
        if (ContainsNoCase("TNT default", m_search)) {
            entries.push_back({Icon::kDefaultBlock, Icon::kDefaultBlock, "TNT (default)"});
        }
        if (!m_blocks.empty() && ContainsNoCase("Random", m_search)) {
            const size_t cycle = static_cast<size_t>(ImGui::GetTime() / kRandomCycle) % m_blocks.size();
            entries.push_back({Icon::kRandom, m_blocks[cycle].id, "Random - a new block every launch"});
        }
        for (const Icon::Model& m : m_blocks) {
            if (m.id == Icon::kDefaultBlock) continue;
            if (ContainsNoCase(m.name, m_search) || ContainsNoCase(m.id, m_search)) {
                entries.push_back({m.id, m.id, m.name});
            }
        }

        const ImVec2 gridSize(w, ImGui::GetContentRegionAvail().y);
        ImGui::BeginChild("##tiles", gridSize, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        {
            ImDrawList* gdl = ImGui::GetWindowDrawList();
            const float gw = ImGui::GetContentRegionAvail().x;
            const int columns = std::max(1, static_cast<int>((gw + kTileGap) / (kTile + kTileGap)));
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const std::string current = choice.empty() ? std::string(Icon::kDefaultBlock) : choice;
            for (size_t i = 0; i < entries.size(); ++i) {
                const Entry& e = entries[i];
                const int col = static_cast<int>(i) % columns;
                const int row = static_cast<int>(i) / columns;
                const ImVec2 pos(origin.x + col * (kTile + kTileGap), origin.y + row * (kTile + kTileGap));
                ImGui::PushID(static_cast<int>(i));
                bool clicked = false;
                DrawTile(gdl, "##tile", e.thumb, e.label, pos, kTile, e.value == current, clicked);
                ImGui::PopID();
                if (e.value == Icon::kRandom) {
                    // A "?" badge on the cycling preview.
                    const ImVec2 c = pos + ImVec2(kTile - 9.0f, kTile - 9.0f);
                    gdl->AddCircleFilled(c, 7.0f, Accent);
                    const ImVec2 ts = Measure(g_fontMono10, "?");
                    Txt(gdl, g_fontMono10, c - ts * 0.5f, OnAccent, "?");
                }
                if (clicked) {
                    choice = e.value;
                    ImGui::CloseCurrentPopup();
                }
            }
            const int rows = (static_cast<int>(entries.size()) + columns - 1) / columns;
            ImGui::SetCursorScreenPos(origin);
            ImGui::Dummy(ImVec2(gw, rows * (kTile + kTileGap)));
            if (entries.empty()) Txt(gdl, g_fontMono10, origin, TextFaint, "NO FULL BLOCK MATCHES");
        }
        ImGui::EndChild();
    }

} // namespace Launcher
