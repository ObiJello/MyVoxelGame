// File: src/common/core/ZipArchive.hpp
//
// Read-only .zip access over libdeflate (Core::Deflate) — resource packs,
// shader packs, and the launcher's update archives. It replaced minizip,
// which was zlib's.
//
// The central directory is read once at Open; an entry is read whole, its
// compressed bytes and then its contents in memory at once (libdeflate has
// no streaming), and checked against its CRC-32. Stored and deflated entries,
// Zip64 sizes and offsets. Not supported, as minizip's build here did not
// support them either: encrypted entries, and methods other than stored and
// deflate (bzip2, LZMA, zstd). Entry names are the archive's bytes as they
// are (UTF-8 for anything written in the last decade).
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace Core {

    class ZipArchive {
    public:
        struct Entry {
            std::string name;
            uint64_t    size = 0;              // uncompressed
            uint64_t    compressedSize = 0;
            uint64_t    localHeaderOffset = 0;
            uint32_t    crc32 = 0;
            uint16_t    method = 0;            // 0 stored, 8 deflate
            uint16_t    flags = 0;
            bool IsDirectory() const { return !name.empty() && name.back() == '/'; }
        };

        ZipArchive() = default;
        explicit ZipArchive(const std::filesystem::path& path) { Open(path); }

        // Reads the central directory. False (and Error()) for a file that is
        // missing, not a zip, or damaged.
        bool Open(const std::filesystem::path& path);
        void Close();
        bool IsOpen() const { return m_file.is_open(); }

        const std::vector<Entry>& Entries() const { return m_entries; }
        // Exact, case-sensitive name match; nullptr when absent.
        const Entry* Find(std::string_view name) const;

        // The entry's contents, whole. False (and Error()) on a damaged entry,
        // a CRC mismatch, or an unsupported method.
        bool Read(const Entry& entry, std::vector<uint8_t>& out);
        bool Read(const Entry& entry, std::string& out);

        const std::string& Error() const { return m_error; }

        // Entries past this are refused rather than allocated.
        static constexpr uint64_t kMaxEntryBytes = 1ull << 30;

    private:
        bool ReadAt(uint64_t offset, void* dst, size_t n);
        bool Fail(std::string why);
        bool EntryData(const Entry& entry, std::vector<uint8_t>& compressed);

        std::ifstream      m_file;
        uint64_t           m_fileSize = 0;
        std::vector<Entry> m_entries;
        std::string        m_error;
    };

} // namespace Core
