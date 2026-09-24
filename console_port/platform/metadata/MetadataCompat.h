#pragma once
#include "NbtCompat.h"
#include "ConsoleWorldLimits.h"
#include "SavePlatform.h"
#include "Abilities.h"
#ifdef CONSOLE_METADATA_REFERENCE
// The archived code reads this one host option through the console application.
inline constexpr int eGameHostOption_CheatsEnabled=0;
struct MetadataReferenceOptions {int GetGameHostOption(int){return SavePlatform::cheatsEnabled.load()?1:0;}};
inline MetadataReferenceOptions app;
#endif
