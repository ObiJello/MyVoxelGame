// File: src/client/resource/ResourcePacks.cpp
#include "client/resource/ResourcePacks.hpp"

#include "common/core/AssetLocator.hpp"
#include "common/core/Log.hpp"

#include <nlohmann/json.hpp>
#include "unzip.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <system_error>
#include <unordered_map>

namespace fs = std::filesystem;

namespace Resources {

    // ═════════════════════════════ formats ═════════════════════════════

    std::string PackFormat::ToString() const {
        char buf[48];
        if (minor == INT_MAX) std::snprintf(buf, sizeof buf, "%d.*", major);
        else                  std::snprintf(buf, sizeof buf, "%d.%d", major, minor);
        return buf;
    }

    const char* CompatibilityDescription(PackCompatibility c) {
        switch (c) {
            case PackCompatibility::TooOld:  return "(Made for an older version of Minecraft)";
            case PackCompatibility::TooNew:  return "(Made for a newer version of Minecraft)";
            case PackCompatibility::Unknown: return "(Unknown pack format)";
            default:                         return "";
        }
    }

    const char* CompatibilityConfirmation(PackCompatibility c) {
        switch (c) {
            case PackCompatibility::TooOld:  return "This pack was made for an older version of Minecraft and may no longer work correctly.";
            case PackCompatibility::TooNew:  return "This pack was made for a newer version of Minecraft and may not work correctly.";
            case PackCompatibility::Unknown: return "This pack does not say which version of Minecraft it was made for and may not work correctly.";
            default:                         return "";
        }
    }

    // PackCompatibility.forVersion.
    PackCompatibility CompatibilityFor(PackFormat declaredMin, PackFormat declaredMax, PackFormat game) {
        if (declaredMin.major == INT_MAX) return PackCompatibility::Unknown;
        if (declaredMax < game) return PackCompatibility::TooOld;
        if (game < declaredMin) return PackCompatibility::TooNew;
        return PackCompatibility::Compatible;
    }

    std::string Pack::ExtendedDescription() const {
        // PackSource.decorateWithSource("pack.source.builtin") → "%s (built-in)".
        return builtIn ? description + " (built-in)" : description;
    }

    // ═════════════════════════════ pack.mcmeta ═════════════════════════

    namespace {

        // ComponentSerialization, the part a pack description uses: a plain
        // string, a text object ("text" + "extra"), or an array of those.
        std::string FlattenComponent(const nlohmann::json& j) {
            if (j.is_string()) return j.get<std::string>();
            if (j.is_number()) return j.dump();
            if (j.is_array()) {
                std::string out;
                for (const auto& e : j) out += FlattenComponent(e);
                return out;
            }
            if (j.is_object()) {
                std::string out;
                if (const auto t = j.find("text"); t != j.end() && t->is_string()) out += t->get<std::string>();
                else if (const auto t2 = j.find("translate"); t2 != j.end() && t2->is_string()) out += t2->get<std::string>();
                if (const auto ex = j.find("extra"); ex != j.end()) out += FlattenComponent(*ex);
                return out;
            }
            return {};
        }

        // Strip §-formatting codes; the pack screen draws plain text.
        std::string StripFormatting(const std::string& s) {
            std::string out;
            for (size_t i = 0; i < s.size(); ++i) {
                // "§" is 0xC2 0xA7 in UTF-8, followed by one code character.
                if (static_cast<unsigned char>(s[i]) == 0xC2 && i + 1 < s.size() &&
                    static_cast<unsigned char>(s[i + 1]) == 0xA7) {
                    i += 2;   // skip the code character too
                    continue;
                }
                out += s[i];
            }
            return out;
        }

        // PackFormat.BOTTOM_CODEC / TOP_CODEC: an int, or [major, minor];
        // the missing minor is 0 for a minimum and "any" for a maximum.
        bool ReadFormat(const nlohmann::json& j, int defaultMinor, PackFormat& out) {
            if (j.is_number_integer()) { out = {j.get<int>(), defaultMinor}; return out.major >= 0; }
            if (j.is_array() && !j.empty() && j[0].is_number_integer()) {
                out.major = j[0].get<int>();
                out.minor = (j.size() > 1 && j[1].is_number_integer()) ? j[1].get<int>() : defaultMinor;
                return out.major >= 0 && out.minor >= 0;
            }
            return false;
        }

        // InclusiveRange<Integer> codec: an int, [min, max], or
        // {"min_inclusive", "max_inclusive"}.
        bool ReadIntRange(const nlohmann::json& j, int& lo, int& hi) {
            if (j.is_number_integer()) { lo = hi = j.get<int>(); return true; }
            if (j.is_array() && j.size() == 2 && j[0].is_number_integer() && j[1].is_number_integer()) {
                lo = j[0].get<int>(); hi = j[1].get<int>(); return lo <= hi;
            }
            if (j.is_object()) {
                const auto a = j.find("min_inclusive"), b = j.find("max_inclusive");
                if (a != j.end() && b != j.end() && a->is_number_integer() && b->is_number_integer()) {
                    lo = a->get<int>(); hi = b->get<int>(); return lo <= hi;
                }
            }
            return false;
        }

        // PackMetadataSection for CLIENT_RESOURCES + PackFormat.IntermediaryFormat
        // .validate(lastPreMinorVersion=64, hasPackFormatField=true, ...):
        //   min_format + max_format   → [min, max] (both or neither)
        //   supported_formats         → [a.0, b.*]
        //   pack_format N (N <= 64)   → [N.0, N.*]; N > 64 must use min/max
        //   nothing                   → error
        // A pack whose "pack" section cannot be parsed is skipped, as MC's
        // Pack.readMetaAndCreate skips it ("Failed to read pack metadata").
        bool ParsePackSection(const nlohmann::json& pack, Pack& out, std::string& error) {
            if (!pack.is_object()) { error = "no \"pack\" object"; return false; }
            if (const auto d = pack.find("description"); d != pack.end()) {
                out.description = StripFormatting(FlattenComponent(*d));
            }
            const bool hasMin = pack.contains("min_format"), hasMax = pack.contains("max_format");
            if (hasMin != hasMax) { error = "must declare both min_format and max_format"; return false; }
            if (hasMin) {
                if (!ReadFormat(pack["min_format"], 0, out.minFormat) ||
                    !ReadFormat(pack["max_format"], INT_MAX, out.maxFormat)) {
                    error = "min_format / max_format are not version numbers";
                    return false;
                }
                if (out.maxFormat < out.minFormat) { error = "max_format below min_format"; return false; }
                return true;
            }
            if (const auto s = pack.find("supported_formats"); s != pack.end()) {
                int lo = 0, hi = 0;
                if (!ReadIntRange(*s, lo, hi)) { error = "supported_formats is not a range"; return false; }
                if (lo > kLastPreMinorVersion) {
                    error = "declares support for a version newer than " + std::to_string(kLastPreMinorVersion) +
                            " but has no min_format / max_format";
                    return false;
                }
                out.minFormat = {lo, 0};
                out.maxFormat = {hi, INT_MAX};
                return true;
            }
            if (const auto f = pack.find("pack_format"); f != pack.end() && f->is_number_integer()) {
                const int n = f->get<int>();
                if (n > kLastPreMinorVersion) {
                    error = "pack_format " + std::to_string(n) + " is newer than " + std::to_string(kLastPreMinorVersion) +
                            " and needs min_format / max_format";
                    return false;
                }
                out.minFormat = {n, 0};
                out.maxFormat = {n, INT_MAX};
                return true;
            }
            error = "missing format version information";
            return false;
        }

        // OverlayMetadataSection + PackFormat.IntermediaryFormat.OVERLAY_CODEC:
        // each entry names a directory and a range, as min_format/max_format
        // or the older "formats" [a, b] (= [a.0, b.*]). An entry that does
        // not parse is skipped with a warning (MC: "Unknown or broken
        // overlay entry").
        void ParseOverlays(const nlohmann::json& j, Pack& out) {
            const auto o = j.find("overlays");
            if (o == j.end() || !o->is_object()) return;
            const auto entries = o->find("entries");
            if (entries == o->end() || !entries->is_array()) return;
            for (const auto& e : *entries) {
                if (!e.is_object()) continue;
                Pack::Overlay ov;
                const auto dir = e.find("directory");
                if (dir == e.end() || !dir->is_string()) continue;
                ov.directory = dir->get<std::string>();
                if (ov.directory.empty() || ov.directory.find("..") != std::string::npos ||
                    ov.directory.find('/') != std::string::npos || ov.directory.find('\\') != std::string::npos) {
                    Log::Warning("[ResourcePacks] %s: overlay directory '%s' ignored", out.id.c_str(), ov.directory.c_str());
                    continue;
                }
                const bool hasMin = e.contains("min_format"), hasMax = e.contains("max_format");
                if (hasMin && hasMax) {
                    if (!ReadFormat(e["min_format"], 0, ov.minFormat) || !ReadFormat(e["max_format"], INT_MAX, ov.maxFormat)) {
                        Log::Warning("[ResourcePacks] %s: unknown or broken overlay entry %s", out.id.c_str(), ov.directory.c_str());
                        continue;
                    }
                } else if (const auto f = e.find("formats"); f != e.end()) {
                    int lo = 0, hi = 0;
                    if (!ReadIntRange(*f, lo, hi)) {
                        Log::Warning("[ResourcePacks] %s: unknown or broken overlay entry %s", out.id.c_str(), ov.directory.c_str());
                        continue;
                    }
                    ov.minFormat = {lo, 0};
                    ov.maxFormat = {hi, INT_MAX};
                } else {
                    Log::Warning("[ResourcePacks] %s: unknown or broken overlay entry %s", out.id.c_str(), ov.directory.c_str());
                    continue;
                }
                out.overlays.push_back(std::move(ov));
            }
        }

        bool ParseMcmeta(const std::string& text, Pack& out, std::string& error) {
            const nlohmann::json j = nlohmann::json::parse(text, nullptr, false, true);
            if (j.is_discarded() || !j.is_object()) { error = "pack.mcmeta is not valid JSON"; return false; }
            const auto p = j.find("pack");
            if (p == j.end()) { error = "pack.mcmeta has no \"pack\" section"; return false; }
            if (!ParsePackSection(*p, out, error)) return false;
            out.compatibility = CompatibilityFor(out.minFormat, out.maxFormat);
            ParseOverlays(j, out);
            return true;
        }

        bool ReadWholeFile(const fs::path& p, std::string& out) {
            std::ifstream f(p, std::ios::binary);
            if (!f) return false;
            out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            return true;
        }

        // ── zip access (minizip) ────────────────────────────────────────

        struct ZipScan {
            bool        ok = false;
            std::string prefix;              // "" or "<folder>/" when the pack sits one folder down
            bool        hasMcmeta = false;
            bool        hasIcon = false;
            bool        hasStartupOnlyContent = false;
            std::string mcmeta;
        };

        bool ReadZipEntry(unzFile zf, std::string& out) {
            if (unzOpenCurrentFile(zf) != UNZ_OK) return false;
            out.clear();
            char buf[64 * 1024];
            for (;;) {
                const int n = unzReadCurrentFile(zf, buf, sizeof buf);
                if (n < 0) { unzCloseCurrentFile(zf); return false; }
                if (n == 0) break;
                out.append(buf, static_cast<size_t>(n));
            }
            unzCloseCurrentFile(zf);
            return true;
        }

        bool SafeRelativeEntry(const std::string& name) {
            if (name.empty() || name[0] == '/' || name[0] == '\\') return false;
            if (name.find("..") != std::string::npos) return false;
            if (name.size() > 1 && name[1] == ':') return false;   // drive letter
            return true;
        }

        // One pass over the zip's directory: where pack.mcmeta is (root, or
        // inside a single top-level folder — the shape a zipped folder has,
        // which MC rejects but people make constantly), and what else the
        // pack carries.
        ZipScan ScanZip(const fs::path& zip) {
            ZipScan scan;
            unzFile zf = unzOpen(zip.string().c_str());
            if (!zf) return scan;
            std::vector<std::string> names;
            if (unzGoToFirstFile(zf) == UNZ_OK) {
                do {
                    char name[1024];
                    unz_file_info info{};
                    if (unzGetCurrentFileInfo(zf, &info, name, sizeof name, nullptr, 0, nullptr, 0) != UNZ_OK) continue;
                    names.emplace_back(name);
                } while (unzGoToNextFile(zf) == UNZ_OK);
            }
            scan.ok = true;
            auto has = [&](const std::string& n) { return std::find(names.begin(), names.end(), n) != names.end(); };
            if (has("pack.mcmeta")) {
                scan.prefix.clear();
            } else {
                // Exactly one top-level folder holding pack.mcmeta.
                std::string found;
                int candidates = 0;
                for (const std::string& n : names) {
                    const size_t slash = n.find('/');
                    if (slash == std::string::npos || slash == 0) continue;
                    if (n.compare(slash + 1, std::string::npos, "pack.mcmeta") == 0) {
                        found = n.substr(0, slash + 1);
                        ++candidates;
                    }
                }
                if (candidates == 1) scan.prefix = found;
                else { unzClose(zf); return scan; }   // not a pack
            }
            scan.hasMcmeta = true;
            scan.hasIcon = has(scan.prefix + "pack.png");
            for (const std::string& n : names) {
                if (n.rfind(scan.prefix + "assets/minecraft/models/", 0) == 0 ||
                    n.rfind(scan.prefix + "assets/minecraft/blockstates/", 0) == 0 ||
                    n.rfind(scan.prefix + "assets/minecraft/items/", 0) == 0 ||
                    n.rfind(scan.prefix + "assets/minecraft/lang/", 0) == 0) {
                    scan.hasStartupOnlyContent = true;
                    break;
                }
            }
            if (unzLocateFile(zf, (scan.prefix + "pack.mcmeta").c_str(), 1) == UNZ_OK) ReadZipEntry(zf, scan.mcmeta);
            unzClose(zf);
            return scan;
        }

        bool ExtractZipEntryTo(const fs::path& zip, const std::string& entry, const fs::path& dest) {
            unzFile zf = unzOpen(zip.string().c_str());
            if (!zf) return false;
            bool ok = false;
            std::string bytes;
            if (unzLocateFile(zf, entry.c_str(), 1) == UNZ_OK && ReadZipEntry(zf, bytes)) {
                std::error_code ec;
                fs::create_directories(dest.parent_path(), ec);
                std::ofstream out(dest, std::ios::binary);
                ok = static_cast<bool>(out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())));
            }
            unzClose(zf);
            return ok;
        }

        bool ExtractZip(const fs::path& zip, const fs::path& destDir) {
            unzFile zf = unzOpen(zip.string().c_str());
            if (!zf) return false;
            std::error_code ec;
            fs::remove_all(destDir, ec);
            fs::create_directories(destDir, ec);
            bool ok = true;
            size_t files = 0;
            if (unzGoToFirstFile(zf) == UNZ_OK) {
                do {
                    char name[1024];
                    unz_file_info info{};
                    if (unzGetCurrentFileInfo(zf, &info, name, sizeof name, nullptr, 0, nullptr, 0) != UNZ_OK) { ok = false; break; }
                    const std::string entry(name);
                    if (entry.empty() || entry.back() == '/') continue;   // directory entry
                    if (!SafeRelativeEntry(entry)) { Log::Warning("[ResourcePacks] skipping unsafe zip entry %s", entry.c_str()); continue; }
                    // Only what a resource pack can use: keeps a 300 MB pack
                    // with stray videos from being copied.
                    if (entry.find("assets/") == std::string::npos && entry.find("pack.") == std::string::npos) continue;
                    std::string bytes;
                    if (!ReadZipEntry(zf, bytes)) { ok = false; break; }
                    const fs::path dest = destDir / entry;
                    fs::create_directories(dest.parent_path(), ec);
                    std::ofstream out(dest, std::ios::binary);
                    if (!out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) { ok = false; break; }
                    ++files;
                } while (unzGoToNextFile(zf) == UNZ_OK);
            }
            unzClose(zf);
            Log::Info("[ResourcePacks] extracted %zu file(s) from %s", files, zip.filename().string().c_str());
            return ok;
        }

        // "<size>:<mtime>" of a zip, the extraction's freshness key.
        std::string ZipStamp(const fs::path& zip) {
            std::error_code ec;
            const auto size = fs::file_size(zip, ec);
            const auto time = fs::last_write_time(zip, ec).time_since_epoch().count();
            return std::to_string(size) + ":" + std::to_string(static_cast<long long>(time));
        }

        // ── module state ────────────────────────────────────────────────

        struct Layer {
            std::string id;
            std::string root;   // directory holding assets/
        };

        PackRepository           s_repository;
        std::string              s_packsDir;
        std::string              s_vanillaRoot;        // engine assets/ (absolute or CWD-relative, as resolved)
        std::vector<Layer>       s_layers;             // highest priority first
        int                      s_generation = 0;
        bool                     s_initialized = false;

        std::string ExtractedDir()  { return s_packsDir + "/.extracted"; }
        std::string IconCacheDir()  { return s_packsDir + "/.extracted/.icons"; }

        // The pack-relative path for an engine asset path: the namespace
        // fold described in the header. "" when the path is not an asset.
        std::string PackRelative(const std::string& enginePath) {
            std::string p = enginePath;
            std::replace(p.begin(), p.end(), '\\', '/');
            if (p.rfind("./", 0) == 0) p.erase(0, 2);
            if (p.rfind("assets/", 0) != 0) return {};
            const std::string rest = p.substr(7);
            if (rest.rfind("minecraft/", 0) == 0) return p;
            return "assets/minecraft/" + rest;
        }

        // Engine-relative "assets/..." for a vanilla path as PlatformMain
        // resolves it (absolute in a bundle, CWD-relative in the tree).
        std::string EngineRelative(const std::string& vanillaAbs) {
            std::string p = vanillaAbs;
            std::replace(p.begin(), p.end(), '\\', '/');
            std::string root = s_vanillaRoot;
            std::replace(root.begin(), root.end(), '\\', '/');
            while (!root.empty() && root.back() == '/') root.pop_back();
            if (!root.empty() && p.rfind(root, 0) == 0 && (p.size() == root.size() || p[root.size()] == '/')) {
                return "assets" + p.substr(root.size());
            }
            if (p.rfind("assets/", 0) == 0) return p;
            const size_t at = p.find("/assets/");
            if (at != std::string::npos) return p.substr(at + 1);
            return {};
        }

        // Layer number of s_layers[i]: 1 = lowest pack, N = highest.
        int LayerNumber(size_t i) { return static_cast<int>(s_layers.size() - i); }

        std::vector<Core::Assets::Entry> ListHook(const std::string& vanillaAbsDir, const char* extension, bool recursive) {
            std::map<std::string, Core::Assets::Entry> merged;
            std::error_code ec;
            auto scan = [&](const fs::path& root, int layer) {
                if (!fs::is_directory(root, ec)) return;
                auto take = [&](const fs::directory_entry& e) {
                    if (!e.is_regular_file(ec)) return;
                    if (extension && e.path().extension() != extension) return;
                    std::string rel = fs::relative(e.path(), root, ec).generic_string();
                    merged[rel] = Core::Assets::Entry{rel, e.path().string(), layer};
                };
                if (recursive) { for (const auto& e : fs::recursive_directory_iterator(root, ec)) take(e); }
                else           { for (const auto& e : fs::directory_iterator(root, ec)) take(e); }
            };
            scan(vanillaAbsDir, 0);
            const std::string rel = PackRelative(EngineRelative(vanillaAbsDir));
            if (!rel.empty()) {
                // Lowest pack first so a higher one overwrites.
                for (size_t i = s_layers.size(); i-- > 0;) {
                    scan(fs::path(s_layers[i].root) / rel, LayerNumber(i));
                }
            }
            std::vector<Core::Assets::Entry> out;
            out.reserve(merged.size());
            for (auto& [k, v] : merged) out.push_back(std::move(v));
            return out;
        }

        std::string LocateHook(const std::string& vanillaAbsPath, int minLayer) {
            return LocateFromLayer(vanillaAbsPath, minLayer);
        }

    } // namespace

    // ═════════════════════════════ repository ══════════════════════════

    const Pack* PackRepository::Get(const std::string& id) const {
        for (const Pack& p : m_available) if (p.id == id) return &p;
        return nullptr;
    }

    std::vector<const Pack*> PackRepository::SelectedPacks() const {
        std::vector<const Pack*> out;
        for (const std::string& id : m_selected) if (const Pack* p = Get(id)) out.push_back(p);
        return out;
    }

    int PackRepository::InsertPosition(const std::vector<std::string>& list, const PackRepository& repo,
                                       PackPosition position, bool reverse) {
        const PackPosition self = reverse ? (position == PackPosition::Top ? PackPosition::Bottom : PackPosition::Top)
                                          : position;
        auto fixedAt = [&](const std::string& id) {
            const Pack* p = repo.Get(id);
            return p && p->fixedPosition && p->defaultPosition == position;
        };
        if (self == PackPosition::Bottom) {
            int index = 0;
            for (; index < static_cast<int>(list.size()); ++index) if (!fixedAt(list[index])) break;
            return index;
        }
        int index = static_cast<int>(list.size()) - 1;
        for (; index >= 0; --index) if (!fixedAt(list[index])) break;
        return index + 1;
    }

    std::vector<std::string> PackRepository::RebuildSelected(const std::vector<std::string>& ids) const {
        std::vector<std::string> out;
        for (const std::string& id : ids) {
            if (Get(id) && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
        }
        for (const Pack& p : m_available) {
            if (p.required && std::find(out.begin(), out.end(), p.id) == out.end()) {
                out.insert(out.begin() + InsertPosition(out, *this, p.defaultPosition, false), p.id);
            }
        }
        return out;
    }

    void PackRepository::SetSelected(const std::vector<std::string>& ids) {
        m_selected = RebuildSelected(ids);
    }

    void PackRepository::Reload() {
        const std::vector<std::string> previous = m_selected;
        std::map<std::string, Pack> discovered;   // TreeMap: sorted by id

        // ClientPackSource: the vanilla pack. Required, default position
        // BOTTOM, not fixed — it can be moved above a pack, never removed.
        {
            Pack vanilla;
            vanilla.id = "vanilla";
            vanilla.title = "Default";
            vanilla.description = "The default look and feel of Minecraft";
            vanilla.builtIn = true;
            vanilla.required = true;
            vanilla.defaultPosition = PackPosition::Bottom;
            vanilla.minFormat = vanilla.maxFormat = kGamePackFormat;
            discovered[vanilla.id] = vanilla;
        }

        // FolderRepositorySource.discoverPacks over resourcepacks/.
        std::error_code ec;
        fs::create_directories(s_packsDir, ec);
        fs::create_directories(IconCacheDir(), ec);
        for (const auto& entry : fs::directory_iterator(s_packsDir, ec)) {
            const std::string name = entry.path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            Pack pack;
            pack.id = "file/" + name;
            pack.title = name;
            pack.sourcePath = entry.path().string();
            std::string mcmeta, error;
            if (entry.is_directory(ec)) {
                if (!fs::is_regular_file(entry.path() / "pack.mcmeta", ec)) {
                    Log::Info("[ResourcePacks] Found non-pack entry '%s', ignoring", name.c_str());
                    continue;
                }
                ReadWholeFile(entry.path() / "pack.mcmeta", mcmeta);
                if (fs::is_regular_file(entry.path() / "pack.png", ec)) pack.iconPath = (entry.path() / "pack.png").string();
                const fs::path mc = entry.path() / "assets" / "minecraft";
                pack.hasStartupOnlyContent = fs::is_directory(mc / "models", ec) || fs::is_directory(mc / "blockstates", ec) ||
                                             fs::is_directory(mc / "items", ec)  || fs::is_directory(mc / "lang", ec);
            } else if (entry.is_regular_file(ec) && entry.path().extension() == ".zip") {
                const ZipScan scan = ScanZip(entry.path());
                if (!scan.ok || !scan.hasMcmeta) {
                    Log::Info("[ResourcePacks] Found non-pack entry '%s', ignoring", name.c_str());
                    continue;
                }
                pack.isZip = true;
                mcmeta = scan.mcmeta;
                pack.hasStartupOnlyContent = scan.hasStartupOnlyContent;
                if (scan.hasIcon) {
                    // The icon is small: pull it out now so the list can show
                    // it without extracting the whole pack.
                    const fs::path icon = fs::path(IconCacheDir()) / (name + ".png");
                    const fs::path stamp = fs::path(IconCacheDir()) / (name + ".stamp");
                    std::string old;
                    ReadWholeFile(stamp, old);
                    if (old != ZipStamp(entry.path()) || !fs::exists(icon, ec)) {
                        if (ExtractZipEntryTo(entry.path(), scan.prefix + "pack.png", icon)) {
                            std::ofstream(stamp) << ZipStamp(entry.path());
                        }
                    }
                    if (fs::exists(icon, ec)) pack.iconPath = icon.string();
                }
            } else {
                continue;
            }
            if (!ParseMcmeta(mcmeta, pack, error)) {
                Log::Warning("[ResourcePacks] Failed to read pack %s metadata: %s", pack.id.c_str(), error.c_str());
                continue;
            }
            discovered[pack.id] = pack;
        }

        m_available.clear();
        for (auto& [id, p] : discovered) m_available.push_back(std::move(p));
        m_selected = RebuildSelected(previous);
    }

    PackRepository& Repository() { return s_repository; }
    const std::string& PacksDirectory() { return s_packsDir; }

    // ═════════════════════════════ layers ══════════════════════════════

    bool ApplySelection() {
        std::vector<Layer> layers;   // built lowest → highest, then reversed
        std::error_code ec;
        for (const Pack* p : s_repository.SelectedPacks()) {
            if (p->id == "vanilla") continue;
            std::string root = p->sourcePath;
            if (p->isZip) {
                const fs::path zip(p->sourcePath);
                const fs::path dir = fs::path(ExtractedDir()) / zip.filename().string();
                const fs::path stamp = dir / ".stamp";
                std::string old;
                ReadWholeFile(stamp, old);
                const std::string now = ZipStamp(zip);
                if (old != now) {
                    Log::Info("[ResourcePacks] extracting %s ...", zip.filename().string().c_str());
                    if (!ExtractZip(zip, dir)) {
                        Log::Warning("[ResourcePacks] could not extract %s; pack skipped", zip.filename().string().c_str());
                        continue;
                    }
                    std::ofstream(stamp) << now;
                }
                root = dir.string();
                if (!fs::is_regular_file(dir / "pack.mcmeta", ec)) {
                    // Zipped folder: the pack is one level down (see ScanZip).
                    for (const auto& sub : fs::directory_iterator(dir, ec)) {
                        if (sub.is_directory(ec) && fs::is_regular_file(sub.path() / "pack.mcmeta", ec)) { root = sub.path().string(); break; }
                    }
                }
            }
            layers.push_back(Layer{p->id, root});
            // OverlayMetadataSection.overlaysForVersion: the overlays whose
            // range holds the game's format, over the base, later on top.
            for (const Pack::Overlay& ov : p->overlays) {
                if (kGamePackFormat < ov.minFormat || ov.maxFormat < kGamePackFormat) continue;
                const fs::path dir = fs::path(root) / ov.directory;
                if (!fs::is_directory(dir, ec)) continue;
                layers.push_back(Layer{p->id + "/" + ov.directory, dir.string()});
            }
        }
        std::reverse(layers.begin(), layers.end());

        bool same = layers.size() == s_layers.size();
        for (size_t i = 0; same && i < layers.size(); ++i) {
            same = layers[i].id == s_layers[i].id && layers[i].root == s_layers[i].root;
        }
        if (same) return false;
        s_layers = std::move(layers);
        ++s_generation;
        std::string list;
        for (const Layer& l : s_layers) { if (!list.empty()) list += ", "; list += l.id; }
        Log::Info("[ResourcePacks] enabled (top first): %s", list.empty() ? "(none)" : list.c_str());
        return true;
    }

    int  Generation() { return s_generation; }
    bool CacheStale(int& seen) {
        if (seen == s_generation) return false;
        seen = s_generation;
        return true;
    }

    std::string FindOverride(const std::string& enginePath) {
        if (s_layers.empty()) return {};
        const std::string rel = PackRelative(enginePath);
        if (rel.empty()) return {};
        std::error_code ec;
        for (const Layer& l : s_layers) {
            fs::path p = fs::path(l.root) / rel;
            if (fs::is_regular_file(p, ec)) return p.string();
        }
        return {};
    }

    std::string LocateFromLayer(const std::string& vanillaAbsPath, int minLayer) {
        std::error_code ec;
        const std::string rel = PackRelative(EngineRelative(vanillaAbsPath));
        if (!rel.empty()) {
            for (size_t i = 0; i < s_layers.size(); ++i) {
                if (LayerNumber(i) < minLayer) break;
                fs::path p = fs::path(s_layers[i].root) / rel;
                if (fs::is_regular_file(p, ec)) return p.string();
            }
        }
        if (minLayer <= 0 && fs::exists(vanillaAbsPath, ec)) return vanillaAbsPath;
        return {};
    }

    std::vector<std::string> EnabledPackRoots() {
        std::vector<std::string> out;
        for (const Layer& l : s_layers) out.push_back(l.root + "/");
        return out;
    }

    bool AnyEnabledPackHasStartupOnlyContent() {
        for (const Layer& l : s_layers) {
            const std::string packId = l.id.substr(0, l.id.find('/', 5));   // "file/<name>" or "file/<name>/<overlay>"
            if (const Pack* p = s_repository.Get(packId); p && p->hasStartupOnlyContent) return true;
        }
        return false;
    }

    // ═════════════════════════════ options ═════════════════════════════

    std::vector<std::string> ParsePackList(const std::string& json) {
        std::vector<std::string> out;
        const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
        if (j.is_array()) for (const auto& e : j) if (e.is_string()) out.push_back(e.get<std::string>());
        return out;
    }

    std::string SerializePackList(const std::vector<std::string>& ids) {
        return nlohmann::json(ids).dump();
    }

    OptionLists CurrentOptionLists() {
        OptionLists lists;
        for (const Pack* p : s_repository.SelectedPacks()) {
            if (p->fixedPosition) continue;
            lists.selected.push_back(p->id);
            if (!IsCompatible(p->compatibility)) lists.incompatible.push_back(p->id);
        }
        return lists;
    }

    void SelectFromOptionLists(const std::vector<std::string>& optionsSelected,
                               const std::vector<std::string>& optionsIncompatible) {
        // Options.loadSelectedResourcePacks.
        std::vector<std::string> selected;
        for (const std::string& raw : optionsSelected) {
            std::string id = raw;
            const Pack* pack = s_repository.Get(id);
            if (!pack && id.rfind("file/", 0) != 0) { id = "file/" + id; pack = s_repository.Get(id); }
            if (!pack) {
                Log::Warning("[ResourcePacks] Removed resource pack %s from options because it doesn't seem to exist anymore", raw.c_str());
                continue;
            }
            const bool accepted = std::find(optionsIncompatible.begin(), optionsIncompatible.end(), raw) != optionsIncompatible.end() ||
                                  std::find(optionsIncompatible.begin(), optionsIncompatible.end(), id) != optionsIncompatible.end();
            if (!IsCompatible(pack->compatibility) && !accepted) {
                Log::Warning("[ResourcePacks] Removed resource pack %s from options because it is no longer compatible", raw.c_str());
                continue;
            }
            selected.push_back(pack->id);
        }
        s_repository.SetSelected(selected);
    }

    void Initialize(const std::string& packsDir, const std::string& vanillaAssetsRoot,
                    const std::vector<std::string>& optionsSelected,
                    const std::vector<std::string>& optionsIncompatible) {
        s_packsDir = packsDir;
        s_vanillaRoot = vanillaAssetsRoot;
        s_repository.Reload();
        SelectFromOptionLists(optionsSelected, optionsIncompatible);
        s_initialized = true;
        ApplySelection();
        Core::Assets::SetOverlayHooks(&ListHook, &LocateHook);
        Log::Info("[ResourcePacks] %zu pack(s) available in %s", s_repository.Available().size(), s_packsDir.c_str());
    }

} // namespace Resources
