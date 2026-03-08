// File: src/launcher/platform/ProcessLauncher.hpp
#pragma once

#include <string>

namespace Launcher {

    // Launch the game process. Returns true if the process was started successfully.
    bool LaunchGame(const std::string& gamePath);

} // namespace Launcher
