// File: src/launcher/appearance/AppearanceView.cpp
#define IMGUI_DEFINE_MATH_OPERATORS
#include "AppearanceView.hpp"
#include "launcher/LauncherConfig.hpp"
#include "launcher/ui/LauncherTheme.hpp"
#include "launcher/ui/LauncherWidgets.hpp"
#include "common/core/Log.hpp"
#include "common/entity/PlayerCapes.hpp"
#include "common/entity/PlayerColors.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <utility>

namespace Launcher::Appearance {

    using namespace Palette;
    using namespace Widgets;

    namespace {

        // The view's frame (design px): LauncherUI's content padding, the
        // header column of the Settings view, the preview beside a
        // scrolling column.
        constexpr float kRailW = 216.0f;
        constexpr float kPadX = 28.0f;
        constexpr float kPadY = 26.0f;
        constexpr float kHeaderH = 43.1f;
        constexpr float kPreviewW = 180.0f;
        constexpr float kColumnGap = 20.0f;

        float ContentX() { return kRailW + kPadX; }
        float ContentRight() { return static_cast<float>(WindowWidth) - kPadX; }

        ImTextureID Tex(GLuint id) { return static_cast<ImTextureID>(static_cast<uintptr_t>(id)); }

        ImU32 ColorOf(Game::PlayerColorId id) {
            const auto& e = Game::LookupPlayerColor(id);
            return IM_COL32(e.r, e.g, e.b, 255);
        }

        std::string Lower(const std::string& s) {
            std::string out = s;
            for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        // A selectable tile's frame: rail fill, hover, the accent ring when
        // selected.
        void TileFrame(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, bool selected, bool hovered) {
            dl->AddRectFilled(p0, p1, hovered && !selected ? BgHover : Rail, 10.0f);
            if (selected) dl->AddRect(p0, p1, Accent, 10.0f, 0, 2.0f);
            else dl->AddRect(p0, p1, hovered ? BorderHover : Border, 10.0f);
        }

        // A slow three-dot pulse for a tile still loading.
        void LoadingDots(ImDrawList* dl, const ImVec2& center) {
            const float t = static_cast<float>(ImGui::GetTime());
            for (int i = 0; i < 3; ++i) {
                const float a = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(t * 5.0f - i * 0.8f));
                dl->AddCircleFilled(center + ImVec2((i - 1) * 7.0f, 0.0f), 2.2f,
                                    IM_COL32(0x8b, 0x90, 0xa0, static_cast<int>(a * 255)));
            }
        }

        int UsernameFilter(ImGuiInputTextCallbackData* data) {
            const ImWchar c = data->EventChar;
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '_';
            return ok ? 0 : 1;
        }

    } // namespace

    void AppearanceView::SetPaths(const Paths& paths) {
        m_paths = paths;
        m_capes.SetCacheDir(paths.CapesDir());
        m_loaded = false;
        m_customsScanned = false;
    }

    // ── Loading ──

    void AppearanceView::EnsureLoaded(const Settings& settings) {
        if (m_loaded || !m_paths.Valid()) return;
        m_loaded = true;
        const auto loadDefault = [&](Game::SkinModel model, Game::SkinImage& img, GlImage& tex) {
            const std::string path = m_paths.DefaultSkin(model);
            if (LoadPngFile(path, img) && NormalizeSkin(img)) {
                tex.Upload(img);
            } else {
                Log::Warning("[Appearance] default skin missing: %s", path.c_str());
                img = Game::SkinImage{};
            }
        };
        loadDefault(Game::SkinModel::Classic, m_steve, m_steveTex);
        loadDefault(Game::SkinModel::Slim, m_alex, m_alexTex);
        if (!settings.username.empty()) LoadProfileCache(settings.username);
    }

    void AppearanceView::LoadProfileCache(const std::string& username) {
        m_profileFor = Lower(username);
        m_profileHasSkin = LoadPngFile(m_paths.ProfileSkin(username), m_profileSkin) && NormalizeSkin(m_profileSkin);
        if (m_profileHasSkin) m_profileTex.Upload(m_profileSkin);
        m_profileHasCape = LoadPngFile(m_paths.ProfileCape(username), m_profileCape) && NormalizeCape(m_profileCape);
        if (m_profileHasCape) m_profileCapeTex.Upload(m_profileCape);
    }

    void AppearanceView::ScanCustomSkins() {
        m_customsScanned = true;
        for (CustomSkin& c : m_customs) c.texture.Release();
        m_customs.clear();
        std::error_code ec;
        const std::filesystem::path dir(m_paths.SkinsDir());
        if (!std::filesystem::is_directory(dir, ec)) return;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const std::filesystem::path p = entry.path();
            std::string ext = p.extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".png") continue;
            CustomSkin skin;
            skin.file = p.filename().string();
            if (!LoadPngFile(p.string(), skin.image) || !NormalizeSkin(skin.image)) continue;
            skin.texture.Upload(skin.image);
            m_customs.push_back(std::move(skin));
        }
        std::sort(m_customs.begin(), m_customs.end(),
                  [](const CustomSkin& a, const CustomSkin& b) { return Lower(a.file) < Lower(b.file); });
    }

    AppearanceView::CustomSkin* AppearanceView::FindCustom(const std::string& file) {
        for (CustomSkin& c : m_customs) {
            if (c.file == file) return &c;
        }
        return nullptr;
    }

    void AppearanceView::PollAsync(Settings& settings, bool& dirty) {
        m_capes.Poll();
        ProfileResult result;
        if (!m_fetcher.Poll(result)) return;
        if (!result.ok) {
            m_fetchStatus = result.error;
            m_fetchFailed = true;
            return;
        }
        m_fetchFailed = false;
        // Cache both textures on disk (the launch hand-off reads them there),
        // then show them.
        std::error_code ec;
        if (result.hasSkin) {
            SavePngFile(m_paths.ProfileSkin(result.name), result.skin);
        } else {
            std::filesystem::remove(m_paths.ProfileSkin(result.name), ec);
        }
        if (result.hasCape) {
            SavePngFile(m_paths.ProfileCape(result.name), result.cape);
        } else {
            std::filesystem::remove(m_paths.ProfileCape(result.name), ec);
        }
        LoadProfileCache(result.name);

        settings.username = result.name;
        settings.usernameModel = result.model;
        settings.usernameHasCape = result.hasCape;
        settings.source = SkinSource::Username;
        settings.mode = Game::AppearanceMode::Skin;
        // A profile wears its own cape unless another was picked.
        if (result.hasCape && settings.cape.empty()) settings.cape = kProfileCape;
        if (!result.hasCape && settings.cape == kProfileCape) settings.cape.clear();
        std::snprintf(m_usernameBuf, sizeof(m_usernameBuf), "%s", result.name.c_str());
        dirty = true;

        m_fetchStatus = result.name + (result.model == Game::SkinModel::Slim ? "  -  slim arms" : "  -  classic arms");
        if (!result.hasSkin) m_fetchStatus += "  -  Minecraft's default skin";
        if (result.hasCape) {
            const Game::CapeInfo* cape = result.capeSlug.empty() ? nullptr : Game::FindCape(result.capeSlug);
            m_fetchStatus += cape ? "  -  " + std::string(cape->name) + " cape" : std::string("  -  a cape");
        }
    }

    // ── Resolution ──

    AppearanceView::SkinRef AppearanceView::CurrentSkin(const Settings& settings) {
        SkinRef ref;
        ref.model = settings.Model();
        const auto byDefault = [&](Game::SkinModel model) {
            if (model == Game::SkinModel::Slim) {
                ref.image = m_alex.Valid() ? &m_alex : nullptr;
                ref.texture = m_alexTex.Id();
            } else {
                ref.image = m_steve.Valid() ? &m_steve : nullptr;
                ref.texture = m_steveTex.Id();
            }
        };
        switch (settings.source) {
            case SkinSource::Steve: byDefault(Game::SkinModel::Classic); break;
            case SkinSource::Alex:  byDefault(Game::SkinModel::Slim); break;
            case SkinSource::Username:
                if (m_profileHasSkin && m_profileFor == Lower(settings.username)) {
                    ref.image = &m_profileSkin;
                    ref.texture = m_profileTex.Id();
                } else {
                    byDefault(ref.model);
                }
                break;
            case SkinSource::Custom:
                if (CustomSkin* c = FindCustom(settings.customSkin)) {
                    ref.image = &c->image;
                    ref.texture = c->texture.Id();
                } else {
                    byDefault(ref.model);
                }
                break;
        }
        return ref;
    }

    GLuint AppearanceView::CurrentCape(const Settings& settings) {
        if (settings.cape.empty()) return 0;
        if (settings.cape == kProfileCape) {
            const bool ours = settings.source == SkinSource::Username && m_profileHasCape &&
                              m_profileFor == Lower(settings.username);
            return ours ? m_profileCapeTex.Id() : 0;
        }
        m_capes.Request(settings.cape);
        return m_capes.Texture(settings.cape);
    }

    std::string AppearanceView::CapeDisplayName(const Settings& settings) const {
        if (settings.cape.empty()) return "No cape";
        if (settings.cape == kProfileCape) return settings.username + "'s cape";
        if (const Game::CapeInfo* c = Game::FindCape(settings.cape)) return std::string(c->name);
        return "No cape";
    }

    // ── View ──

    void AppearanceView::Draw(Settings& settings, std::string& playerColor, bool& dirty) {
        EnsureLoaded(settings);
        if (!m_customsScanned) ScanCustomSkins();
        PollAsync(settings, dirty);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float x0 = ContentX();
        const float x1 = ContentRight();

        // ── Header + mode switch ──
        TxtTracked(dl, g_fontMono10, ImVec2(x0, kPadY), TextFaint, "APPEARANCE", 2.0f);
        Txt(dl, g_fontH2, ImVec2(x0, kPadY + 19.2f), TextPrimary, "Your look");
        {
            const char* labels[] = { "Stick figure", "Minecraft skin" };
            const float boxH = Measure(g_fontSmallMed, "G").y + 14.0f + 8.0f;
            const int active = settings.mode == Game::AppearanceMode::Skin ? 1 : 0;
            const int clicked = Segmented("##appearanceMode", labels, 2, active,
                                          ImVec2(x1, kPadY + kHeaderH - boxH), true);
            if (clicked >= 0 && clicked != active) {
                settings.mode = clicked == 1 ? Game::AppearanceMode::Skin : Game::AppearanceMode::StickFigure;
                dirty = true;
            }
        }

        const float top = kPadY + kHeaderH + 18.0f;
        const float bottom = static_cast<float>(WindowHeight) - kPadY;

        // ── Preview (left) ──
        DrawPreview(ImVec2(x0, top), ImVec2(x0 + kPreviewW, bottom), settings, playerColor, dirty);

        // ── Options (right, scrolling) ──
        const float colX = x0 + kPreviewW + kColumnGap;
        ImGui::SetCursorScreenPos(ImVec2(colX, top));
        ImGui::BeginChild("##appearanceScroll", ImVec2(x1 - colX, bottom - top),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            // Leave the slim scrollbar its lane.
            const float width = ImGui::GetContentRegionAvail().x - 10.0f;
            const float h = settings.mode == Game::AppearanceMode::Skin
                ? DrawSkinPane(origin, width, settings, dirty)
                : DrawStickPane(origin, width, settings, playerColor, dirty);
            ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + h + 12.0f));
            ImGui::Dummy(ImVec2(0, 0));
        }
        ImGui::EndChild();
    }

    void AppearanceView::DrawPreview(const ImVec2& p0, const ImVec2& p1, Settings& settings,
                                     const std::string& playerColor, bool& dirty) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 size = p1 - p0;
        const bool stick = settings.mode == Game::AppearanceMode::StickFigure;
        const bool painting = stick && settings.painted;
        const Game::StickFigurePaint shownPaint =
            settings.painted ? settings.paint : Game::StickFigurePaint::Uniform(Game::ParsePlayerColorName(playerColor));

        m_preview.FitTo(size);

        // Input first, so this frame's geometry shows this frame's paint.
        ImGui::SetCursorScreenPos(p0);
        ImGui::InvisibleButton("##appearancePreview", size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        m_hoverCell = -1;
        if (painting && (hovered || active)) {
            glm::vec3 o, d;
            m_preview.Ray(io.MousePos - p0, size, o, d);
            m_hoverCell = PickStickCell(shownPaint, false, m_preview.Eye(), o, d);
        }
        if (ImGui::IsItemActivated()) {
            m_painting = painting && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hoverCell >= 0;
            m_orbiting = !m_painting;
            m_lastPaintedCell = -1;
        }
        if (active) {
            if (m_painting) {
                if (m_hoverCell >= 0 && m_hoverCell != m_lastPaintedCell) {
                    if (settings.paint.At(m_hoverCell) != m_brush) {
                        settings.paint.Set(m_hoverCell, m_brush);
                        dirty = true;
                    }
                    m_lastPaintedCell = m_hoverCell;
                }
            } else if (m_orbiting) {
                m_preview.Orbit(io.MouseDelta);
            }
        } else {
            m_painting = m_orbiting = false;
        }
        if (hovered) m_preview.Zoom(io.MouseWheel, 0.5f, 1.6f);

        // ── Scene ──
        m_preview.Clear();
        if (stick) {
            const Game::StickFigurePaint& paint = settings.painted ? settings.paint : shownPaint;
            BuildStickMesh(paint, /*uniformLook=*/!settings.painted, m_preview.Eye(), 0.022f,
                           painting ? m_hoverCell : -1, m_stickMesh);
            m_preview.AddBatch(m_stickMesh.lines, 0, /*cull=*/false, /*lit=*/false);
            m_preview.AddBatch(m_stickMesh.faces, 0, /*cull=*/true, /*lit=*/false);
        } else {
            const SkinRef skin = CurrentSkin(settings);
            PlayerPose pose;
            pose.ageTicks = static_cast<float>(ImGui::GetTime() * 20.0);
            std::vector<MeshTri> tris;
            std::vector<PreviewVertex> verts;
            if (skin.texture != 0) {
                BuildPlayerMesh(skin.model, true, Game::ModelPartBits::All, pose, tris);
                AppendMeshVertices(tris, 64.0f, 64.0f, verts);
                m_preview.AddBatch(verts, skin.texture, false, true);
            }
            const GLuint cape = CurrentCape(settings);
            if (cape != 0) {
                tris.clear();
                verts.clear();
                BuildCapeMesh(pose, tris);
                AppendMeshVertices(tris, 64.0f, 32.0f, verts);
                m_preview.AddBatch(verts, cape, false, true);
            }
        }
        m_preview.Draw(dl, p0, p1, BgStripeA, 12.0f);
        dl->AddRect(p0, p1, Border, 12.0f);

        // Caption: the hovered cell while painting, else how to turn it.
        std::string caption;
        if (painting && m_hoverCell >= 0) {
            const auto part = Game::StickFigurePaint::PartOf(m_hoverCell);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s  %d/%d", Game::StickFigurePaint::PartName(part),
                          Game::StickFigurePaint::IndexInPart(m_hoverCell) + 1,
                          Game::StickFigurePaint::CellCount(part));
            caption = buf;
            for (char& c : caption) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        } else {
            caption = painting ? "CLICK TO PAINT" : "DRAG TO TURN";
        }
        const float cw = MeasureTracked(g_fontMono9, caption.c_str(), 1.26f);
        TxtTracked(dl, g_fontMono9, ImVec2(p0.x + (size.x - cw) * 0.5f, p1.y - 22.0f), TextGhost,
                   caption.c_str(), 1.26f);
    }

    float AppearanceView::DrawStickPane(const ImVec2& origin, float width, Settings& settings,
                                        std::string& playerColor, bool& dirty) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float y = origin.y;
        const float x = origin.x;
        const Game::PlayerColorId current = Game::ParsePlayerColorName(playerColor);
        if (!m_brushSeeded) {
            m_brush = current;
            m_brushSeeded = true;
        }

        // ── Colour (the plain figure; also what servers without paint see) ──
        Txt(dl, g_fontLabel12, ImVec2(x, y), TextMuted, "Colour");
        {
            std::string hint = "--color ";
            hint += playerColor.empty() ? "default" : playerColor;
            const ImVec2 ts = Measure(g_fontMono10, hint.c_str());
            Txt(dl, g_fontMono10, ImVec2(x + width - ts.x, y + 2), TextFaint, hint.c_str());
        }
        y += 26.0f;
        {
            constexpr float kCell = 34.0f, kGap = 9.0f;
            const size_t count = sizeof(Game::kPlayerColorTable) / sizeof(Game::kPlayerColorTable[0]);
            for (size_t i = 0; i < count; ++i) {
                const auto& entry = Game::kPlayerColorTable[i];
                const float cx = x + static_cast<float>(i % 5) * (kCell + kGap);
                const float cy = y + static_cast<float>(i / 5) * (kCell + kGap);
                ImGui::SetCursorScreenPos(ImVec2(cx, cy));
                ImGui::PushID(entry.slug);
                const bool clicked = ImGui::InvisibleButton("##swatch", ImVec2(kCell, kCell));
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                dl->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + kCell, cy + kCell),
                                  IM_COL32(entry.r, entry.g, entry.b, 255), 9.0f);
                if (entry.id == current) {
                    dl->AddRect(ImVec2(cx - 2, cy - 2), ImVec2(cx + kCell + 2, cy + kCell + 2), TextPrimary, 11.0f, 0, 2.0f);
                } else if (hovered) {
                    dl->AddRect(ImVec2(cx - 2, cy - 2), ImVec2(cx + kCell + 2, cy + kCell + 2), BorderHover, 11.0f, 0, 2.0f);
                }
                if (hovered) Tooltip(entry.name);
                if (clicked) {
                    // Default → empty, as the Character tab always stored it.
                    playerColor = entry.id == Game::PlayerColorId::Default ? std::string() : entry.slug;
                    dirty = true;
                }
            }
            y += 2.0f * kCell + kGap + 12.0f;
        }
        Txt(dl, g_fontSmall, ImVec2(x, y), TextBody, Game::LookupPlayerColor(current).name);
        y += 30.0f;
        dl->AddLine(ImVec2(x, y), ImVec2(x + width, y), BorderSoft);
        y += 16.0f;

        // ── The painter ──
        Txt(dl, g_fontBodySemi, ImVec2(x, y), TextPrimary, "Paint your figure");
        Txt(dl, g_fontSmall, ImVec2(x, y + 20.0f), TextMuted, "Colour each part from the same palette.");
        ImGui::SetCursorScreenPos(ImVec2(x + width - 34.0f, y + 6.0f));
        if (Toggle("##stickPainted", settings.painted)) {
            settings.painted = !settings.painted;
            // Switching on starts from the plain figure in your colour —
            // unless a painting from before is still there to go back to.
            if (settings.painted && settings.paint.IsUniform()) {
                settings.paint = Game::StickFigurePaint::Uniform(current);
                m_brush = current;
            }
            dirty = true;
        }
        y += 48.0f;
        if (!settings.painted) {
            Txt(dl, g_fontSmall, ImVec2(x, y), TextGhost, "Off: the whole figure is your colour.", width);
            return y + 20.0f - origin.y;
        }

        // Brush.
        SectionLabel(dl, ImVec2(x, y), "BRUSH");
        y += 18.0f;
        {
            const size_t count = sizeof(Game::kPlayerColorTable) / sizeof(Game::kPlayerColorTable[0]);
            const float gap = 6.0f;
            const float cell = std::min(24.0f, (width - gap * (count - 1)) / count);
            for (size_t i = 0; i < count; ++i) {
                const auto& entry = Game::kPlayerColorTable[i];
                const ImVec2 c0(x + i * (cell + gap), y);
                ImGui::SetCursorScreenPos(c0);
                ImGui::PushID(static_cast<int>(i));
                const bool clicked = ImGui::InvisibleButton("##brush", ImVec2(cell, cell));
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                dl->AddRectFilled(c0, c0 + ImVec2(cell, cell), IM_COL32(entry.r, entry.g, entry.b, 255), 7.0f);
                if (entry.id == m_brush) {
                    dl->AddRect(c0 - ImVec2(2, 2), c0 + ImVec2(cell + 2, cell + 2), TextPrimary, 9.0f, 0, 2.0f);
                } else if (hovered) {
                    dl->AddRect(c0 - ImVec2(2, 2), c0 + ImVec2(cell + 2, cell + 2), BorderHover, 9.0f, 0, 2.0f);
                }
                if (hovered) Tooltip(entry.name);
                if (clicked) m_brush = entry.id;
            }
            y += cell + 12.0f;
        }
        Txt(dl, g_fontSmall, ImVec2(x, y), TextMuted, "Click the figure to paint a segment; drag to keep painting.", width);
        y += 38.0f;

        // Whole parts at once.
        SectionLabel(dl, ImVec2(x, y), "FILL A PART");
        y += 18.0f;
        {
            float px = x;
            const ImVec2 pad(11, 6);
            const float rowH = PillSize("G", g_fontSmallMed, pad).y + 6.0f;
            for (int p = 0; p < Game::StickFigurePaint::kPartCount; ++p) {
                const auto part = static_cast<Game::StickFigurePaint::Part>(p);
                const char* name = Game::StickFigurePaint::PartName(part);
                const ImVec2 sz = PillSize(name, g_fontSmallMed, pad);
                if (px + sz.x > x + width) {
                    px = x;
                    y += rowH;
                }
                ImGui::SetCursorScreenPos(ImVec2(px, y));
                ImGui::PushID(p);
                if (Pill("##fillPart", name, g_fontSmallMed, pad, BgActive, BgActiveHov, TextBody, 8.0f)) {
                    settings.paint.Fill(part, m_brush);
                    dirty = true;
                }
                ImGui::PopID();
                px += sz.x + 6.0f;
            }
            y += rowH + 6.0f;
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            if (Pill("##fillAll", "Whole figure", g_fontSmallMed, pad, BgActive, BgActiveHov, TextBody, 8.0f)) {
                settings.paint = Game::StickFigurePaint::Uniform(m_brush);
                dirty = true;
            }
            y += rowH;
        }
        return y - origin.y;
    }

    float AppearanceView::DrawSkinPane(const ImVec2& origin, float width, Settings& settings, bool& dirty) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float y = origin.y;
        const float x = origin.x;

        if (!m_usernameSeeded) {
            std::snprintf(m_usernameBuf, sizeof(m_usernameBuf), "%s", settings.username.c_str());
            m_usernameSeeded = true;
        }

        // ── Source tiles ──
        Txt(dl, g_fontLabel12, ImVec2(x, y), TextMuted, "Skin");
        y += 24.0f;
        {
            struct SourceTile { const char* label; SkinSource source; };
            const SourceTile tiles[] = {
                { "Steve", SkinSource::Steve }, { "Alex", SkinSource::Alex },
                { "Username", SkinSource::Username }, { "Custom", SkinSource::Custom },
            };
            const float gap = 8.0f;
            const float tw = (width - gap * 3.0f) / 4.0f, th = 88.0f;
            for (int i = 0; i < 4; ++i) {
                const ImVec2 t0(x + i * (tw + gap), y), t1 = t0 + ImVec2(tw, th);
                ImGui::SetCursorScreenPos(t0);
                ImGui::PushID(i);
                const bool clicked = ImGui::InvisibleButton("##source", ImVec2(tw, th));
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                const bool selected = settings.source == tiles[i].source;
                TileFrame(dl, t0, t1, selected, hovered);

                GLuint face = 0;
                switch (tiles[i].source) {
                    case SkinSource::Steve: face = m_steveTex.Id(); break;
                    case SkinSource::Alex:  face = m_alexTex.Id(); break;
                    case SkinSource::Username:
                        if (m_profileHasSkin && m_profileFor == Lower(settings.username)) face = m_profileTex.Id();
                        break;
                    case SkinSource::Custom:
                        if (CustomSkin* c = FindCustom(settings.customSkin)) face = c->texture.Id();
                        else if (!m_customs.empty()) face = m_customs.front().texture.Id();
                        break;
                }
                const ImVec2 f0(t0.x + (tw - 40.0f) * 0.5f, t0.y + 12.0f);
                if (face != 0) {
                    DrawFace(dl, face, f0, f0 + ImVec2(40, 40), 6.0f);
                } else {
                    dl->AddRectFilled(f0, f0 + ImVec2(40, 40), BgActive, 6.0f);
                    const char* glyph = tiles[i].source == SkinSource::Username ? "?" : "+";
                    const ImVec2 gs = Measure(g_fontH3, glyph);
                    Txt(dl, g_fontH3, f0 + (ImVec2(40, 40) - gs) * 0.5f, TextDim, glyph);
                }
                // A fetched profile's tile is captioned with the player's name
                // (as Mojang spells it), trimmed with an ellipsis to the tile.
                std::string label = tiles[i].label;
                if (tiles[i].source == SkinSource::Username && face != 0 && !settings.username.empty()) {
                    label = settings.username;
                    const float maxW = tw - 12.0f;
                    if (Measure(g_fontSmallMed, label.c_str()).x > maxW) {
                        while (label.size() > 1 &&
                               Measure(g_fontSmallMed, (label + "...").c_str()).x > maxW) {
                            label.pop_back();
                        }
                        label += "...";
                    }
                }
                const ImVec2 ls = Measure(g_fontSmallMed, label.c_str());
                Txt(dl, g_fontSmallMed, ImVec2(t0.x + (tw - ls.x) * 0.5f, t0.y + 62.0f),
                    selected ? TextPrimary : TextBody, label.c_str());
                if (clicked && !selected) {
                    settings.source = tiles[i].source;
                    if (settings.source == SkinSource::Custom && settings.customSkin.empty() && !m_customs.empty()) {
                        settings.customSkin = m_customs.front().file;
                    }
                    dirty = true;
                }
            }
            y += th + 16.0f;
        }

        // ── The source's own options ──
        switch (settings.source) {
            case SkinSource::Steve:
            case SkinSource::Alex: {
                const bool slim = settings.source == SkinSource::Alex;
                Txt(dl, g_fontSmall, ImVec2(x, y), TextMuted,
                    slim ? "Minecraft's default Alex skin, with slim 3-pixel arms."
                         : "Minecraft's default Steve skin, with classic 4-pixel arms.", width);
                y += 26.0f;
                break;
            }
            case SkinSource::Username: {
                const float btnW = 74.0f, h = 34.0f;
                ImGui::SetCursorScreenPos(ImVec2(x, y));
                const bool enter = Input("##javaName", "Java username", m_usernameBuf, sizeof(m_usernameBuf),
                                         width - btnW - 8.0f, h, g_fontInput13,
                                         ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_EnterReturnsTrue,
                                         UsernameFilter);
                const std::string name = m_usernameBuf;
                const bool valid = ProfileFetcher::IsValidName(name);
                const bool busy = m_fetcher.Busy();
                const bool fetch = Secondary("##fetchProfile", busy ? "..." : "Fetch",
                                             ImVec2(x + width - btnW, y), ImVec2(btnW, h), valid && !busy);
                if ((fetch || (enter && valid)) && !busy) {
                    m_fetcher.Start(name);
                    m_fetchStatus = "Looking up " + name + "...";
                    m_fetchFailed = false;
                }
                y += h + 10.0f;
                std::string status = m_fetchStatus;
                if (status.empty()) {
                    status = settings.username.empty()
                        ? "Uses the skin and cape of a Minecraft: Java Edition account."
                        : settings.username + (settings.usernameModel == Game::SkinModel::Slim
                                                   ? "  -  slim arms" : "  -  classic arms");
                }
                Txt(dl, g_fontSmall, ImVec2(x, y), m_fetchFailed ? RedFg : TextMuted, status.c_str(), width);
                y += 26.0f;
                break;
            }
            case SkinSource::Custom: {
                if (m_customs.empty()) {
                    Txt(dl, g_fontSmall, ImVec2(x, y), TextMuted,
                        "No skins yet. Make one in the skin editor.", width);
                    y += 26.0f;
                } else {
                    const int cols = 5;
                    const float gap = 8.0f;
                    const float tw = (width - gap * (cols - 1)) / cols, th = 72.0f;
                    for (size_t i = 0; i < m_customs.size(); ++i) {
                        CustomSkin& c = m_customs[i];
                        const ImVec2 t0(x + (i % cols) * (tw + gap), y + (i / cols) * (th + gap));
                        ImGui::SetCursorScreenPos(t0);
                        ImGui::PushID(static_cast<int>(i));
                        const bool clicked = ImGui::InvisibleButton("##custom", ImVec2(tw, th));
                        const bool hovered = ImGui::IsItemHovered();
                        ImGui::PopID();
                        const bool selected = settings.customSkin == c.file;
                        TileFrame(dl, t0, t0 + ImVec2(tw, th), selected, hovered);
                        const ImVec2 f0(t0.x + (tw - 34.0f) * 0.5f, t0.y + 9.0f);
                        DrawFace(dl, c.texture.Id(), f0, f0 + ImVec2(34, 34), 5.0f);
                        std::string label = c.file.substr(0, c.file.size() - 4);
                        label = Ellipsize(g_fontMono9, label, tw - 8.0f);
                        const float lw = Measure(g_fontMono9, label.c_str()).x;
                        Txt(dl, g_fontMono9, ImVec2(t0.x + (tw - lw) * 0.5f, t0.y + 52.0f), TextDim, label.c_str());
                        if (hovered) Tooltip(c.file.c_str());
                        if (clicked && !selected) {
                            settings.customSkin = c.file;
                            dirty = true;
                        }
                    }
                    const size_t rows = (m_customs.size() + cols - 1) / cols;
                    y += rows * (th + gap) + 6.0f;
                }
                // The custom skin's arms.
                Txt(dl, g_fontLabel12, ImVec2(x, y + 8.0f), TextMuted, "Arms");
                const char* labels[] = { "Classic", "Slim" };
                const int active = settings.customModel == Game::SkinModel::Slim ? 1 : 0;
                const int clicked = Segmented("##customArms", labels, 2, active, ImVec2(x + 44.0f, y), false);
                if (clicked >= 0 && clicked != active) {
                    settings.customModel = clicked == 1 ? Game::SkinModel::Slim : Game::SkinModel::Classic;
                    dirty = true;
                }
                y += 42.0f;
                break;
            }
        }

        // The editor, on a copy of whatever is shown.
        if (Secondary("##openEditor", settings.source == SkinSource::Custom && FindCustom(settings.customSkin)
                                          ? "Edit in skin editor" : "Open skin editor",
                      ImVec2(x, y), ImVec2(width, 34.0f))) {
            OpenEditor(settings);
        }
        y += 34.0f + 18.0f;
        dl->AddLine(ImVec2(x, y), ImVec2(x + width, y), BorderSoft);
        y += 16.0f;

        y += DrawCapeGrid(ImVec2(x, y), width, settings, dirty);
        return y - origin.y;
    }

    float AppearanceView::DrawCapeGrid(const ImVec2& origin, float width, Settings& settings, bool& dirty) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float y = origin.y;
        const float x = origin.x;

        Txt(dl, g_fontLabel12, ImVec2(x, y), TextMuted, "Cape");
        {
            const std::string current = CapeDisplayName(settings);
            const ImVec2 ts = Measure(g_fontMono10, current.c_str());
            Txt(dl, g_fontMono10, ImVec2(x + width - ts.x, y + 2), TextFaint, current.c_str());
        }
        y += 24.0f;

        const int cols = 5;
        const float gap = 8.0f;
        const float tw = (width - gap * (cols - 1)) / cols, th = 82.0f;
        int col = 0;
        const auto nextCell = [&]() {
            const ImVec2 p(x + col * (tw + gap), y);
            if (++col == cols) {
                col = 0;
                y += th + gap;
            }
            return p;
        };
        const auto endRow = [&]() {
            if (col != 0) {
                col = 0;
                y += th + gap;
            }
        };
        // One tile: `texture` 0 shows `glyph` (or the loading dots).
        const auto tile = [&](const char* id, const std::string& value, const char* label, GLuint texture,
                              bool loading, const char* glyph, const char* tooltip) {
            const ImVec2 t0 = nextCell(), t1 = t0 + ImVec2(tw, th);
            ImGui::SetCursorScreenPos(t0);
            const bool clicked = ImGui::InvisibleButton(id, ImVec2(tw, th));
            const bool hovered = ImGui::IsItemHovered();
            const bool selected = settings.cape == value;
            TileFrame(dl, t0, t1, selected, hovered);
            // The cape's back face: texels (1, 1)–(11, 17) of the 64x32 sheet.
            const ImVec2 c0(t0.x + (tw - 30.0f) * 0.5f, t0.y + 9.0f), c1 = c0 + ImVec2(30.0f, 48.0f);
            if (texture != 0) {
                dl->AddImage(Tex(texture), c0, c1, ImVec2(1.0f / 64.0f, 1.0f / 32.0f),
                             ImVec2(11.0f / 64.0f, 17.0f / 32.0f));
            } else {
                dl->AddRectFilled(c0, c1, BgActive, 4.0f);
                if (loading) {
                    LoadingDots(dl, (c0 + c1) * 0.5f);
                } else if (glyph) {
                    const ImVec2 gs = Measure(g_fontBody, glyph);
                    Txt(dl, g_fontBody, (c0 + c1 - gs) * 0.5f, TextDim, glyph);
                }
            }
            std::string name = Ellipsize(g_fontMono9, label, tw - 6.0f);
            const float lw = Measure(g_fontMono9, name.c_str()).x;
            Txt(dl, g_fontMono9, ImVec2(t0.x + (tw - lw) * 0.5f, t0.y + 63.0f),
                selected ? TextPrimary : TextDim, name.c_str());
            if (hovered && tooltip) Tooltip(tooltip);
            return clicked && !selected;
        };

        if (tile("##capeNone", "", "None", 0, false, "-", "No cape")) {
            settings.cape.clear();
            dirty = true;
        }
        const bool profileCape = settings.source == SkinSource::Username && m_profileHasCape &&
                                 m_profileFor == Lower(settings.username);
        if (profileCape) {
            const std::string label = settings.username;
            const std::string tip = settings.username + "'s own cape";
            if (tile("##capeProfile", kProfileCape, label.c_str(), m_profileCapeTex.Id(), false, nullptr, tip.c_str())) {
                settings.cape = kProfileCape;
                dirty = true;
            }
        }
        endRow();
        y += 4.0f;

        Game::CapeCategory section = Game::kCapes[0].category;
        bool first = true;
        for (size_t i = 0; i < Game::kCapeCount; ++i) {
            const Game::CapeInfo& cape = Game::kCapes[i];
            if (first || cape.category != section) {
                endRow();
                if (!first) y += 6.0f;
                section = cape.category;
                first = false;
                SectionLabel(dl, ImVec2(x, y), Game::CapeCategoryName(section));
                y += 18.0f;
            }
            const std::string slug(cape.slug);
            // Fetch a cape only once its tile is on screen.
            const ImVec2 at(x + col * (tw + gap), y);
            CapeLibrary::Status status = m_capes.StatusOf(slug);
            if (status == CapeLibrary::Status::Unrequested && ImGui::IsRectVisible(at, at + ImVec2(tw, th))) {
                status = m_capes.Request(slug);
            }
            const std::string name(cape.name);
            const std::string tip = name + "\n" + std::string(cape.note);
            ImGui::PushID(static_cast<int>(i));
            const bool clicked = tile("##cape", slug, name.c_str(), m_capes.Texture(slug),
                                      status == CapeLibrary::Status::Loading,
                                      status == CapeLibrary::Status::Failed ? "!" : nullptr, tip.c_str());
            ImGui::PopID();
            if (clicked) {
                if (status == CapeLibrary::Status::Failed) {
                    m_capes.Retry(slug);
                    m_capes.Request(slug);
                }
                settings.cape = slug;
                dirty = true;
            }
        }
        endRow();
        Txt(dl, g_fontSmall, ImVec2(x, y), TextGhost,
            "Capes show on the Minecraft skin, and an elytra takes the cape's look.", width);
        y += 34.0f;
        return y - origin.y;
    }

    void AppearanceView::DrawFace(ImDrawList* dl, GLuint texture, const ImVec2& p0, const ImVec2& p1,
                                  float rounding) {
        if (texture == 0) return;
        // The head's front (8,8)–(16,16), then the hat's front over it.
        dl->AddImageRounded(Tex(texture), p0, p1, ImVec2(8.0f / 64.0f, 8.0f / 64.0f),
                            ImVec2(16.0f / 64.0f, 16.0f / 64.0f), IM_COL32_WHITE, rounding);
        dl->AddImageRounded(Tex(texture), p0, p1, ImVec2(40.0f / 64.0f, 8.0f / 64.0f),
                            ImVec2(48.0f / 64.0f, 16.0f / 64.0f), IM_COL32_WHITE, rounding);
    }

    void AppearanceView::DrawAvatar(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, const Settings& settings,
                                    ImU32 playerColor, float rounding) {
        if (settings.mode == Game::AppearanceMode::Skin) {
            EnsureLoaded(settings);
            if (!m_customsScanned && settings.source == SkinSource::Custom) ScanCustomSkins();
            const SkinRef skin = CurrentSkin(settings);
            if (skin.texture != 0) {
                dl->AddRectFilled(p0, p1, BgActive, rounding);
                DrawFace(dl, skin.texture, p0, p1, rounding);
                return;
            }
        } else if (settings.painted) {
            // The painted figure's head outline colour.
            dl->AddRectFilled(p0, p1, ColorOf(settings.paint.At(Game::StickFigurePaint::Part::HeadRing, 0)), rounding);
            return;
        }
        dl->AddRectFilled(p0, p1, playerColor, rounding);
    }

    void AppearanceView::OpenEditor(const Settings& settings) {
        const SkinRef skin = CurrentSkin(settings);
        std::string name = "my_skin";
        if (settings.source == SkinSource::Custom && FindCustom(settings.customSkin)) {
            name = settings.customSkin.substr(0, settings.customSkin.size() - 4);
        } else if (settings.source == SkinSource::Username && !settings.username.empty()) {
            name = settings.username + "_edit";
        }
        m_editor.Open(skin.image ? *skin.image : BlankSkin(), skin.model, name);
    }

    void AppearanceView::DrawEditor(Settings& settings, bool& dirty) {
        EnsureLoaded(settings);
        PollAsync(settings, dirty);
        EditorTemplates t;
        t.steve = m_steve.Valid() ? &m_steve : nullptr;
        t.alex = m_alex.Valid() ? &m_alex : nullptr;
        if (m_profileHasSkin && !settings.username.empty() && m_profileFor == Lower(settings.username)) {
            t.profile = &m_profileSkin;
            t.profileName = settings.username;
            t.profileModel = settings.usernameModel;
        }
        const SkinRef current = CurrentSkin(settings);
        t.current = current.image;
        t.currentModel = current.model;
        if (m_editor.Draw(m_paths, t)) {
            const SkinEditor::SaveResult& saved = m_editor.LastSave();
            ScanCustomSkins();
            settings.mode = Game::AppearanceMode::Skin;
            settings.source = SkinSource::Custom;
            settings.customSkin = saved.fileName;
            settings.customModel = saved.model;
            dirty = true;
        }
    }

    void AppearanceView::ReleaseGpu() {
        m_steveTex.Release();
        m_alexTex.Release();
        m_profileTex.Release();
        m_profileCapeTex.Release();
        for (CustomSkin& c : m_customs) c.texture.Release();
        m_capes.ReleaseGpu();
        m_preview.Release();
        m_editor.ReleaseGpu();
    }

} // namespace Launcher::Appearance
