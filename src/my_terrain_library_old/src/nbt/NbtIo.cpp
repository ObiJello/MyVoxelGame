#include "nbt/NbtIo.h"
#include "nbt/PrimitiveTags.h"
#include "nbt/ArrayTags.h"
#include "nbt/ListTag.h"
#include <cstring>
#include <stdexcept>
#include <zlib.h>

// Reference: net/minecraft/nbt/NbtIo.java

namespace minecraft {
namespace nbt {

// =========================================================================
// Reading primitives (big-endian)
// =========================================================================

int8_t NbtIo::readByte(std::istream& input) {
    char c;
    input.get(c);
    return static_cast<int8_t>(c);
}

int16_t NbtIo::readShort(std::istream& input) {
    uint8_t b1, b2;
    input.read(reinterpret_cast<char*>(&b1), 1);
    input.read(reinterpret_cast<char*>(&b2), 1);
    return static_cast<int16_t>((b1 << 8) | b2);
}

int32_t NbtIo::readInt(std::istream& input) {
    uint8_t bytes[4];
    input.read(reinterpret_cast<char*>(bytes), 4);
    return static_cast<int32_t>(
        (static_cast<uint32_t>(bytes[0]) << 24) |
        (static_cast<uint32_t>(bytes[1]) << 16) |
        (static_cast<uint32_t>(bytes[2]) << 8) |
        static_cast<uint32_t>(bytes[3])
    );
}

int64_t NbtIo::readLong(std::istream& input) {
    uint8_t bytes[8];
    input.read(reinterpret_cast<char*>(bytes), 8);
    return static_cast<int64_t>(
        (static_cast<uint64_t>(bytes[0]) << 56) |
        (static_cast<uint64_t>(bytes[1]) << 48) |
        (static_cast<uint64_t>(bytes[2]) << 40) |
        (static_cast<uint64_t>(bytes[3]) << 32) |
        (static_cast<uint64_t>(bytes[4]) << 24) |
        (static_cast<uint64_t>(bytes[5]) << 16) |
        (static_cast<uint64_t>(bytes[6]) << 8) |
        static_cast<uint64_t>(bytes[7])
    );
}

float NbtIo::readFloat(std::istream& input) {
    union { int32_t i; float f; } u;
    u.i = readInt(input);
    return u.f;
}

double NbtIo::readDouble(std::istream& input) {
    union { int64_t i; double d; } u;
    u.i = readLong(input);
    return u.d;
}

std::string NbtIo::readString(std::istream& input) {
    uint16_t length = static_cast<uint16_t>(readShort(input));
    std::string str(length, '\0');
    input.read(&str[0], length);
    return str;
}

// =========================================================================
// Writing primitives (big-endian)
// =========================================================================

void NbtIo::writeByte(std::ostream& output, int8_t value) {
    output.put(static_cast<char>(value));
}

void NbtIo::writeShort(std::ostream& output, int16_t value) {
    output.put(static_cast<char>((value >> 8) & 0xFF));
    output.put(static_cast<char>(value & 0xFF));
}

void NbtIo::writeInt(std::ostream& output, int32_t value) {
    output.put(static_cast<char>((value >> 24) & 0xFF));
    output.put(static_cast<char>((value >> 16) & 0xFF));
    output.put(static_cast<char>((value >> 8) & 0xFF));
    output.put(static_cast<char>(value & 0xFF));
}

void NbtIo::writeLong(std::ostream& output, int64_t value) {
    for (int i = 7; i >= 0; --i) {
        output.put(static_cast<char>((value >> (i * 8)) & 0xFF));
    }
}

void NbtIo::writeFloat(std::ostream& output, float value) {
    union { float f; int32_t i; } u;
    u.f = value;
    writeInt(output, u.i);
}

void NbtIo::writeDouble(std::ostream& output, double value) {
    union { double d; int64_t i; } u;
    u.d = value;
    writeLong(output, u.i);
}

void NbtIo::writeString(std::ostream& output, const std::string& value) {
    writeShort(output, static_cast<int16_t>(value.size()));
    output.write(value.data(), value.size());
}

// =========================================================================
// Tag reading implementations
// =========================================================================

std::unique_ptr<Tag> NbtIo::readByteTag(std::istream& input) {
    return std::make_unique<ByteTag>(readByte(input));
}

std::unique_ptr<Tag> NbtIo::readShortTag(std::istream& input) {
    return std::make_unique<ShortTag>(readShort(input));
}

std::unique_ptr<Tag> NbtIo::readIntTag(std::istream& input) {
    return std::make_unique<IntTag>(readInt(input));
}

std::unique_ptr<Tag> NbtIo::readLongTag(std::istream& input) {
    return std::make_unique<LongTag>(readLong(input));
}

std::unique_ptr<Tag> NbtIo::readFloatTag(std::istream& input) {
    return std::make_unique<FloatTag>(readFloat(input));
}

std::unique_ptr<Tag> NbtIo::readDoubleTag(std::istream& input) {
    return std::make_unique<DoubleTag>(readDouble(input));
}

std::unique_ptr<Tag> NbtIo::readStringTag(std::istream& input) {
    return std::make_unique<StringTag>(readString(input));
}

std::unique_ptr<Tag> NbtIo::readByteArrayTag(std::istream& input) {
    int32_t length = readInt(input);
    std::vector<int8_t> data(length);
    input.read(reinterpret_cast<char*>(data.data()), length);
    return std::make_unique<ByteArrayTag>(std::move(data));
}

std::unique_ptr<Tag> NbtIo::readIntArrayTag(std::istream& input) {
    int32_t length = readInt(input);
    std::vector<int32_t> data(length);
    for (int32_t i = 0; i < length; ++i) {
        data[i] = readInt(input);
    }
    return std::make_unique<IntArrayTag>(std::move(data));
}

std::unique_ptr<Tag> NbtIo::readLongArrayTag(std::istream& input) {
    int32_t length = readInt(input);
    std::vector<int64_t> data(length);
    for (int32_t i = 0; i < length; ++i) {
        data[i] = readLong(input);
    }
    return std::make_unique<LongArrayTag>(std::move(data));
}

std::unique_ptr<Tag> NbtIo::readListTag(std::istream& input) {
    TagType elementType = static_cast<TagType>(readByte(input));
    int32_t length = readInt(input);

    std::vector<std::unique_ptr<Tag>> tags;
    tags.reserve(length);

    for (int32_t i = 0; i < length; ++i) {
        auto tag = readTagPayload(input, elementType);
        if (tag) {
            tags.push_back(std::move(tag));
        }
    }

    return std::make_unique<ListTag>(std::move(tags), elementType);
}

std::unique_ptr<Tag> NbtIo::readCompoundTag(std::istream& input) {
    auto compound = std::make_unique<CompoundTag>();

    while (true) {
        auto [name, tag] = readNamedTag(input);
        if (!tag) {
            // TAG_END encountered
            break;
        }
        compound->put(name, std::move(tag));
    }

    return compound;
}

// Reference: NbtIo.java readTagPayload
std::unique_ptr<Tag> NbtIo::readTagPayload(std::istream& input, TagType type) {
    switch (type) {
        case TagType::TAG_END:
            return nullptr;
        case TagType::TAG_BYTE:
            return readByteTag(input);
        case TagType::TAG_SHORT:
            return readShortTag(input);
        case TagType::TAG_INT:
            return readIntTag(input);
        case TagType::TAG_LONG:
            return readLongTag(input);
        case TagType::TAG_FLOAT:
            return readFloatTag(input);
        case TagType::TAG_DOUBLE:
            return readDoubleTag(input);
        case TagType::TAG_BYTE_ARRAY:
            return readByteArrayTag(input);
        case TagType::TAG_STRING:
            return readStringTag(input);
        case TagType::TAG_LIST:
            return readListTag(input);
        case TagType::TAG_COMPOUND:
            return readCompoundTag(input);
        case TagType::TAG_INT_ARRAY:
            return readIntArrayTag(input);
        case TagType::TAG_LONG_ARRAY:
            return readLongArrayTag(input);
        default:
            throw std::runtime_error("Unknown tag type: " + std::to_string(static_cast<int>(type)));
    }
}

// Reference: NbtIo.java readNamedTag
std::pair<std::string, std::unique_ptr<Tag>> NbtIo::readNamedTag(std::istream& input) {
    TagType type = static_cast<TagType>(readByte(input));

    if (type == TagType::TAG_END) {
        return {"", nullptr};
    }

    std::string name = readString(input);
    auto tag = readTagPayload(input, type);

    return {name, std::move(tag)};
}

// =========================================================================
// High-level read/write functions
// Reference: NbtIo.java lines 30-100
// =========================================================================

std::unique_ptr<CompoundTag> NbtIo::read(std::istream& input) {
    auto [name, tag] = readNamedTag(input);

    if (!tag || tag->getId() != TagType::TAG_COMPOUND) {
        throw std::runtime_error("Root tag must be a compound tag");
    }

    return std::unique_ptr<CompoundTag>(static_cast<CompoundTag*>(tag.release()));
}

// Reference: NbtIo.java write(CompoundTag, DataOutput)
void NbtIo::write(const CompoundTag& tag, std::ostream& output) {
    // Write type byte
    writeByte(output, static_cast<int8_t>(TagType::TAG_COMPOUND));

    // Write empty name for root tag
    writeString(output, "");

    // Write compound payload
    tag.write(output);
}

// =========================================================================
// Compression support using zlib
// Reference: NbtIo.java readCompressed/writeCompressed
// =========================================================================

std::unique_ptr<CompoundTag> NbtIo::readCompressed(std::istream& input) {
    // Read entire compressed data
    std::vector<char> compressedData(std::istreambuf_iterator<char>(input), {});

    // Decompress using zlib
    z_stream strm{};
    strm.next_in = reinterpret_cast<Bytef*>(compressedData.data());
    strm.avail_in = static_cast<uInt>(compressedData.size());

    // Initialize for gzip decompression (windowBits + 16)
    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {
        throw std::runtime_error("Failed to initialize zlib inflation");
    }

    std::vector<char> decompressedData;
    char buffer[16384];

    int ret;
    do {
        strm.next_out = reinterpret_cast<Bytef*>(buffer);
        strm.avail_out = sizeof(buffer);

        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            inflateEnd(&strm);
            throw std::runtime_error("zlib decompression error");
        }

        size_t have = sizeof(buffer) - strm.avail_out;
        decompressedData.insert(decompressedData.end(), buffer, buffer + have);
    } while (ret != Z_STREAM_END);

    inflateEnd(&strm);

    // Parse the decompressed NBT data
    std::string decompressedStr(decompressedData.begin(), decompressedData.end());
    std::istringstream iss(decompressedStr, std::ios::binary);
    return read(iss);
}

void NbtIo::writeCompressed(const CompoundTag& tag, std::ostream& output) {
    // First write uncompressed to a buffer
    std::ostringstream buffer(std::ios::binary);
    write(tag, buffer);
    std::string uncompressedData = buffer.str();

    // Compress using zlib/gzip
    z_stream strm{};
    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(uncompressedData.data()));
    strm.avail_in = static_cast<uInt>(uncompressedData.size());

    // Initialize for gzip compression (windowBits + 16)
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("Failed to initialize zlib deflation");
    }

    char outBuffer[16384];
    int ret;

    do {
        strm.next_out = reinterpret_cast<Bytef*>(outBuffer);
        strm.avail_out = sizeof(outBuffer);

        ret = deflate(&strm, Z_FINISH);
        if (ret == Z_STREAM_ERROR) {
            deflateEnd(&strm);
            throw std::runtime_error("zlib compression error");
        }

        size_t have = sizeof(outBuffer) - strm.avail_out;
        output.write(outBuffer, have);
    } while (strm.avail_out == 0);

    deflateEnd(&strm);
}

std::unique_ptr<CompoundTag> NbtIo::readCompressedFromFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return nullptr;
    }

    // Check if file is gzip compressed (magic bytes 0x1f 0x8b)
    char magic[2];
    file.read(magic, 2);
    file.seekg(0);

    if (static_cast<uint8_t>(magic[0]) == 0x1f &&
        static_cast<uint8_t>(magic[1]) == 0x8b) {
        return readCompressed(file);
    } else {
        return read(file);
    }
}

void NbtIo::writeCompressedToFile(const CompoundTag& tag, const std::string& path) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open file for writing: " + path);
    }
    writeCompressed(tag, file);
}

} // namespace nbt
} // namespace minecraft
