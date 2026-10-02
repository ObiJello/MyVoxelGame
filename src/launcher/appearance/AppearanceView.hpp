// File: src/launcher/appearance/AppearanceView.hpp
//
// The launcher's Appearance view (rail → Appearance): how the player looks
// in game. Two modes, the design's segmented control on the header:
//
//   Stick figure    the colour swatches (--color), and the painter — colour
//                   the figure's parts cell by cell from the same palette,
//                   by clicking them on the rotatable 3D figure.
//   Minecraft skin  Steve, Alex, a Java profile's skin (fetched from Mojang
//                   by username) or a custom skin from the skin editor, on
//                   the rotatable player model; and the cape grid — every
//                   cape Minecraft has released, downloaded on demand.
//
// It edits the persisted Appearance::Settings (and the player colour) in
// place and raises `dirty`; the app saves launcher.json and hands the result
// to the game at launch (AppearanceSettings.hpp).
#pragma once

#include "AppearanceSettings.hpp"
#include "CapeLibrary.hpp"
#include "PlayerMesh.hpp"
#include "Preview3D.hpp"
#include "ProfileFetcher.hpp"
#include "SkinEditor.hpp"
#include "SkinIO.hpp"

#include <imgui.h>

#include <string>
#include <vector>

namespace Launcher::Appearance {

    class AppearanceView {
    public:
        AppearanceView() = default;
        AppearanceView(const AppearanceView&) = delete;
        AppearanceView& operator=(const AppearanceView&) = delete;

        void SetPaths(const Paths& paths);

        // The view, in the content area right of the rail.
        void Draw(Settings& settings, std::string& playerColor, bool& dirty);

        // The skin editor takes the whole window while open.
        bool EditorOpen() const { return m_editor.IsOpen(); }
        void DrawEditor(Settings& settings, bool& dirty);

        // The rail account card's avatar: the skin's face (with its hat) in
        // Skin mode, the player colour otherwise.
        void DrawAvatar(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, const Settings& settings,
                        ImU32 playerColor, float rounding);

        // Frees every GL object; call while the GL context is current.
        void ReleaseGpu();

    private:
        struct SkinRef {
            const Game::SkinImage* image = nullptr;
            GLuint texture = 0;
            Game::SkinModel model = Game::SkinModel::Classic;
        };
        struct CustomSkin {
            std::string file;
            Game::SkinImage image;
            GlImage texture;
        };

        void EnsureLoaded(const Settings& settings);
        void LoadProfileCache(const std::string& username);
        void ScanCustomSkins();
        void PollAsync(Settings& settings, bool& dirty);

        SkinRef CurrentSkin(const Settings& settings);
        GLuint CurrentCape(const Settings& settings);
        std::string CapeDisplayName(const Settings& settings) const;
        CustomSkin* FindCustom(const std::string& file);

        void DrawPreview(const ImVec2& p0, const ImVec2& p1, Settings& settings,
                         const std::string& playerColor, bool& dirty);
        float DrawStickPane(const ImVec2& origin, float width, Settings& settings,
                            std::string& playerColor, bool& dirty);
        float DrawSkinPane(const ImVec2& origin, float width, Settings& settings, bool& dirty);
        float DrawCapeGrid(const ImVec2& origin, float width, Settings& settings, bool& dirty);
        void DrawFace(ImDrawList* dl, GLuint texture, const ImVec2& p0, const ImVec2& p1, float rounding);
        void OpenEditor(const Settings& settings);

        Paths m_paths;
        bool m_loaded = false;

        Game::SkinImage m_steve, m_alex;
        GlImage m_steveTex, m_alexTex;

        // The fetched profile's skin and cape, as cached on disk.
        std::string m_profileFor;          // the username they belong to (lower case)
        bool m_profileHasSkin = false;
        Game::SkinImage m_profileSkin;
        GlImage m_profileTex;
        bool m_profileHasCape = false;
        Game::SkinImage m_profileCape;
        GlImage m_profileCapeTex;

        std::vector<CustomSkin> m_customs;
        bool m_customsScanned = false;

        ProfileFetcher m_fetcher;
        char m_usernameBuf[20] = "";
        bool m_usernameSeeded = false;
        std::string m_fetchStatus;
        bool m_fetchFailed = false;

        CapeLibrary m_capes;

        Preview3D m_preview;
        StickMesh m_stickMesh;
        int m_hoverCell = -1;
        Game::PlayerColorId m_brush = Game::PlayerColorId::Default;
        bool m_brushSeeded = false;
        bool m_painting = false;           // a paint drag on the figure
        int m_lastPaintedCell = -1;
        bool m_orbiting = false;

        SkinEditor m_editor;
    };

} // namespace Launcher::Appearance
