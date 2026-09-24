#pragma once
// Unchanged version enum from the supplied FileHeader.h.
enum ESaveVersions
{
	// Pre-release version
	SAVE_FILE_VERSION_PRE_LAUNCH = 1,

	// This is the version at which we launched the Xbox360 version
	SAVE_FILE_VERSION_LAUNCH = 2,

	// This is the version at which we had made changes that broke older saves
	SAVE_FILE_VERSION_POST_LAUNCH = 3,

	// This is the version at which we introduced the End, and any saves older than this will have their End data deleted
	SAVE_FILE_VERSION_NEW_END = 4,

	// This is the version at which we change the stronghold generation, and any saves older than this should should the original version
	SAVE_FILE_VERSION_MOVED_STRONGHOLD = 5,

	// This is the version at which we changed the playeruid format for PS3
	SAVE_FILE_VERSION_CHANGE_MAP_DATA_MAPPING_SIZE = 6,

	// This is the version at which we changed the playeruid format for Xbox One
	SAVE_FILE_VERSION_DURANGO_CHANGE_MAP_DATA_MAPPING_SIZE = 7,

	// This is the version at which we changed the chunk format to directly save the compressed storage formats
	SAVE_FILE_VERSION_COMPRESSED_CHUNK_STORAGE,

	SAVE_FILE_VERSION_NEXT,
};
#define SAVE_FILE_VERSION_NUMBER (SAVE_FILE_VERSION_NEXT - 1)
