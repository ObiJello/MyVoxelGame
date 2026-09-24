#pragma once
#include <filesystem>
#include <string>
#include <vector>
namespace console {
struct SavedWorld { std::filesystem::path path; std::string name; bool readable=true; };
std::vector<SavedWorld> listSavedWorlds(const std::filesystem::path& directory);
// Creates a unique directory; display names never become filesystem paths.
std::filesystem::path allocateWorldSave(const std::filesystem::path& directory);
}
