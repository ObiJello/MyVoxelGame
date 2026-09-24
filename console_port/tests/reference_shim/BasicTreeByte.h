#pragma once
// The original LevelType.h imports std::byte into global lookup on C++20.
// Load the platform headers first, then preserve the console's byte spelling
// for the archived BasicTree translation unit without changing that source.
#include "stdafx.h"
#include "net.minecraft.world.level.h"
#include "net.minecraft.world.level.tile.h"
using std::max;
#define byte unsigned char
