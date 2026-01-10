#pragma once

#include "nbt/CompoundTag.h"
#include <fstream>
#include <memory>
#include <optional>
#include <string>

// Reference: net/minecraft/nbt/NbtIo.java

namespace minecraft {
namespace nbt {

/**
 * NbtIo - Utilities for reading and writing NBT data
 * Reference: NbtIo.java
 *
 * NBT format:
 * - All numbers are big-endian
 * - Root tag is always a CompoundTag
 * - Format: type_byte + name_length(2 bytes) + name + payload
 */
class NbtIo {
public:
    // =========================================================================
    // Reading NBT data
    // Reference: NbtIo.java lines 30-100
    // =========================================================================

    /**
     * Read a compound tag from an uncompressed input stream
     * Reference: NbtIo.java read(DataInput)
     */
    static std::unique_ptr<CompoundTag> read(std::istream& input);

    /**
     * Read a compound tag from a compressed (gzip) input stream
     * Reference: NbtIo.java readCompressed(InputStream)
     */
    static std::unique_ptr<CompoundTag> readCompressed(std::istream& input);

    /**
     * Read a compound tag from a file (auto-detects compression)
     * Reference: NbtIo.java readCompressed(Path)
     */
    static std::unique_ptr<CompoundTag> readCompressedFromFile(const std::string& path);

    // =========================================================================
    // Writing NBT data
    // Reference: NbtIo.java lines 102-180
    // =========================================================================

    /**
     * Write a compound tag to an uncompressed output stream
     * Reference: NbtIo.java write(CompoundTag, DataOutput)
     */
    static void write(const CompoundTag& tag, std::ostream& output);

    /**
     * Write a compound tag to a compressed (gzip) output stream
     * Reference: NbtIo.java writeCompressed(CompoundTag, OutputStream)
     */
    static void writeCompressed(const CompoundTag& tag, std::ostream& output);

    /**
     * Write a compound tag to a compressed file
     * Reference: NbtIo.java writeCompressed(CompoundTag, Path)
     */
    static void writeCompressedToFile(const CompoundTag& tag, const std::string& path);

    // =========================================================================
    // Low-level reading utilities
    // Reference: NbtIo.java lines 182-250
    // =========================================================================

    /**
     * Read a single tag from input (without type byte - type is provided)
     */
    static std::unique_ptr<Tag> readTagPayload(std::istream& input, TagType type);

    /**
     * Read a named tag from input (with type byte and name)
     * Returns nullptr if TAG_END is encountered
     */
    static std::pair<std::string, std::unique_ptr<Tag>> readNamedTag(std::istream& input);

    // =========================================================================
    // Helper methods for reading primitives (big-endian)
    // =========================================================================

    static int8_t readByte(std::istream& input);
    static int16_t readShort(std::istream& input);
    static int32_t readInt(std::istream& input);
    static int64_t readLong(std::istream& input);
    static float readFloat(std::istream& input);
    static double readDouble(std::istream& input);
    static std::string readString(std::istream& input);

    // =========================================================================
    // Helper methods for writing primitives (big-endian)
    // =========================================================================

    static void writeByte(std::ostream& output, int8_t value);
    static void writeShort(std::ostream& output, int16_t value);
    static void writeInt(std::ostream& output, int32_t value);
    static void writeLong(std::ostream& output, int64_t value);
    static void writeFloat(std::ostream& output, float value);
    static void writeDouble(std::ostream& output, double value);
    static void writeString(std::ostream& output, const std::string& value);

private:
    NbtIo() = delete;  // Static utility class

    // Read tag implementations
    static std::unique_ptr<Tag> readByteTag(std::istream& input);
    static std::unique_ptr<Tag> readShortTag(std::istream& input);
    static std::unique_ptr<Tag> readIntTag(std::istream& input);
    static std::unique_ptr<Tag> readLongTag(std::istream& input);
    static std::unique_ptr<Tag> readFloatTag(std::istream& input);
    static std::unique_ptr<Tag> readDoubleTag(std::istream& input);
    static std::unique_ptr<Tag> readStringTag(std::istream& input);
    static std::unique_ptr<Tag> readByteArrayTag(std::istream& input);
    static std::unique_ptr<Tag> readIntArrayTag(std::istream& input);
    static std::unique_ptr<Tag> readLongArrayTag(std::istream& input);
    static std::unique_ptr<Tag> readListTag(std::istream& input);
    static std::unique_ptr<Tag> readCompoundTag(std::istream& input);
};

} // namespace nbt
} // namespace minecraft
