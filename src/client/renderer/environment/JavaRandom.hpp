// File: src/client/renderer/environment/JavaRandom.hpp
// The implementation moved to src/common/core/JavaRandom.hpp when the loot
// system needed the same bit-exact java.util.Random on the server. This alias
// keeps `Render::JavaRandom` working for the star field.
#pragma once

#include "common/core/JavaRandom.hpp"

namespace Render {
    using JavaRandom = Game::JavaRandom;
}
