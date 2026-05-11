// File: src/common/data/DataComponentMap.cpp
#include "DataComponentMap.hpp"

namespace Game {

    bool DataComponentMap::has(const DataComponentTypeBase& key) const {
        for (const auto& e : entries) {
            if (e.type == &key) return true;
        }
        return false;
    }

    void DataComponentMap::remove(const DataComponentTypeBase& key) {
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->type == &key) { entries.erase(it); return; }
        }
    }

} // namespace Game
