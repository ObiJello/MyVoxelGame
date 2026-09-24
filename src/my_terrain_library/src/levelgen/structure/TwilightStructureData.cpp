#include "levelgen/structure/TwilightStructureData.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace minecraft {
namespace levelgen {
namespace structure {
namespace twilight_data {

namespace {

namespace fs = std::filesystem;

// Same discovery contract as StructureSets.cpp / TemplateEngine.cpp.
fs::path dataRoot() {
    if (const char* env = std::getenv("MC_DATA_ROOT")) {
        return fs::path(env);
    }
    fs::path current = fs::current_path();
    while (!current.empty()) {
        fs::path candidate = current / "data";
        if (fs::exists(candidate) && fs::is_directory(candidate)) return candidate;
        if (current == current.root_path()) break;
        current = current.parent_path();
    }
    throw std::runtime_error("Data root not found (Twilight Forest structure data)");
}

std::mutex s_mutex;
// unique_ptr values: references handed out stay valid as the map grows.
std::map<std::string, std::unique_ptr<nlohmann::json>> s_cache;

} // namespace

const nlohmann::json& file(const std::string& relativePath) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_cache.find(relativePath);
    if (it != s_cache.end()) return *it->second;

    const fs::path path = dataRoot() / "twilightforest" / relativePath;
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Twilight Forest data file missing: " + path.string());
    }
    auto parsed = std::make_unique<nlohmann::json>();
    try {
        input >> *parsed;
    } catch (const std::exception& e) {
        throw std::runtime_error("Twilight Forest data file malformed: " + path.string() + " (" + e.what() + ")");
    }
    const nlohmann::json& ref = *parsed;
    s_cache.emplace(relativePath, std::move(parsed));
    return ref;
}

const nlohmann::json& structure(const std::string& structureName) {
    std::string path = structureName;
    const size_t colon = path.find(':');
    if (colon != std::string::npos) path = path.substr(colon + 1);
    return file("worldgen/structure/" + path + ".json");
}

bool exists(const std::string& relativePath) {
    try {
        return fs::exists(dataRoot() / "twilightforest" / relativePath);
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace twilight_data
} // namespace structure
} // namespace levelgen
} // namespace minecraft
