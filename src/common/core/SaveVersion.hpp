// File: src/common/core/SaveVersion.hpp
//
// The one place the save format's version number lives.
//
// DataVersion is how Minecraft decides what to do with a file it did not
// write. Get it wrong in either direction and the damage is silent:
//
//   too LOW   MC runs its whole DataFixerUpper chain from that version to its
//             own over data that is already modern. Those fixes rename blocks
//             and items. Nothing errors; the world just comes back subtly
//             wrong.
//   too HIGH  MC returns the tag untouched and shows a dismissible "played in
//             a newer version" backup prompt. The world still opens.
//
// High is therefore strictly the safer error, which is why this never gets
// clamped down to match an older install — see --data-version if you want to
// target one deliberately.
//
// 4764 is SharedConstants.WORLD_VERSION in the vendored minecraft_code tree,
// which is the release named "26.1 Snapshot 1". Verified against the user's
// own saves: New World (1..3) are real vanilla worlds stamped 4764.
#pragma once

#include <cstdint>

namespace Game::Save {

    inline constexpr int32_t kDefaultDataVersion = 4764;

    // Data.Version.* in level.dat. MC compares only `Series` for
    // compatibility (LevelSummary.isCompatible), and a series other than
    // "main" is rejected outright — `Name` is a display string.
    inline constexpr const char* kVersionName   = "26.1 Snapshot 1";
    inline constexpr const char* kVersionSeries = "main";
    inline constexpr bool        kVersionSnapshot = true;

    // Data.version (lowercase) — the Anvil storage format revision. Constant
    // since 1.2, and unrelated to DataVersion.
    inline constexpr int32_t kAnvilStorageVersion = 19133;

    namespace detail {
        inline int32_t& DataVersionStorage() {
            static int32_t value = kDefaultDataVersion;
            return value;
        }
    }

    // Read by exactly four writers: chunk NBT, the entity region root,
    // level.dat (both Data.DataVersion and Data.Version.Id) and playerdata.
    inline int32_t DataVersion() { return detail::DataVersionStorage(); }

    // Set once at startup from --data-version. Not thread-safe by design:
    // it must be called before any world is opened and never again.
    inline void SetDataVersionOverride(int32_t v) { detail::DataVersionStorage() = v; }

} // namespace Game::Save
