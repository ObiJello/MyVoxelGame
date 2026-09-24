// File: src/client/shader/ShaderPacks.hpp
//
// Shader packs: discovery, selection and on-disk preparation.
//
// A shader pack is a folder or .zip in <game dir>/shaderpacks/ that holds a
// `shaders/` folder (OptiFine / Iris layout). Selection is one pack or none
// ("" = off), kept in the settings under `shaderPack`. A .zip is extracted
// once into shaderpacks/.extracted/<name>/ (re-extracted when the zip
// changes), the same arrangement ResourcePacks uses, so the pipeline can read
// files by path.
//
// What runs a pack is Render::ShaderPipeline; this module only knows about
// files.
#pragma once

#include <string>
#include <vector>

namespace Shaders {

    struct PackInfo {
        std::string id;          // "file/<folder or zip name>"
        std::string name;        // shown in the list: the folder or zip name
        std::string sourcePath;  // the folder or .zip in shaderpacks/
        bool        isZip = false;
    };

    // <game dir>/shaderpacks — created on first use.
    const std::string& PacksDirectory();

    // Every pack in the folder, sorted by name. A folder counts when it (or
    // one folder inside it) holds `shaders/` or the programs themselves; a
    // .zip is listed and inspected when selected.
    std::vector<PackInfo> Discover();

    // The selected pack id ("" = off), from the settings.
    std::string Selected();
    void        SetSelected(const std::string& id);   // "" = off; saves settings

    // Make a pack readable by path: for a folder, the folder itself; for a
    // .zip, the extraction. On success `shadersDirOut` is the pack's
    // SHADERS directory — the `shaders/` folder, or the pack folder itself
    // when the programs were unpacked without it (composite.fsh at the top
    // level, a common way to install). `error` says why otherwise.
    bool Prepare(const PackInfo& pack, std::string& shadersDirOut, std::string& error);

    // The pack for an id, if it is still there.
    bool Find(const std::string& id, PackInfo& out);

} // namespace Shaders
