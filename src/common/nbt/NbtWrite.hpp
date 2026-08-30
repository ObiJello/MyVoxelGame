// File: src/common/nbt/NbtWrite.hpp
//
// Write-only NBT encoder for the Anvil save path.
//
// WHY A NEW ONE. The engine already has a reader (World::NBTParser) and the
// terrain library carries a full read+write port, but neither can be the
// writer here. NBTParser has no write path at all. The terrain library's
// NbtIo::writeString emits raw UTF-8 through a truncating cast where vanilla
// reads Java modified UTF-8, and its CompoundTag is an unordered_map, so key
// order is not the caller's to choose — which rules out ever diffing our
// output against itself, let alone against vanilla's.
//
// So: a streaming writer over a byte vector. The CALLER decides key order,
// which makes the same chunk serialise to the same bytes every time. Every
// later verification gate leans on that.
//
// The encoder never throws. A refusal (over-long string, mismatched scope,
// runaway nesting) latches ok() to false and turns every later call into a
// no-op, so one bad value cannot produce a half-formed tag that a reader
// would happily misinterpret.
#pragma once

#include "common/nbt/ModifiedUtf8.hpp"   // string encoding, header-only

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Game::Nbt {

    enum class TagType : uint8_t {
        End       = 0,
        Byte      = 1,
        Short     = 2,
        Int       = 3,
        Long      = 4,
        Float     = 5,
        Double    = 6,
        ByteArray = 7,
        String    = 8,
        List      = 9,
        Compound  = 10,
        IntArray  = 11,
        LongArray = 12,
    };

    // ── Compression ─────────────────────────────────────────────────────────
    //
    // Gzip is what level.dat and playerdata use; zlib (raw deflate with a
    // zlib header) is region-file compression id 2, which is vanilla's
    // default and the only id we emit.
    bool GzipCompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, int level = 6);
    bool ZlibCompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, int level = 6);

    // The read side, for the gzipped files (level.dat, playerdata). Auto-detects
    // gzip vs zlib, and refuses to inflate past `maxBytes` so a corrupt length
    // cannot ask for all of memory.
    bool GzipDecompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                        size_t maxBytes = 64u * 1024u * 1024u);

    // ── Writer ──────────────────────────────────────────────────────────────
    class Writer {
    public:
        // Returned by BeginList and handed back to every element call so the
        // element count can be back-patched on EndList. The count is not known
        // up front for any list we write — block_entities skips empty slots,
        // sections skip empty sections.
        struct ListScope {
            size_t   typeOffset  = 0;
            size_t   countOffset = 0;
            TagType  elem        = TagType::End;
            int32_t  count       = 0;
            size_t   depth       = 0;
        };

        Writer() { m_buf.reserve(64 * 1024); }

        // Root is an unnamed compound: tag id, then a zero-length name.
        void BeginRootCompound();
        void EndRootCompound();

        void BeginCompound(std::string_view name);
        void EndCompound();

        void Byte  (std::string_view name, int8_t   v);
        void Bool  (std::string_view name, bool     v) { Byte(name, v ? 1 : 0); }
        void Short (std::string_view name, int16_t  v);
        void Int   (std::string_view name, int32_t  v);
        void Long  (std::string_view name, int64_t  v);
        void Float (std::string_view name, float    v);
        void Double(std::string_view name, double   v);
        void String(std::string_view name, std::string_view v);

        void ByteArray (std::string_view name, const int8_t*  v, size_t n);
        void IntArray  (std::string_view name, const int32_t* v, size_t n);
        void LongArray (std::string_view name, const int64_t* v, size_t n);
        // Same tag, unsigned source. Packed block/biome/heightmap data is
        // built as uint64 and the top bit is meaningful, so it must go out as
        // a raw bit pattern rather than through a signed conversion.
        void LongArray (std::string_view name, const uint64_t* v, size_t n);

        // A list of `elem`. Vanilla writes element type 0 for an EMPTY list
        // (ListTag.identifyRawElementType returns 0 when the list is empty),
        // so EndList rewrites the type byte when nothing was added.
        [[nodiscard]] ListScope BeginList(std::string_view name, TagType elem);
        void EndList(ListScope& s);

        // Elements carry no type byte and no name — the list header holds the
        // type. A compound element is a bare payload terminated by TAG_End.
        void ListCompoundBegin(ListScope& s);
        void ListCompoundEnd  (ListScope& s);
        void ListString(ListScope& s, std::string_view v);
        // Scalar element kinds. Entities need Double (Pos, Motion) and Float
        // (Rotation); PostProcessing needs Short. The rest are here so the
        // next list shape vanilla uses is not another round of this.
        void ListByte  (ListScope& s, int8_t  v);
        void ListShort (ListScope& s, int16_t v);
        void ListInt   (ListScope& s, int32_t v);
        void ListLong  (ListScope& s, int64_t v);
        void ListFloat (ListScope& s, float   v);
        void ListDouble(ListScope& s, double  v);
        // Arrays DO appear as list elements: structures.starts.<id>.Children[]
        // .Entrances is a list of int arrays (structure bounding boxes). The
        // parity harness found this against a real mineshaft.
        void ListByteArray(ListScope& s, const int8_t*  v, size_t n);
        void ListIntArray (ListScope& s, const int32_t* v, size_t n);
        void ListLongArray(ListScope& s, const int64_t* v, size_t n);
        // A nested list element; end it with EndList like any other.
        [[nodiscard]] ListScope ListListBegin(ListScope& parent, TagType elem);

        bool ok() const { return m_ok; }
        const std::vector<uint8_t>& Bytes() const { return m_buf; }
        std::vector<uint8_t>&& TakeBytes() { return std::move(m_buf); }
        size_t Size() const { return m_buf.size(); }
        void Reset() { m_buf.clear(); m_scopes.clear(); m_ok = true; }

        // Deep nesting is only ever reached by malformed input or a bug; the
        // cap keeps a runaway from growing the buffer without bound.
        static constexpr size_t kMaxDepth = 512;

    private:
        void Fail(const char* why);
        void Raw8 (uint8_t v)  { m_buf.push_back(v); }
        void Raw16(uint16_t v);
        void Raw32(uint32_t v);
        void Raw64(uint64_t v);
        void RawName(std::string_view name);
        bool Header(TagType t, std::string_view name);   // false if !ok
        bool ListElem(ListScope& s, TagType want);       // validates + counts
        void Patch32(size_t offset, uint32_t v);

        std::vector<uint8_t> m_buf;
        std::vector<char>    m_scopes;   // 'C' compound, 'L' list — validates End calls
        bool                 m_ok = true;
    };

} // namespace Game::Nbt
