// File: src/server/commands/NbtPath.cpp
#include "NbtPath.hpp"
#include "SnbtParser.hpp"
#include "common/network/packets/game/ChatMessageS2CPacket.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <type_traits>

namespace Server::Nbt {

    using ::World::NBTTag;
    using ::World::NBTTagType;
    using T = ::World::NBTTagType;

    namespace {

        // ── Collections (MC CollectionTag: lists and the three arrays) ─────

        bool IsCollection(const NBTTag& tag) {
            return tag.type == T::TAG_List || tag.type == T::TAG_Byte_Array ||
                   tag.type == T::TAG_Int_Array || tag.type == T::TAG_Long_Array;
        }

        size_t CollectionSize(const NBTTag& tag) {
            switch (tag.type) {
                case T::TAG_List:       return static_cast<const ::World::NBTTagList&>(tag).value.size();
                case T::TAG_Byte_Array: return static_cast<const ::World::NBTTagByteArray&>(tag).value.size();
                case T::TAG_Int_Array:  return static_cast<const ::World::NBTTagIntArray&>(tag).value.size();
                case T::TAG_Long_Array: return static_cast<const ::World::NBTTagLongArray&>(tag).value.size();
                default:                return 0;
            }
        }

        // An element: the list's own tag, or (an array) a fresh numeric tag.
        TagPtr CollectionGet(const NBTTag& tag, size_t i) {
            switch (tag.type) {
                case T::TAG_List: return static_cast<const ::World::NBTTagList&>(tag).value[i];
                case T::TAG_Byte_Array:
                    return std::make_shared<::World::NBTTagByte>(static_cast<const ::World::NBTTagByteArray&>(tag).value[i]);
                case T::TAG_Int_Array:
                    return std::make_shared<::World::NBTTagInt>(static_cast<const ::World::NBTTagIntArray&>(tag).value[i]);
                case T::TAG_Long_Array:
                    return std::make_shared<::World::NBTTagLong>(static_cast<const ::World::NBTTagLongArray&>(tag).value[i]);
                default: return nullptr;
            }
        }

        // A list stays homogeneous on disk (the writer takes one element
        // type): a differing element is stored the way MC 26 writes a mixed
        // list — every element wrapped as {"": value}.
        TagPtr WrapForList(::World::NBTTagList& list, const TagPtr& value) {
            if (list.value.empty()) {
                list.listType = value->type;
                return value;
            }
            if (value->type == list.listType) return value;
            const auto wrap = [](const TagPtr& v) -> TagPtr {
                if (v->type == T::TAG_Compound) {
                    const auto& c = static_cast<const ::World::NBTTagCompound&>(*v);
                    if (c.value.size() == 1 && c.value.count("")) return v;
                }
                auto w = std::make_shared<::World::NBTTagCompound>();
                w->value[""] = v;
                return w;
            };
            if (list.listType != T::TAG_Compound) {
                for (TagPtr& e : list.value) e = wrap(e);
                list.listType = T::TAG_Compound;
            }
            return value->type == T::TAG_Compound ? value : wrap(value);
        }

        // MC CollectionTag.addTag(index, tag): false when the tag cannot go
        // in (an array takes numbers only). Throws std::out_of_range for a
        // bad index (MC's IndexOutOfBoundsException).
        bool CollectionAdd(NBTTag& tag, size_t index, const TagPtr& value) {
            if (index > CollectionSize(tag)) throw std::out_of_range("index");
            switch (tag.type) {
                case T::TAG_List: {
                    auto& list = static_cast<::World::NBTTagList&>(tag);
                    TagPtr v = WrapForList(list, value);
                    list.value.insert(list.value.begin() + static_cast<std::ptrdiff_t>(index), std::move(v));
                    return true;
                }
                case T::TAG_Byte_Array:
                    if (!IsNumeric(*value)) return false;
                    static_cast<::World::NBTTagByteArray&>(tag).value.insert(
                        static_cast<::World::NBTTagByteArray&>(tag).value.begin() + static_cast<std::ptrdiff_t>(index),
                        static_cast<int8_t>(static_cast<int64_t>(NumericValue(*value))));
                    return true;
                case T::TAG_Int_Array:
                    if (!IsNumeric(*value)) return false;
                    static_cast<::World::NBTTagIntArray&>(tag).value.insert(
                        static_cast<::World::NBTTagIntArray&>(tag).value.begin() + static_cast<std::ptrdiff_t>(index),
                        static_cast<int32_t>(static_cast<int64_t>(NumericValue(*value))));
                    return true;
                case T::TAG_Long_Array:
                    if (!IsNumeric(*value)) return false;
                    static_cast<::World::NBTTagLongArray&>(tag).value.insert(
                        static_cast<::World::NBTTagLongArray&>(tag).value.begin() + static_cast<std::ptrdiff_t>(index),
                        static_cast<int64_t>(NumericValue(*value)));
                    return true;
                default:
                    return false;
            }
        }

        bool CollectionSet(NBTTag& tag, size_t index, const TagPtr& value) {
            if (index >= CollectionSize(tag)) return false;
            switch (tag.type) {
                case T::TAG_List: {
                    auto& list = static_cast<::World::NBTTagList&>(tag);
                    if (list.value.size() == 1) list.value.clear();
                    TagPtr v = WrapForList(list, value);
                    if (list.value.empty()) list.value.push_back(std::move(v));
                    else list.value[index] = std::move(v);
                    return true;
                }
                case T::TAG_Byte_Array:
                    if (!IsNumeric(*value)) return false;
                    static_cast<::World::NBTTagByteArray&>(tag).value[index] =
                        static_cast<int8_t>(static_cast<int64_t>(NumericValue(*value)));
                    return true;
                case T::TAG_Int_Array:
                    if (!IsNumeric(*value)) return false;
                    static_cast<::World::NBTTagIntArray&>(tag).value[index] =
                        static_cast<int32_t>(static_cast<int64_t>(NumericValue(*value)));
                    return true;
                case T::TAG_Long_Array:
                    if (!IsNumeric(*value)) return false;
                    static_cast<::World::NBTTagLongArray&>(tag).value[index] = static_cast<int64_t>(NumericValue(*value));
                    return true;
                default:
                    return false;
            }
        }

        void CollectionRemove(NBTTag& tag, size_t index) {
            switch (tag.type) {
                case T::TAG_List: {
                    auto& v = static_cast<::World::NBTTagList&>(tag).value;
                    v.erase(v.begin() + static_cast<std::ptrdiff_t>(index));
                    break;
                }
                case T::TAG_Byte_Array: { auto& v = static_cast<::World::NBTTagByteArray&>(tag).value; v.erase(v.begin() + static_cast<std::ptrdiff_t>(index)); break; }
                case T::TAG_Int_Array:  { auto& v = static_cast<::World::NBTTagIntArray&>(tag).value;  v.erase(v.begin() + static_cast<std::ptrdiff_t>(index)); break; }
                case T::TAG_Long_Array: { auto& v = static_cast<::World::NBTTagLongArray&>(tag).value; v.erase(v.begin() + static_cast<std::ptrdiff_t>(index)); break; }
                default: break;
            }
        }

        void CollectionClear(NBTTag& tag) {
            switch (tag.type) {
                case T::TAG_List:       static_cast<::World::NBTTagList&>(tag).value.clear(); break;
                case T::TAG_Byte_Array: static_cast<::World::NBTTagByteArray&>(tag).value.clear(); break;
                case T::TAG_Int_Array:  static_cast<::World::NBTTagIntArray&>(tag).value.clear(); break;
                case T::TAG_Long_Array: static_cast<::World::NBTTagLongArray&>(tag).value.clear(); break;
                default: break;
            }
        }

        ::World::NBTTagCompound* AsCompound(const TagPtr& tag) {
            return tag && tag->type == T::TAG_Compound ? static_cast<::World::NBTTagCompound*>(tag.get()) : nullptr;
        }

        TagPtr ChildOf(const ::World::NBTTagCompound& c, const std::string& key) {
            auto it = c.value.find(key);
            return it == c.value.end() ? nullptr : it->second;
        }

        bool LooksLikeId(const std::string& s) {
            if (s.empty()) return false;
            int colons = 0;
            for (char c : s) {
                if (c == ':') { if (++colons > 1) return false; continue; }
                if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) ||
                      c == '_' || c == '-' || c == '.' || c == '/')) {
                    return false;
                }
            }
            return s.front() != ':' && s.back() != ':';
        }

        // Java's Float/Double.toString, near enough: the shortest digits that
        // round-trip, a ".0" on integral values, E notation outside 1e-3..1e7.
        template <typename F>
        std::string JavaNumber(F v) {
            if (std::isnan(v)) return "NaN";
            if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
            if (v == 0) return std::signbit(v) ? "-0.0" : "0.0";
            const double a = std::fabs(static_cast<double>(v));
            const bool sci = a >= 1e7 || a < 1e-3;
            const int maxDigits = std::is_same_v<F, float> ? 9 : 17;
            char buf[64];
            for (int precision = 1; precision <= maxDigits; ++precision) {
                std::snprintf(buf, sizeof(buf), sci ? "%.*e" : "%.*g", sci ? precision - 1 : precision,
                              static_cast<double>(v));
                F back{};
                if constexpr (std::is_same_v<F, float>) back = std::strtof(buf, nullptr);
                else back = std::strtod(buf, nullptr);
                if (back == v) break;
            }
            std::string s(buf);
            if (sci) {
                const size_t e = s.find('e');
                std::string mant = s.substr(0, e);
                std::string exp = s.substr(e + 1);
                if (mant.find('.') == std::string::npos) mant += ".0";
                bool neg = false;
                if (!exp.empty() && (exp[0] == '+' || exp[0] == '-')) { neg = exp[0] == '-'; exp.erase(0, 1); }
                while (exp.size() > 1 && exp[0] == '0') exp.erase(0, 1);
                return mant + "E" + (neg ? "-" : "") + exp;
            }
            if (s.find('e') != std::string::npos) {
                // %g chose an exponent inside the plain range: print it plainly.
                std::snprintf(buf, sizeof(buf), "%.*f", maxDigits, static_cast<double>(v));
                s = buf;
                while (!s.empty() && s.back() == '0') s.pop_back();
            }
            if (s.find('.') == std::string::npos) s += ".0";
            if (s.back() == '.') s += "0";
            return s;
        }

        // StringTag.quoteAndEscape: double quotes unless the text holds a '"'
        // first, then single.
        std::string QuoteAndEscape(const std::string& input) {
            char quote = 0;
            std::string body;
            for (char c : input) {
                if (c == '\\') { body += "\\\\"; continue; }
                if (c == '"' || c == '\'') {
                    if (!quote) quote = c == '"' ? '\'' : '"';
                    if (quote == c) body += '\\';
                    body += c;
                    continue;
                }
                if (c == '\n') { body += "\\n"; continue; }
                if (c == '\t') { body += "\\t"; continue; }
                if (c == '\r') { body += "\\r"; continue; }
                body += c;
            }
            if (!quote) quote = '"';
            return std::string(1, quote) + body + std::string(1, quote);
        }

        bool SimpleKey(const std::string& key) {
            if (key.empty()) return false;
            for (char c : key) {
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '+' || c == '-')) return false;
            }
            return true;
        }

        std::vector<std::string> SortedKeys(const ::World::NBTTagCompound& c) {
            std::vector<std::string> keys;
            keys.reserve(c.value.size());
            for (const auto& [k, v] : c.value) if (v) keys.push_back(k);
            std::sort(keys.begin(), keys.end());
            return keys;
        }

        // ── Pretty printing (TextComponentTagVisitor, RichStyling) ─────────

        constexpr uint32_t kWhite = 0xFFFFFFFFu;
        constexpr uint32_t kAqua  = 0xFF55FFFFu;   // ChatFormatting.AQUA — keys
        constexpr uint32_t kGreen = 0xFF55FF55u;   // GREEN — strings
        constexpr uint32_t kGold  = 0xFFFFAA00u;   // GOLD — numbers
        constexpr uint32_t kRed   = 0xFFFF5555u;   // RED — type suffixes / array prefixes
        constexpr uint32_t kGray  = 0xFFAAAAAAu;   // GRAY — <...>

        struct Pretty {
            std::vector<Network::ChatSegmentData>& out;
            int depth = 0;

            void Add(const std::string& text, uint32_t color) {
                if (text.empty()) return;
                if (!out.empty() && out.back().color == color && out.back().click == Network::ChatClickAction::None) {
                    out.back().text += text;
                    return;
                }
                Network::ChatSegmentData s;
                s.text = text;
                s.color = color;
                s.click = Network::ChatClickAction::None;
                out.push_back(std::move(s));
            }

            template <typename V>
            void Array(char prefix, const std::vector<V>& data, const char* suffix) {
                Add("[", kWhite);
                Add(std::string(1, prefix), kRed);
                Add(";", kWhite);
                for (size_t i = 0; i < data.size() && i < 128; ++i) {
                    Add(" ", kWhite);
                    Add(std::to_string(static_cast<int64_t>(data[i])), kGold);
                    if (*suffix) Add(suffix, kRed);
                    if (i != data.size() - 1) Add(",", kWhite);
                }
                if (data.size() > 128) Add("<...>", kGray);
                Add("]", kWhite);
            }

            void Visit(const NBTTag& tag) {
                switch (tag.type) {
                    case T::TAG_String: {
                        const std::string q = QuoteAndEscape(static_cast<const ::World::NBTTagString&>(tag).value);
                        Add(q.substr(0, 1), kWhite);
                        Add(q.substr(1, q.size() - 2), kGreen);
                        Add(q.substr(q.size() - 1), kWhite);
                        break;
                    }
                    case T::TAG_Byte:  Add(std::to_string(static_cast<const ::World::NBTTagByte&>(tag).value), kGold); Add("b", kRed); break;
                    case T::TAG_Short: Add(std::to_string(static_cast<const ::World::NBTTagShort&>(tag).value), kGold); Add("s", kRed); break;
                    case T::TAG_Int:   Add(std::to_string(static_cast<const ::World::NBTTagInt&>(tag).value), kGold); break;
                    case T::TAG_Long:  Add(std::to_string(static_cast<const ::World::NBTTagLong&>(tag).value), kGold); Add("L", kRed); break;
                    case T::TAG_Float: Add(JavaNumber(static_cast<const ::World::NBTTagFloat&>(tag).value), kGold); Add("f", kRed); break;
                    case T::TAG_Double: Add(JavaNumber(static_cast<const ::World::NBTTagDouble&>(tag).value), kGold); Add("d", kRed); break;
                    case T::TAG_Byte_Array: Array('B', static_cast<const ::World::NBTTagByteArray&>(tag).value, "B"); break;
                    case T::TAG_Int_Array:  Array('I', static_cast<const ::World::NBTTagIntArray&>(tag).value, ""); break;
                    case T::TAG_Long_Array: Array('L', static_cast<const ::World::NBTTagLongArray&>(tag).value, "L"); break;
                    case T::TAG_List: {
                        const auto& list = static_cast<const ::World::NBTTagList&>(tag).value;
                        if (list.empty()) { Add("[]", kWhite); break; }
                        if (depth >= 64) { Add("[", kWhite); Add("<...>", kGray); Add("]", kWhite); break; }
                        Add("[", kWhite);
                        for (size_t i = 0; i < list.size() && i < 128; ++i) {
                            if (i) Add(", ", kWhite);
                            ++depth;
                            if (list[i]) Visit(*list[i]);
                            --depth;
                        }
                        if (list.size() > 128) Add("<...>", kGray);
                        Add("]", kWhite);
                        break;
                    }
                    case T::TAG_Compound: {
                        const auto& c = static_cast<const ::World::NBTTagCompound&>(tag);
                        if (c.value.empty()) { Add("{}", kWhite); break; }
                        if (depth >= 64) { Add("{", kWhite); Add("<...>", kGray); Add("}", kWhite); break; }
                        Add("{", kWhite);
                        bool first = true;
                        for (const std::string& key : SortedKeys(c)) {
                            if (!first) Add(", ", kWhite);
                            first = false;
                            if (SimpleKey(key)) {
                                Add(key, kAqua);
                            } else {
                                const std::string q = QuoteAndEscape(key);
                                Add(q.substr(0, 1), kWhite);
                                Add(q.substr(1, q.size() - 2), kAqua);
                                Add(q.substr(q.size() - 1), kWhite);
                            }
                            Add(": ", kWhite);
                            ++depth;
                            Visit(*c.value.at(key));
                            --depth;
                        }
                        Add("}", kWhite);
                        break;
                    }
                    case T::TAG_End: break;
                }
            }
        };

    } // namespace

    // ── Basics ──────────────────────────────────────────────────────────────

    TagPtr Copy(const NBTTag& tag) {
        switch (tag.type) {
            case T::TAG_Byte:   return std::make_shared<::World::NBTTagByte>(static_cast<const ::World::NBTTagByte&>(tag).value);
            case T::TAG_Short:  return std::make_shared<::World::NBTTagShort>(static_cast<const ::World::NBTTagShort&>(tag).value);
            case T::TAG_Int:    return std::make_shared<::World::NBTTagInt>(static_cast<const ::World::NBTTagInt&>(tag).value);
            case T::TAG_Long:   return std::make_shared<::World::NBTTagLong>(static_cast<const ::World::NBTTagLong&>(tag).value);
            case T::TAG_Float:  return std::make_shared<::World::NBTTagFloat>(static_cast<const ::World::NBTTagFloat&>(tag).value);
            case T::TAG_Double: return std::make_shared<::World::NBTTagDouble>(static_cast<const ::World::NBTTagDouble&>(tag).value);
            case T::TAG_String: return std::make_shared<::World::NBTTagString>(static_cast<const ::World::NBTTagString&>(tag).value);
            case T::TAG_Byte_Array: { auto t = std::make_shared<::World::NBTTagByteArray>(); t->value = static_cast<const ::World::NBTTagByteArray&>(tag).value; return t; }
            case T::TAG_Int_Array:  { auto t = std::make_shared<::World::NBTTagIntArray>();  t->value = static_cast<const ::World::NBTTagIntArray&>(tag).value;  return t; }
            case T::TAG_Long_Array: { auto t = std::make_shared<::World::NBTTagLongArray>(); t->value = static_cast<const ::World::NBTTagLongArray&>(tag).value; return t; }
            case T::TAG_List: {
                const auto& src = static_cast<const ::World::NBTTagList&>(tag);
                auto t = std::make_shared<::World::NBTTagList>(src.listType);
                t->value.reserve(src.value.size());
                for (const TagPtr& e : src.value) if (e) t->value.push_back(Copy(*e));
                return t;
            }
            case T::TAG_Compound: return CopyCompound(static_cast<const ::World::NBTTagCompound&>(tag));
            case T::TAG_End: break;
        }
        return nullptr;
    }

    std::shared_ptr<::World::NBTTagCompound> CopyCompound(const ::World::NBTTagCompound& tag) {
        auto t = std::make_shared<::World::NBTTagCompound>();
        for (const auto& [k, v] : tag.value) if (v) t->value[k] = Copy(*v);
        return t;
    }

    bool IsNumeric(const NBTTag& tag) {
        switch (tag.type) {
            case T::TAG_Byte: case T::TAG_Short: case T::TAG_Int:
            case T::TAG_Long: case T::TAG_Float: case T::TAG_Double: return true;
            default: return false;
        }
    }

    double NumericValue(const NBTTag& tag) {
        switch (tag.type) {
            case T::TAG_Byte:   return static_cast<const ::World::NBTTagByte&>(tag).value;
            case T::TAG_Short:  return static_cast<const ::World::NBTTagShort&>(tag).value;
            case T::TAG_Int:    return static_cast<const ::World::NBTTagInt&>(tag).value;
            case T::TAG_Long:   return static_cast<double>(static_cast<const ::World::NBTTagLong&>(tag).value);
            case T::TAG_Float:  return static_cast<const ::World::NBTTagFloat&>(tag).value;
            case T::TAG_Double: return static_cast<const ::World::NBTTagDouble&>(tag).value;
            default:            return 0.0;
        }
    }

    namespace {
        bool EqualsImpl(const NBTTag* a, const NBTTag* b, bool lenientIds, bool partial) {
            if (a == b) return true;
            if (!a) return partial;
            if (!b) return false;
            if (a->type != b->type) return false;
            switch (a->type) {
                case T::TAG_Compound: {
                    const auto& x = static_cast<const ::World::NBTTagCompound&>(*a);
                    const auto& y = static_cast<const ::World::NBTTagCompound&>(*b);
                    if (partial ? y.value.size() < x.value.size() : y.value.size() != x.value.size()) return false;
                    for (const auto& [k, v] : x.value) {
                        auto it = y.value.find(k);
                        if (it == y.value.end()) return false;
                        if (!EqualsImpl(v.get(), it->second.get(), lenientIds, partial)) return false;
                    }
                    return true;
                }
                case T::TAG_List: {
                    const auto& x = static_cast<const ::World::NBTTagList&>(*a).value;
                    const auto& y = static_cast<const ::World::NBTTagList&>(*b).value;
                    if (partial) {
                        if (x.empty()) return y.empty();
                        for (const auto& want : x) {
                            const bool found = std::any_of(y.begin(), y.end(), [&](const TagPtr& have) {
                                return EqualsImpl(want.get(), have.get(), lenientIds, true);
                            });
                            if (!found) return false;
                        }
                        return true;
                    }
                    if (x.size() != y.size()) return false;
                    for (size_t i = 0; i < x.size(); ++i) {
                        if (!EqualsImpl(x[i].get(), y[i].get(), lenientIds, false)) return false;
                    }
                    return true;
                }
                case T::TAG_Byte:  return static_cast<const ::World::NBTTagByte&>(*a).value == static_cast<const ::World::NBTTagByte&>(*b).value;
                case T::TAG_Short: return static_cast<const ::World::NBTTagShort&>(*a).value == static_cast<const ::World::NBTTagShort&>(*b).value;
                case T::TAG_Int:   return static_cast<const ::World::NBTTagInt&>(*a).value == static_cast<const ::World::NBTTagInt&>(*b).value;
                case T::TAG_Long:  return static_cast<const ::World::NBTTagLong&>(*a).value == static_cast<const ::World::NBTTagLong&>(*b).value;
                case T::TAG_Float: {
                    const float x = static_cast<const ::World::NBTTagFloat&>(*a).value, y = static_cast<const ::World::NBTTagFloat&>(*b).value;
                    uint32_t bx, by; std::memcpy(&bx, &x, 4); std::memcpy(&by, &y, 4);
                    return bx == by || (std::isnan(x) && std::isnan(y));
                }
                case T::TAG_Double: {
                    const double x = static_cast<const ::World::NBTTagDouble&>(*a).value, y = static_cast<const ::World::NBTTagDouble&>(*b).value;
                    uint64_t bx, by; std::memcpy(&bx, &x, 8); std::memcpy(&by, &y, 8);
                    return bx == by || (std::isnan(x) && std::isnan(y));
                }
                case T::TAG_String: {
                    const std::string& x = static_cast<const ::World::NBTTagString&>(*a).value;
                    const std::string& y = static_cast<const ::World::NBTTagString&>(*b).value;
                    if (x == y) return true;
                    if (!lenientIds || !LooksLikeId(x) || !LooksLikeId(y)) return false;
                    const auto bare = [](const std::string& s) { return s.rfind("minecraft:", 0) == 0 ? s.substr(10) : s; };
                    return bare(x) == bare(y);
                }
                case T::TAG_Byte_Array: return static_cast<const ::World::NBTTagByteArray&>(*a).value == static_cast<const ::World::NBTTagByteArray&>(*b).value;
                case T::TAG_Int_Array:  return static_cast<const ::World::NBTTagIntArray&>(*a).value == static_cast<const ::World::NBTTagIntArray&>(*b).value;
                case T::TAG_Long_Array: return static_cast<const ::World::NBTTagLongArray&>(*a).value == static_cast<const ::World::NBTTagLongArray&>(*b).value;
                case T::TAG_End: return true;
            }
            return false;
        }
    }

    bool Equals(const NBTTag* a, const NBTTag* b) {
        if (!a || !b) return a == b;
        return EqualsImpl(a, b, false, false);
    }

    bool Compare(const NBTTag* expected, const NBTTag* actual) {
        return EqualsImpl(expected, actual, true, true);
    }

    void Merge(::World::NBTTagCompound& target, const ::World::NBTTagCompound& source) {
        for (const auto& [key, value] : source.value) {
            if (!value) continue;
            if (value->type == T::TAG_Compound) {
                auto it = target.value.find(key);
                if (it != target.value.end() && it->second && it->second->type == T::TAG_Compound) {
                    Merge(static_cast<::World::NBTTagCompound&>(*it->second),
                          static_cast<const ::World::NBTTagCompound&>(*value));
                    continue;
                }
            }
            target.value[key] = Copy(*value);
        }
    }

    bool IsTooDeep(const NBTTag& tag, int depth) {
        if (depth >= 512) return true;
        if (tag.type == T::TAG_Compound) {
            for (const auto& [k, v] : static_cast<const ::World::NBTTagCompound&>(tag).value) {
                if (v && IsTooDeep(*v, depth + 1)) return true;
            }
        } else if (tag.type == T::TAG_List) {
            for (const auto& v : static_cast<const ::World::NBTTagList&>(tag).value) {
                if (v && IsTooDeep(*v, depth + 1)) return true;
            }
        }
        return false;
    }

    std::string ToSnbt(const NBTTag& tag) {
        switch (tag.type) {
            case T::TAG_String: return QuoteAndEscape(static_cast<const ::World::NBTTagString&>(tag).value);
            case T::TAG_Byte:   return std::to_string(static_cast<const ::World::NBTTagByte&>(tag).value) + "b";
            case T::TAG_Short:  return std::to_string(static_cast<const ::World::NBTTagShort&>(tag).value) + "s";
            case T::TAG_Int:    return std::to_string(static_cast<const ::World::NBTTagInt&>(tag).value);
            case T::TAG_Long:   return std::to_string(static_cast<const ::World::NBTTagLong&>(tag).value) + "L";
            case T::TAG_Float:  return JavaNumber(static_cast<const ::World::NBTTagFloat&>(tag).value) + "f";
            case T::TAG_Double: return JavaNumber(static_cast<const ::World::NBTTagDouble&>(tag).value) + "d";
            case T::TAG_Byte_Array: {
                std::string s = "[B;";
                const auto& v = static_cast<const ::World::NBTTagByteArray&>(tag).value;
                for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]) + "B";
                return s + "]";
            }
            case T::TAG_Int_Array: {
                std::string s = "[I;";
                const auto& v = static_cast<const ::World::NBTTagIntArray&>(tag).value;
                for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]);
                return s + "]";
            }
            case T::TAG_Long_Array: {
                std::string s = "[L;";
                const auto& v = static_cast<const ::World::NBTTagLongArray&>(tag).value;
                for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]) + "L";
                return s + "]";
            }
            case T::TAG_List: {
                std::string s = "[";
                const auto& v = static_cast<const ::World::NBTTagList&>(tag).value;
                for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + (v[i] ? ToSnbt(*v[i]) : std::string());
                return s + "]";
            }
            case T::TAG_Compound: {
                const auto& c = static_cast<const ::World::NBTTagCompound&>(tag);
                std::string s = "{";
                bool first = true;
                for (const std::string& key : SortedKeys(c)) {
                    s += (first ? "" : ",") + (SimpleKey(key) ? key : QuoteAndEscape(key)) + ":" + ToSnbt(*c.value.at(key));
                    first = false;
                }
                return s + "}";
            }
            case T::TAG_End: break;
        }
        return {};
    }

    void AppendPretty(const NBTTag& tag, std::vector<Network::ChatSegmentData>& out) {
        Pretty p{out};
        p.Visit(tag);
    }

    // ── Path nodes (NbtPathArgument.Node and its six kinds) ────────────────

    struct Path::Node {
        enum class Kind : uint8_t { MatchRootObject, MatchElement, AllElements, IndexedElement, MatchObject, CompoundChild };
        Kind kind = Kind::CompoundChild;
        std::string name;
        int index = 0;
        std::shared_ptr<::World::NBTTagCompound> pattern;

        bool Matches(const TagPtr& tag) const { return Compare(pattern.get(), tag.get()); }

        TagPtr PreferredParent() const {
            switch (kind) {
                case Kind::MatchElement: case Kind::AllElements: case Kind::IndexedElement:
                    return std::make_shared<::World::NBTTagList>();
                default:
                    return std::make_shared<::World::NBTTagCompound>();
            }
        }

        void GetTag(const TagPtr& parent, std::vector<TagPtr>& out) const {
            if (!parent) return;
            switch (kind) {
                case Kind::MatchRootObject:
                    if (parent->type == T::TAG_Compound && Matches(parent)) out.push_back(parent);
                    break;
                case Kind::MatchElement:
                    if (parent->type == T::TAG_List) {
                        for (const TagPtr& e : static_cast<::World::NBTTagList&>(*parent).value) if (Matches(e)) out.push_back(e);
                    }
                    break;
                case Kind::AllElements:
                    if (IsCollection(*parent)) {
                        for (size_t i = 0; i < CollectionSize(*parent); ++i) out.push_back(CollectionGet(*parent, i));
                    }
                    break;
                case Kind::IndexedElement:
                    if (IsCollection(*parent)) {
                        const int size = static_cast<int>(CollectionSize(*parent));
                        const int actual = index < 0 ? size + index : index;
                        if (actual >= 0 && actual < size) out.push_back(CollectionGet(*parent, static_cast<size_t>(actual)));
                    }
                    break;
                case Kind::MatchObject:
                    if (auto* c = AsCompound(parent)) {
                        TagPtr result = ChildOf(*c, name);
                        if (result && Matches(result)) out.push_back(result);
                    }
                    break;
                case Kind::CompoundChild:
                    if (auto* c = AsCompound(parent)) {
                        if (TagPtr result = ChildOf(*c, name)) out.push_back(result);
                    }
                    break;
            }
        }

        void GetOrCreateTag(const TagPtr& parent, const std::function<TagPtr()>& child, std::vector<TagPtr>& out) const {
            if (!parent) return;
            switch (kind) {
                case Kind::MatchElement:
                    if (parent->type == T::TAG_List) {
                        auto& list = static_cast<::World::NBTTagList&>(*parent);
                        bool found = false;
                        for (const TagPtr& e : list.value) if (Matches(e)) { out.push_back(e); found = true; }
                        if (!found) {
                            TagPtr created = CopyCompound(*pattern);
                            CollectionAdd(list, list.value.size(), created);
                            out.push_back(list.value.back());
                        }
                    }
                    break;
                case Kind::AllElements:
                    if (IsCollection(*parent)) {
                        if (CollectionSize(*parent) == 0) {
                            TagPtr result = child();
                            if (CollectionAdd(*parent, 0, result)) out.push_back(CollectionGet(*parent, 0));
                        } else {
                            for (size_t i = 0; i < CollectionSize(*parent); ++i) out.push_back(CollectionGet(*parent, i));
                        }
                    }
                    break;
                case Kind::MatchObject:
                    if (auto* c = AsCompound(parent)) {
                        TagPtr result = ChildOf(*c, name);
                        if (!result) {
                            result = CopyCompound(*pattern);
                            c->value[name] = result;
                            out.push_back(result);
                        } else if (Matches(result)) {
                            out.push_back(result);
                        }
                    }
                    break;
                case Kind::CompoundChild:
                    if (auto* c = AsCompound(parent)) {
                        TagPtr result = ChildOf(*c, name);
                        if (!result) {
                            result = child();
                            c->value[name] = result;
                        }
                        out.push_back(result);
                    }
                    break;
                case Kind::MatchRootObject:
                case Kind::IndexedElement:
                    GetTag(parent, out);
                    break;
            }
        }

        int SetTag(const TagPtr& parent, const std::function<TagPtr()>& toAdd) const {
            if (!parent) return 0;
            switch (kind) {
                case Kind::MatchRootObject:
                    return 0;
                case Kind::MatchElement: {
                    if (parent->type != T::TAG_List) return 0;
                    auto& list = static_cast<::World::NBTTagList&>(*parent);
                    int changed = 0;
                    if (list.value.empty()) {
                        CollectionAdd(list, 0, toAdd());
                        return 1;
                    }
                    for (size_t i = 0; i < list.value.size(); ++i) {
                        if (!Matches(list.value[i])) continue;
                        TagPtr value = toAdd();
                        if (!Equals(value.get(), list.value[i].get()) && CollectionSet(list, i, value)) ++changed;
                    }
                    return changed;
                }
                case Kind::AllElements: {
                    if (!IsCollection(*parent)) return 0;
                    const size_t size = CollectionSize(*parent);
                    if (size == 0) {
                        CollectionAdd(*parent, 0, toAdd());
                        return 1;
                    }
                    TagPtr value = toAdd();
                    int same = 0;
                    for (size_t i = 0; i < size; ++i) {
                        TagPtr e = CollectionGet(*parent, i);
                        if (Equals(value.get(), e.get())) ++same;
                    }
                    const int changed = static_cast<int>(size) - same;
                    if (changed == 0) return 0;
                    CollectionClear(*parent);
                    if (!CollectionAdd(*parent, 0, value)) return 0;
                    for (size_t i = 1; i < size; ++i) CollectionAdd(*parent, i, toAdd());
                    return changed;
                }
                case Kind::IndexedElement: {
                    if (!IsCollection(*parent)) return 0;
                    const int size = static_cast<int>(CollectionSize(*parent));
                    const int actual = index < 0 ? size + index : index;
                    if (actual < 0 || actual >= size) return 0;
                    TagPtr previous = CollectionGet(*parent, static_cast<size_t>(actual));
                    TagPtr value = toAdd();
                    return !Equals(value.get(), previous.get()) && CollectionSet(*parent, static_cast<size_t>(actual), value) ? 1 : 0;
                }
                case Kind::MatchObject: {
                    auto* c = AsCompound(parent);
                    if (!c) return 0;
                    TagPtr current = ChildOf(*c, name);
                    if (!Matches(current)) return 0;
                    TagPtr value = toAdd();
                    if (Equals(value.get(), current.get())) return 0;
                    c->value[name] = value;
                    return 1;
                }
                case Kind::CompoundChild: {
                    auto* c = AsCompound(parent);
                    if (!c) return 0;
                    TagPtr value = toAdd();
                    TagPtr previous = ChildOf(*c, name);
                    c->value[name] = value;
                    return Equals(value.get(), previous.get()) ? 0 : 1;
                }
            }
            return 0;
        }

        int RemoveTag(const TagPtr& parent) const {
            if (!parent) return 0;
            switch (kind) {
                case Kind::MatchRootObject:
                    return 0;
                case Kind::MatchElement: {
                    if (parent->type != T::TAG_List) return 0;
                    auto& v = static_cast<::World::NBTTagList&>(*parent).value;
                    int changed = 0;
                    for (int i = static_cast<int>(v.size()) - 1; i >= 0; --i) {
                        if (Matches(v[static_cast<size_t>(i)])) {
                            v.erase(v.begin() + i);
                            ++changed;
                        }
                    }
                    return changed;
                }
                case Kind::AllElements: {
                    if (!IsCollection(*parent)) return 0;
                    const int size = static_cast<int>(CollectionSize(*parent));
                    if (size > 0) { CollectionClear(*parent); return size; }
                    return 0;
                }
                case Kind::IndexedElement: {
                    if (!IsCollection(*parent)) return 0;
                    const int size = static_cast<int>(CollectionSize(*parent));
                    const int actual = index < 0 ? size + index : index;
                    if (actual < 0 || actual >= size) return 0;
                    CollectionRemove(*parent, static_cast<size_t>(actual));
                    return 1;
                }
                case Kind::MatchObject: {
                    auto* c = AsCompound(parent);
                    if (!c) return 0;
                    if (!Matches(ChildOf(*c, name))) return 0;
                    c->value.erase(name);
                    return 1;
                }
                case Kind::CompoundChild: {
                    auto* c = AsCompound(parent);
                    if (!c || !c->value.count(name)) return 0;
                    c->value.erase(name);
                    return 1;
                }
            }
            return 0;
        }
    };

    // ── Parsing ─────────────────────────────────────────────────────────────

    namespace {
        // The extent of an SNBT compound starting at text[i] == '{'.
        size_t CompoundEnd(const std::string& text, size_t i) {
            int depth = 0;
            char quote = 0;
            for (; i < text.size(); ++i) {
                const char c = text[i];
                if (quote) {
                    if (c == '\\') { ++i; continue; }
                    if (c == quote) quote = 0;
                    continue;
                }
                if (c == '"' || c == '\'') { quote = c; continue; }
                if (c == '{' || c == '[') ++depth;
                else if (c == '}' || c == ']') {
                    if (--depth == 0) return i + 1;
                }
            }
            return std::string::npos;
        }

        bool ParsePattern(const std::string& text, size_t& i, std::shared_ptr<::World::NBTTagCompound>& out,
                          std::string& error) {
            const size_t end = CompoundEnd(text, i);
            if (end == std::string::npos) { error = "Unterminated compound in NBT path"; return false; }
            std::string parseError;
            out = Snbt::ParseCompound(text.substr(i, end - i), parseError);
            if (!out) { error = parseError; return false; }
            i = end;
            return true;
        }

        bool AllowedUnquoted(char c) {
            return c != ' ' && c != '"' && c != '\'' && c != '[' && c != ']' && c != '.' && c != '{' && c != '}';
        }

        // StringReader.readString on a quoted string.
        bool ReadQuoted(const std::string& text, size_t& i, std::string& out, std::string& error) {
            const char quote = text[i++];
            out.clear();
            while (i < text.size()) {
                const char c = text[i++];
                if (c == '\\') {
                    if (i >= text.size()) break;
                    const char next = text[i++];
                    if (next != quote && next != '\\') { error = "Invalid escape sequence '" + std::string(1, next) + "' in quoted string"; return false; }
                    out += next;
                    continue;
                }
                if (c == quote) return true;
                out += c;
            }
            error = "Unclosed quoted string";
            return false;
        }

        constexpr const char* kInvalidNode = "Invalid NBT path element";
    }

    bool Path::Parse(const std::string& text, Path& out, std::string& error) {
        out = Path{};
        size_t i = 0;
        bool first = true;
        while (i < text.size() && text[i] != ' ') {
            auto node = std::make_shared<Node>();
            const char c = text[i];
            if (c == '"' || c == '\'') {
                std::string name;
                if (!ReadQuoted(text, i, name, error)) return false;
                if (name.empty()) { error = kInvalidNode; return false; }
                node->name = name;
                if (i < text.size() && text[i] == '{') {
                    node->kind = Node::Kind::MatchObject;
                    if (!ParsePattern(text, i, node->pattern, error)) return false;
                } else {
                    node->kind = Node::Kind::CompoundChild;
                }
            } else if (c == '[') {
                ++i;
                if (i < text.size() && text[i] == '{') {
                    node->kind = Node::Kind::MatchElement;
                    if (!ParsePattern(text, i, node->pattern, error)) return false;
                    if (i >= text.size() || text[i] != ']') { error = "Expected ']'"; return false; }
                    ++i;
                } else if (i < text.size() && text[i] == ']') {
                    ++i;
                    node->kind = Node::Kind::AllElements;
                } else {
                    size_t j = i;
                    if (j < text.size() && (text[j] == '-' || text[j] == '+')) ++j;
                    while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) ++j;
                    int value = 0;
                    const auto r = std::from_chars(text.data() + i + (text[i] == '+' ? 1 : 0), text.data() + j, value);
                    if (j == i || r.ec != std::errc()) { error = "Expected integer"; return false; }
                    i = j;
                    if (i >= text.size() || text[i] != ']') { error = "Expected ']'"; return false; }
                    ++i;
                    node->kind = Node::Kind::IndexedElement;
                    node->index = value;
                }
            } else if (c == '{') {
                if (!first) { error = kInvalidNode; return false; }
                node->kind = Node::Kind::MatchRootObject;
                if (!ParsePattern(text, i, node->pattern, error)) return false;
            } else {
                const size_t start = i;
                while (i < text.size() && AllowedUnquoted(text[i])) ++i;
                if (i == start) { error = kInvalidNode; return false; }
                node->name = text.substr(start, i - start);
                if (i < text.size() && text[i] == '{') {
                    node->kind = Node::Kind::MatchObject;
                    if (!ParsePattern(text, i, node->pattern, error)) return false;
                } else {
                    node->kind = Node::Kind::CompoundChild;
                }
            }
            out.m_nodes.push_back(std::move(node));
            out.m_endOffsets.push_back(i);
            first = false;
            if (i < text.size()) {
                const char next = text[i];
                if (next != ' ' && next != '[' && next != '{') {
                    if (next != '.') { error = "Expected '.'"; return false; }
                    ++i;
                }
            }
        }
        if (out.m_nodes.empty()) { error = kInvalidNode; return false; }
        out.m_text = text.substr(0, i);
        return true;
    }

    // ── Operations ──────────────────────────────────────────────────────────

    bool Path::Get(const TagPtr& root, std::vector<TagPtr>& out, std::string& error) const {
        std::vector<TagPtr> result{root};
        for (size_t n = 0; n < m_nodes.size(); ++n) {
            std::vector<TagPtr> next;
            for (const TagPtr& t : result) m_nodes[n]->GetTag(t, next);
            result = std::move(next);
            if (result.empty()) {
                error = "Found no elements matching " + m_text.substr(0, m_endOffsets[n]);
                return false;
            }
        }
        out = std::move(result);
        return true;
    }

    int Path::CountMatching(const TagPtr& root) const {
        std::vector<TagPtr> result{root};
        for (const auto& node : m_nodes) {
            std::vector<TagPtr> next;
            for (const TagPtr& t : result) node->GetTag(t, next);
            result = std::move(next);
            if (result.empty()) return 0;
        }
        return static_cast<int>(result.size());
    }

    bool Path::GetOrCreate(const TagPtr& root, const std::function<TagPtr()>& create,
                           std::vector<TagPtr>& out, std::string& error) const {
        std::vector<TagPtr> result{root};
        for (size_t n = 0; n + 1 < m_nodes.size(); ++n) {
            const Node& next = *m_nodes[n + 1];
            std::vector<TagPtr> step;
            for (const TagPtr& t : result) m_nodes[n]->GetOrCreateTag(t, [&] { return next.PreferredParent(); }, step);
            result = std::move(step);
            if (result.empty()) {
                error = "Found no elements matching " + m_text.substr(0, m_endOffsets[n]);
                return false;
            }
        }
        std::vector<TagPtr> last;
        for (const TagPtr& t : result) m_nodes.back()->GetOrCreateTag(t, create, last);
        out = std::move(last);
        return true;
    }

    bool Path::Set(const TagPtr& root, const NBTTag& value, int& changed, std::string& error) const {
        changed = 0;
        if (IsTooDeep(value, static_cast<int>(m_nodes.size()))) { error = "Resulting NBT too deeply nested"; return false; }
        std::vector<TagPtr> parents{root};
        for (size_t n = 0; n + 1 < m_nodes.size(); ++n) {
            const Node& next = *m_nodes[n + 1];
            std::vector<TagPtr> step;
            for (const TagPtr& t : parents) m_nodes[n]->GetOrCreateTag(t, [&] { return next.PreferredParent(); }, step);
            parents = std::move(step);
            if (parents.empty()) {
                error = "Found no elements matching " + m_text.substr(0, m_endOffsets[n]);
                return false;
            }
        }
        for (const TagPtr& p : parents) changed += m_nodes.back()->SetTag(p, [&] { return Copy(value); });
        return true;
    }

    bool Path::Insert(int index, const TagPtr& root, const std::vector<TagPtr>& values, int& changed,
                      std::string& error) const {
        changed = 0;
        for (const TagPtr& v : values) {
            if (v && IsTooDeep(*v, static_cast<int>(m_nodes.size()))) { error = "Resulting NBT too deeply nested"; return false; }
        }
        std::vector<TagPtr> targets;
        if (!GetOrCreate(root, [] { return std::make_shared<::World::NBTTagList>(); }, targets, error)) return false;
        for (const TagPtr& target : targets) {
            if (!target || !IsCollection(*target)) {
                error = "Expected a list: got " + (target ? ToSnbt(*target) : std::string("nothing"));
                return false;
            }
            bool modified = false;
            int actual = index < 0 ? static_cast<int>(CollectionSize(*target)) + index + 1 : index;
            for (const TagPtr& v : values) {
                try {
                    if (actual < 0) throw std::out_of_range("index");
                    if (CollectionAdd(*target, static_cast<size_t>(actual), Copy(*v))) {
                        ++actual;
                        modified = true;
                    }
                } catch (const std::out_of_range&) {
                    error = "Invalid list index: " + std::to_string(actual);
                    return false;
                }
            }
            if (modified) ++changed;
        }
        return true;
    }

    int Path::Remove(const TagPtr& root) const {
        std::vector<TagPtr> result{root};
        for (size_t n = 0; n + 1 < m_nodes.size(); ++n) {
            std::vector<TagPtr> next;
            for (const TagPtr& t : result) m_nodes[n]->GetTag(t, next);
            result = std::move(next);
        }
        int count = 0;
        for (const TagPtr& t : result) count += m_nodes.back()->RemoveTag(t);
        return count;
    }

} // namespace Server::Nbt
