#pragma once
#include <atomic>
// Explicit host option used by LevelData::setGameType. Connect the eventual
// original host-options menu here; this is not an emulation of the full app API.
namespace SavePlatform {inline std::atomic_bool cheatsEnabled{false};}
