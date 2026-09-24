// File: src/common/text/Language.cpp
#include "common/text/Language.hpp"

#include "common/core/Log.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <mutex>
#include <unordered_map>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Game::Language {

    namespace {

        // MC ClientLanguage.loadFrom: every string entry of the json. Built
        // once under call_once, then only read.
        const std::unordered_map<std::string, std::string>& Table() {
            static std::unordered_map<std::string, std::string> table;
            static std::once_flag once;
            std::call_once(once, [] {
                try {
                    std::ifstream in(PlatformMain::GetAssetPath("assets/lang/en_us.json"));
                    if (!in.is_open()) {
                        Log::Warning("[Language] assets/lang/en_us.json not found; translatable text shows its keys");
                        return;
                    }
                    nlohmann::json j;
                    in >> j;
                    for (auto it = j.begin(); it != j.end(); ++it) {
                        if (it.value().is_string()) table.emplace(it.key(), it.value().get<std::string>());
                    }
                } catch (const std::exception& e) {
                    Log::Warning("[Language] en_us.json: %s", e.what());
                }
            });
            return table;
        }

    } // namespace

    bool Has(const std::string& key) {
        return Table().count(key) != 0;
    }

    std::string GetOrDefault(const std::string& key, const std::string& fallback) {
        const auto& table = Table();
        auto it = table.find(key);
        return it != table.end() ? it->second : fallback;
    }

} // namespace Game::Language
