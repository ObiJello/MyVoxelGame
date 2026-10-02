// File: src/launcher/appearance/ProfileFetcher.hpp
//
// A Java Edition profile's skin and cape, from Mojang's public API:
//
//   GET api.mojang.com/users/profiles/minecraft/<name>         → { id, name }
//       (fallback: api.minecraftservices.com/minecraft/profile/lookup/name/<name>)
//   GET sessionserver.mojang.com/session/minecraft/profile/<id> → properties[
//       { name: "textures", value: base64({ textures: { SKIN: { url,
//         metadata: { model: "slim" } }, CAPE: { url } } }) } ]
//   GET textures.minecraft.net/texture/<hash>                   → the PNGs
//
// Runs on a worker thread (the requests block); the UI polls. The worker
// shares only a reference-counted job with the fetcher, so dropping the
// fetcher — or closing the launcher — mid-request is safe.
#pragma once

#include "common/entity/PlayerAppearance.hpp"

#include <memory>
#include <string>

namespace Launcher::Appearance {

    struct ProfileResult {
        bool ok = false;
        std::string error;            // shown inline when !ok
        std::string name;             // the profile's own capitalisation
        std::string uuid;
        Game::SkinModel model = Game::SkinModel::Classic;
        bool hasSkin = false;         // false: the profile uses a default skin
        Game::SkinImage skin;         // normalised 64x64
        bool hasCape = false;
        Game::SkinImage cape;         // normalised 64x32
        std::string capeSlug;         // its Game::kCapes slug when known
    };

    class ProfileFetcher {
    public:
        ProfileFetcher();
        ~ProfileFetcher();
        ProfileFetcher(const ProfileFetcher&) = delete;
        ProfileFetcher& operator=(const ProfileFetcher&) = delete;

        // Starts a lookup, superseding any in flight (its result is dropped).
        void Start(const std::string& username);
        bool Busy() const;
        // True once, when a result is ready; it is moved into `out`.
        bool Poll(ProfileResult& out);
        const std::string& Pending() const { return m_pendingName; }

        // [A-Za-z0-9_]{1,16} — what a Java name can be.
        static bool IsValidName(const std::string& name);

    private:
        struct Job;
        std::shared_ptr<Job> m_job;
        std::string m_pendingName;
    };

} // namespace Launcher::Appearance
