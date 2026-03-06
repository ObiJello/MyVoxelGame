#pragma once

#include "nbt/Tag.h"
#include <vector>
#include <sstream>
#include <stdexcept>

// Reference: net/minecraft/nbt/ListTag.java

namespace minecraft {
namespace nbt {

// Forward declaration
class CompoundTag;

/**
 * ListTag - A list of tags all having the same type
 * Reference: ListTag.java
 */
class ListTag : public Tag {
public:
    // Reference: ListTag.java line 25
    ListTag() : m_type(TagType::TAG_END) {}

    explicit ListTag(std::vector<std::unique_ptr<Tag>> tags, TagType type)
        : m_tags(std::move(tags)), m_type(type) {}

    TagType getId() const override { return TagType::TAG_LIST; }

    // Reference: ListTag.java getType()
    TagType getElementType() const { return m_type; }

    size_t size() const { return m_tags.size(); }
    bool isEmpty() const { return m_tags.empty(); }

    // Reference: ListTag.java get(int)
    Tag* get(size_t index) const {
        if (index >= m_tags.size()) {
            return nullptr;
        }
        return m_tags[index].get();
    }

    // Reference: ListTag.java getCompound(int)
    CompoundTag* getCompound(size_t index) const;

    // Reference: ListTag.java getList(int)
    ListTag* getList(size_t index) const {
        Tag* tag = get(index);
        if (tag && tag->getId() == TagType::TAG_LIST) {
            return static_cast<ListTag*>(tag);
        }
        return nullptr;
    }

    // Reference: ListTag.java getShort(int)
    int16_t getShort(size_t index) const {
        Tag* tag = get(index);
        if (tag) {
            auto val = tag->asShort();
            if (val) return *val;
        }
        return 0;
    }

    // Reference: ListTag.java getInt(int)
    int32_t getInt(size_t index) const {
        Tag* tag = get(index);
        if (tag) {
            auto val = tag->asInt();
            if (val) return *val;
        }
        return 0;
    }

    // Reference: ListTag.java getIntArray(int)
    std::vector<int32_t> getIntArray(size_t index) const {
        Tag* tag = get(index);
        if (tag) {
            auto val = tag->asIntArray();
            if (val) return *val;
        }
        return {};
    }

    // Reference: ListTag.java getLongArray(int)
    std::vector<int64_t> getLongArray(size_t index) const {
        Tag* tag = get(index);
        if (tag) {
            auto val = tag->asLongArray();
            if (val) return *val;
        }
        return {};
    }

    // Reference: ListTag.java getDouble(int)
    double getDouble(size_t index) const {
        Tag* tag = get(index);
        if (tag) {
            auto val = tag->asDouble();
            if (val) return *val;
        }
        return 0.0;
    }

    // Reference: ListTag.java getFloat(int)
    float getFloat(size_t index) const {
        Tag* tag = get(index);
        if (tag) {
            auto val = tag->asFloat();
            if (val) return *val;
        }
        return 0.0f;
    }

    // Reference: ListTag.java getString(int)
    std::string getString(size_t index) const {
        Tag* tag = get(index);
        if (tag) {
            auto val = tag->asString();
            if (val) return *val;
        }
        return "";
    }

    // Reference: ListTag.java add(Tag)
    bool add(std::unique_ptr<Tag> tag) {
        if (!tag) return false;
        if (!updateType(tag->getId())) {
            return false;
        }
        m_tags.push_back(std::move(tag));
        return true;
    }

    // Reference: ListTag.java set(int, Tag)
    bool set(size_t index, std::unique_ptr<Tag> tag) {
        if (!tag || index >= m_tags.size()) return false;
        if (!updateType(tag->getId())) {
            return false;
        }
        m_tags[index] = std::move(tag);
        return true;
    }

    // Reference: ListTag.java addTag(int, Tag)
    bool addTag(size_t index, std::unique_ptr<Tag> tag) {
        if (!tag) return false;
        if (!updateType(tag->getId())) {
            return false;
        }
        if (index > m_tags.size()) {
            index = m_tags.size();
        }
        m_tags.insert(m_tags.begin() + index, std::move(tag));
        return true;
    }

    // Reference: ListTag.java remove(int)
    std::unique_ptr<Tag> remove(size_t index) {
        if (index >= m_tags.size()) {
            return nullptr;
        }
        auto tag = std::move(m_tags[index]);
        m_tags.erase(m_tags.begin() + index);
        return tag;
    }

    // Reference: ListTag.java clear()
    void clear() {
        m_tags.clear();
        m_type = TagType::TAG_END;
    }

    // Reference: ListTag.java write(DataOutput)
    void write(std::ostream& output) const override {
        // Write element type
        output.put(static_cast<char>(m_type));

        // Write size as int32 (big-endian)
        int32_t len = static_cast<int32_t>(m_tags.size());
        output.put(static_cast<char>((len >> 24) & 0xFF));
        output.put(static_cast<char>((len >> 16) & 0xFF));
        output.put(static_cast<char>((len >> 8) & 0xFF));
        output.put(static_cast<char>(len & 0xFF));

        // Write each tag's payload (no type byte or name, just payload)
        for (const auto& tag : m_tags) {
            tag->write(output);
        }
    }

    // Reference: ListTag.java toString()
    std::string toString() const override {
        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < m_tags.size(); ++i) {
            if (i > 0) oss << ",";
            oss << m_tags[i]->toString();
        }
        oss << "]";
        return oss.str();
    }

    // Reference: ListTag.java copy()
    std::unique_ptr<Tag> copy() const override {
        std::vector<std::unique_ptr<Tag>> copiedTags;
        copiedTags.reserve(m_tags.size());
        for (const auto& tag : m_tags) {
            copiedTags.push_back(tag->copy());
        }
        return std::make_unique<ListTag>(std::move(copiedTags), m_type);
    }

    // Reference: ListTag.java sizeInBytes()
    int sizeInBytes() const override {
        int size = 40;  // Base overhead (8 header + 12 type + 4 size + 16 list overhead)
        for (const auto& tag : m_tags) {
            size += tag->sizeInBytes();
        }
        return size;
    }

    ListTag* asList() override { return this; }
    const ListTag* asList() const override { return this; }

    // Iterator support
    auto begin() { return m_tags.begin(); }
    auto end() { return m_tags.end(); }
    auto begin() const { return m_tags.begin(); }
    auto end() const { return m_tags.end(); }

private:
    std::vector<std::unique_ptr<Tag>> m_tags;
    TagType m_type;

    // Reference: ListTag.java updateType(byte)
    bool updateType(TagType newType) {
        if (newType == TagType::TAG_END) {
            return false;
        }
        if (m_type == TagType::TAG_END) {
            m_type = newType;
            return true;
        }
        return m_type == newType;
    }
};

} // namespace nbt
} // namespace minecraft
