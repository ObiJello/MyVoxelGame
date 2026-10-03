// File: src/launcher/appearance/AppearanceSettings.cpp
#include "AppearanceSettings.hpp"
#include "SkinIO.hpp"
#include "common/core/Log.hpp"
#include "common/entity/PlayerCapes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace Launcher::Appearance {

    namespace {
        const char* SourceSlug(SkinSource s) {
            switch (s) {
                case SkinSource::Steve:    return "steve";
                case SkinSource::Alex:     return "alex";
                case SkinSource::Username: return "username";
                case SkinSource::Custom:   return "custom";
            }
            return "steve";
        }

        SkinSource ParseSource(const std::string& s) {
            if (s == "alex") return SkinSource::Alex;
            if (s == "username") return SkinSource::Username;
            if (s == "custom") return SkinSource::Custom;
            return SkinSource::Steve;
        }

        std::string LowerName(const std::string& s) {
            std::string out;
            out.reserve(s.size());
            for (const char c : s) {
                const auto u = static_cast<unsigned char>(c);
                // Java names are [A-Za-z0-9_]; anything else never reaches a path.
                if (std::isalnum(u) || c == '_') out.push_back(static_cast<char>(std::tolower(u)));
            }
            return out;
        }

        bool Exists(const std::string& path) {
            std::error_code ec;
            return !path.empty() && std::filesystem::is_regular_file(path, ec);
        }

        void RemoveFile(const std::string& path) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    } // namespace

    Game::SkinModel Settings::Model() const {
        switch (source) {
            case SkinSource::Steve:    return Game::SkinModel::Classic;
            case SkinSource::Alex:     return Game::SkinModel::Slim;
            case SkinSource::Username: return usernameModel;
            case SkinSource::Custom:   return customModel;
        }
        return Game::SkinModel::Classic;
    }

    void Settings::ToJson(nlohmann::json& out) const {
        out = nlohmann::json::object();
        out["mode"] = Game::AppearanceModeSlug(mode);
        out["skin_source"] = SourceSlug(source);
        out["username"] = username;
        out["username_model"] = Game::SkinModelSlug(usernameModel);
        out["username_has_cape"] = usernameHasCape;
        out["custom_skin"] = customSkin;
        out["custom_model"] = Game::SkinModelSlug(customModel);
        out["cape"] = cape;
        out["stick_painted"] = painted;
        out["stick_paint"] = paint.ToText();
        out["stick_drawn"] = drawn;
        out["stick_drawing"] = drawing.Empty() ? std::string() : drawing.ToHex();
    }

    Settings Settings::FromJson(const nlohmann::json& in) {
        Settings s;
        if (!in.is_object()) return s;
        try {
            if (const auto m = Game::ParseAppearanceMode(in.value("mode", std::string()))) s.mode = *m;
            s.source = ParseSource(in.value("skin_source", std::string()));
            s.username = in.value("username", std::string());
            if (const auto m = Game::ParseSkinModel(in.value("username_model", std::string()))) s.usernameModel = *m;
            s.usernameHasCape = in.value("username_has_cape", false);
            s.customSkin = in.value("custom_skin", std::string());
            if (const auto m = Game::ParseSkinModel(in.value("custom_model", std::string()))) s.customModel = *m;
            s.cape = in.value("cape", std::string());
            if (!s.cape.empty() && s.cape != kProfileCape && !Game::FindCape(s.cape)) s.cape.clear();
            s.painted = in.value("stick_painted", false);
            if (const auto p = Game::StickFigurePaint::FromText(in.value("stick_paint", std::string()))) {
                s.paint = *p;
            } else {
                s.painted = false;
            }
            // (An older launcher's "stick_sculpt" — the retired 3D voxels —
            // is not read, and goes with the next save.)
            s.drawn = in.value("stick_drawn", false);
            const std::string drawingHex = in.value("stick_drawing", std::string());
            if (!drawingHex.empty()) {
                std::string why;
                if (auto drawing = Game::StickFigureDrawing::FromHex(drawingHex, &why)) {
                    s.drawing = std::move(*drawing);
                } else {
                    Log::Warning("[Appearance] drawn figure dropped (%s)", why.c_str());
                }
            }
            // A custom skin name is a bare file name; never a path.
            if (s.customSkin.find('/') != std::string::npos || s.customSkin.find('\\') != std::string::npos ||
                s.customSkin.find("..") != std::string::npos) {
                s.customSkin.clear();
            }
        } catch (...) {
            Log::Warning("[Appearance] malformed appearance settings, using defaults");
            return Settings{};
        }
        return s;
    }

    std::string Paths::ProfileSkin(const std::string& username) const {
        return ProfilesDir() + "/" + LowerName(username) + ".png";
    }

    std::string Paths::ProfileCape(const std::string& username) const {
        return ProfilesDir() + "/" + LowerName(username) + "_cape.png";
    }

    std::string Paths::DefaultSkin(Game::SkinModel model) const {
        return m_bundled + (model == Game::SkinModel::Slim ? "/alex.png" : "/steve.png");
    }

    std::string SanitizeSkinFileName(const std::string& name) {
        std::string out;
        for (const char c : name) {
            const auto u = static_cast<unsigned char>(c);
            if (std::isalnum(u) || c == '-' || c == '_') out.push_back(c);
            else if (c == ' ' || c == '.') out.push_back('_');
            if (out.size() >= 48) break;
        }
        while (!out.empty() && out.back() == '_') out.pop_back();
        if (out.empty()) return std::string();
        return out + ".png";
    }

    LaunchFiles PrepareLaunchFiles(const Settings& settings, const Paths& paths) {
        LaunchFiles files;
        files.mode = settings.mode;
        files.model = settings.Model();
        if (!paths.Valid()) return files;

        const std::string dir = paths.LaunchDir();
        const std::string skinOut = dir + "/skin.png";
        const std::string capeOut = dir + "/cape.png";
        const std::string stickOut = dir + "/stickfigure.txt";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        if (settings.mode == Game::AppearanceMode::StickFigure) {
            RemoveFile(skinOut);
            RemoveFile(capeOut);
            // The version-4 file: the paint and the drawing worn in its
            // place, whichever there are.
            Game::StickFigureFile file;
            file.hasPaint = settings.painted;
            file.paint = settings.paint;
            if (settings.drawn) file.drawing = settings.drawing;
            if (file.Any()) {
                const std::string text = file.ToText();
                if (WriteFileBytes(stickOut, std::vector<uint8_t>(text.begin(), text.end()))) {
                    files.stickFigurePath = stickOut;
                }
            } else {
                RemoveFile(stickOut);
            }
            return files;
        }

        RemoveFile(stickOut);

        // The skin: re-encoded from the normalised 64x64 image, so the game
        // always receives something its validation takes.
        std::string skinSrc;
        if (settings.source == SkinSource::Username && !settings.username.empty()) {
            skinSrc = paths.ProfileSkin(settings.username);
        } else if (settings.source == SkinSource::Custom && !settings.customSkin.empty()) {
            skinSrc = paths.CustomSkin(settings.customSkin);
        }
        bool wroteSkin = false;
        if (Exists(skinSrc)) {
            Game::SkinImage img;
            if (LoadPngFile(skinSrc, img) && NormalizeSkin(img) && SavePngFile(skinOut, img)) {
                files.skinPath = skinOut;
                wroteSkin = true;
            } else {
                Log::Warning("[Appearance] skin %s is unusable, launching with the default", skinSrc.c_str());
            }
        }
        if (!wroteSkin) {
            RemoveFile(skinOut);
            // A missing profile / custom skin: the game's default for this
            // model (Steve or Alex), which needs no file.
        }

        std::string capeSrc;
        if (settings.cape == kProfileCape) {
            if (settings.source == SkinSource::Username && !settings.username.empty()) {
                capeSrc = paths.ProfileCape(settings.username);
            }
        } else if (!settings.cape.empty()) {
            capeSrc = paths.CapeCache(settings.cape);
        }
        bool wroteCape = false;
        if (Exists(capeSrc)) {
            Game::SkinImage img;
            if (LoadPngFile(capeSrc, img) && NormalizeCape(img) && SavePngFile(capeOut, img)) {
                files.capePath = capeOut;
                wroteCape = true;
            }
        }
        if (!wroteCape) RemoveFile(capeOut);
        return files;
    }

    std::string QuoteArg(const std::string& arg) {
#ifdef _WIN32
        // ShellExecute → CommandLineToArgvW rules: wrap in double quotes,
        // double up embedded quotes (paths cannot hold them anyway).
        std::string out = "\"";
        for (const char c : arg) {
            if (c == '"') out += "\\\"";
            else out.push_back(c);
        }
        out += "\"";
        return out;
#else
        // POSIX sh: single quotes, an embedded quote closed, escaped, reopened.
        std::string out = "'";
        for (const char c : arg) {
            if (c == '\'') out += "'\\''";
            else out.push_back(c);
        }
        out += "'";
        return out;
#endif
    }

} // namespace Launcher::Appearance
