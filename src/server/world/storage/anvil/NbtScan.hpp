// File: src/server/world/storage/anvil/NbtScan.hpp
//
// Reads one root-level tag of an encoded NBT buffer without building a tree:
// the tags before it are skipped over, the ones after it never touched. For
// the few places that need a single field of a whole chunk — the Status
// check under the region lock, and the "structures" compound carried through
// a load unchanged.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Game::Anvil::NbtScan {

    // Tag ids (NBT TAG_*), as the scanner sees them.
    constexpr uint8_t kString   = 8;
    constexpr uint8_t kCompound = 10;

    // The payload of the root compound's tag `name` of type `type`: bytes
    // [begin, end) of `nbt`. False when the buffer is not a compound, the tag
    // is absent or of another type, or the data is truncated.
    bool FindRootTag(const std::vector<uint8_t>& nbt, std::string_view name, uint8_t type,
                     size_t& begin, size_t& end);

    // A root-level string tag (ASCII/UTF-8 payload taken as is).
    bool ReadRootString(const std::vector<uint8_t>& nbt, std::string_view name, std::string& out);

} // namespace Game::Anvil::NbtScan
