// File: src/server/commands/CommandStorage.cpp
#include "CommandStorage.hpp"
#include "NbtPath.hpp"
#include "../IntegratedServer.hpp"
#include "../level/ServerLevel.hpp"
#include "../world/storage/anvil/SaveRoot.hpp"
#include "../world/storage/anvil/SpawnerNbt.hpp"   // WriteNbtTree

#include "common/core/Log.hpp"
#include "common/core/SaveVersion.hpp"
#include "common/nbt/NbtWrite.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

namespace Server::CommandStorage {

    namespace {

        namespace fs = std::filesystem;

        struct Namespace {
            std::map<std::string, std::shared_ptr<::World::NBTTagCompound>> contents;   // by path
            bool loaded = false;
            bool dirty = false;
        };

        std::map<std::string, Namespace>& Store() {
            static std::map<std::string, Namespace> store;
            return store;
        }

        fs::path DataDir() {
            if (!g_integratedServer) return {};
            ServerLevel* overworld = g_integratedServer->GetLevel(Game::DimensionId::Overworld);
            if (!overworld || overworld->Config().savePath.empty()) return {};
            std::string reason;
            const auto root = Game::Anvil::SaveRoot::Open(overworld->Config().savePath, reason);
            return root ? root->DataDir(Game::DimensionId::Overworld) : fs::path{};
        }

        bool ReadOnlyWorld() {
            if (!g_integratedServer) return true;
            ServerLevel* overworld = g_integratedServer->GetLevel(Game::DimensionId::Overworld);
            return !overworld || overworld->Config().readOnly;
        }

        fs::path FileFor(const std::string& ns) {
            const fs::path dir = DataDir();
            return dir.empty() ? fs::path{} : dir / ("command_storage_" + ns + ".dat");
        }

        Namespace& Load(const std::string& ns) {
            Namespace& space = Store()[ns];
            if (space.loaded) return space;
            space.loaded = true;
            const fs::path file = FileFor(ns);
            if (file.empty()) return space;
            std::ifstream f(file, std::ios::binary);
            if (!f) return space;
            const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::vector<uint8_t> nbt;
            if (raw.empty() || !Game::Nbt::GzipDecompress(raw, nbt)) return space;
            std::shared_ptr<::World::NBTTagCompound> root;
            try {
                root = std::dynamic_pointer_cast<::World::NBTTagCompound>(::World::NBTParser::Parse(nbt));
            } catch (const std::exception& e) {
                Log::Warning("[CommandStorage] unreadable %s: %s", file.string().c_str(), e.what());
                return space;
            }
            auto data = root ? std::dynamic_pointer_cast<::World::NBTTagCompound>(root->GetTag("data")) : nullptr;
            auto contents = data ? std::dynamic_pointer_cast<::World::NBTTagCompound>(data->GetTag("contents")) : nullptr;
            if (!contents) return space;
            for (const auto& [path, value] : contents->value) {
                if (auto c = std::dynamic_pointer_cast<::World::NBTTagCompound>(value)) space.contents[path] = c;
            }
            return space;
        }

        bool SaveNamespace(const std::string& ns, const Namespace& space) {
            const fs::path file = FileFor(ns);
            if (file.empty()) return false;
            Game::Nbt::Writer w;
            w.BeginRootCompound();
            w.BeginCompound("data");
            w.BeginCompound("contents");
            for (const auto& [path, value] : space.contents) {
                if (value) Game::Anvil::WriteNbtTree(w, path, *value);
            }
            w.EndCompound();
            w.EndCompound();
            w.Int("DataVersion", Game::Save::DataVersion());
            w.EndRootCompound();
            if (!w.ok()) return false;
            std::vector<uint8_t> gz;
            if (!Game::Nbt::GzipCompress(w.Bytes(), gz)) return false;
            std::error_code ec;
            fs::create_directories(file.parent_path(), ec);
            const fs::path tmp = file.string() + ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f) return false;
                f.write(reinterpret_cast<const char*>(gz.data()), static_cast<std::streamsize>(gz.size()));
                if (!f) return false;
            }
            fs::rename(tmp, file, ec);
            return !ec;
        }

        bool ValidChars(const std::string& s, bool path) {
            if (s.empty()) return false;
            for (char c : s) {
                const bool ok = std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) ||
                                c == '_' || c == '-' || c == '.' || (path && c == '/');
                if (!ok) return false;
            }
            return true;
        }

    } // namespace

    bool NormalizeId(const std::string& text, std::string& out) {
        const size_t colon = text.find(':');
        const std::string ns = colon == std::string::npos ? "minecraft" : text.substr(0, colon);
        const std::string path = colon == std::string::npos ? text : text.substr(colon + 1);
        if (!ValidChars(ns, false) || !ValidChars(path, true)) return false;
        out = ns + ":" + path;
        return true;
    }

    std::shared_ptr<::World::NBTTagCompound> Get(const std::string& id) {
        const size_t colon = id.find(':');
        Namespace& space = Load(id.substr(0, colon));
        auto it = space.contents.find(id.substr(colon + 1));
        if (it == space.contents.end() || !it->second) return std::make_shared<::World::NBTTagCompound>();
        return Nbt::CopyCompound(*it->second);
    }

    void Set(const std::string& id, const ::World::NBTTagCompound& data) {
        const size_t colon = id.find(':');
        Namespace& space = Load(id.substr(0, colon));
        const std::string path = id.substr(colon + 1);
        if (data.value.empty()) space.contents.erase(path);
        else space.contents[path] = Nbt::CopyCompound(data);
        space.dirty = true;
    }

    std::vector<std::string> Ids() {
        std::vector<std::string> out;
        // Every namespace on disk too, so completion offers what a past
        // session stored.
        std::error_code ec;
        const fs::path dir = DataDir();
        if (!dir.empty()) {
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                const std::string name = entry.path().filename().string();
                const std::string prefix = "command_storage_";
                if (name.rfind(prefix, 0) == 0 && name.size() > prefix.size() + 4 &&
                    name.compare(name.size() - 4, 4, ".dat") == 0) {
                    Load(name.substr(prefix.size(), name.size() - prefix.size() - 4));
                }
            }
        }
        for (const auto& [ns, space] : Store()) {
            for (const auto& [path, value] : space.contents) out.push_back(ns + ":" + path);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    void Save() {
        if (ReadOnlyWorld()) return;
        for (auto& [ns, space] : Store()) {
            if (!space.dirty) continue;
            if (SaveNamespace(ns, space)) space.dirty = false;
            else Log::Warning("[CommandStorage] could not save command_storage_%s.dat", ns.c_str());
        }
    }

    void Close() {
        Save();
        Store().clear();
    }

} // namespace Server::CommandStorage
