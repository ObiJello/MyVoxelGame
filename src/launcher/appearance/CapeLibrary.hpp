// File: src/launcher/appearance/CapeLibrary.hpp
//
// The capes of Game::kCapes, loaded on demand: the grid asks for a cape when
// its tile scrolls into view (or it is picked), the library answers from the
// on-disk cache ({obeycraft}/capes/<slug>.png) or downloads it from
// textures.minecraft.net on a worker thread, normalises it to 64x32, caches
// it, and hands the image back to the UI thread for a GL texture.
//
// One worker at a time drains the queue over one HTTP session (one TLS
// handshake for the lot). It shares only reference-counted state with the
// library, so the launcher can close mid-download.
#pragma once

#include "SkinIO.hpp"

#include <memory>
#include <string>
#include <unordered_map>

namespace Launcher::Appearance {

    class CapeLibrary {
    public:
        enum class Status { Unrequested, Loading, Ready, Failed };

        CapeLibrary();
        ~CapeLibrary();
        CapeLibrary(const CapeLibrary&) = delete;
        CapeLibrary& operator=(const CapeLibrary&) = delete;

        void SetCacheDir(const std::string& dir);

        // Starts loading `slug` if nothing has yet; returns where it stands.
        Status Request(const std::string& slug);
        Status StatusOf(const std::string& slug) const;
        // Ready capes: the 64x32 image and its texture.
        const Game::SkinImage* Image(const std::string& slug) const;
        GLuint Texture(const std::string& slug) const;

        // Moves finished loads onto the UI thread (GL uploads). Once a frame.
        void Poll();
        // Forgets a failed load so the next Request tries again.
        void Retry(const std::string& slug);
        // Frees the GL textures (before the GL context goes away).
        void ReleaseGpu();

        // The queue and results the worker thread shares (CapeLibrary.cpp).
        struct Shared;

    private:
        struct Entry {
            Status status = Status::Unrequested;
            Game::SkinImage image;
            GlImage texture;
        };
        std::shared_ptr<Shared> m_shared;
        std::unordered_map<std::string, Entry> m_entries;
        std::string m_cacheDir;
        void Kick();
    };

} // namespace Launcher::Appearance
