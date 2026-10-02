// File: src/common/core/ZipArchive.hpp
//
// Read-only .zip access over libdeflate (Core::Deflate) — resource packs,
// shader packs, and the launcher's update archives. It replaced minizip,
// which was zlib's.
//
// The static half works on bytes already in memory, for an archive that is
// not a local file: the launcher reads a release's central directory and
// single entries over HTTP range requests, and inflates entries out of the
// download stream as it arrives.
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
            uint16_t    versionMadeBy = 0;     // high byte: the host system (3 Unix, 19 OS X)
            uint32_t    externalAttributes = 0;
            bool IsDirectory() const { return !name.empty() && name.back() == '/'; }
            // st_mode bits recorded by a Unix zipper (file type + permissions); 0 when the
            // archive was written elsewhere and does not carry them.
            uint32_t UnixMode() const {
                const int host = versionMadeBy >> 8;
                return (host == 3 || host == 19) ? (externalAttributes >> 16) : 0;
            }
        };

        // Where the central directory sits, from the end-of-directory record.
        struct DirectoryInfo {
            uint64_t offset = 0;
            uint64_t size = 0;
            uint64_t entryCount = 0;
        };

        // A local file header (the copy of an entry's metadata in front of its data).
        struct LocalHeader {
            std::string name;
            uint16_t    flags = 0;             // bit 3: sizes and CRC follow the data instead
            uint16_t    method = 0;
            uint32_t    crc32 = 0;
            uint64_t    compressedSize = 0;
            uint64_t    size = 0;
            size_t      headerSize = 0;        // fixed part + name + extra field
        };

        ZipArchive() = default;
        explicit ZipArchive(const std::filesystem::path& path) { Open(path); }

        // Reads the central directory. False (and Error()) for a file that is
        // missing, not a zip, or damaged.
        bool Open(const std::filesystem::path& path);
        void Close();
        bool IsOpen() const { return m_file.is_open(); }

        const std::vector<Entry>& Entries() const { return m_entries; }
        uint64_t DirectoryBytes() const { return m_directoryBytes; }   // the central directory's size
        // Exact, case-sensitive name match; nullptr when absent.
        const Entry* Find(std::string_view name) const;

        // The entry's contents, whole. False (and Error()) on a damaged entry,
        // a CRC mismatch, or an unsupported method.
        bool Read(const Entry& entry, std::vector<uint8_t>& out);
        bool Read(const Entry& entry, std::string& out);
        // The entry's bytes as stored (still compressed), for a caller that
        // inflates them elsewhere (with Decode).
        bool ReadCompressed(const Entry& entry, std::vector<uint8_t>& compressed);

        const std::string& Error() const { return m_error; }

        // Entries past this are refused rather than allocated.
        static constexpr uint64_t kMaxEntryBytes = 1ull << 30;

        // ── In-memory archive pieces ──

        // The central directory's place, from the archive's last bytes: `tail` holds
        // bytes [tailOffset, tailOffset + tailSize) of a `fileSize`-byte archive and
        // must include the end-of-directory record, plus the Zip64 one for a Zip64
        // archive (both sit in the last 64 KiB + 98 bytes of any archive).
        static bool LocateDirectory(const uint8_t* tail, size_t tailSize, uint64_t tailOffset, uint64_t fileSize,
                                    DirectoryInfo& out, std::string& error);
        // The central directory's entries, from its bytes.
        static bool ParseDirectory(const uint8_t* dir, size_t size, uint64_t entryCount,
                                   std::vector<Entry>& out, std::string& error);

        enum class HeaderResult : uint8_t { Ok, NeedMore, NotAHeader };
        // The local header at `p`. NeedMore until all of its `headerSize` bytes are
        // there; NotAHeader for any other record (the central directory, once a
        // stream has passed every entry).
        static HeaderResult ParseLocalHeader(const uint8_t* p, size_t available, LocalHeader& out);

        // An entry's contents from its compressed bytes, checked against `crc32`.
        static bool Decode(const std::string& name, uint16_t method, uint32_t crc32, uint64_t size,
                           const uint8_t* compressed, size_t compressedSize,
                           std::vector<uint8_t>& out, std::string& error);

    private:
        bool ReadAt(uint64_t offset, void* dst, size_t n);
        bool Fail(std::string why);

        std::ifstream      m_file;
        uint64_t           m_fileSize = 0;
        uint64_t           m_directoryBytes = 0;
        std::vector<Entry> m_entries;
        std::string        m_error;
    };

} // namespace Core
