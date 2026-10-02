// File: src/launcher/updater/BinaryPatch.hpp
//
// Binary patches between two releases' files (.obpatch), so an update that
// rebuilt the game executable downloads the difference instead of the whole
// entry. The difference is Colin Percival's bsdiff (qsufsort suffix array,
// then approximate matches written as control triples + a diff stream + an
// extra stream), with the three streams deflated by libdeflate instead of
// bzip2 — libdeflate is the only compressor in the build.
//
// A patch file holds any number of entries. Each names a path inside the
// release zip and the CRC-32 + size of the file it applies to (the base) and
// of the file it produces (the target). The launcher applies an entry only
// when the installed file's manifest record matches the base, and keeps the
// result only when it hashes to the target; anything else falls back to
// fetching the entry from the zip.
//
// Layout (little-endian):
//   "OBPATCH1"  u32 entryCount
//   per entry:  u16 nameLen, name,
//               u32 baseCrc, u64 baseSize, u32 targetCrc, u64 targetSize,
//               u64 controlSize, u64 controlBytes, u64 diffSize, u64 diffBytes,
//               u64 extraSize, u64 extraBytes      (sizes decompressed, bytes as stored)
//   then each entry's control, diff and extra streams (raw deflate), in entry order.
// The control stream is int64 triples: bytes to add from the base, bytes to
// copy from the extra stream, then how far to move in the base.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Launcher::BinaryPatch {

    struct Entry {
        std::string name;
        uint32_t baseCrc = 0;
        uint64_t baseSize = 0;
        uint32_t targetCrc = 0;
        uint64_t targetSize = 0;
        uint64_t controlSize = 0, diffSize = 0, extraSize = 0;   // decompressed
        std::vector<uint8_t> control, diff, extra;               // raw deflate

        uint64_t StoredBytes() const { return control.size() + diff.size() + extra.size(); }
    };

    // bsdiff of base -> target, the target split into `jobs` slices diffed in
    // parallel against the whole base (each slice starts its own match chain,
    // which costs a few bytes per slice). Streams deflated at libdeflate `level`.
    bool Create(const uint8_t* base, size_t baseSize, const uint8_t* target, size_t targetSize,
                Entry& out, int jobs = 8, int level = 12);

    // Rebuilds the target. False on a damaged entry, a base of the wrong size,
    // or a result whose CRC-32 is not targetCrc.
    bool Apply(const Entry& entry, const uint8_t* base, size_t baseSize, std::vector<uint8_t>& out);

    void Write(const std::vector<Entry>& entries, std::vector<uint8_t>& file);
    bool Read(const uint8_t* data, size_t size, std::vector<Entry>& out);

} // namespace Launcher::BinaryPatch
