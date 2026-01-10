#pragma once

#include "nbt/Tag.h"
#include <sstream>

// Reference: net/minecraft/nbt/ByteArrayTag.java, IntArrayTag.java, LongArrayTag.java

namespace minecraft {
namespace nbt {

// =========================================================================
// ByteArrayTag - Array of bytes
// Reference: ByteArrayTag.java
// =========================================================================
class ByteArrayTag : public Tag {
public:
    explicit ByteArrayTag(std::vector<int8_t> value) : m_value(std::move(value)) {}

    ByteArrayTag(const int8_t* data, size_t size) : m_value(data, data + size) {}

    const std::vector<int8_t>& getValue() const { return m_value; }
    std::vector<int8_t>& getValue() { return m_value; }

    const int8_t* getAsByteArray() const { return m_value.data(); }
    size_t size() const { return m_value.size(); }

    int8_t get(size_t index) const { return m_value[index]; }
    void set(size_t index, int8_t value) { m_value[index] = value; }

    TagType getId() const override { return TagType::TAG_BYTE_ARRAY; }

    void write(std::ostream& output) const override {
        // Write length as int32 (big-endian)
        int32_t len = static_cast<int32_t>(m_value.size());
        output.put(static_cast<char>((len >> 24) & 0xFF));
        output.put(static_cast<char>((len >> 16) & 0xFF));
        output.put(static_cast<char>((len >> 8) & 0xFF));
        output.put(static_cast<char>(len & 0xFF));
        // Write bytes
        for (int8_t b : m_value) {
            output.put(static_cast<char>(b));
        }
    }

    std::string toString() const override {
        std::ostringstream oss;
        oss << "[B;";
        for (size_t i = 0; i < m_value.size(); ++i) {
            if (i > 0) oss << ",";
            oss << static_cast<int>(m_value[i]) << "B";
        }
        oss << "]";
        return oss.str();
    }

    std::unique_ptr<Tag> copy() const override {
        return std::make_unique<ByteArrayTag>(m_value);
    }

    int sizeInBytes() const override {
        return 24 + static_cast<int>(m_value.size());  // 12 array header + 12 + size
    }

    std::optional<std::vector<int8_t>> asByteArray() const override { return m_value; }

private:
    std::vector<int8_t> m_value;
};

// =========================================================================
// IntArrayTag - Array of 32-bit integers
// Reference: IntArrayTag.java
// =========================================================================
class IntArrayTag : public Tag {
public:
    explicit IntArrayTag(std::vector<int32_t> value) : m_value(std::move(value)) {}

    IntArrayTag(const int32_t* data, size_t size) : m_value(data, data + size) {}

    const std::vector<int32_t>& getValue() const { return m_value; }
    std::vector<int32_t>& getValue() { return m_value; }

    const int32_t* getAsIntArray() const { return m_value.data(); }
    size_t size() const { return m_value.size(); }

    int32_t get(size_t index) const { return m_value[index]; }
    void set(size_t index, int32_t value) { m_value[index] = value; }

    TagType getId() const override { return TagType::TAG_INT_ARRAY; }

    void write(std::ostream& output) const override {
        // Write length as int32 (big-endian)
        int32_t len = static_cast<int32_t>(m_value.size());
        output.put(static_cast<char>((len >> 24) & 0xFF));
        output.put(static_cast<char>((len >> 16) & 0xFF));
        output.put(static_cast<char>((len >> 8) & 0xFF));
        output.put(static_cast<char>(len & 0xFF));
        // Write ints (big-endian)
        for (int32_t v : m_value) {
            output.put(static_cast<char>((v >> 24) & 0xFF));
            output.put(static_cast<char>((v >> 16) & 0xFF));
            output.put(static_cast<char>((v >> 8) & 0xFF));
            output.put(static_cast<char>(v & 0xFF));
        }
    }

    std::string toString() const override {
        std::ostringstream oss;
        oss << "[I;";
        for (size_t i = 0; i < m_value.size(); ++i) {
            if (i > 0) oss << ",";
            oss << m_value[i];
        }
        oss << "]";
        return oss.str();
    }

    std::unique_ptr<Tag> copy() const override {
        return std::make_unique<IntArrayTag>(m_value);
    }

    int sizeInBytes() const override {
        return 24 + 4 * static_cast<int>(m_value.size());  // 12 array header + 12 + 4*size
    }

    std::optional<std::vector<int32_t>> asIntArray() const override { return m_value; }

private:
    std::vector<int32_t> m_value;
};

// =========================================================================
// LongArrayTag - Array of 64-bit integers
// Reference: LongArrayTag.java
// =========================================================================
class LongArrayTag : public Tag {
public:
    explicit LongArrayTag(std::vector<int64_t> value) : m_value(std::move(value)) {}

    LongArrayTag(const int64_t* data, size_t size) : m_value(data, data + size) {}

    const std::vector<int64_t>& getValue() const { return m_value; }
    std::vector<int64_t>& getValue() { return m_value; }

    const int64_t* getAsLongArray() const { return m_value.data(); }
    size_t size() const { return m_value.size(); }

    int64_t get(size_t index) const { return m_value[index]; }
    void set(size_t index, int64_t value) { m_value[index] = value; }

    TagType getId() const override { return TagType::TAG_LONG_ARRAY; }

    void write(std::ostream& output) const override {
        // Write length as int32 (big-endian)
        int32_t len = static_cast<int32_t>(m_value.size());
        output.put(static_cast<char>((len >> 24) & 0xFF));
        output.put(static_cast<char>((len >> 16) & 0xFF));
        output.put(static_cast<char>((len >> 8) & 0xFF));
        output.put(static_cast<char>(len & 0xFF));
        // Write longs (big-endian)
        for (int64_t v : m_value) {
            for (int i = 7; i >= 0; --i) {
                output.put(static_cast<char>((v >> (i * 8)) & 0xFF));
            }
        }
    }

    std::string toString() const override {
        std::ostringstream oss;
        oss << "[L;";
        for (size_t i = 0; i < m_value.size(); ++i) {
            if (i > 0) oss << ",";
            oss << m_value[i] << "L";
        }
        oss << "]";
        return oss.str();
    }

    std::unique_ptr<Tag> copy() const override {
        return std::make_unique<LongArrayTag>(m_value);
    }

    int sizeInBytes() const override {
        return 24 + 8 * static_cast<int>(m_value.size());  // 12 array header + 12 + 8*size
    }

    std::optional<std::vector<int64_t>> asLongArray() const override { return m_value; }

private:
    std::vector<int64_t> m_value;
};

} // namespace nbt
} // namespace minecraft
