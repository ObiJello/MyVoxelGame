#pragma once

#include "nbt/Tag.h"
#include <sstream>
#include <iomanip>

// Reference: net/minecraft/nbt/ByteTag.java, ShortTag.java, IntTag.java, etc.

namespace minecraft {
namespace nbt {

// =========================================================================
// ByteTag - 8-bit signed integer
// Reference: ByteTag.java
// =========================================================================
class ByteTag : public NumericTag {
public:
    explicit ByteTag(int8_t value) : m_value(value) {}

    static std::unique_ptr<ByteTag> valueOf(int8_t value) {
        return std::make_unique<ByteTag>(value);
    }

    static std::unique_ptr<ByteTag> valueOf(bool value) {
        return std::make_unique<ByteTag>(value ? 1 : 0);
    }

    int8_t getValue() const { return m_value; }

    TagType getId() const override { return TagType::TAG_BYTE; }

    void write(std::ostream& output) const override {
        output.put(static_cast<char>(m_value));
    }

    std::string toString() const override {
        return std::to_string(m_value) + "b";
    }

    std::unique_ptr<Tag> copy() const override {
        return std::make_unique<ByteTag>(m_value);
    }

    int sizeInBytes() const override { return 9; }  // 8 header + 1 byte

    int8_t byteValue() const override { return m_value; }
    int16_t shortValue() const override { return m_value; }
    int32_t intValue() const override { return m_value; }
    int64_t longValue() const override { return m_value; }
    float floatValue() const override { return static_cast<float>(m_value); }
    double doubleValue() const override { return static_cast<double>(m_value); }

private:
    int8_t m_value;
};

// =========================================================================
// ShortTag - 16-bit signed integer
// Reference: ShortTag.java
// =========================================================================
class ShortTag : public NumericTag {
public:
    explicit ShortTag(int16_t value) : m_value(value) {}

    static std::unique_ptr<ShortTag> valueOf(int16_t value) {
        return std::make_unique<ShortTag>(value);
    }

    int16_t getValue() const { return m_value; }

    TagType getId() const override { return TagType::TAG_SHORT; }

    void write(std::ostream& output) const override {
        // Big-endian
        output.put(static_cast<char>((m_value >> 8) & 0xFF));
        output.put(static_cast<char>(m_value & 0xFF));
    }

    std::string toString() const override {
        return std::to_string(m_value) + "s";
    }

    std::unique_ptr<Tag> copy() const override {
        return std::make_unique<ShortTag>(m_value);
    }

    int sizeInBytes() const override { return 10; }  // 8 header + 2 bytes

    int8_t byteValue() const override { return static_cast<int8_t>(m_value); }
    int16_t shortValue() const override { return m_value; }
    int32_t intValue() const override { return m_value; }
    int64_t longValue() const override { return m_value; }
    float floatValue() const override { return static_cast<float>(m_value); }
    double doubleValue() const override { return static_cast<double>(m_value); }

private:
    int16_t m_value;
};

// =========================================================================
// IntTag - 32-bit signed integer
// Reference: IntTag.java
// =========================================================================
class IntTag : public NumericTag {
public:
    explicit IntTag(int32_t value) : m_value(value) {}

    static std::unique_ptr<IntTag> valueOf(int32_t value) {
        return std::make_unique<IntTag>(value);
    }

    int32_t getValue() const { return m_value; }

    TagType getId() const override { return TagType::TAG_INT; }

    void write(std::ostream& output) const override {
        // Big-endian
        output.put(static_cast<char>((m_value >> 24) & 0xFF));
        output.put(static_cast<char>((m_value >> 16) & 0xFF));
        output.put(static_cast<char>((m_value >> 8) & 0xFF));
        output.put(static_cast<char>(m_value & 0xFF));
    }

    std::string toString() const override {
        return std::to_string(m_value);
    }

    std::unique_ptr<Tag> copy() const override {
        return std::make_unique<IntTag>(m_value);
    }

    int sizeInBytes() const override { return 12; }  // 8 header + 4 bytes

    int8_t byteValue() const override { return static_cast<int8_t>(m_value); }
    int16_t shortValue() const override { return static_cast<int16_t>(m_value); }
    int32_t intValue() const override { return m_value; }
    int64_t longValue() const override { return m_value; }
    float floatValue() const override { return static_cast<float>(m_value); }
    double doubleValue() const override { return static_cast<double>(m_value); }

private:
    int32_t m_value;
};

// =========================================================================
// LongTag - 64-bit signed integer
// Reference: LongTag.java
// =========================================================================
class LongTag : public NumericTag {
public:
    explicit LongTag(int64_t value) : m_value(value) {}

    static std::unique_ptr<LongTag> valueOf(int64_t value) {
        return std::make_unique<LongTag>(value);
    }

    int64_t getValue() const { return m_value; }

    TagType getId() const override { return TagType::TAG_LONG; }

    void write(std::ostream& output) const override {
        // Big-endian
        for (int i = 7; i >= 0; --i) {
            output.put(static_cast<char>((m_value >> (i * 8)) & 0xFF));
        }
    }

    std::string toString() const override {
        return std::to_string(m_value) + "L";
    }

    std::unique_ptr<Tag> copy() const override {
        return std::make_unique<LongTag>(m_value);
    }

    int sizeInBytes() const override { return 16; }  // 8 header + 8 bytes

    int8_t byteValue() const override { return static_cast<int8_t>(m_value); }
    int16_t shortValue() const override { return static_cast<int16_t>(m_value); }
    int32_t intValue() const override { return static_cast<int32_t>(m_value); }
    int64_t longValue() const override { return m_value; }
    float floatValue() const override { return static_cast<float>(m_value); }
    double doubleValue() const override { return static_cast<double>(m_value); }

private:
    int64_t m_value;
};

// =========================================================================
// FloatTag - 32-bit floating point
// Reference: FloatTag.java
// =========================================================================
class FloatTag : public NumericTag {
public:
    explicit FloatTag(float value) : m_value(value) {}

    static std::unique_ptr<FloatTag> valueOf(float value) {
        return std::make_unique<FloatTag>(value);
    }

    float getValue() const { return m_value; }

    TagType getId() const override { return TagType::TAG_FLOAT; }

    void write(std::ostream& output) const override {
        // Convert to int32 bits and write big-endian
        union { float f; int32_t i; } u;
        u.f = m_value;
        output.put(static_cast<char>((u.i >> 24) & 0xFF));
        output.put(static_cast<char>((u.i >> 16) & 0xFF));
        output.put(static_cast<char>((u.i >> 8) & 0xFF));
        output.put(static_cast<char>(u.i & 0xFF));
    }

    std::string toString() const override {
        std::ostringstream oss;
        oss << m_value << "f";
        return oss.str();
    }

    std::unique_ptr<Tag> copy() const override {
        return std::make_unique<FloatTag>(m_value);
    }

    int sizeInBytes() const override { return 12; }  // 8 header + 4 bytes

    int8_t byteValue() const override { return static_cast<int8_t>(m_value); }
    int16_t shortValue() const override { return static_cast<int16_t>(m_value); }
    int32_t intValue() const override { return static_cast<int32_t>(m_value); }
    int64_t longValue() const override { return static_cast<int64_t>(m_value); }
    float floatValue() const override { return m_value; }
    double doubleValue() const override { return static_cast<double>(m_value); }

private:
    float m_value;
};

// =========================================================================
// DoubleTag - 64-bit floating point
// Reference: DoubleTag.java
// =========================================================================
class DoubleTag : public NumericTag {
public:
    explicit DoubleTag(double value) : m_value(value) {}

    static std::unique_ptr<DoubleTag> valueOf(double value) {
        return std::make_unique<DoubleTag>(value);
    }

    double getValue() const { return m_value; }

    TagType getId() const override { return TagType::TAG_DOUBLE; }

    void write(std::ostream& output) const override {
        // Convert to int64 bits and write big-endian
        union { double d; int64_t i; } u;
        u.d = m_value;
        for (int i = 7; i >= 0; --i) {
            output.put(static_cast<char>((u.i >> (i * 8)) & 0xFF));
        }
    }

    std::string toString() const override {
        std::ostringstream oss;
        oss << m_value << "d";
        return oss.str();
    }

    std::unique_ptr<Tag> copy() const override {
        return std::make_unique<DoubleTag>(m_value);
    }

    int sizeInBytes() const override { return 16; }  // 8 header + 8 bytes

    int8_t byteValue() const override { return static_cast<int8_t>(m_value); }
    int16_t shortValue() const override { return static_cast<int16_t>(m_value); }
    int32_t intValue() const override { return static_cast<int32_t>(m_value); }
    int64_t longValue() const override { return static_cast<int64_t>(m_value); }
    float floatValue() const override { return static_cast<float>(m_value); }
    double doubleValue() const override { return m_value; }

private:
    double m_value;
};

// =========================================================================
// StringTag - UTF-8 string
// Reference: StringTag.java
// =========================================================================
class StringTag : public Tag {
public:
    explicit StringTag(std::string value) : m_value(std::move(value)) {}

    static std::unique_ptr<StringTag> valueOf(const std::string& value) {
        return std::make_unique<StringTag>(value);
    }

    const std::string& getValue() const { return m_value; }

    TagType getId() const override { return TagType::TAG_STRING; }

    void write(std::ostream& output) const override {
        // Write length as unsigned short (big-endian), then UTF-8 bytes
        uint16_t len = static_cast<uint16_t>(m_value.size());
        output.put(static_cast<char>((len >> 8) & 0xFF));
        output.put(static_cast<char>(len & 0xFF));
        output.write(m_value.data(), m_value.size());
    }

    std::string toString() const override {
        // Escape and quote the string
        std::ostringstream oss;
        oss << '"';
        for (char c : m_value) {
            if (c == '"' || c == '\\') {
                oss << '\\';
            }
            oss << c;
        }
        oss << '"';
        return oss.str();
    }

    std::unique_ptr<Tag> copy() const override {
        return std::make_unique<StringTag>(m_value);
    }

    int sizeInBytes() const override {
        return 36 + 2 * static_cast<int>(m_value.size());  // 8 header + 28 string + 2*length
    }

    std::optional<std::string> asString() const override { return m_value; }

private:
    std::string m_value;
};

} // namespace nbt
} // namespace minecraft
