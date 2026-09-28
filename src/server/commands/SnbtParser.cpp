// File: src/server/commands/SnbtParser.cpp
#include "SnbtParser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>

namespace Server::Snbt {

    namespace {

        using ::World::NBTTagPtr;
        using ::World::NBTTagType;

        // MC TagParser's depth cap (NbtAccounter's max depth).
        constexpr int kMaxDepth = 512;

        bool IsUnquotedKeyChar(char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '+';
        }
        // A value word may also carry a namespace ("minecraft:ashen"): a
        // player typing an id unquoted means the id.
        bool IsUnquotedValueChar(char c) { return IsUnquotedKeyChar(c) || c == ':'; }

        std::string Lower(std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // ── numbers ────────────────────────────────────────────────────────

        enum class IntKind : uint8_t { Byte, Short, Int, Long };

        // A whole-token integer literal: [+-] (0x hex | 0b binary | decimal)
        // [u|s](b|s|i|l). nullopt when the token is not integer-shaped;
        // `rangeError` set when it is but does not fit.
        std::optional<NBTTagPtr> ParseInteger(const std::string& token, std::string& rangeError) {
            std::string t = Lower(token);
            t.erase(std::remove(t.begin(), t.end(), '_'), t.end());
            if (t.empty()) return std::nullopt;

            size_t i = 0;
            bool negative = false;
            if (t[i] == '+' || t[i] == '-') { negative = t[i] == '-'; ++i; }
            if (i >= t.size()) return std::nullopt;

            int base = 10;
            if (t.size() - i > 2 && t[i] == '0' && t[i + 1] == 'x') { base = 16; i += 2; }
            else if (t.size() - i > 2 && t[i] == '0' && t[i + 1] == 'b' &&
                     (t[i + 2] == '0' || t[i + 2] == '1')) { base = 2; i += 2; }

            // Suffix: the type letter, optionally preceded by signedness.
            std::string body = t.substr(i);
            IntKind kind = IntKind::Int;
            std::optional<bool> isSigned;
            auto takeType = [&](char c) {
                switch (c) {
                    case 'b': kind = IntKind::Byte;  return true;
                    case 's': kind = IntKind::Short; return true;
                    case 'i': kind = IntKind::Int;   return true;
                    case 'l': kind = IntKind::Long;  return true;
                    default:  return false;
                }
            };
            if (!body.empty()) {
                const char last = body.back();
                // In hex, 'b' is a digit: a byte suffix there needs its
                // signedness letter in front ("0x7fsb").
                const bool hexDigit = base == 16 && std::isxdigit(static_cast<unsigned char>(last));
                const bool preSigned = body.size() >= 2 && (body[body.size() - 2] == 'u' || body[body.size() - 2] == 's');
                if ((!hexDigit || preSigned) && takeType(last)) {
                    body.pop_back();
                    if (!body.empty() && (body.back() == 'u' || body.back() == 's')) {
                        // "1s" alone is a short, and in "1ss" the first 's' is
                        // the signedness.
                        if (body.size() >= 2) {
                            isSigned = body.back() == 's';
                            body.pop_back();
                        }
                    }
                }
            }
            if (body.empty()) return std::nullopt;

            uint64_t magnitude = 0;
            for (char c : body) {
                int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
                else return std::nullopt;
                if (d >= base) return std::nullopt;
                if (magnitude > (std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(d)) / static_cast<uint64_t>(base)) {
                    rangeError = "Number out of range: " + token;
                    return std::nullopt;
                }
                magnitude = magnitude * static_cast<uint64_t>(base) + static_cast<uint64_t>(d);
            }

            // Hex and binary literals read unsigned by default (0xFFFFFFFF is
            // -1 as an int); decimal ones signed.
            const bool unsignedForm = isSigned ? !*isSigned : base != 10;
            int bits = 32;
            switch (kind) {
                case IntKind::Byte:  bits = 8;  break;
                case IntKind::Short: bits = 16; break;
                case IntKind::Int:   bits = 32; break;
                case IntKind::Long:  bits = 64; break;
            }
            int64_t value = 0;
            if (unsignedForm) {
                if (negative) { rangeError = "Unsigned number can't be negative: " + token; return std::nullopt; }
                const uint64_t max = bits == 64 ? std::numeric_limits<uint64_t>::max() : ((uint64_t{1} << bits) - 1);
                if (magnitude > max) { rangeError = "Number out of range: " + token; return std::nullopt; }
                value = static_cast<int64_t>(magnitude);   // wraps into the signed type below
            } else {
                const uint64_t maxPos = (uint64_t{1} << (bits - 1)) - 1;
                if (negative ? magnitude > maxPos + 1 : magnitude > maxPos) {
                    rangeError = "Number out of range: " + token;
                    return std::nullopt;
                }
                value = negative ? static_cast<int64_t>(0 - magnitude) : static_cast<int64_t>(magnitude);
            }

            switch (kind) {
                case IntKind::Byte:  return std::make_shared<::World::NBTTagByte>(static_cast<int8_t>(value));
                case IntKind::Short: return std::make_shared<::World::NBTTagShort>(static_cast<int16_t>(value));
                case IntKind::Int:   return std::make_shared<::World::NBTTagInt>(static_cast<int32_t>(value));
                case IntKind::Long:  return std::make_shared<::World::NBTTagLong>(value);
            }
            return std::nullopt;
        }

        // [+-] digits [. digits] [e [+-] digits] [f|d]; at least one digit.
        std::optional<NBTTagPtr> ParseFloating(const std::string& token) {
            std::string t = Lower(token);
            t.erase(std::remove(t.begin(), t.end(), '_'), t.end());
            if (t.empty()) return std::nullopt;
            bool isFloat = false;
            bool explicitType = false;
            if (t.back() == 'f' || t.back() == 'd') {
                isFloat = t.back() == 'f';
                explicitType = true;
                t.pop_back();
            }
            // Shape check before strtod, which would accept "inf", "nan", hex.
            size_t i = 0, digits = 0;
            if (i < t.size() && (t[i] == '+' || t[i] == '-')) ++i;
            while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) { ++i; ++digits; }
            bool fractional = false;
            if (i < t.size() && t[i] == '.') {
                fractional = true;
                ++i;
                while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) { ++i; ++digits; }
            }
            if (digits == 0) return std::nullopt;
            if (i < t.size() && t[i] == 'e') {
                fractional = true;
                ++i;
                if (i < t.size() && (t[i] == '+' || t[i] == '-')) ++i;
                size_t expDigits = 0;
                while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) { ++i; ++expDigits; }
                if (expDigits == 0) return std::nullopt;
            }
            if (i != t.size()) return std::nullopt;
            // A bare integer is an int (ParseInteger's job) unless typed f/d.
            if (!fractional && !explicitType) return std::nullopt;
            const double v = std::strtod(t.c_str(), nullptr);
            if (!std::isfinite(v)) return std::nullopt;
            if (isFloat) return std::make_shared<::World::NBTTagFloat>(static_cast<float>(v));
            return std::make_shared<::World::NBTTagDouble>(v);
        }

        // ── the reader ─────────────────────────────────────────────────────

        class Reader {
        public:
            explicit Reader(const std::string& s) : m_s(s) {}

            NBTTagPtr Value(int depth) {
                SkipWs();
                if (depth > kMaxDepth) return Fail("Tag nested too deeply");
                if (AtEnd()) return Fail("Expected value");
                const char c = m_s[m_i];
                if (c == '{') return Compound(depth);
                if (c == '[') return List(depth);
                if (c == '"' || c == '\'') {
                    std::string text;
                    if (!Quoted(text)) return nullptr;
                    return std::make_shared<::World::NBTTagString>(text);
                }
                const size_t start = m_i;
                while (!AtEnd() && IsUnquotedValueChar(m_s[m_i])) ++m_i;
                if (m_i == start) return Fail("Expected value");
                return Word(m_s.substr(start, m_i - start), start);
            }

            bool AtEndAfterWs() { SkipWs(); return AtEnd(); }
            const std::string& Error() const { return m_error; }
            bool Failed() const { return !m_error.empty(); }
            NBTTagPtr FailAt(const std::string& what) { return Fail(what); }

        private:
            bool AtEnd() const { return m_i >= m_s.size(); }
            void SkipWs() { while (!AtEnd() && std::isspace(static_cast<unsigned char>(m_s[m_i]))) ++m_i; }

            // MC CommandSyntaxException: "<what> at position N: <ctx><--[HERE]".
            NBTTagPtr Fail(const std::string& what) {
                if (m_error.empty()) {
                    const size_t from = m_i > 10 ? m_i - 10 : 0;
                    const size_t to = std::min(m_i, m_s.size());
                    m_error = what + " at position " + std::to_string(m_i) + ": " +
                              (from > 0 ? "..." : "") + m_s.substr(from, to - from) + "<--[HERE]";
                }
                return nullptr;
            }

            bool Expect(char c) {
                SkipWs();
                if (!AtEnd() && m_s[m_i] == c) { ++m_i; return true; }
                Fail(std::string("Expected '") + c + "'");
                return false;
            }

            bool Quoted(std::string& out) {
                const char quote = m_s[m_i++];
                while (!AtEnd()) {
                    const char c = m_s[m_i++];
                    if (c == quote) return true;
                    if (c != '\\') { out += c; continue; }
                    if (AtEnd()) break;
                    const char e = m_s[m_i++];
                    switch (e) {
                        case '\\': out += '\\'; break;
                        case '"':  out += '"';  break;
                        case '\'': out += '\''; break;
                        case 'n':  out += '\n'; break;
                        case 't':  out += '\t'; break;
                        case 'r':  out += '\r'; break;
                        case 'b':  out += '\b'; break;
                        case 'f':  out += '\f'; break;
                        case 's':  out += ' ';  break;
                        case 'u': {
                            if (m_i + 4 > m_s.size()) { Fail("Invalid unicode escape"); return false; }
                            unsigned cp = 0;
                            for (int k = 0; k < 4; ++k) {
                                const char h = m_s[m_i++];
                                cp <<= 4;
                                if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                                else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(10 + h - 'a');
                                else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(10 + h - 'A');
                                else { Fail("Invalid unicode escape"); return false; }
                            }
                            // UTF-8 encode (BMP only; surrogates pass through
                            // as their own code units, as Java strings hold them).
                            if (cp < 0x80) out += static_cast<char>(cp);
                            else if (cp < 0x800) {
                                out += static_cast<char>(0xC0 | (cp >> 6));
                                out += static_cast<char>(0x80 | (cp & 0x3F));
                            } else {
                                out += static_cast<char>(0xE0 | (cp >> 12));
                                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                                out += static_cast<char>(0x80 | (cp & 0x3F));
                            }
                            break;
                        }
                        default:
                            --m_i;
                            Fail(std::string("Invalid escape sequence '\\") + e + "' in quoted string");
                            return false;
                    }
                }
                Fail("Unclosed quoted string");
                return false;
            }

            NBTTagPtr Word(const std::string& word, size_t start) {
                std::string rangeError;
                if (auto integer = ParseInteger(word, rangeError)) return *integer;
                if (!rangeError.empty()) { m_i = start; return Fail(rangeError); }
                if (auto floating = ParseFloating(word)) return *floating;
                if (word == "true")  return std::make_shared<::World::NBTTagByte>(static_cast<int8_t>(1));
                if (word == "false") return std::make_shared<::World::NBTTagByte>(static_cast<int8_t>(0));
                return std::make_shared<::World::NBTTagString>(word);
            }

            NBTTagPtr Compound(int depth) {
                ++m_i;   // '{'
                auto out = std::make_shared<::World::NBTTagCompound>();
                SkipWs();
                if (!AtEnd() && m_s[m_i] == '}') { ++m_i; return out; }
                while (true) {
                    SkipWs();
                    if (AtEnd()) return Fail("Expected key");
                    std::string key;
                    if (m_s[m_i] == '"' || m_s[m_i] == '\'') {
                        if (!Quoted(key)) return nullptr;
                    } else {
                        const size_t start = m_i;
                        while (!AtEnd() && IsUnquotedKeyChar(m_s[m_i])) ++m_i;
                        if (m_i == start) return Fail("Expected key");
                        key = m_s.substr(start, m_i - start);
                    }
                    if (!Expect(':')) return nullptr;
                    NBTTagPtr v = Value(depth + 1);
                    if (!v) return nullptr;
                    v->name = key;
                    out->value[key] = std::move(v);
                    SkipWs();
                    if (!AtEnd() && m_s[m_i] == ',') {
                        ++m_i;
                        SkipWs();
                        // A trailing comma before '}' is accepted, as MC.
                        if (!AtEnd() && m_s[m_i] == '}') { ++m_i; return out; }
                        continue;
                    }
                    if (!Expect('}')) return nullptr;
                    return out;
                }
            }

            template <typename ArrayTag, typename Elem>
            NBTTagPtr TypedArray(char kindLetter) {
                auto out = std::make_shared<ArrayTag>();
                SkipWs();
                if (!AtEnd() && m_s[m_i] == ']') { ++m_i; return out; }
                while (true) {
                    SkipWs();
                    const size_t start = m_i;
                    while (!AtEnd() && IsUnquotedValueChar(m_s[m_i])) ++m_i;
                    const std::string word = m_s.substr(start, m_i - start);
                    std::string rangeError;
                    auto tag = word.empty() ? std::nullopt : ParseInteger(word, rangeError);
                    if (!tag) {
                        m_i = start;
                        return Fail(rangeError.empty()
                                        ? std::string("Invalid array element type for [") + kindLetter + ";...]"
                                        : rangeError);
                    }
                    int64_t v = 0;
                    switch ((*tag)->type) {
                        case NBTTagType::TAG_Byte:  v = static_cast<::World::NBTTagByte&>(**tag).value;  break;
                        case NBTTagType::TAG_Short: v = static_cast<::World::NBTTagShort&>(**tag).value; break;
                        case NBTTagType::TAG_Int:   v = static_cast<::World::NBTTagInt&>(**tag).value;   break;
                        case NBTTagType::TAG_Long:  v = static_cast<::World::NBTTagLong&>(**tag).value;  break;
                        default: break;
                    }
                    if (v < static_cast<int64_t>(std::numeric_limits<Elem>::min()) ||
                        v > static_cast<int64_t>(std::numeric_limits<Elem>::max())) {
                        m_i = start;
                        return Fail("Number out of range: " + word);
                    }
                    out->value.push_back(static_cast<Elem>(v));
                    SkipWs();
                    if (!AtEnd() && m_s[m_i] == ',') { ++m_i; continue; }
                    if (!Expect(']')) return nullptr;
                    return out;
                }
            }

            NBTTagPtr List(int depth) {
                ++m_i;   // '['
                SkipWs();
                if (m_i + 1 < m_s.size() && m_s[m_i + 1] == ';') {
                    const char kind = static_cast<char>(std::toupper(static_cast<unsigned char>(m_s[m_i])));
                    if (kind == 'B') { m_i += 2; return TypedArray<::World::NBTTagByteArray, int8_t>('B'); }
                    if (kind == 'I') { m_i += 2; return TypedArray<::World::NBTTagIntArray, int32_t>('I'); }
                    if (kind == 'L') { m_i += 2; return TypedArray<::World::NBTTagLongArray, int64_t>('L'); }
                    return Fail(std::string("Invalid array type '") + m_s[m_i] + "'");
                }
                auto out = std::make_shared<::World::NBTTagList>();
                if (!AtEnd() && m_s[m_i] == ']') { ++m_i; return out; }
                while (true) {
                    NBTTagPtr v = Value(depth + 1);
                    if (!v) return nullptr;
                    if (out->value.empty()) out->listType = v->type;
                    out->value.push_back(std::move(v));
                    SkipWs();
                    if (!AtEnd() && m_s[m_i] == ',') {
                        ++m_i;
                        SkipWs();
                        if (!AtEnd() && m_s[m_i] == ']') { ++m_i; return out; }
                        continue;
                    }
                    if (!Expect(']')) return nullptr;
                    return out;
                }
            }

            const std::string& m_s;
            size_t             m_i = 0;
            std::string        m_error;
        };

    } // namespace

    ::World::NBTTagPtr ParseValue(const std::string& text, std::string& error) {
        Reader reader(text);
        NBTTagPtr value = reader.Value(0);
        if (value && !reader.AtEndAfterWs()) {
            reader.FailAt("Unexpected trailing data");
            value = nullptr;
        }
        if (!value) error = reader.Error().empty() ? "Invalid NBT" : reader.Error();
        return value;
    }

    std::shared_ptr<::World::NBTTagCompound> ParseCompound(const std::string& text, std::string& error) {
        const size_t first = text.find_first_not_of(" \t");
        if (first == std::string::npos || text[first] != '{') {
            error = "Expected '{' at position " + std::to_string(first == std::string::npos ? text.size() : first) +
                    ": <--[HERE]";
            return nullptr;
        }
        NBTTagPtr value = ParseValue(text, error);
        if (!value) return nullptr;
        return std::dynamic_pointer_cast<::World::NBTTagCompound>(value);
    }

} // namespace Server::Snbt
