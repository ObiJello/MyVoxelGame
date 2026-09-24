#pragma once
#include "SaveFormat.h"
#include <filesystem>
namespace console {
struct NativeSaveCommit {
    // The rename committed successfully. False means the directory could not
    // be fsynced, so crash durability of that rename is not confirmed.
    bool directorySynced=false;
};
// Native POSIX storage for the inner console archive. This does not implement
// Sony's packaging/signing or write the client's temporary .mcp format.
class NativeSaveFile {
public:
    static std::vector<unsigned char> read(const std::filesystem::path& path);
    // Parent directory must exist. Uses a unique sibling temp file, fsync and
    // atomic rename. A failure before rename leaves the previous save intact.
    static NativeSaveCommit replace(const std::filesystem::path& path,std::span<const unsigned char> bytes);
};
}
