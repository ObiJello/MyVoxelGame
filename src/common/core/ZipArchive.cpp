// File: src/common/core/ZipArchive.cpp
#include "common/core/ZipArchive.hpp"

#include "common/core/Deflate.hpp"

#include <algorithm>
#include <cstring>

namespace Core {

    namespace {

        constexpr uint32_t kLocalHeaderSig   = 0x04034b50;
        constexpr uint32_t kCentralHeaderSig = 0x02014b50;
        constexpr uint32_t kEndSig           = 0x06054b50;
        constexpr uint32_t kZip64EndSig      = 0x06064b50;
        constexpr uint32_t kZip64LocatorSig  = 0x07064b50;
        constexpr size_t   kEndSize          = 22;
        constexpr size_t   kZip64LocatorSize = 20;
        constexpr size_t   kZip64EndSize     = 56;
        constexpr size_t   kCentralSize      = 46;
        constexpr size_t   kLocalSize        = 30;
        constexpr uint64_t kMaxDirectoryBytes = 256ull << 20;

        uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
        uint32_t U32(const uint8_t* p) {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }
        uint64_t U64(const uint8_t* p) { return static_cast<uint64_t>(U32(p)) | (static_cast<uint64_t>(U32(p + 4)) << 32); }

    } // namespace

    bool ZipArchive::Fail(std::string why) {
        m_error = std::move(why);
        return false;
    }

    bool ZipArchive::ReadAt(uint64_t offset, void* dst, size_t n) {
        if (offset > m_fileSize || n > m_fileSize - offset) return false;
        m_file.clear();
        m_file.seekg(static_cast<std::streamoff>(offset));
        m_file.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
        return static_cast<size_t>(m_file.gcount()) == n;
    }

    void ZipArchive::Close() {
        if (m_file.is_open()) m_file.close();
        m_fileSize = 0;
        m_entries.clear();
    }

    bool ZipArchive::Open(const std::filesystem::path& path) {
        Close();
        m_error.clear();
        m_file.open(path, std::ios::binary);
        if (!m_file) return Fail("cannot open " + path.string());
        m_file.seekg(0, std::ios::end);
        m_fileSize = static_cast<uint64_t>(m_file.tellg());
        if (m_fileSize < kEndSize) { Close(); return Fail("not a zip (too short)"); }

        // The end-of-central-directory record sits in the last 22 bytes plus
        // up to a 65,535-byte archive comment: scan back for its signature.
        const uint64_t tailSize = std::min<uint64_t>(m_fileSize, kEndSize + 0xFFFF + kZip64LocatorSize);
        std::vector<uint8_t> tail(static_cast<size_t>(tailSize));
        const uint64_t tailStart = m_fileSize - tailSize;
        if (!ReadAt(tailStart, tail.data(), tail.size())) { Close(); return Fail("cannot read the archive's end"); }
        size_t eocd = std::string::npos;
        for (size_t i = tail.size() - kEndSize + 1; i-- > 0;) {
            if (U32(&tail[i]) == kEndSig && i + kEndSize + U16(&tail[i + 20]) <= tail.size()) { eocd = i; break; }
        }
        if (eocd == std::string::npos) { Close(); return Fail("not a zip (no end-of-directory record)"); }

        uint64_t entryCount = U16(&tail[eocd + 10]);
        uint64_t dirSize    = U32(&tail[eocd + 12]);
        uint64_t dirOffset  = U32(&tail[eocd + 16]);

        // Zip64: a locator right before the record points at the 64-bit one.
        if (eocd >= kZip64LocatorSize && U32(&tail[eocd - kZip64LocatorSize]) == kZip64LocatorSig) {
            const uint64_t z64Offset = U64(&tail[eocd - kZip64LocatorSize + 8]);
            uint8_t z64[kZip64EndSize];
            if (!ReadAt(z64Offset, z64, sizeof z64) || U32(z64) != kZip64EndSig) {
                Close();
                return Fail("damaged Zip64 end-of-directory record");
            }
            entryCount = U64(z64 + 32);
            dirSize    = U64(z64 + 40);
            dirOffset  = U64(z64 + 48);
        }
        if (dirOffset > m_fileSize || dirSize > m_fileSize - dirOffset || dirSize > kMaxDirectoryBytes ||
            entryCount > dirSize / kCentralSize) {
            Close();
            return Fail("damaged central directory");
        }

        std::vector<uint8_t> dir(static_cast<size_t>(dirSize));
        if (!ReadAt(dirOffset, dir.data(), dir.size())) { Close(); return Fail("cannot read the central directory"); }
        m_entries.reserve(static_cast<size_t>(entryCount));
        size_t p = 0;
        for (uint64_t i = 0; i < entryCount; ++i) {
            if (p + kCentralSize > dir.size() || U32(&dir[p]) != kCentralHeaderSig) {
                Close();
                return Fail("damaged central directory entry");
            }
            const uint8_t* h = &dir[p];
            Entry e;
            e.flags  = U16(h + 8);
            e.method = U16(h + 10);
            e.crc32  = U32(h + 16);
            e.compressedSize    = U32(h + 20);
            e.size              = U32(h + 24);
            const size_t nameLen    = U16(h + 28);
            const size_t extraLen   = U16(h + 30);
            const size_t commentLen = U16(h + 32);
            e.localHeaderOffset = U32(h + 42);
            if (p + kCentralSize + nameLen + extraLen + commentLen > dir.size()) {
                Close();
                return Fail("damaged central directory entry");
            }
            e.name.assign(reinterpret_cast<const char*>(h + kCentralSize), nameLen);

            // Zip64 extra field (id 1): the 64-bit value of each 32-bit field
            // that holds 0xFFFFFFFF, in this order.
            const uint8_t* x   = h + kCentralSize + nameLen;
            const uint8_t* end = x + extraLen;
            while (x + 4 <= end) {
                const uint16_t id  = U16(x);
                const uint16_t len = U16(x + 2);
                const uint8_t* v   = x + 4;
                if (v + len > end) break;
                if (id == 0x0001) {
                    const uint8_t* q = v;
                    const uint8_t* qEnd = v + len;
                    if (e.size == 0xFFFFFFFFu && q + 8 <= qEnd)              { e.size = U64(q); q += 8; }
                    if (e.compressedSize == 0xFFFFFFFFu && q + 8 <= qEnd)    { e.compressedSize = U64(q); q += 8; }
                    if (e.localHeaderOffset == 0xFFFFFFFFu && q + 8 <= qEnd) { e.localHeaderOffset = U64(q); }
                }
                x = v + len;
            }
            m_entries.push_back(std::move(e));
            p += kCentralSize + nameLen + extraLen + commentLen;
        }
        return true;
    }

    const ZipArchive::Entry* ZipArchive::Find(std::string_view name) const {
        for (const Entry& e : m_entries) {
            if (e.name == name) return &e;
        }
        return nullptr;
    }

    bool ZipArchive::EntryData(const Entry& entry, std::vector<uint8_t>& compressed) {
        if (!IsOpen()) return Fail("archive not open");
        if (entry.flags & 0x0001) return Fail(entry.name + ": encrypted entries are not supported");
        if (entry.method != 0 && entry.method != 8) {
            return Fail(entry.name + ": compression method " + std::to_string(entry.method) + " is not supported");
        }
        if (entry.size > kMaxEntryBytes || entry.compressedSize > kMaxEntryBytes) {
            return Fail(entry.name + ": entry too large");
        }
        // The local header repeats the name and carries its own extra field,
        // which may differ in length from the central directory's.
        uint8_t local[kLocalSize];
        if (!ReadAt(entry.localHeaderOffset, local, sizeof local) || U32(local) != kLocalHeaderSig) {
            return Fail(entry.name + ": damaged local header");
        }
        const uint64_t dataOffset = entry.localHeaderOffset + kLocalSize + U16(local + 26) + U16(local + 28);
        compressed.resize(static_cast<size_t>(entry.compressedSize));
        if (!ReadAt(dataOffset, compressed.data(), compressed.size())) return Fail(entry.name + ": truncated");
        return true;
    }

    bool ZipArchive::Read(const Entry& entry, std::vector<uint8_t>& out) {
        std::vector<uint8_t> compressed;
        if (!EntryData(entry, compressed)) return false;
        if (entry.method == 0) {
            if (compressed.size() != entry.size) return Fail(entry.name + ": stored size mismatch");
            out = std::move(compressed);
        } else if (entry.size == 0) {
            out.clear();   // an empty file: nothing to inflate (its CRC is 0)
        } else {
            out.resize(static_cast<size_t>(entry.size));
            if (!Deflate::DecompressExact(compressed.data(), compressed.size(), out.data(), out.size(),
                                          Deflate::Format::Raw)) {
                out.clear();
                return Fail(entry.name + ": damaged deflate data");
            }
        }
        if (Deflate::Crc32(out.data(), out.size()) != entry.crc32) {
            out.clear();
            return Fail(entry.name + ": CRC mismatch");
        }
        return true;
    }

    bool ZipArchive::Read(const Entry& entry, std::string& out) {
        std::vector<uint8_t> bytes;
        if (!Read(entry, bytes)) return false;
        out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    }

} // namespace Core
