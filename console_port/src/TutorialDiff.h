#pragma once
#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>
namespace console {
struct TutorialBytePatch {
    std::uint32_t offset=0;
    std::vector<std::uint8_t> bytes;
};
// The supplied title-update tutorialDiff stores sparse edits to serialized
// chunk records, grouped by region-file slot. This decoder preserves the raw
// edits; applying them requires an exact matching source chunk byte stream.
class TutorialDiff {
    std::map<std::pair<int,int>,std::vector<TutorialBytePatch>> patches_;
public:
    static TutorialDiff read(std::span<const std::uint8_t> bytes);
    const std::vector<TutorialBytePatch>* chunk(int x,int z)const;
    std::size_t chunkCount()const{return patches_.size();}
    std::size_t patchCount()const;
    // Transactionally apply a chunk's sparse edits to an exact source record.
    // Returns false when that chunk has no edits.
    bool apply(int x,int z,std::vector<std::uint8_t>& record)const;
};
}
