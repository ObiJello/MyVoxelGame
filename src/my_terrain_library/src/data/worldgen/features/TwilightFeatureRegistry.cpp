#include "data/worldgen/features/TwilightFeatureRegistry.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {
namespace twilight {

namespace {

struct Registry {
    std::mutex mutex;
    std::map<std::string, levelgen::ConfiguredFeature*> byId;
    std::vector<std::unique_ptr<levelgen::ConfiguredFeature>> owned;
};

Registry& registry() {
    static Registry s_registry;
    return s_registry;
}

} // namespace

void registerConfigured(const std::string& id, levelgen::ConfiguredFeature* feature) {
    if (feature == nullptr) return;
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.byId[id] = feature;
}

levelgen::ConfiguredFeature* registerOwned(const std::string& id,
                                           std::unique_ptr<levelgen::ConfiguredFeature> feature) {
    if (!feature) return nullptr;
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    levelgen::ConfiguredFeature* raw = feature.get();
    r.owned.push_back(std::move(feature));
    r.byId[id] = raw;
    return raw;
}

levelgen::ConfiguredFeature* findConfigured(const std::string& id) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.byId.find(id);
    return it == r.byId.end() ? nullptr : it->second;
}

std::vector<std::string> registeredIds() {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    std::vector<std::string> out;
    out.reserve(r.byId.size());
    for (const auto& [id, feature] : r.byId) out.push_back(id);
    return out;
}

} // namespace twilight
} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
