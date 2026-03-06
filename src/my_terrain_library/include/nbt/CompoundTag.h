#pragma once

#include "nbt/Tag.h"
#include "nbt/ListTag.h"
#include <unordered_map>
#include <set>
#include <sstream>

// Reference: net/minecraft/nbt/CompoundTag.java

namespace minecraft {
namespace nbt {

// Forward declarations for primitive tags (will be included via AllTags.h)
class ByteTag;
class ShortTag;
class IntTag;
class LongTag;
class FloatTag;
class DoubleTag;
class StringTag;
class ByteArrayTag;
class IntArrayTag;
class LongArrayTag;

/**
 * CompoundTag - A map of string keys to Tag values
 * Reference: CompoundTag.java
 */
class CompoundTag : public Tag {
public:
    // Reference: CompoundTag.java line 30
    CompoundTag() = default;

    explicit CompoundTag(std::unordered_map<std::string, std::unique_ptr<Tag>> tags)
        : m_tags(std::move(tags)) {}

    TagType getId() const override { return TagType::TAG_COMPOUND; }

    // =========================================================================
    // Size and containment checks
    // Reference: CompoundTag.java lines 32-58
    // =========================================================================

    size_t size() const { return m_tags.size(); }
    bool isEmpty() const { return m_tags.empty(); }

    // Reference: CompoundTag.java contains(String)
    bool contains(const std::string& key) const {
        return m_tags.find(key) != m_tags.end();
    }

    // Reference: CompoundTag.java contains(String, int)
    bool contains(const std::string& key, int tagType) const {
        auto it = m_tags.find(key);
        if (it == m_tags.end()) return false;
        TagType type = it->second->getId();
        if (tagType == static_cast<int>(type)) return true;
        // Special case: numeric types can be converted
        if (tagType == 99) {  // TAG_ANY_NUMERIC
            return type == TagType::TAG_BYTE || type == TagType::TAG_SHORT ||
                   type == TagType::TAG_INT || type == TagType::TAG_LONG ||
                   type == TagType::TAG_FLOAT || type == TagType::TAG_DOUBLE;
        }
        return false;
    }

    // =========================================================================
    // Put methods
    // Reference: CompoundTag.java lines 60-130
    // =========================================================================

    // Reference: CompoundTag.java put(String, Tag)
    Tag* put(const std::string& key, std::unique_ptr<Tag> tag) {
        Tag* result = tag.get();
        m_tags[key] = std::move(tag);
        return result;
    }

    // Reference: CompoundTag.java putByte(String, byte)
    void putByte(const std::string& key, int8_t value);

    // Reference: CompoundTag.java putShort(String, short)
    void putShort(const std::string& key, int16_t value);

    // Reference: CompoundTag.java putInt(String, int)
    void putInt(const std::string& key, int32_t value);

    // Reference: CompoundTag.java putLong(String, long)
    void putLong(const std::string& key, int64_t value);

    // Reference: CompoundTag.java putFloat(String, float)
    void putFloat(const std::string& key, float value);

    // Reference: CompoundTag.java putDouble(String, double)
    void putDouble(const std::string& key, double value);

    // Reference: CompoundTag.java putString(String, String)
    void putString(const std::string& key, const std::string& value);

    // Reference: CompoundTag.java putByteArray(String, byte[])
    void putByteArray(const std::string& key, std::vector<int8_t> value);

    // Reference: CompoundTag.java putIntArray(String, int[])
    void putIntArray(const std::string& key, std::vector<int32_t> value);

    // Reference: CompoundTag.java putLongArray(String, long[])
    void putLongArray(const std::string& key, std::vector<int64_t> value);

    // Reference: CompoundTag.java putBoolean(String, boolean)
    void putBoolean(const std::string& key, bool value);

    // =========================================================================
    // Get methods (raw Tag access)
    // Reference: CompoundTag.java lines 132-170
    // =========================================================================

    // Reference: CompoundTag.java get(String)
    Tag* get(const std::string& key) const {
        auto it = m_tags.find(key);
        return it != m_tags.end() ? it->second.get() : nullptr;
    }

    // Reference: CompoundTag.java remove(String)
    std::unique_ptr<Tag> remove(const std::string& key) {
        auto it = m_tags.find(key);
        if (it == m_tags.end()) return nullptr;
        auto tag = std::move(it->second);
        m_tags.erase(it);
        return tag;
    }

    // =========================================================================
    // Get methods with defaults (getXxxOr)
    // Reference: CompoundTag.java lines 172-250
    // =========================================================================

    // Reference: CompoundTag.java getByteOr(String, byte)
    int8_t getByteOr(const std::string& key, int8_t defaultVal) const {
        Tag* tag = get(key);
        if (tag) {
            auto val = tag->asByte();
            if (val) return *val;
        }
        return defaultVal;
    }

    // Reference: CompoundTag.java getShortOr(String, short)
    int16_t getShortOr(const std::string& key, int16_t defaultVal) const {
        Tag* tag = get(key);
        if (tag) {
            auto val = tag->asShort();
            if (val) return *val;
        }
        return defaultVal;
    }

    // Reference: CompoundTag.java getIntOr(String, int)
    int32_t getIntOr(const std::string& key, int32_t defaultVal) const {
        Tag* tag = get(key);
        if (tag) {
            auto val = tag->asInt();
            if (val) return *val;
        }
        return defaultVal;
    }

    // Reference: CompoundTag.java getLongOr(String, long)
    int64_t getLongOr(const std::string& key, int64_t defaultVal) const {
        Tag* tag = get(key);
        if (tag) {
            auto val = tag->asLong();
            if (val) return *val;
        }
        return defaultVal;
    }

    // Reference: CompoundTag.java getFloatOr(String, float)
    float getFloatOr(const std::string& key, float defaultVal) const {
        Tag* tag = get(key);
        if (tag) {
            auto val = tag->asFloat();
            if (val) return *val;
        }
        return defaultVal;
    }

    // Reference: CompoundTag.java getDoubleOr(String, double)
    double getDoubleOr(const std::string& key, double defaultVal) const {
        Tag* tag = get(key);
        if (tag) {
            auto val = tag->asDouble();
            if (val) return *val;
        }
        return defaultVal;
    }

    // Reference: CompoundTag.java getStringOr(String, String)
    std::string getStringOr(const std::string& key, const std::string& defaultVal) const {
        Tag* tag = get(key);
        if (tag) {
            auto val = tag->asString();
            if (val) return *val;
        }
        return defaultVal;
    }

    // Reference: CompoundTag.java getBooleanOr(String, boolean)
    bool getBooleanOr(const std::string& key, bool defaultVal) const {
        return getByteOr(key, defaultVal ? 1 : 0) != 0;
    }

    // =========================================================================
    // Get methods for arrays
    // Reference: CompoundTag.java lines 252-290
    // =========================================================================

    // Reference: CompoundTag.java getByteArray(String)
    std::vector<int8_t> getByteArray(const std::string& key) const {
        Tag* tag = get(key);
        if (tag) {
            auto val = tag->asByteArray();
            if (val) return *val;
        }
        return {};
    }

    // Reference: CompoundTag.java getIntArray(String)
    std::vector<int32_t> getIntArray(const std::string& key) const {
        Tag* tag = get(key);
        if (tag) {
            auto val = tag->asIntArray();
            if (val) return *val;
        }
        return {};
    }

    // Reference: CompoundTag.java getLongArray(String)
    std::vector<int64_t> getLongArray(const std::string& key) const {
        Tag* tag = get(key);
        if (tag) {
            auto val = tag->asLongArray();
            if (val) return *val;
        }
        return {};
    }

    // =========================================================================
    // Get methods for compound/list (return optional or pointers)
    // Reference: CompoundTag.java lines 292-340
    // =========================================================================

    // Reference: CompoundTag.java getCompound(String)
    std::optional<CompoundTag> getCompound(const std::string& key) const {
        Tag* tag = get(key);
        if (tag && tag->getId() == TagType::TAG_COMPOUND) {
            CompoundTag* compound = static_cast<CompoundTag*>(tag);
            // Return a deep copy
            auto copyTag = compound->copy();
            return std::optional<CompoundTag>(std::move(*static_cast<CompoundTag*>(copyTag.release())));
        }
        return std::nullopt;
    }

    // Get compound pointer (no copy)
    CompoundTag* getCompoundPtr(const std::string& key) const {
        Tag* tag = get(key);
        if (tag && tag->getId() == TagType::TAG_COMPOUND) {
            return static_cast<CompoundTag*>(tag);
        }
        return nullptr;
    }

    // Reference: CompoundTag.java getList(String)
    ListTag getListOrEmpty(const std::string& key) const {
        Tag* tag = get(key);
        if (tag && tag->getId() == TagType::TAG_LIST) {
            ListTag* list = static_cast<ListTag*>(tag);
            auto copyTag = list->copy();
            return std::move(*static_cast<ListTag*>(copyTag.release()));
        }
        return ListTag();
    }

    // Get list pointer (no copy)
    ListTag* getListPtr(const std::string& key) const {
        Tag* tag = get(key);
        if (tag && tag->getId() == TagType::TAG_LIST) {
            return static_cast<ListTag*>(tag);
        }
        return nullptr;
    }

    // Reference: CompoundTag.java getList(String, int) - filtered by element type
    ListTag getList(const std::string& key, int elementType) const {
        Tag* tag = get(key);
        if (tag && tag->getId() == TagType::TAG_LIST) {
            ListTag* list = static_cast<ListTag*>(tag);
            if (list->isEmpty() || static_cast<int>(list->getElementType()) == elementType) {
                auto copyTag = list->copy();
                return std::move(*static_cast<ListTag*>(copyTag.release()));
            }
        }
        return ListTag();
    }

    // =========================================================================
    // Key iteration
    // Reference: CompoundTag.java lines 342-350
    // =========================================================================

    // Reference: CompoundTag.java keySet()
    std::set<std::string> keySet() const {
        std::set<std::string> keys;
        for (const auto& pair : m_tags) {
            keys.insert(pair.first);
        }
        return keys;
    }

    // Reference: CompoundTag.java keys() - alias
    std::set<std::string> keys() const { return keySet(); }

    // =========================================================================
    // Merge
    // Reference: CompoundTag.java merge(CompoundTag)
    // =========================================================================

    // Reference: CompoundTag.java merge(CompoundTag)
    CompoundTag& merge(const CompoundTag& other) {
        for (const auto& pair : other.m_tags) {
            Tag* existing = get(pair.first);
            if (existing && existing->getId() == TagType::TAG_COMPOUND &&
                pair.second->getId() == TagType::TAG_COMPOUND) {
                // Recursively merge compounds
                static_cast<CompoundTag*>(existing)->merge(
                    *static_cast<CompoundTag*>(pair.second.get()));
            } else {
                m_tags[pair.first] = pair.second->copy();
            }
        }
        return *this;
    }

    // =========================================================================
    // Serialization
    // Reference: CompoundTag.java lines 352-400
    // =========================================================================

    // Reference: CompoundTag.java write(DataOutput)
    void write(std::ostream& output) const override {
        for (const auto& pair : m_tags) {
            writeNamedTag(output, pair.first, *pair.second);
        }
        // Write end tag
        output.put(static_cast<char>(TagType::TAG_END));
    }

    // Reference: CompoundTag.java toString()
    std::string toString() const override {
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto& pair : m_tags) {
            if (!first) oss << ",";
            first = false;
            oss << pair.first << ":" << pair.second->toString();
        }
        oss << "}";
        return oss.str();
    }

    // Reference: CompoundTag.java copy()
    std::unique_ptr<Tag> copy() const override {
        std::unordered_map<std::string, std::unique_ptr<Tag>> copiedTags;
        for (const auto& pair : m_tags) {
            copiedTags[pair.first] = pair.second->copy();
        }
        return std::make_unique<CompoundTag>(std::move(copiedTags));
    }

    // Reference: CompoundTag.java sizeInBytes()
    int sizeInBytes() const override {
        int size = 48;  // Base overhead
        for (const auto& pair : m_tags) {
            size += 36;  // Key overhead
            size += 2 * static_cast<int>(pair.first.size());  // Key string
            size += pair.second->sizeInBytes();
        }
        return size;
    }

    CompoundTag* asCompound() override { return this; }
    const CompoundTag* asCompound() const override { return this; }

    // Iterator support
    auto begin() { return m_tags.begin(); }
    auto end() { return m_tags.end(); }
    auto begin() const { return m_tags.begin(); }
    auto end() const { return m_tags.end(); }

private:
    std::unordered_map<std::string, std::unique_ptr<Tag>> m_tags;

    // Reference: CompoundTag.java writeNamedTag(DataOutput, String, Tag)
    static void writeNamedTag(std::ostream& output, const std::string& name, const Tag& tag) {
        // Write type byte
        output.put(static_cast<char>(tag.getId()));

        // Write name as modified UTF-8 (length as uint16, then bytes)
        uint16_t nameLen = static_cast<uint16_t>(name.size());
        output.put(static_cast<char>((nameLen >> 8) & 0xFF));
        output.put(static_cast<char>(nameLen & 0xFF));
        output.write(name.data(), name.size());

        // Write tag payload
        tag.write(output);
    }
};

// Implementation of ListTag::getCompound that depends on CompoundTag
inline CompoundTag* ListTag::getCompound(size_t index) const {
    Tag* tag = get(index);
    if (tag && tag->getId() == TagType::TAG_COMPOUND) {
        return static_cast<CompoundTag*>(tag);
    }
    return nullptr;
}

} // namespace nbt
} // namespace minecraft
