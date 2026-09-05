// File: src/client/resource/ResourcePacks.hpp
//
// Minecraft resource packs, the client half of MC's pack system:
//
//   PackRepository        server/packs/repository/PackRepository.java
//   FolderRepositorySource  ../FolderRepositorySource.java (resourcepacks/,
//                         folders and .zip files, ids "file/<name>")
//   PackCompatibility     ../PackCompatibility.java
//   PackMetadataSection / PackFormat  server/packs/metadata/pack/*
//   Options.resourcePacks / incompatibleResourcePacks  client/Options.java
//   FallbackResourceManager  the lookup order: the selected list is lowest
//                         priority first (vanilla at index 0); a lookup
//                         walks it from the end, so the top pack wins.
//
// What differs from MC, and why:
//   • MC reads packs straight out of their zips. Every loader in this
//     engine opens files by path, so a selected .zip is extracted once into
//     resourcepacks/.extracted/<name>/ (re-extracted when the zip changes)
//     and read from there. A folder pack is read in place.
//   • MC's assets are namespaced (assets/minecraft/textures/...); this
//     engine's tree drops the namespace (assets/textures/...). The overlay
//     folds one onto the other, so a pack's assets/minecraft/X stands in
//     for the engine's assets/X. Other namespaces in a pack are reachable
//     through OptiFine-style resource locations only (see SkyRenderer).
//   • Reloading: textures, GUI, font, sky and clouds are rebuilt live
//     (PlatformMain::ReloadResources). Block models, blockstates, item
//     definitions and lang are read at startup only — the registries they
//     fill are shared with mesh workers and the server — so those parts of
//     a pack apply at the next launch. The pack screen says so.
#pragma once

#include <climits>
#include <string>
#include <vector>

namespace Resources {

    // PackFormat.java: major.minor, INT_MAX minor = "any minor" (76.*).
    struct PackFormat {
        int major = 0;
        int minor = 0;
        bool operator<(const PackFormat& o) const { return major != o.major ? major < o.major : minor < o.minor; }
        std::string ToString() const;
    };

    // SharedConstants.RESOURCE_PACK_FORMAT_MAJOR / _MINOR of the Minecraft
    // version this engine mirrors (26.1 snapshot 1, DataVersion 4764), and
    // PackFormat.lastPreMinorVersion(CLIENT_RESOURCES): the last major
    // version a pack may declare with the bare `pack_format` key.
    constexpr PackFormat kGamePackFormat{76, 0};
    constexpr int        kLastPreMinorVersion = 64;

    // PackCompatibility.java.
    enum class PackCompatibility { TooOld, TooNew, Unknown, Compatible };
    inline bool IsCompatible(PackCompatibility c) { return c == PackCompatibility::Compatible; }
    // pack.incompatible.<key> / pack.incompatible.confirm.<key> (en_us).
    const char* CompatibilityDescription(PackCompatibility c);
    const char* CompatibilityConfirmation(PackCompatibility c);
    PackCompatibility CompatibilityFor(PackFormat declaredMin, PackFormat declaredMax, PackFormat game = kGamePackFormat);

    // Pack.Position.
    enum class PackPosition { Top, Bottom };

    struct Pack {
        std::string id;            // "vanilla", or "file/<folder or zip name>"
        std::string title;         // "Default", or the file name (PackLocationInfo title)
        std::string description;   // pack.mcmeta "description", flattened to text
        std::string sourcePath;    // the folder or .zip in resourcepacks/ ("" for vanilla)
        bool        isZip = false;
        bool        builtIn = false;         // PackSource.BUILT_IN → "(built-in)" suffix
        bool        required = false;        // PackSelectionConfig.required (vanilla)
        bool        fixedPosition = false;   // PackSelectionConfig.fixedPosition
        PackPosition defaultPosition = PackPosition::Top;
        PackCompatibility compatibility = PackCompatibility::Compatible;
        PackFormat  minFormat, maxFormat;
        std::string iconPath;      // pack.png to show, "" = the default icon
        // OverlayMetadataSection: pack.mcmeta "overlays.entries", in file
        // order. Those whose range holds the game's format are layered
        // over the pack's base, later entries on top (CompositePackResources).
        struct Overlay { std::string directory; PackFormat minFormat, maxFormat; };
        std::vector<Overlay> overlays;
        // The pack carries models / blockstates / items / lang, which this
        // engine loads at startup only.
        bool        hasStartupOnlyContent = false;

        // PackSource.decorate: "<description> (built-in)" for built-ins.
        std::string ExtendedDescription() const;
    };

    // PackRepository.java, with FolderRepositorySource + the built-in
    // vanilla pack as its sources.
    class PackRepository {
    public:
        // reload(): rediscover what is available; the selection keeps the
        // ids that still exist and the required packs are re-inserted.
        void Reload();

        const std::vector<Pack>& Available() const { return m_available; }   // sorted by id
        const Pack* Get(const std::string& id) const;

        // Lowest priority first, vanilla normally at index 0.
        const std::vector<std::string>& SelectedIds() const { return m_selected; }
        std::vector<const Pack*> SelectedPacks() const;
        // setSelected(): unknown ids are dropped, required packs inserted at
        // their default position when missing.
        void SetSelected(const std::vector<std::string>& ids);

        // Pack.Position.insert, on a list of ids: the slot a pack with
        // `position` goes to when `reverse` (the screen's top-first lists)
        // or not (the repository's bottom-first list).
        static int InsertPosition(const std::vector<std::string>& list, const PackRepository& repo,
                                  PackPosition position, bool reverse);

    private:
        std::vector<std::string> RebuildSelected(const std::vector<std::string>& ids) const;

        std::vector<Pack>        m_available;
        std::vector<std::string> m_selected;
    };

    PackRepository& Repository();
    const std::string& PacksDirectory();

    // Startup, before any asset is read. `packsDir` is <gamedir>/resourcepacks
    // (created if missing); `vanillaAssetsRoot` is the engine's own assets
    // directory as PlatformMain resolves it. The two lists are options.txt's
    // resourcePacks / incompatibleResourcePacks; Options.loadSelectedResourcePacks
    // drops ids that no longer exist or are no longer compatible, and the
    // cleaned lists are what CurrentOptionLists() reports afterwards.
    void Initialize(const std::string& packsDir, const std::string& vanillaAssetsRoot,
                    const std::vector<std::string>& optionsSelected,
                    const std::vector<std::string>& optionsIncompatible);

    // Options.loadSelectedResourcePacks on an already-initialized module:
    // makes `selected` (with `incompatible` as the accepted-incompatible
    // list) the repository's selection, dropping ids that do not exist or
    // are not compatible and not accepted. The caller reloads resources.
    void SelectFromOptionLists(const std::vector<std::string>& selected,
                               const std::vector<std::string>& incompatible);

    // Options.updateResourcePacks: what options.txt should hold for the
    // repository's current selection.
    struct OptionLists {
        std::vector<std::string> selected;       // ids of the selected non-fixed packs, repository order
        std::vector<std::string> incompatible;   // those among them that are not compatible
    };
    OptionLists CurrentOptionLists();

    // Build the lookup layers from the repository's selection (extracting
    // zips that need it). True when the effective layers changed, which is
    // when the caller reloads resources. Main thread.
    bool ApplySelection();

    // Bumped every time ApplySelection changes the layers. Caches of loaded
    // textures compare against it: CacheStale() returns true once per
    // change for a given `seen` slot.
    int  Generation();
    bool CacheStale(int& seen);

    // The enabled pack that overrides an engine asset path
    // ("assets/textures/block/stone.png"), highest priority first. "" when
    // none does, or the path is not under assets/.
    std::string FindOverride(const std::string& enginePath);

    // Core::Assets hooks (see AssetLocator.hpp): a vanilla ABSOLUTE path
    // resolved through the layers >= minLayer (0 = the vanilla file itself).
    std::string LocateFromLayer(const std::string& vanillaAbsPath, int minLayer);
    struct OverlayEntry { std::string relative, absolute; int layer; };

    // Root directories of the enabled packs, highest priority first.
    std::vector<std::string> EnabledPackRoots();
    // Any enabled pack carries content this engine reads at startup only.
    bool AnyEnabledPackHasStartupOnlyContent();

    // options.txt round trip: `["vanilla","file/x.zip"]` (Options.readListOfStrings / GSON).
    std::vector<std::string> ParsePackList(const std::string& json);
    std::string SerializePackList(const std::vector<std::string>& ids);

} // namespace Resources
