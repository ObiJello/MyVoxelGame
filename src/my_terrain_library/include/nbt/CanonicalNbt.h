#pragma once

#include "nbt/AllTags.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Canonical single-line NBT serialization for parity E lines.
// Reference: tests/parity/FORMAT.md "E block-entity lines" - a spec
// implemented identically in MinecraftAsyncChunkTest.java; NOT SNBT.
//  - Compound: {key:value,...}, keys sorted bytewise ascending; a key is raw
//    if it matches [A-Za-z0-9_.+-]+ else double-quoted with \\ and \" escapes.
//    The TOP-LEVEL compound drops keys x, y, z.
//  - byte <n>b, short <n>s, int <n>, long <n>l (decimal).
//  - float f0x<8 hex>, double d0x<16 hex> - IEEE-754 bit patterns, lowercase,
//    zero-padded (never shortest-round-trip text).
//  - String: double-quoted, escaping only \\ and \"; raw UTF-8 otherwise.
//  - List [v1,v2,...]; arrays [B;1b,...], [I;1,...], [L;1l,...]; empty [B;].

namespace minecraft {
namespace nbt {
namespace canonical {

inline bool isRawKey(const std::string& key) {
    if (key.empty()) return false;
    for (char c : key) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
              || (c >= '0' && c <= '9') || c == '_' || c == '.'
              || c == '+' || c == '-')) {
            return false;
        }
    }
    return true;
}

inline void appendQuoted(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    out += '"';
}

inline void appendTag(std::string& out, const Tag* tag, bool topLevel = false);

inline void appendCompound(std::string& out, const CompoundTag* compound,
                           bool topLevel) {
    std::vector<std::pair<std::string, const Tag*>> entries;
    for (const auto& [key, value] : *compound) {
        if (topLevel && (key == "x" || key == "y" || key == "z")) continue;
        entries.emplace_back(key, value.get());
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    out += '{';
    bool first = true;
    for (const auto& [key, value] : entries) {
        if (!first) out += ',';
        first = false;
        if (isRawKey(key)) out += key;
        else appendQuoted(out, key);
        out += ':';
        appendTag(out, value);
    }
    out += '}';
}

inline void appendTag(std::string& out, const Tag* tag, bool topLevel) {
    char buf[32];
    switch (tag->getId()) {
        case TagType::TAG_BYTE:
            out += std::to_string(static_cast<const ByteTag*>(tag)->getValue());
            out += 'b';
            break;
        case TagType::TAG_SHORT:
            out += std::to_string(static_cast<const ShortTag*>(tag)->getValue());
            out += 's';
            break;
        case TagType::TAG_INT:
            out += std::to_string(static_cast<const IntTag*>(tag)->getValue());
            break;
        case TagType::TAG_LONG:
            out += std::to_string(static_cast<const LongTag*>(tag)->getValue());
            out += 'l';
            break;
        case TagType::TAG_FLOAT: {
            float v = static_cast<const FloatTag*>(tag)->getValue();
            uint32_t bits;
            std::memcpy(&bits, &v, sizeof(bits));
            std::snprintf(buf, sizeof(buf), "f0x%08x", bits);
            out += buf;
            break;
        }
        case TagType::TAG_DOUBLE: {
            double v = static_cast<const DoubleTag*>(tag)->getValue();
            uint64_t bits;
            std::memcpy(&bits, &v, sizeof(bits));
            std::snprintf(buf, sizeof(buf), "d0x%016llx",
                          static_cast<unsigned long long>(bits));
            out += buf;
            break;
        }
        case TagType::TAG_STRING:
            appendQuoted(out, static_cast<const StringTag*>(tag)->getValue());
            break;
        case TagType::TAG_LIST: {
            const ListTag* list = static_cast<const ListTag*>(tag)->asList();
            out += '[';
            for (size_t i = 0; i < list->size(); ++i) {
                if (i) out += ',';
                appendTag(out, list->get(i));
            }
            out += ']';
            break;
        }
        case TagType::TAG_COMPOUND:
            appendCompound(out, tag->asCompound(), topLevel);
            break;
        case TagType::TAG_BYTE_ARRAY: {
            auto values = *tag->asByteArray();
            out += "[B;";
            for (size_t i = 0; i < values.size(); ++i) {
                if (i) out += ',';
                out += std::to_string(values[i]);
                out += 'b';
            }
            out += ']';
            break;
        }
        case TagType::TAG_INT_ARRAY: {
            auto values = *tag->asIntArray();
            out += "[I;";
            for (size_t i = 0; i < values.size(); ++i) {
                if (i) out += ',';
                out += std::to_string(values[i]);
            }
            out += ']';
            break;
        }
        case TagType::TAG_LONG_ARRAY: {
            auto values = *tag->asLongArray();
            out += "[L;";
            for (size_t i = 0; i < values.size(); ++i) {
                if (i) out += ',';
                out += std::to_string(values[i]);
                out += 'l';
            }
            out += ']';
            break;
        }
        default:
            out += "END";
            break;
    }
}

/** Serialize a block-entity compound as the E-line payload (top-level x/y/z
 *  dropped). */
inline std::string serializeBlockEntity(const CompoundTag& compound) {
    std::string out;
    appendCompound(out, &compound, /*topLevel=*/true);
    return out;
}

// ---------------------------------------------------------------------------
// Reading: the exact inverse of appendTag, so a payload round-trips bit for
// bit. Proto chunks keep pending block-entity tags as canonical strings and
// the chunk serializer turns them back into NBT. Throws std::runtime_error
// on malformed input.
// ---------------------------------------------------------------------------

class Reader {
public:
    explicit Reader(const std::string& text) : m_text(text) {}

    std::unique_ptr<Tag> readValue() {
        skipSpace();
        const char c = peek();
        if (c == '{') return readCompound();
        if (c == '[') return readListOrArray();
        if (c == '"') return std::make_unique<StringTag>(readQuoted());
        if (c == 'f' && startsWith("f0x")) {
            m_pos += 3;
            const uint32_t bits = static_cast<uint32_t>(readHex(8));
            float v;
            std::memcpy(&v, &bits, sizeof(v));
            return std::make_unique<FloatTag>(v);
        }
        if (c == 'd' && startsWith("d0x")) {
            m_pos += 3;
            const uint64_t bits = readHex(16);
            double v;
            std::memcpy(&v, &bits, sizeof(v));
            return std::make_unique<DoubleTag>(v);
        }
        const int64_t n = readInteger();
        switch (peekOr('\0')) {
            case 'b': ++m_pos; return std::make_unique<ByteTag>(static_cast<int8_t>(n));
            case 's': ++m_pos; return std::make_unique<ShortTag>(static_cast<int16_t>(n));
            case 'l': ++m_pos; return std::make_unique<LongTag>(n);
            default: return std::make_unique<IntTag>(static_cast<int32_t>(n));
        }
    }

    bool atEnd() {
        skipSpace();
        return m_pos >= m_text.size();
    }

private:
    const std::string& m_text;
    size_t m_pos = 0;

    [[noreturn]] void fail(const char* what) const {
        throw std::runtime_error(std::string("canonical nbt: ") + what + " at offset "
                                 + std::to_string(m_pos));
    }
    char peek() const {
        if (m_pos >= m_text.size()) fail("unexpected end");
        return m_text[m_pos];
    }
    char peekOr(char fallback) const { return m_pos < m_text.size() ? m_text[m_pos] : fallback; }
    bool startsWith(const char* prefix) const { return m_text.compare(m_pos, std::strlen(prefix), prefix) == 0; }
    void skipSpace() {
        while (m_pos < m_text.size() && (m_text[m_pos] == ' ' || m_text[m_pos] == '\n')) ++m_pos;
    }
    void expect(char c) {
        skipSpace();
        if (peek() != c) fail("unexpected character");
        ++m_pos;
    }

    std::string readQuoted() {
        expect('"');
        std::string out;
        while (true) {
            char c = peek();
            ++m_pos;
            if (c == '"') return out;
            if (c == '\\') {
                c = peek();
                ++m_pos;
            }
            out += c;
        }
    }

    std::string readKey() {
        skipSpace();
        if (peek() == '"') return readQuoted();
        const size_t start = m_pos;
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                  || c == '_' || c == '.' || c == '+' || c == '-')) {
                break;
            }
            ++m_pos;
        }
        if (m_pos == start) fail("empty key");
        return m_text.substr(start, m_pos - start);
    }

    int64_t readInteger() {
        skipSpace();
        bool negative = false;
        if (peek() == '-') { negative = true; ++m_pos; }
        if (!(peek() >= '0' && peek() <= '9')) fail("expected a number");
        uint64_t magnitude = 0;
        while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') {
            magnitude = magnitude * 10 + static_cast<uint64_t>(m_text[m_pos] - '0');
            ++m_pos;
        }
        return negative ? static_cast<int64_t>(0 - magnitude) : static_cast<int64_t>(magnitude);
    }

    uint64_t readHex(int digits) {
        uint64_t v = 0;
        for (int i = 0; i < digits; ++i) {
            const char c = peek();
            ++m_pos;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint64_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint64_t>(c - 'a' + 10);
            else fail("bad hex digit");
        }
        return v;
    }

    std::unique_ptr<Tag> readCompound() {
        expect('{');
        auto compound = std::make_unique<CompoundTag>();
        skipSpace();
        if (peek() == '}') { ++m_pos; return compound; }
        while (true) {
            std::string key = readKey();
            expect(':');
            compound->put(key, readValue());
            skipSpace();
            if (peek() == ',') { ++m_pos; continue; }
            expect('}');
            return compound;
        }
    }

    std::unique_ptr<Tag> readListOrArray() {
        expect('[');
        skipSpace();
        if ((peek() == 'B' || peek() == 'I' || peek() == 'L') && m_pos + 1 < m_text.size()
            && m_text[m_pos + 1] == ';') {
            const char kind = m_text[m_pos];
            m_pos += 2;
            std::vector<int64_t> values;
            skipSpace();
            while (peek() != ']') {
                values.push_back(readInteger());
                if (kind == 'B' && peekOr('\0') == 'b') ++m_pos;
                if (kind == 'L' && peekOr('\0') == 'l') ++m_pos;
                skipSpace();
                if (peek() == ',') ++m_pos;
                skipSpace();
            }
            ++m_pos;
            if (kind == 'B') {
                std::vector<int8_t> bytes(values.begin(), values.end());
                return std::make_unique<ByteArrayTag>(std::move(bytes));
            }
            if (kind == 'I') {
                std::vector<int32_t> ints(values.begin(), values.end());
                return std::make_unique<IntArrayTag>(std::move(ints));
            }
            return std::make_unique<LongArrayTag>(std::move(values));
        }
        auto list = std::make_unique<ListTag>();
        if (peek() == ']') { ++m_pos; return list; }
        while (true) {
            if (!list->add(readValue())) fail("mixed list element types");
            skipSpace();
            if (peek() == ',') { ++m_pos; continue; }
            expect(']');
            return list;
        }
    }
};

/** Parse a canonical payload (as written by serializeBlockEntity) back into
 *  a compound. Throws std::runtime_error when the text is not canonical NBT. */
inline std::unique_ptr<CompoundTag> parseCompound(const std::string& text) {
    Reader reader(text);
    std::unique_ptr<Tag> tag = reader.readValue();
    if (!reader.atEnd() || tag->getId() != TagType::TAG_COMPOUND) {
        throw std::runtime_error("canonical nbt: not a single compound");
    }
    return std::unique_ptr<CompoundTag>(static_cast<CompoundTag*>(tag.release()));
}

} // namespace canonical
} // namespace nbt
} // namespace minecraft
