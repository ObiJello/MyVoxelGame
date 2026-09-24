#pragma once
#include "ConsoleSaveArchive.h"
#include "ConsoleRegionFile.h"
#include "ChunkRecord.h"
#include "LevelData.h"
#include <map>
#include <memory>
#include <span>
namespace console {
// The archived Common/Tutorial/Tutorial is a complete Xbox-era 54x54 world:
// eight-byte wrapper, framed LZX save table, and LZX/RLE region chunks.
// The separately supplied tutorialDiff targets a different source version;
// it must not be applied to this save's records.
class TutorialWorldSource {
    std::unique_ptr<ConsoleSaveArchive> save_;
    std::map<std::pair<int,int>,ConsoleRegionFile> regions_;
public:
    explicit TutorialWorldSource(std::span<const unsigned char> wrappedSave);
    std::unique_ptr<LevelData> metadata()const;
    std::unique_ptr<ChunkRecord> chunk(int x,int z)const;
};
}
