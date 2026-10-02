// File: src/launcher/appearance/AppearanceSettings.hpp
//
// The player's look as the launcher keeps it: the persisted choice
// (launcher.json "appearance"), where its files live under the obeycraft
// directory, and the hand-off to the game — the files written next to
// launch and the CLI flags that point at them (docs/player-appearance.md).
//
//   {obeycraft}/skins/<name>.png            custom skins (the skin editor)
//   {obeycraft}/skins/profiles/<user>.png   a Java profile's fetched skin
//   {obeycraft}/skins/profiles/<user>_cape.png   …and its cape (64x32)
//   {obeycraft}/capes/<slug>.png            downloaded capes (Game::kCapes)
//   {obeycraft}/appearance/skin.png         what the game is launched with
//   {obeycraft}/appearance/cape.png
//   {obeycraft}/appearance/stickfigure.txt
#pragma once

#include "common/entity/PlayerAppearance.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace Launcher::Appearance {

    // Where a Skin-mode player's skin comes from.
    enum class SkinSource : uint8_t {
        Steve = 0,   // MC DefaultPlayerSkin wide Steve (classic arms)
        Alex,        // slim Alex
        Username,    // a Java profile's skin, fetched from Mojang
        Custom,      // a skin from the editor (skins/<name>.png)
    };

    // The cape id that means "the fetched profile's own cape".
    inline constexpr const char* kProfileCape = "profile";

    struct Settings {
        Game::AppearanceMode mode = Game::AppearanceMode::StickFigure;
        SkinSource source = SkinSource::Steve;
        std::string username;                              // the Java profile picked
        Game::SkinModel usernameModel = Game::SkinModel::Classic;   // from its metadata
        bool usernameHasCape = false;                      // the profile carries a cape
        std::string customSkin;                            // file name in skins/
        Game::SkinModel customModel = Game::SkinModel::Classic;
        std::string cape;                                  // "", a kCapes slug, or kProfileCape
        // The stick-figure painter. The plain figure is the player's colour
        // (LauncherUIState::playerColor, --color); painted, these cells win.
        bool painted = false;
        Game::StickFigurePaint paint;

        // The arm model the chosen skin draws with.
        Game::SkinModel Model() const;

        void ToJson(nlohmann::json& out) const;
        static Settings FromJson(const nlohmann::json& in);
    };

    // The obeycraft directory's appearance paths.
    class Paths {
    public:
        Paths() = default;
        Paths(std::string obeycraftDir, std::string bundledSkinsDir)
            : m_root(std::move(obeycraftDir)), m_bundled(std::move(bundledSkinsDir)) {}

        bool Valid() const { return !m_root.empty(); }
        const std::string& Root() const { return m_root; }

        std::string SkinsDir() const      { return m_root + "/skins"; }
        std::string ProfilesDir() const   { return m_root + "/skins/profiles"; }
        std::string CapesDir() const      { return m_root + "/capes"; }
        std::string LaunchDir() const     { return m_root + "/appearance"; }

        std::string CustomSkin(const std::string& fileName) const { return SkinsDir() + "/" + fileName; }
        std::string ProfileSkin(const std::string& username) const;
        std::string ProfileCape(const std::string& username) const;
        std::string CapeCache(const std::string& slug) const { return CapesDir() + "/" + slug + ".png"; }

        // The launcher's bundled default skins (assets/launcher/skins).
        std::string DefaultSkin(Game::SkinModel model) const;

    private:
        std::string m_root;
        std::string m_bundled;
    };

    // A file name made safe for skins/ ("My Skin!" → "My_Skin_.png"). Empty
    // for a name with nothing usable in it.
    std::string SanitizeSkinFileName(const std::string& name);

    // ── Launch hand-off ─────────────────────────────────────────────────────
    //
    // Writes {obeycraft}/appearance/* for the current settings and says what
    // the CLI should carry. A source whose file is missing (a profile never
    // fetched, a deleted custom skin, a cape not downloaded) falls back the
    // way the game would: the model's default skin, no cape.
    struct LaunchFiles {
        Game::AppearanceMode mode = Game::AppearanceMode::StickFigure;
        Game::SkinModel model = Game::SkinModel::Classic;
        std::string skinPath;          // empty: the model's default skin
        std::string capePath;          // empty: no cape
        std::string stickFigurePath;   // empty: the plain one-colour figure
    };
    LaunchFiles PrepareLaunchFiles(const Settings& settings, const Paths& paths);

    // One argument quoted for the command line ProcessLauncher hands to the
    // shell (`open … --args` on macOS, ShellExecute on Windows) — the
    // obeycraft directory has a space in it on macOS.
    std::string QuoteArg(const std::string& arg);

} // namespace Launcher::Appearance
