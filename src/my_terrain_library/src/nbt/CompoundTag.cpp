#include "nbt/CompoundTag.h"
#include "nbt/PrimitiveTags.h"
#include "nbt/ArrayTags.h"

// Reference: net/minecraft/nbt/CompoundTag.java

namespace minecraft {
namespace nbt {

// Reference: CompoundTag.java putByte(String, byte)
void CompoundTag::putByte(const std::string& key, int8_t value) {
    m_tags[key] = std::make_unique<ByteTag>(value);
}

// Reference: CompoundTag.java putShort(String, short)
void CompoundTag::putShort(const std::string& key, int16_t value) {
    m_tags[key] = std::make_unique<ShortTag>(value);
}

// Reference: CompoundTag.java putInt(String, int)
void CompoundTag::putInt(const std::string& key, int32_t value) {
    m_tags[key] = std::make_unique<IntTag>(value);
}

// Reference: CompoundTag.java putLong(String, long)
void CompoundTag::putLong(const std::string& key, int64_t value) {
    m_tags[key] = std::make_unique<LongTag>(value);
}

// Reference: CompoundTag.java putFloat(String, float)
void CompoundTag::putFloat(const std::string& key, float value) {
    m_tags[key] = std::make_unique<FloatTag>(value);
}

// Reference: CompoundTag.java putDouble(String, double)
void CompoundTag::putDouble(const std::string& key, double value) {
    m_tags[key] = std::make_unique<DoubleTag>(value);
}

// Reference: CompoundTag.java putString(String, String)
void CompoundTag::putString(const std::string& key, const std::string& value) {
    m_tags[key] = std::make_unique<StringTag>(value);
}

// Reference: CompoundTag.java putByteArray(String, byte[])
void CompoundTag::putByteArray(const std::string& key, std::vector<int8_t> value) {
    m_tags[key] = std::make_unique<ByteArrayTag>(std::move(value));
}

// Reference: CompoundTag.java putIntArray(String, int[])
void CompoundTag::putIntArray(const std::string& key, std::vector<int32_t> value) {
    m_tags[key] = std::make_unique<IntArrayTag>(std::move(value));
}

// Reference: CompoundTag.java putLongArray(String, long[])
void CompoundTag::putLongArray(const std::string& key, std::vector<int64_t> value) {
    m_tags[key] = std::make_unique<LongArrayTag>(std::move(value));
}

// Reference: CompoundTag.java putBoolean(String, boolean)
void CompoundTag::putBoolean(const std::string& key, bool value) {
    m_tags[key] = std::make_unique<ByteTag>(value ? 1 : 0);
}

} // namespace nbt
} // namespace minecraft
