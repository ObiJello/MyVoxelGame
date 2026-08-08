#include "levelgen/structure/FossilTemplate.h"

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <unordered_map>

namespace minecraft {
namespace levelgen {
namespace structure {

namespace fs = std::filesystem;

namespace {

std::string templatePath(const std::string& id) {
    // "minecraft:fossil/spine_1" -> <data>/minecraft/structure/fossil/spine_1.nbt
    size_t colon = id.find(':');
    std::string ns = colon == std::string::npos ? "minecraft" : id.substr(0, colon);
    std::string path = colon == std::string::npos ? id : id.substr(colon + 1);

    fs::path root;
    if (const char* env = std::getenv("MC_DATA_ROOT")) {
        root = fs::path(env);
    } else {
        fs::path current = fs::current_path();
        for (;;) {
            if (fs::exists(current / "data" / "minecraft")) { root = current / "data"; break; }
            if (current == current.root_path()) break;
            current = current.parent_path();
        }
    }
    if (root.empty()) {
        throw std::runtime_error("FossilTemplate: no data/ root found for " + id);
    }
    return (root / ns / "structure" / (path + ".nbt")).string();
}

}  // namespace

const FossilTemplate& getFossilTemplate(const std::string& id) {
    static std::mutex mutex;
    static std::unordered_map<std::string, std::unique_ptr<FossilTemplate>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(id);
    if (it != cache.end()) return *it->second;

    auto tpl = std::make_unique<FossilTemplate>();
    nbt_detail::Reader reader(nbt_detail::gunzipFile(templatePath(id)));
    int rootTag = reader.u8();
    if (rootTag != 10) throw std::runtime_error("FossilTemplate: bad root tag in " + id);
    reader.str();  // root name
    nbt_detail::Parser parser{reader, *tpl};
    parser.compound("");
    if (tpl->palette.empty() || tpl->blocks.empty()) {
        throw std::runtime_error("FossilTemplate: empty template " + id);
    }
    auto& ref = *tpl;
    cache.emplace(id, std::move(tpl));
    return ref;
}

}  // namespace structure
}  // namespace levelgen
}  // namespace minecraft
