#pragma once

#include "nbt/AllTags.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

} // namespace canonical
} // namespace nbt
} // namespace minecraft
