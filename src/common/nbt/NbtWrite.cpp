// File: src/common/nbt/NbtWrite.cpp
#include "common/nbt/NbtWrite.hpp"

#include "common/nbt/ModifiedUtf8.hpp"

#include "common/core/Log.hpp"

#include <algorithm>
#include <cstring>
#include <zlib.h>

namespace Game::Nbt {

    // ── Modified UTF-8 ──────────────────────────────────────────────────────

    // ── Compression ─────────────────────────────────────────────────────────

    namespace {

        bool DeflateTo(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                       int level, int windowBits, const char* what) {
            z_stream s{};
            if (deflateInit2(&s, level, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
                Log::Error("[Nbt] %s: deflateInit2 failed", what);
                return false;
            }

            std::vector<uint8_t> result;
            result.resize(deflateBound(&s, static_cast<uLong>(in.size())) + 64);

            s.next_in   = const_cast<Bytef*>(in.data());
            s.avail_in  = static_cast<uInt>(in.size());
            s.next_out  = result.data();
            s.avail_out = static_cast<uInt>(result.size());

            const int rc = deflate(&s, Z_FINISH);
            const size_t produced = result.size() - s.avail_out;
            deflateEnd(&s);

            if (rc != Z_STREAM_END) {
                Log::Error("[Nbt] %s: deflate returned %d", what, rc);
                return false;
            }
            result.resize(produced);
            out = std::move(result);
            return true;
        }

    } // namespace

    // 15 + 16 selects a gzip wrapper; plain 15 selects the zlib wrapper.
    bool GzipCompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, int level) {
        return DeflateTo(in, out, level, 15 + 16, "gzip");
    }

    bool ZlibCompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, int level) {
        return DeflateTo(in, out, level, 15, "zlib");
    }

    bool GzipDecompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, size_t maxBytes) {
        if (in.empty()) return false;

        z_stream s{};
        // 15 + 32 auto-detects the gzip and zlib wrappers, so this reads both.
        if (inflateInit2(&s, 15 + 32) != Z_OK) return false;

        std::vector<uint8_t> result(std::min<size_t>(in.size() * 6 + 8192, maxBytes));
        s.next_in  = const_cast<Bytef*>(in.data());
        s.avail_in = static_cast<uInt>(in.size());

        size_t produced = 0;
        for (;;) {
            if (produced == result.size()) {
                if (result.size() >= maxBytes) { inflateEnd(&s); return false; }
                result.resize(std::min(result.size() * 2, maxBytes));
            }
            s.next_out  = result.data() + produced;
            s.avail_out = static_cast<uInt>(result.size() - produced);

            const int rc = inflate(&s, Z_NO_FLUSH);
            produced = result.size() - s.avail_out;
            if (rc == Z_STREAM_END) break;
            // Z_BUF_ERROR with the input exhausted means a truncated stream,
            // not a small output buffer — treating it as retryable is how the
            // vendored port spins forever.
            if (rc != Z_OK) { inflateEnd(&s); return false; }
        }
        inflateEnd(&s);
        result.resize(produced);
        out = std::move(result);
        return true;
    }

    // ── Writer ──────────────────────────────────────────────────────────────

    void Writer::Fail(const char* why) {
        if (m_ok) Log::Error("[Nbt] writer refused: %s", why);
        m_ok = false;
    }

    void Writer::Raw16(uint16_t v) {
        m_buf.push_back(static_cast<uint8_t>(v >> 8));
        m_buf.push_back(static_cast<uint8_t>(v));
    }

    void Writer::Raw32(uint32_t v) {
        m_buf.push_back(static_cast<uint8_t>(v >> 24));
        m_buf.push_back(static_cast<uint8_t>(v >> 16));
        m_buf.push_back(static_cast<uint8_t>(v >> 8));
        m_buf.push_back(static_cast<uint8_t>(v));
    }

    void Writer::Raw64(uint64_t v) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            m_buf.push_back(static_cast<uint8_t>(v >> shift));
        }
    }

    void Writer::Patch32(size_t offset, uint32_t v) {
        m_buf[offset + 0] = static_cast<uint8_t>(v >> 24);
        m_buf[offset + 1] = static_cast<uint8_t>(v >> 16);
        m_buf[offset + 2] = static_cast<uint8_t>(v >> 8);
        m_buf[offset + 3] = static_cast<uint8_t>(v);
    }

    void Writer::RawName(std::string_view name) {
        std::vector<uint8_t> encoded;
        if (!EncodeModifiedUtf8(name, encoded)) { Fail("tag name is not encodable"); return; }
        Raw16(static_cast<uint16_t>(encoded.size()));
        m_buf.insert(m_buf.end(), encoded.begin(), encoded.end());
    }

    bool Writer::Header(TagType t, std::string_view name) {
        if (!m_ok) return false;
        if (m_scopes.empty() || m_scopes.back() != 'C') { Fail("named tag outside a compound"); return false; }
        Raw8(static_cast<uint8_t>(t));
        RawName(name);
        return m_ok;
    }

    void Writer::BeginRootCompound() {
        if (!m_ok) return;
        if (!m_scopes.empty()) { Fail("root compound opened twice"); return; }
        Raw8(static_cast<uint8_t>(TagType::Compound));
        Raw16(0);                       // the root tag's name is always empty
        m_scopes.push_back('C');
    }

    void Writer::EndRootCompound() {
        if (!m_ok) return;
        if (m_scopes.size() != 1 || m_scopes.back() != 'C') { Fail("unbalanced root compound"); return; }
        Raw8(0);
        m_scopes.pop_back();
    }

    void Writer::BeginCompound(std::string_view name) {
        if (m_scopes.size() >= kMaxDepth) { Fail("nesting too deep"); return; }
        if (!Header(TagType::Compound, name)) return;
        m_scopes.push_back('C');
    }

    void Writer::EndCompound() {
        if (!m_ok) return;
        if (m_scopes.size() < 2 || m_scopes.back() != 'C') { Fail("EndCompound without BeginCompound"); return; }
        Raw8(0);
        m_scopes.pop_back();
    }

    void Writer::Byte(std::string_view name, int8_t v) {
        if (!Header(TagType::Byte, name)) return;
        Raw8(static_cast<uint8_t>(v));
    }

    void Writer::Short(std::string_view name, int16_t v) {
        if (!Header(TagType::Short, name)) return;
        Raw16(static_cast<uint16_t>(v));
    }

    void Writer::Int(std::string_view name, int32_t v) {
        if (!Header(TagType::Int, name)) return;
        Raw32(static_cast<uint32_t>(v));
    }

    void Writer::Long(std::string_view name, int64_t v) {
        if (!Header(TagType::Long, name)) return;
        Raw64(static_cast<uint64_t>(v));
    }

    void Writer::Float(std::string_view name, float v) {
        if (!Header(TagType::Float, name)) return;
        uint32_t bits; std::memcpy(&bits, &v, sizeof(bits));
        Raw32(bits);
    }

    void Writer::Double(std::string_view name, double v) {
        if (!Header(TagType::Double, name)) return;
        uint64_t bits; std::memcpy(&bits, &v, sizeof(bits));
        Raw64(bits);
    }

    void Writer::String(std::string_view name, std::string_view v) {
        std::vector<uint8_t> encoded;
        if (!EncodeModifiedUtf8(v, encoded)) { Fail("string value is not encodable"); return; }
        if (!Header(TagType::String, name)) return;
        Raw16(static_cast<uint16_t>(encoded.size()));
        m_buf.insert(m_buf.end(), encoded.begin(), encoded.end());
    }

    void Writer::ByteArray(std::string_view name, const int8_t* v, size_t n) {
        if (n > 0x7FFFFFFFu) { Fail("byte array too long"); return; }
        if (!Header(TagType::ByteArray, name)) return;
        Raw32(static_cast<uint32_t>(n));
        m_buf.insert(m_buf.end(), reinterpret_cast<const uint8_t*>(v),
                     reinterpret_cast<const uint8_t*>(v) + n);
    }

    void Writer::IntArray(std::string_view name, const int32_t* v, size_t n) {
        if (n > 0x7FFFFFFFu) { Fail("int array too long"); return; }
        if (!Header(TagType::IntArray, name)) return;
        Raw32(static_cast<uint32_t>(n));
        m_buf.reserve(m_buf.size() + n * 4);
        for (size_t i = 0; i < n; ++i) Raw32(static_cast<uint32_t>(v[i]));
    }

    void Writer::LongArray(std::string_view name, const int64_t* v, size_t n) {
        if (n > 0x7FFFFFFFu) { Fail("long array too long"); return; }
        if (!Header(TagType::LongArray, name)) return;
        Raw32(static_cast<uint32_t>(n));
        m_buf.reserve(m_buf.size() + n * 8);
        for (size_t i = 0; i < n; ++i) Raw64(static_cast<uint64_t>(v[i]));
    }

    void Writer::LongArray(std::string_view name, const uint64_t* v, size_t n) {
        if (n > 0x7FFFFFFFu) { Fail("long array too long"); return; }
        if (!Header(TagType::LongArray, name)) return;
        Raw32(static_cast<uint32_t>(n));
        m_buf.reserve(m_buf.size() + n * 8);
        for (size_t i = 0; i < n; ++i) Raw64(v[i]);
    }

    void Writer::ListByteArray(ListScope& s, const int8_t* v, size_t n) {
        if (n > 0x7FFFFFFFu) { Fail("list byte array too long"); return; }
        if (!ListElem(s, TagType::ByteArray)) return;
        Raw32(static_cast<uint32_t>(n));
        m_buf.insert(m_buf.end(), reinterpret_cast<const uint8_t*>(v),
                     reinterpret_cast<const uint8_t*>(v) + n);
    }

    void Writer::ListIntArray(ListScope& s, const int32_t* v, size_t n) {
        if (n > 0x7FFFFFFFu) { Fail("list int array too long"); return; }
        if (!ListElem(s, TagType::IntArray)) return;
        Raw32(static_cast<uint32_t>(n));
        for (size_t i = 0; i < n; ++i) Raw32(static_cast<uint32_t>(v[i]));
    }

    void Writer::ListLongArray(ListScope& s, const int64_t* v, size_t n) {
        if (n > 0x7FFFFFFFu) { Fail("list long array too long"); return; }
        if (!ListElem(s, TagType::LongArray)) return;
        Raw32(static_cast<uint32_t>(n));
        for (size_t i = 0; i < n; ++i) Raw64(static_cast<uint64_t>(v[i]));
    }

    Writer::ListScope Writer::BeginList(std::string_view name, TagType elem) {
        ListScope s;
        if (m_scopes.size() >= kMaxDepth) { Fail("nesting too deep"); return s; }
        if (!Header(TagType::List, name)) return s;
        s.elem       = elem;
        s.typeOffset = m_buf.size();
        Raw8(static_cast<uint8_t>(elem));
        s.countOffset = m_buf.size();
        Raw32(0);                        // back-patched by EndList
        m_scopes.push_back('L');
        s.depth = m_scopes.size();
        return s;
    }

    Writer::ListScope Writer::ListListBegin(ListScope& parent, TagType elem) {
        ListScope s;
        if (!m_ok) return s;
        if (m_scopes.empty() || m_scopes.back() != 'L' || m_scopes.size() != parent.depth) {
            Fail("nested list opened outside its parent list"); return s;
        }
        if (parent.elem != TagType::List) { Fail("nested list in a non-list list"); return s; }
        if (m_scopes.size() >= kMaxDepth) { Fail("nesting too deep"); return s; }
        ++parent.count;
        s.elem       = elem;
        s.typeOffset = m_buf.size();     // an element carries no type byte or name
        Raw8(static_cast<uint8_t>(elem));
        s.countOffset = m_buf.size();
        Raw32(0);
        m_scopes.push_back('L');
        s.depth = m_scopes.size();
        return s;
    }

    void Writer::EndList(ListScope& s) {
        if (!m_ok) return;
        if (m_scopes.empty() || m_scopes.back() != 'L' || m_scopes.size() != s.depth) {
            Fail("EndList does not match its BeginList"); return;
        }
        // Vanilla's ListTag.identifyRawElementType returns 0 for an empty
        // list, so an empty list on disk declares element type TAG_End. Match
        // that or a byte comparison against a vanilla file diverges on every
        // empty block_entities / block_ticks / PostProcessing list.
        if (s.count == 0) m_buf[s.typeOffset] = static_cast<uint8_t>(TagType::End);
        Patch32(s.countOffset, static_cast<uint32_t>(s.count));
        m_scopes.pop_back();
    }

    void Writer::ListCompoundBegin(ListScope& s) {
        if (!m_ok) return;
        if (m_scopes.empty() || m_scopes.back() != 'L' || m_scopes.size() != s.depth) {
            Fail("list element outside its list"); return;
        }
        if (s.elem != TagType::Compound) { Fail("compound element in a non-compound list"); return; }
        if (m_scopes.size() >= kMaxDepth) { Fail("nesting too deep"); return; }
        ++s.count;
        m_scopes.push_back('C');
    }

    void Writer::ListCompoundEnd(ListScope& s) {
        if (!m_ok) return;
        if (m_scopes.size() < 2 || m_scopes.back() != 'C' || m_scopes.size() != s.depth + 1) {
            Fail("ListCompoundEnd does not match its Begin"); return;
        }
        Raw8(0);
        m_scopes.pop_back();
    }

    bool Writer::ListElem(ListScope& s, TagType want) {
        if (!m_ok) return false;
        if (m_scopes.empty() || m_scopes.back() != 'L' || m_scopes.size() != s.depth) {
            Fail("list element outside its list"); return false;
        }
        if (s.elem != want) { Fail("list element does not match the list's declared type"); return false; }
        ++s.count;
        return true;
    }

    void Writer::ListString(ListScope& s, std::string_view v) {
        std::vector<uint8_t> encoded;
        if (!EncodeModifiedUtf8(v, encoded)) { Fail("list string is not encodable"); return; }
        if (!ListElem(s, TagType::String)) return;
        Raw16(static_cast<uint16_t>(encoded.size()));
        m_buf.insert(m_buf.end(), encoded.begin(), encoded.end());
    }

    void Writer::ListByte(ListScope& s, int8_t v) {
        if (!ListElem(s, TagType::Byte)) return;
        Raw8(static_cast<uint8_t>(v));
    }

    void Writer::ListShort(ListScope& s, int16_t v) {
        if (!ListElem(s, TagType::Short)) return;
        Raw16(static_cast<uint16_t>(v));
    }

    void Writer::ListInt(ListScope& s, int32_t v) {
        if (!ListElem(s, TagType::Int)) return;
        Raw32(static_cast<uint32_t>(v));
    }

    void Writer::ListLong(ListScope& s, int64_t v) {
        if (!ListElem(s, TagType::Long)) return;
        Raw64(static_cast<uint64_t>(v));
    }

    void Writer::ListFloat(ListScope& s, float v) {
        if (!ListElem(s, TagType::Float)) return;
        uint32_t bits; std::memcpy(&bits, &v, sizeof(bits));
        Raw32(bits);
    }

    void Writer::ListDouble(ListScope& s, double v) {
        if (!ListElem(s, TagType::Double)) return;
        uint64_t bits; std::memcpy(&bits, &v, sizeof(bits));
        Raw64(bits);
    }

} // namespace Game::Nbt
