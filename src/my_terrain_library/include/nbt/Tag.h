#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <optional>
#include <vector>
#include <ostream>
#include <istream>

// Reference: net/minecraft/nbt/Tag.java

namespace minecraft {
namespace nbt {

// Forward declarations
class CompoundTag;
class ListTag;

/**
 * TagType - NBT tag type identifiers
 * Reference: Tag.java lines 12-24
 */
enum class TagType : uint8_t {
    TAG_END = 0,
    TAG_BYTE = 1,
    TAG_SHORT = 2,
    TAG_INT = 3,
    TAG_LONG = 4,
    TAG_FLOAT = 5,
    TAG_DOUBLE = 6,
    TAG_BYTE_ARRAY = 7,
    TAG_STRING = 8,
    TAG_LIST = 9,
    TAG_COMPOUND = 10,
    TAG_INT_ARRAY = 11,
    TAG_LONG_ARRAY = 12
};

/**
 * Get the name of a tag type
 */
inline const char* getTagTypeName(TagType type) {
    switch (type) {
        case TagType::TAG_END: return "TAG_End";
        case TagType::TAG_BYTE: return "TAG_Byte";
        case TagType::TAG_SHORT: return "TAG_Short";
        case TagType::TAG_INT: return "TAG_Int";
        case TagType::TAG_LONG: return "TAG_Long";
        case TagType::TAG_FLOAT: return "TAG_Float";
        case TagType::TAG_DOUBLE: return "TAG_Double";
        case TagType::TAG_BYTE_ARRAY: return "TAG_Byte_Array";
        case TagType::TAG_STRING: return "TAG_String";
        case TagType::TAG_LIST: return "TAG_List";
        case TagType::TAG_COMPOUND: return "TAG_Compound";
        case TagType::TAG_INT_ARRAY: return "TAG_Int_Array";
        case TagType::TAG_LONG_ARRAY: return "TAG_Long_Array";
        default: return "UNKNOWN";
    }
}

/**
 * Tag - Base class for all NBT tags
 * Reference: Tag.java
 */
class Tag {
public:
    // Constants from Tag.java
    static constexpr int MAX_DEPTH = 512;

    virtual ~Tag() = default;

    /**
     * Get the tag type ID
     * Reference: Tag.java getId()
     */
    virtual TagType getId() const = 0;

    /**
     * Write the tag to a data output stream
     * Reference: Tag.java write()
     */
    virtual void write(std::ostream& output) const = 0;

    /**
     * Get a string representation
     * Reference: Tag.java toString()
     */
    virtual std::string toString() const = 0;

    /**
     * Create a deep copy of this tag
     * Reference: Tag.java copy()
     */
    virtual std::unique_ptr<Tag> copy() const = 0;

    /**
     * Get the size of this tag in bytes (for memory accounting)
     * Reference: Tag.java sizeInBytes()
     */
    virtual int sizeInBytes() const = 0;

    // =========================================================================
    // Optional type conversions (return empty if not applicable)
    // Reference: Tag.java lines 51-105
    // =========================================================================

    virtual std::optional<std::string> asString() const { return std::nullopt; }
    virtual std::optional<int8_t> asByte() const { return std::nullopt; }
    virtual std::optional<int16_t> asShort() const { return std::nullopt; }
    virtual std::optional<int32_t> asInt() const { return std::nullopt; }
    virtual std::optional<int64_t> asLong() const { return std::nullopt; }
    virtual std::optional<float> asFloat() const { return std::nullopt; }
    virtual std::optional<double> asDouble() const { return std::nullopt; }
    virtual std::optional<bool> asBoolean() const {
        auto b = asByte();
        return b ? std::optional<bool>(*b != 0) : std::nullopt;
    }
    virtual std::optional<std::vector<int8_t>> asByteArray() const { return std::nullopt; }
    virtual std::optional<std::vector<int32_t>> asIntArray() const { return std::nullopt; }
    virtual std::optional<std::vector<int64_t>> asLongArray() const { return std::nullopt; }

    // These return pointers to allow checking if conversion is valid
    virtual CompoundTag* asCompound() { return nullptr; }
    virtual const CompoundTag* asCompound() const { return nullptr; }
    virtual ListTag* asList() { return nullptr; }
    virtual const ListTag* asList() const { return nullptr; }

protected:
    Tag() = default;
    Tag(const Tag&) = default;
    Tag& operator=(const Tag&) = default;
};

/**
 * EndTag - Marks the end of a compound tag
 * Reference: net/minecraft/nbt/EndTag.java
 */
class EndTag : public Tag {
public:
    static EndTag INSTANCE;

    TagType getId() const override { return TagType::TAG_END; }
    void write(std::ostream& /*output*/) const override { /* Empty */ }
    std::string toString() const override { return "END"; }
    std::unique_ptr<Tag> copy() const override { return std::make_unique<EndTag>(); }
    int sizeInBytes() const override { return 8; }
};

/**
 * NumericTag - Base class for numeric tags
 * Reference: net/minecraft/nbt/NumericTag.java
 */
class NumericTag : public Tag {
public:
    virtual int8_t byteValue() const = 0;
    virtual int16_t shortValue() const = 0;
    virtual int32_t intValue() const = 0;
    virtual int64_t longValue() const = 0;
    virtual float floatValue() const = 0;
    virtual double doubleValue() const = 0;

    std::optional<int8_t> asByte() const override { return byteValue(); }
    std::optional<int16_t> asShort() const override { return shortValue(); }
    std::optional<int32_t> asInt() const override { return intValue(); }
    std::optional<int64_t> asLong() const override { return longValue(); }
    std::optional<float> asFloat() const override { return floatValue(); }
    std::optional<double> asDouble() const override { return doubleValue(); }
};

} // namespace nbt
} // namespace minecraft
