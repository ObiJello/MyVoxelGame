// File: src/common/data/NbtCompoundValue.cpp
#include "NbtCompoundValue.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace Game {

    namespace {

        Nbt::TagType ToWriterType(::World::NBTTagType t) {
            return static_cast<Nbt::TagType>(static_cast<uint8_t>(t));
        }

        std::vector<const std::pair<const std::string, ::World::NBTTagPtr>*> SortedEntries(
                const ::World::NBTTagCompound& c) {
            std::vector<const std::pair<const std::string, ::World::NBTTagPtr>*> entries;
            entries.reserve(c.value.size());
            for (const auto& e : c.value) if (e.second) entries.push_back(&e);
            std::sort(entries.begin(), entries.end(),
                      [](const auto* a, const auto* b) { return a->first < b->first; });
            return entries;
        }

        void WriteListElement(Nbt::Writer& w, Nbt::Writer::ListScope& list, const ::World::NBTTag& tag, int depth);

        void WriteListBody(Nbt::Writer& w, Nbt::Writer::ListScope& scope, const ::World::NBTTagList& l, int depth) {
            for (const auto& e : l.value) {
                if (e) WriteListElement(w, scope, *e, depth + 1);
            }
            w.EndList(scope);
        }

        // A list's element type: the declared one, else (a list built in
        // code with TAG_End) the first element's.
        ::World::NBTTagType ElementType(const ::World::NBTTagList& l) {
            if (l.listType != ::World::NBTTagType::TAG_End) return l.listType;
            for (const auto& e : l.value) if (e) return e->type;
            return ::World::NBTTagType::TAG_End;
        }

        void WriteListElement(Nbt::Writer& w, Nbt::Writer::ListScope& list, const ::World::NBTTag& tag, int depth) {
            using T = ::World::NBTTagType;
            if (depth > 512) return;
            switch (tag.type) {
                case T::TAG_Byte:   w.ListByte(list, static_cast<const ::World::NBTTagByte&>(tag).value); break;
                case T::TAG_Short:  w.ListShort(list, static_cast<const ::World::NBTTagShort&>(tag).value); break;
                case T::TAG_Int:    w.ListInt(list, static_cast<const ::World::NBTTagInt&>(tag).value); break;
                case T::TAG_Long:   w.ListLong(list, static_cast<const ::World::NBTTagLong&>(tag).value); break;
                case T::TAG_Float:  w.ListFloat(list, static_cast<const ::World::NBTTagFloat&>(tag).value); break;
                case T::TAG_Double: w.ListDouble(list, static_cast<const ::World::NBTTagDouble&>(tag).value); break;
                case T::TAG_String: w.ListString(list, static_cast<const ::World::NBTTagString&>(tag).value); break;
                case T::TAG_Byte_Array: {
                    const auto& v = static_cast<const ::World::NBTTagByteArray&>(tag).value;
                    w.ListByteArray(list, v.data(), v.size());
                    break;
                }
                case T::TAG_Int_Array: {
                    const auto& v = static_cast<const ::World::NBTTagIntArray&>(tag).value;
                    w.ListIntArray(list, v.data(), v.size());
                    break;
                }
                case T::TAG_Long_Array: {
                    const auto& v = static_cast<const ::World::NBTTagLongArray&>(tag).value;
                    w.ListLongArray(list, v.data(), v.size());
                    break;
                }
                case T::TAG_List: {
                    const auto& l = static_cast<const ::World::NBTTagList&>(tag);
                    auto inner = w.ListListBegin(list, ToWriterType(ElementType(l)));
                    WriteListBody(w, inner, l, depth);
                    break;
                }
                case T::TAG_Compound:
                    w.ListCompoundBegin(list);
                    for (const auto* e : SortedEntries(static_cast<const ::World::NBTTagCompound&>(tag))) {
                        WriteNbtTag(w, e->first, *e->second);
                    }
                    w.ListCompoundEnd(list);
                    break;
                case T::TAG_End:
                    break;
            }
        }

        void WriteNamed(Nbt::Writer& w, std::string_view name, const ::World::NBTTag& tag, int depth) {
            using T = ::World::NBTTagType;
            if (depth > 512) return;
            switch (tag.type) {
                case T::TAG_Byte:   w.Byte(name, static_cast<const ::World::NBTTagByte&>(tag).value); break;
                case T::TAG_Short:  w.Short(name, static_cast<const ::World::NBTTagShort&>(tag).value); break;
                case T::TAG_Int:    w.Int(name, static_cast<const ::World::NBTTagInt&>(tag).value); break;
                case T::TAG_Long:   w.Long(name, static_cast<const ::World::NBTTagLong&>(tag).value); break;
                case T::TAG_Float:  w.Float(name, static_cast<const ::World::NBTTagFloat&>(tag).value); break;
                case T::TAG_Double: w.Double(name, static_cast<const ::World::NBTTagDouble&>(tag).value); break;
                case T::TAG_String: w.String(name, static_cast<const ::World::NBTTagString&>(tag).value); break;
                case T::TAG_Byte_Array: {
                    const auto& v = static_cast<const ::World::NBTTagByteArray&>(tag).value;
                    w.ByteArray(name, v.data(), v.size());
                    break;
                }
                case T::TAG_Int_Array: {
                    const auto& v = static_cast<const ::World::NBTTagIntArray&>(tag).value;
                    w.IntArray(name, v.data(), v.size());
                    break;
                }
                case T::TAG_Long_Array: {
                    const auto& v = static_cast<const ::World::NBTTagLongArray&>(tag).value;
                    w.LongArray(name, v.data(), v.size());
                    break;
                }
                case T::TAG_List: {
                    const auto& l = static_cast<const ::World::NBTTagList&>(tag);
                    auto scope = w.BeginList(name, ToWriterType(ElementType(l)));
                    WriteListBody(w, scope, l, depth);
                    break;
                }
                case T::TAG_Compound:
                    w.BeginCompound(name);
                    for (const auto* e : SortedEntries(static_cast<const ::World::NBTTagCompound&>(tag))) {
                        WriteNamed(w, e->first, *e->second, depth + 1);
                    }
                    w.EndCompound();
                    break;
                case T::TAG_End:
                    break;
            }
        }

        // ── SNBT (MC SnbtPrinterTagVisitor's compact form) ────────────────
        bool IsBareKey(std::string_view key) {
            if (key.empty()) return false;
            for (char c : key) {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                c == '_' || c == '-' || c == '.' || c == '+';
                if (!ok) return false;
            }
            return true;
        }

        std::string Quote(std::string_view s) {
            // StringTag.quoteAndEscape: double quotes unless the text holds a
            // double quote and no single quote.
            const bool useSingle = s.find('"') != std::string_view::npos && s.find('\'') == std::string_view::npos;
            const char q = useSingle ? '\'' : '"';
            std::string out;
            out.reserve(s.size() + 2);
            out.push_back(q);
            for (char c : s) {
                if (c == '\\' || c == q) out.push_back('\\');
                out.push_back(c);
            }
            out.push_back(q);
            return out;
        }

        std::string FormatFloating(double v, char suffix) {
            if (std::isnan(v)) return std::string("NaN") + suffix;
            char buf[64];
            std::snprintf(buf, sizeof(buf), suffix == 'f' ? "%.9g" : "%.17g", v);
            std::string s = buf;
            if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
            s.push_back(suffix);
            return s;
        }

        void AppendSnbt(std::string& out, const ::World::NBTTag& tag, int depth) {
            using T = ::World::NBTTagType;
            if (depth > 512) return;
            switch (tag.type) {
                case T::TAG_Byte:   out += std::to_string(static_cast<const ::World::NBTTagByte&>(tag).value) + "b"; break;
                case T::TAG_Short:  out += std::to_string(static_cast<const ::World::NBTTagShort&>(tag).value) + "s"; break;
                case T::TAG_Int:    out += std::to_string(static_cast<const ::World::NBTTagInt&>(tag).value); break;
                case T::TAG_Long:   out += std::to_string(static_cast<const ::World::NBTTagLong&>(tag).value) + "L"; break;
                case T::TAG_Float:  out += FormatFloating(static_cast<const ::World::NBTTagFloat&>(tag).value, 'f'); break;
                case T::TAG_Double: out += FormatFloating(static_cast<const ::World::NBTTagDouble&>(tag).value, 'd'); break;
                case T::TAG_String: out += Quote(static_cast<const ::World::NBTTagString&>(tag).value); break;
                case T::TAG_Byte_Array: {
                    out += "[B;";
                    bool first = true;
                    for (int8_t v : static_cast<const ::World::NBTTagByteArray&>(tag).value) {
                        if (!first) out += ',';
                        first = false;
                        out += std::to_string(v) + "B";
                    }
                    out += ']';
                    break;
                }
                case T::TAG_Int_Array: {
                    out += "[I;";
                    bool first = true;
                    for (int32_t v : static_cast<const ::World::NBTTagIntArray&>(tag).value) {
                        if (!first) out += ',';
                        first = false;
                        out += std::to_string(v);
                    }
                    out += ']';
                    break;
                }
                case T::TAG_Long_Array: {
                    out += "[L;";
                    bool first = true;
                    for (int64_t v : static_cast<const ::World::NBTTagLongArray&>(tag).value) {
                        if (!first) out += ',';
                        first = false;
                        out += std::to_string(v) + "L";
                    }
                    out += ']';
                    break;
                }
                case T::TAG_List: {
                    out += '[';
                    bool first = true;
                    for (const auto& e : static_cast<const ::World::NBTTagList&>(tag).value) {
                        if (!e) continue;
                        if (!first) out += ',';
                        first = false;
                        AppendSnbt(out, *e, depth + 1);
                    }
                    out += ']';
                    break;
                }
                case T::TAG_Compound: {
                    out += '{';
                    bool first = true;
                    for (const auto* e : SortedEntries(static_cast<const ::World::NBTTagCompound&>(tag))) {
                        if (!first) out += ',';
                        first = false;
                        out += IsBareKey(e->first) ? e->first : Quote(e->first);
                        out += ':';
                        AppendSnbt(out, *e->second, depth + 1);
                    }
                    out += '}';
                    break;
                }
                case T::TAG_End:
                    break;
            }
        }

        const ::World::NBTTagCompound& EmptyCompound() {
            static const ::World::NBTTagCompound empty;
            return empty;
        }

    } // namespace

    NbtCompoundValue::NbtCompoundValue() : tag(std::make_shared<::World::NBTTagCompound>()) {}

    NbtCompoundValue::NbtCompoundValue(std::shared_ptr<const ::World::NBTTagCompound> t)
        : tag(t ? std::move(t) : std::make_shared<::World::NBTTagCompound>()) {}

    const ::World::NBTTagCompound& NbtCompoundValue::Tag() const {
        return tag ? *tag : EmptyCompound();
    }

    std::shared_ptr<::World::NBTTagCompound> NbtCompoundValue::Copy() const {
        auto copy = std::dynamic_pointer_cast<::World::NBTTagCompound>(CloneNbtTag(Tag()));
        return copy ? copy : std::make_shared<::World::NBTTagCompound>();
    }

    std::vector<uint8_t> NbtCompoundValue::Encode() const {
        Nbt::Writer w;
        w.BeginRootCompound();
        WriteNbtCompoundEntries(w, Tag());
        w.EndRootCompound();
        if (!w.ok()) return {};
        return std::vector<uint8_t>(w.Bytes());
    }

    NbtCompoundValue NbtCompoundValue::Decode(const std::vector<uint8_t>& bytes) {
        if (bytes.empty()) return NbtCompoundValue{};
        size_t offset = 0;
        ::World::NBTTagPtr root;
        try {
            root = ::World::NBTTag::ParseTag(bytes, offset, true);
        } catch (const std::exception&) {
            root = nullptr;
        }
        auto compound = std::dynamic_pointer_cast<::World::NBTTagCompound>(root);
        if (!compound) return NbtCompoundValue{};
        return NbtCompoundValue(std::move(compound));
    }

    std::string NbtCompoundValue::ToSnbt() const {
        return NbtTagToSnbt(Tag());
    }

    void WriteNbtTag(Nbt::Writer& w, std::string_view name, const ::World::NBTTag& tag) {
        WriteNamed(w, name, tag, 0);
    }

    void WriteNbtCompoundEntries(Nbt::Writer& w, const ::World::NBTTagCompound& compound) {
        for (const auto* e : SortedEntries(compound)) WriteNamed(w, e->first, *e->second, 1);
    }

    ::World::NBTTagPtr CloneNbtTag(const ::World::NBTTag& tag) {
        using T = ::World::NBTTagType;
        switch (tag.type) {
            case T::TAG_Byte:   return std::make_shared<::World::NBTTagByte>(static_cast<const ::World::NBTTagByte&>(tag));
            case T::TAG_Short:  return std::make_shared<::World::NBTTagShort>(static_cast<const ::World::NBTTagShort&>(tag));
            case T::TAG_Int:    return std::make_shared<::World::NBTTagInt>(static_cast<const ::World::NBTTagInt&>(tag));
            case T::TAG_Long:   return std::make_shared<::World::NBTTagLong>(static_cast<const ::World::NBTTagLong&>(tag));
            case T::TAG_Float:  return std::make_shared<::World::NBTTagFloat>(static_cast<const ::World::NBTTagFloat&>(tag));
            case T::TAG_Double: return std::make_shared<::World::NBTTagDouble>(static_cast<const ::World::NBTTagDouble&>(tag));
            case T::TAG_String: return std::make_shared<::World::NBTTagString>(static_cast<const ::World::NBTTagString&>(tag));
            case T::TAG_Byte_Array: return std::make_shared<::World::NBTTagByteArray>(static_cast<const ::World::NBTTagByteArray&>(tag));
            case T::TAG_Int_Array:  return std::make_shared<::World::NBTTagIntArray>(static_cast<const ::World::NBTTagIntArray&>(tag));
            case T::TAG_Long_Array: return std::make_shared<::World::NBTTagLongArray>(static_cast<const ::World::NBTTagLongArray&>(tag));
            case T::TAG_List: {
                const auto& src = static_cast<const ::World::NBTTagList&>(tag);
                auto out = std::make_shared<::World::NBTTagList>(src.listType);
                out->name = src.name;
                out->value.reserve(src.value.size());
                for (const auto& e : src.value) if (e) out->value.push_back(CloneNbtTag(*e));
                return out;
            }
            case T::TAG_Compound: {
                const auto& src = static_cast<const ::World::NBTTagCompound&>(tag);
                auto out = std::make_shared<::World::NBTTagCompound>();
                out->name = src.name;
                for (const auto& [k, v] : src.value) if (v) out->value[k] = CloneNbtTag(*v);
                return out;
            }
            case T::TAG_End:
                break;
        }
        return nullptr;
    }

    std::string NbtTagToSnbt(const ::World::NBTTag& tag) {
        std::string out;
        AppendSnbt(out, tag, 0);
        return out;
    }

    void SerNbtCompoundValue(Network::PacketBuffer& b, const NbtCompoundValue& v) {
        const std::vector<uint8_t> bytes = v.Encode();
        b.WriteVarInt(static_cast<uint32_t>(bytes.size()));
        b.WriteBytes(bytes);
    }

    NbtCompoundValue DeNbtCompoundValue(Network::PacketReader& r) {
        const uint32_t size = r.ReadVarInt();
        if (size > (2u << 20)) throw std::runtime_error("item NBT component too large: " + std::to_string(size));
        return NbtCompoundValue::Decode(r.ReadBytes(size));
    }

} // namespace Game
