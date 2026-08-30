// File: src/common/nbt/ModifiedUtf8.hpp
//
// Java's "modified UTF-8", which is what every NBT string actually is.
//
// Vanilla reads strings with DataInput.readUTF and writes them with
// writeUTF. That encoding is NOT UTF-8. Two differences matter:
//
//   * U+0000 is written as the two bytes C0 80, never as a bare NUL.
//   * A scalar above the BMP is written as a SURROGATE PAIR of two
//     three-byte sequences (six bytes total), not as one four-byte
//     sequence.
//
// An emoji in a world name, on a sign, or in an item's custom name is the
// ordinary way to meet this. Treating the bytes as UTF-8 in either
// direction produces a file real Minecraft cannot read, or a string that
// re-encodes into something different from what came in.
//
// Header-only and dependency-free on purpose: the reader (NBTParser) is
// compiled into the launcher, which must not gain a link dependency on the
// save stack just to decode a level.dat string.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Game::Nbt {

    namespace detail {

        // Decode one UTF-8 scalar. Returns the byte length consumed, or 0 for
        // malformed input. Rejects overlong forms and surrogate code points,
        // both of which would otherwise re-encode into something a Java reader
        // reads back differently.
        inline size_t DecodeUtf8Scalar(const uint8_t* p, size_t avail, uint32_t& cp) {
            const uint8_t b0 = p[0];
            if (b0 < 0x80) { cp = b0; return 1; }

            if ((b0 & 0xE0) == 0xC0) {
                if (avail < 2 || (p[1] & 0xC0) != 0x80) return 0;
                cp = (uint32_t(b0 & 0x1F) << 6) | uint32_t(p[1] & 0x3F);
                return cp < 0x80 ? 0 : 2;                       // overlong
            }
            if ((b0 & 0xF0) == 0xE0) {
                if (avail < 3 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
                cp = (uint32_t(b0 & 0x0F) << 12) | (uint32_t(p[1] & 0x3F) << 6) | uint32_t(p[2] & 0x3F);
                if (cp < 0x800) return 0;                       // overlong
                if (cp >= 0xD800 && cp <= 0xDFFF) return 0;      // lone surrogate
                return 3;
            }
            if ((b0 & 0xF8) == 0xF0) {
                if (avail < 4 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return 0;
                cp = (uint32_t(b0 & 0x07) << 18) | (uint32_t(p[1] & 0x3F) << 12)
                   | (uint32_t(p[2] & 0x3F) << 6) | uint32_t(p[3] & 0x3F);
                if (cp < 0x10000 || cp > 0x10FFFF) return 0;
                return 4;
            }
            return 0;
        }

        inline void AppendUtf8Scalar(uint32_t cp, std::string& out) {
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }

        // Three-byte form, used for every BMP scalar and for each half of a
        // surrogate pair.
        inline void AppendThreeByte(uint32_t v, std::vector<uint8_t>& out) {
            out.push_back(static_cast<uint8_t>(0xE0 | ((v >> 12) & 0x0F)));
            out.push_back(static_cast<uint8_t>(0x80 | ((v >> 6) & 0x3F)));
            out.push_back(static_cast<uint8_t>(0x80 | (v & 0x3F)));
        }

    } // namespace detail

    inline bool EncodeModifiedUtf8(std::string_view utf8, std::vector<uint8_t>& out) {
        const auto* p   = reinterpret_cast<const uint8_t*>(utf8.data());
        const size_t n  = utf8.size();

        // Almost every string we write is a registry slug or a property name:
        // pure ASCII with no NUL, where modified UTF-8 and UTF-8 agree byte
        // for byte. Scan first so the common case is a straight copy.
        bool plainAscii = true;
        for (size_t i = 0; i < n; ++i) {
            if (p[i] == 0x00 || p[i] >= 0x80) { plainAscii = false; break; }
        }
        if (plainAscii) {
            if (n > 65535) return false;
            out.assign(p, p + n);
            return true;
        }

        std::vector<uint8_t> tmp;
        tmp.reserve(n + n / 2);
        for (size_t i = 0; i < n; ) {
            uint32_t cp = 0;
            const size_t used = detail::DecodeUtf8Scalar(p + i, n - i, cp);
            if (used == 0) return false;
            i += used;

            if (cp == 0x0000) {                 // NUL -> C0 80, never a bare 00
                tmp.push_back(0xC0);
                tmp.push_back(0x80);
            } else if (cp < 0x80) {
                tmp.push_back(static_cast<uint8_t>(cp));
            } else if (cp < 0x800) {
                tmp.push_back(static_cast<uint8_t>(0xC0 | (cp >> 6)));
                tmp.push_back(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                detail::AppendThreeByte(cp, tmp);
            } else {                            // supplementary -> surrogate pair
                const uint32_t v  = cp - 0x10000;
                detail::AppendThreeByte(0xD800 + (v >> 10), tmp);
                detail::AppendThreeByte(0xDC00 + (v & 0x3FF), tmp);
            }
            if (tmp.size() > 65535) return false;
        }
        out = std::move(tmp);
        return true;
    }

    inline bool DecodeModifiedUtf8(const uint8_t* data, size_t length, std::string& out) {
        std::string result;
        result.reserve(length);

        for (size_t i = 0; i < length; ) {
            const uint8_t b0 = data[i];
            uint32_t cp = 0;

            if (b0 < 0x80) {
                // A bare NUL is not legal in modified UTF-8, but accepting it
                // costs nothing and a foreign writer may have emitted one.
                cp = b0; i += 1;
            } else if ((b0 & 0xE0) == 0xC0) {
                if (i + 1 >= length || (data[i + 1] & 0xC0) != 0x80) return false;
                cp = (uint32_t(b0 & 0x1F) << 6) | uint32_t(data[i + 1] & 0x3F);
                i += 2;
            } else if ((b0 & 0xF0) == 0xE0) {
                if (i + 2 >= length || (data[i + 1] & 0xC0) != 0x80 || (data[i + 2] & 0xC0) != 0x80) return false;
                cp = (uint32_t(b0 & 0x0F) << 12) | (uint32_t(data[i + 1] & 0x3F) << 6)
                   | uint32_t(data[i + 2] & 0x3F);
                i += 3;

                // A high surrogate must be followed by its low half; together
                // they are one supplementary scalar. This is the whole reason
                // modified UTF-8 needs its own decoder.
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 2 < length
                    && (data[i] & 0xF0) == 0xE0
                    && (data[i + 1] & 0xC0) == 0x80 && (data[i + 2] & 0xC0) == 0x80) {
                    const uint32_t lo = (uint32_t(data[i] & 0x0F) << 12)
                                      | (uint32_t(data[i + 1] & 0x3F) << 6)
                                      | uint32_t(data[i + 2] & 0x3F);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 3;
                    }
                }
            } else {
                // 4-byte UTF-8 never appears in modified UTF-8.
                return false;
            }
            detail::AppendUtf8Scalar(cp, result);
        }
        out = std::move(result);
        return true;
    }

} // namespace Game::Nbt
