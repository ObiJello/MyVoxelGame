// File: src/common/core/AssetLocator.hpp
//
// The one seam between the loaders that walk the engine's assets/ tree
// (block models, blockstates, colormaps, the two atlases) and whatever
// sits on top of it. With nothing installed it is the plain file system;
// the client installs the resource-pack overlay (client/resource/
// ResourcePacks) at startup, and from then on every listing is the union
// of the vanilla directory and the enabled packs, higher pack winning —
// MC's FallbackResourceManager, walked from the top pack down.
//
// Lives in common so the common loaders need no client include; the
// server target keeps the file-system behaviour.
#pragma once

#include <string>
#include <vector>

namespace Core::Assets {

    struct Entry {
        std::string relative;   // path below the directory asked for, '/' separators, e.g. "grass_block.json"
        std::string absolute;   // the file to open (vanilla or a pack's copy)
        int         layer = 0;  // 0 = vanilla, 1.. = packs, higher = higher priority
    };

    using ListFn   = std::vector<Entry> (*)(const std::string& vanillaAbsDir, const char* extension, bool recursive);
    using LocateFn = std::string (*)(const std::string& vanillaAbsPath, int minLayer);

    // Installed once by the client; nullptr restores the file-system default.
    void SetOverlayHooks(ListFn list, LocateFn locate);

    // Every file with `extension` (".png", ".json"; nullptr = any) under the
    // vanilla directory and its overlays. One entry per relative path, the
    // highest layer's copy. Sorted by relative path.
    std::vector<Entry> ListFiles(const std::string& vanillaAbsDir, const char* extension, bool recursive);

    // The file to open for a vanilla path: a pack's copy when an enabled
    // pack has one, else the vanilla path itself (whether or not it exists).
    std::string Locate(const std::string& vanillaAbsPath);

    // Same, but only layers >= minLayer are consulted and "" is returned
    // when none of them has the file. MC looks a resource's .mcmeta up
    // from the top pack down to the pack that provided the resource, never
    // below it (FallbackResourceManager.createStackMetadataFinder) — pass
    // the resource's layer here for that rule.
    std::string LocateFromLayer(const std::string& vanillaAbsPath, int minLayer);

} // namespace Core::Assets
