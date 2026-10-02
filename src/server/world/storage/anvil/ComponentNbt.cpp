// File: src/server/world/storage/anvil/ComponentNbt.cpp
#include "server/world/storage/anvil/ComponentNbt.hpp"

#include "common/data/DataComponentType.hpp"

#include <algorithm>
#include <cmath>

namespace Game::Anvil::ComponentNbt {

    namespace {
        std::vector<Codec>& Registry() {
            static std::vector<Codec> registry;
            return registry;
        }

        constexpr const char* kDyeNames[16] = {
            "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
            "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black" };
    }

    void Register(const Codec& codec) {
        if (!codec.type) return;
        for (Codec& existing : Registry()) {
            if (existing.type == codec.type) { existing = codec; return; }
        }
        Registry().push_back(codec);
    }

    const std::vector<Codec>& All() { return Registry(); }

    const Codec* Find(const DataComponentTypeBase* type) {
        for (const Codec& c : Registry()) if (c.type == type) return &c;
        return nullptr;
    }

    std::optional<double> NumberOf(const ::World::NBTTag& tag) {
        switch (tag.type) {
            case ::World::NBTTagType::TAG_Byte:   return static_cast<const ::World::NBTTagByte&>(tag).value;
            case ::World::NBTTagType::TAG_Short:  return static_cast<const ::World::NBTTagShort&>(tag).value;
            case ::World::NBTTagType::TAG_Int:    return static_cast<const ::World::NBTTagInt&>(tag).value;
            case ::World::NBTTagType::TAG_Long:   return static_cast<double>(static_cast<const ::World::NBTTagLong&>(tag).value);
            case ::World::NBTTagType::TAG_Float:  return static_cast<const ::World::NBTTagFloat&>(tag).value;
            case ::World::NBTTagType::TAG_Double: return static_cast<const ::World::NBTTagDouble&>(tag).value;
            default: return std::nullopt;
        }
    }

    std::optional<bool> BoolOf(const ::World::NBTTag& tag) {
        if (auto n = NumberOf(tag)) return *n != 0.0;
        if (tag.type == ::World::NBTTagType::TAG_String) {
            const std::string& s = static_cast<const ::World::NBTTagString&>(tag).value;
            if (s == "true") return true;
            if (s == "false") return false;
        }
        return std::nullopt;
    }

    std::optional<std::string> StringOf(const ::World::NBTTag& tag) {
        const ::World::NBTTag* t = Unwrap(&tag);
        if (t && t->type == ::World::NBTTagType::TAG_String) {
            return static_cast<const ::World::NBTTagString&>(*t).value;
        }
        return std::nullopt;
    }

    const ::World::NBTTag* Unwrap(const ::World::NBTTag* tag) {
        auto compound = dynamic_cast<const ::World::NBTTagCompound*>(tag);
        if (compound && compound->value.size() == 1) {
            auto it = compound->value.find("");
            if (it != compound->value.end() && it->second) return it->second.get();
        }
        return tag;
    }

    const ::World::NBTTagCompound* AsCompound(const ::World::NBTTag* tag) {
        return tag && tag->type == ::World::NBTTagType::TAG_Compound
            ? static_cast<const ::World::NBTTagCompound*>(tag) : nullptr;
    }

    const ::World::NBTTagList* AsList(const ::World::NBTTag* tag) {
        return tag && tag->type == ::World::NBTTagType::TAG_List
            ? static_cast<const ::World::NBTTagList*>(tag) : nullptr;
    }

    std::string WithNamespace(std::string_view id) {
        if (!id.empty() && id[0] == '#') {
            const std::string_view rest = id.substr(1);
            return rest.find(':') == std::string_view::npos ? "#minecraft:" + std::string(rest) : std::string(id);
        }
        return id.find(':') == std::string_view::npos ? "minecraft:" + std::string(id) : std::string(id);
    }

    std::string_view StripMinecraft(std::string_view id) {
        return id.rfind("minecraft:", 0) == 0 ? id.substr(10) : id;
    }

    std::vector<std::string> ReadHolderSet(const ::World::NBTTag* tag) {
        std::vector<std::string> out;
        if (!tag) return out;
        if (auto s = StringOf(*tag)) {
            out.push_back(*s);
        } else if (const auto* list = AsList(tag)) {
            for (const auto& e : list->value) {
                if (!e) continue;
                if (auto str = StringOf(*e)) out.push_back(*str);
            }
        }
        return out;
    }

    void WriteHolderSet(Nbt::Writer& w, std::string_view name, const std::vector<std::string>& entries) {
        if (entries.size() == 1) {
            w.String(name, entries.front());
            return;
        }
        auto list = w.BeginList(name, Nbt::TagType::String);
        for (const std::string& e : entries) w.ListString(list, e);
        w.EndList(list);
    }

    std::vector<int32_t> ReadIntList(const ::World::NBTTag* tag) {
        std::vector<int32_t> out;
        if (!tag) return out;
        if (auto arr = dynamic_cast<const ::World::NBTTagIntArray*>(tag)) return arr->value;
        if (auto arr = dynamic_cast<const ::World::NBTTagByteArray*>(tag)) {
            for (int8_t v : arr->value) out.push_back(v);
            return out;
        }
        if (auto arr = dynamic_cast<const ::World::NBTTagLongArray*>(tag)) {
            for (int64_t v : arr->value) out.push_back(static_cast<int32_t>(v));
            return out;
        }
        if (const auto* list = AsList(tag)) {
            for (const auto& e : list->value) {
                if (!e) continue;
                if (auto n = NumberOf(*Unwrap(e.get()))) out.push_back(static_cast<int32_t>(*n));
            }
        }
        return out;
    }

    std::vector<float> ReadFloatList(const ::World::NBTTag* tag) {
        std::vector<float> out;
        if (!tag) return out;
        if (const auto* list = AsList(tag)) {
            for (const auto& e : list->value) {
                if (!e) continue;
                if (auto n = NumberOf(*Unwrap(e.get()))) out.push_back(static_cast<float>(*n));
            }
        } else {
            for (int32_t v : ReadIntList(tag)) out.push_back(static_cast<float>(v));
        }
        return out;
    }

    std::vector<std::string> ReadStringList(const ::World::NBTTag* tag) {
        std::vector<std::string> out;
        if (!tag) return out;
        if (auto s = StringOf(*tag)) {
            out.push_back(*s);
            return out;
        }
        if (const auto* list = AsList(tag)) {
            for (const auto& e : list->value) {
                if (!e) continue;
                if (auto s = StringOf(*e)) out.push_back(*s);
            }
        }
        return out;
    }

    namespace {
        std::optional<int32_t> ReadColor(const ::World::NBTTag& tag, size_t channels) {
            if (auto n = NumberOf(tag); n && tag.type != ::World::NBTTagType::TAG_Float &&
                                           tag.type != ::World::NBTTagType::TAG_Double) {
                return static_cast<int32_t>(static_cast<int64_t>(*n));
            }
            const std::vector<float> v = ReadFloatList(&tag);
            if (v.size() != channels) return std::nullopt;
            const auto channel = [](float f) {
                return static_cast<uint32_t>(std::clamp(static_cast<int>(std::floor(f * 255.0f + 0.5f)), 0, 255));
            };
            uint32_t rgb = 0;
            if (channels == 4) {
                // ARGB_COLOR_CODEC's vector form is [r, g, b, a].
                rgb = (channel(v[3]) << 24) | (channel(v[0]) << 16) | (channel(v[1]) << 8) | channel(v[2]);
            } else {
                rgb = (channel(v[0]) << 16) | (channel(v[1]) << 8) | channel(v[2]);
            }
            return static_cast<int32_t>(rgb);
        }
    }

    std::optional<int32_t> ReadRgb(const ::World::NBTTag& tag)  { return ReadColor(tag, 3); }
    std::optional<int32_t> ReadArgb(const ::World::NBTTag& tag) { return ReadColor(tag, 4); }

    std::optional<int> DyeFromName(std::string_view name) {
        name = StripMinecraft(name);
        for (int i = 0; i < 16; ++i) if (name == kDyeNames[i]) return i;
        return std::nullopt;
    }

    const char* DyeName(int id) {
        return kDyeNames[static_cast<size_t>(id) & 15u];
    }

    nlohmann::json NbtToJson(const ::World::NBTTag& tag) {
        const ::World::NBTTag& t = *Unwrap(&tag);
        switch (t.type) {
            case ::World::NBTTagType::TAG_String: return static_cast<const ::World::NBTTagString&>(t).value;
            case ::World::NBTTagType::TAG_Byte:   return static_cast<int>(static_cast<const ::World::NBTTagByte&>(t).value);
            case ::World::NBTTagType::TAG_Short:  return static_cast<int>(static_cast<const ::World::NBTTagShort&>(t).value);
            case ::World::NBTTagType::TAG_Int:    return static_cast<const ::World::NBTTagInt&>(t).value;
            case ::World::NBTTagType::TAG_Long:   return static_cast<const ::World::NBTTagLong&>(t).value;
            case ::World::NBTTagType::TAG_Float:  return static_cast<const ::World::NBTTagFloat&>(t).value;
            case ::World::NBTTagType::TAG_Double: return static_cast<const ::World::NBTTagDouble&>(t).value;
            case ::World::NBTTagType::TAG_List: {
                nlohmann::json out = nlohmann::json::array();
                for (const auto& e : static_cast<const ::World::NBTTagList&>(t).value) if (e) out.push_back(NbtToJson(*e));
                return out;
            }
            case ::World::NBTTagType::TAG_Compound: {
                nlohmann::json out = nlohmann::json::object();
                for (const auto& [k, v] : static_cast<const ::World::NBTTagCompound&>(t).value) if (v) out[k] = NbtToJson(*v);
                return out;
            }
            case ::World::NBTTagType::TAG_Byte_Array: {
                nlohmann::json out = nlohmann::json::array();
                for (int8_t v : static_cast<const ::World::NBTTagByteArray&>(t).value) out.push_back(static_cast<int>(v));
                return out;
            }
            case ::World::NBTTagType::TAG_Int_Array: {
                nlohmann::json out = nlohmann::json::array();
                for (int32_t v : static_cast<const ::World::NBTTagIntArray&>(t).value) out.push_back(v);
                return out;
            }
            case ::World::NBTTagType::TAG_Long_Array: {
                nlohmann::json out = nlohmann::json::array();
                for (int64_t v : static_cast<const ::World::NBTTagLongArray&>(t).value) out.push_back(v);
                return out;
            }
            default:
                return nullptr;
        }
    }

} // namespace Game::Anvil::ComponentNbt
