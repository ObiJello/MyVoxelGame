#pragma once

// This file is kept for backwards compatibility.
// The full ParameterPoint implementation is now in Climate.h

#include "world/biome/Climate.h"

namespace minecraft {
namespace levelgen {

// ParameterPoint is now defined in Climate as Climate::ParameterPoint
// This alias provides backwards compatibility
using ParameterPoint = world::biome::Climate::ParameterPoint;
using Parameter = world::biome::Climate::Parameter;

} // namespace levelgen
} // namespace minecraft
