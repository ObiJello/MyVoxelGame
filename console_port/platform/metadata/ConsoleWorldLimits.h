#pragma once
// Extracted unchanged from the supplied ChunkSource.h. PS3 uses the non-large-world branch.
// The maximum number of chunks that we can store
#ifdef _LARGE_WORLDS
// 4J Stu - Our default map (at zoom level 3) is 1024x1024 blocks (or 64 chunks)
#define LEVEL_MAX_WIDTH (5*64) //(6*54)
#else
#define LEVEL_MAX_WIDTH 54
#endif
#define LEVEL_MIN_WIDTH 54
#define LEVEL_LEGACY_WIDTH 54

// Scale was 8 in the Java game, but that would make our nether tiny
// Every 1 block you move in the nether maps to HELL_LEVEL_SCALE blocks in the overworld
#ifdef _LARGE_WORLDS
#define HELL_LEVEL_MAX_SCALE 8
#else
#define HELL_LEVEL_MAX_SCALE 3
#endif
#define HELL_LEVEL_MIN_SCALE 3
#define HELL_LEVEL_LEGACY_SCALE 3

#define HELL_LEVEL_MAX_WIDTH (LEVEL_MAX_WIDTH / HELL_LEVEL_MAX_SCALE)
#define HELL_LEVEL_MIN_WIDTH 18

#define END_LEVEL_SCALE 3
// 4J Stu - Fix the size of the end for all platforms
// 54 / 3 = 18
#define END_LEVEL_MAX_WIDTH 18
#define END_LEVEL_MIN_WIDTH 18
//#define END_LEVEL_MAX_WIDTH (LEVEL_MAX_WIDTH / END_LEVEL_SCALE)

