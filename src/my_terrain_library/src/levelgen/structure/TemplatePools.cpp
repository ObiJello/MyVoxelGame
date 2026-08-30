#include "levelgen/structure/TemplatePool.h"

#include "external/json.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

// Reference: StructureTemplatePool.java DIRECT_CODEC + StructurePoolElement
// codecs; data at data/<ns>/worldgen/template_pool/<path>.json.

namespace minecraft {
namespace levelgen {
namespace structure {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

fs::path poolDataRoot() {
    if (const char* env = std::getenv("MC_DATA_ROOT")) return fs::path(env);
    fs::path current = fs::current_path();
    while (!current.empty()) {
        fs::path candidate = current / "data";
        if (fs::exists(candidate) && fs::is_directory(candidate)) return candidate;
        if (current == current.root_path()) break;
        current = current.parent_path();
    }
    throw std::runtime_error("Data root not found for template pools");
}

std::string normalizeId(const std::string& id) {
    return id.find(':') != std::string::npos ? id : "minecraft:" + id;
}

PoolElement parseElement(const json& e);

PoolElement parseElementBody(const json& body) {
    PoolElement element;
    std::string type = normalizeId(body.at("element_type").get<std::string>());
    if (type == "minecraft:single_pool_element" || type == "minecraft:legacy_single_pool_element") {
        element.kind = (type == "minecraft:single_pool_element")
            ? PoolElementKind::SINGLE : PoolElementKind::LEGACY_SINGLE;
        element.location = normalizeId(body.at("location").get<std::string>());
        if (body.contains("processors") && body["processors"].is_string()) {
            element.processors = normalizeId(body["processors"].get<std::string>());
        }
        element.projection = body.value("projection", std::string("rigid"));
    } else if (type == "minecraft:feature_pool_element") {
        element.kind = PoolElementKind::FEATURE;
        element.feature = normalizeId(body.at("feature").get<std::string>());
        element.projection = body.value("projection", std::string("rigid"));
    } else if (type == "minecraft:list_pool_element") {
        element.kind = PoolElementKind::LIST;
        element.projection = body.value("projection", std::string("rigid"));
        for (const auto& sub : body.at("elements")) {
            element.listElements.push_back(parseElementBody(sub));
        }
    } else if (type == "minecraft:empty_pool_element") {
        element.kind = PoolElementKind::EMPTY;
    } else {
        throw std::runtime_error("Unknown pool element type: " + type);
    }
    return element;
}

class PoolRegistry {
public:
    static PoolRegistry& instance() {
        static PoolRegistry s_instance;
        return s_instance;
    }

    const TemplatePool* find(const std::string& rawName) {
        std::string name = normalizeId(rawName);
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pools.find(name);
        if (it != m_pools.end()) return it->second.get();

        size_t colon = name.find(':');
        std::string ns = name.substr(0, colon);
        std::string path = name.substr(colon + 1);
        fs::path file = m_root / ns / "worldgen" / "template_pool" / (path + ".json");
        if (!fs::exists(file)) {
            if (name == "minecraft:empty") {
                auto pool = std::make_unique<TemplatePool>();
                pool->name = name;
                pool->fallback = "minecraft:empty";
                return m_pools.emplace(name, std::move(pool)).first->second.get();
            }
            return nullptr;
        }
        std::ifstream input(file);
        json parsed;
        input >> parsed;

        auto pool = std::make_unique<TemplatePool>();
        pool->name = name;
        pool->fallback = normalizeId(parsed.at("fallback").get<std::string>());
        for (const auto& entry : parsed.at("elements")) {
            int weight = entry.value("weight", 1);
            auto owned = std::make_unique<PoolElement>(parseElementBody(entry.at("element")));
            const PoolElement* ptr = owned.get();
            pool->ownedElements.push_back(std::move(owned));
            for (int i = 0; i < weight; ++i) {
                pool->templates.push_back(ptr);
            }
        }
        return m_pools.emplace(name, std::move(pool)).first->second.get();
    }

private:
    PoolRegistry() : m_root(poolDataRoot()) {}
    fs::path m_root;
    std::mutex m_mutex;
    std::unordered_map<std::string, std::unique_ptr<TemplatePool>> m_pools;
};

} // namespace

namespace TemplatePools {

const TemplatePool& byName(const std::string& name) {
    const TemplatePool* pool = PoolRegistry::instance().find(name);
    if (pool == nullptr) {
        throw std::runtime_error("Unknown template pool: " + name);
    }
    return *pool;
}

bool exists(const std::string& name) {
    return PoolRegistry::instance().find(name) != nullptr;
}

const TemplatePool& empty() {
    return byName("minecraft:empty");
}

} // namespace TemplatePools

} // namespace structure
} // namespace levelgen
} // namespace minecraft
