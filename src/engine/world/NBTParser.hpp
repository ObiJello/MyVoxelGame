// File: src/engine/world/NBTParser.hpp
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <iostream>

namespace World {

    // NBT Tag types as defined by Minecraft specification
    enum class NBTTagType : uint8_t {
        TAG_End = 0,
        TAG_Byte = 1,
        TAG_Short = 2,
        TAG_Int = 3,
        TAG_Long = 4,
        TAG_Float = 5,
        TAG_Double = 6,
        TAG_Byte_Array = 7,
        TAG_String = 8,
        TAG_List = 9,
        TAG_Compound = 10,
        TAG_Int_Array = 11,
        TAG_Long_Array = 12
    };

    // Forward declaration
    class NBTTag;
    using NBTTagPtr = std::shared_ptr<NBTTag>;

    // Base NBT tag class
    class NBTTag {
    public:
        NBTTagType type;
        std::string name;

        explicit NBTTag(NBTTagType tagType) : type(tagType) {}
        virtual ~NBTTag() = default;

        // Pure virtual methods for type-specific functionality
        virtual void Print(std::ostream& os, int indent = 0) const = 0;
        virtual std::string ToString() const = 0;

        // Static factory method to create tags from binary data
        static NBTTagPtr ParseTag(const std::vector<uint8_t>& data, size_t& offset, bool hasName = true);

        // Helper to get tag type name
        static std::string GetTagTypeName(NBTTagType type);
    };

    // Specific tag implementations
    class NBTTagByte : public NBTTag {
    public:
        int8_t value;

        explicit NBTTagByte(int8_t val = 0) : NBTTag(NBTTagType::TAG_Byte), value(val) {}
        void Print(std::ostream& os, int indent = 0) const override;
        std::string ToString() const override;
    };

    class NBTTagShort : public NBTTag {
    public:
        int16_t value;

        explicit NBTTagShort(int16_t val = 0) : NBTTag(NBTTagType::TAG_Short), value(val) {}
        void Print(std::ostream& os, int indent = 0) const override;
        std::string ToString() const override;
    };

    class NBTTagInt : public NBTTag {
    public:
        int32_t value;

        explicit NBTTagInt(int32_t val = 0) : NBTTag(NBTTagType::TAG_Int), value(val) {}
        void Print(std::ostream& os, int indent = 0) const override;
        std::string ToString() const override;
    };

    class NBTTagLong : public NBTTag {
    public:
        int64_t value;

        explicit NBTTagLong(int64_t val = 0) : NBTTag(NBTTagType::TAG_Long), value(val) {}
        void Print(std::ostream& os, int indent = 0) const override;
        std::string ToString() const override;
    };

    class NBTTagFloat : public NBTTag {
    public:
        float value;

        explicit NBTTagFloat(float val = 0.0f) : NBTTag(NBTTagType::TAG_Float), value(val) {}
        void Print(std::ostream& os, int indent = 0) const override;
        std::string ToString() const override;
    };

    class NBTTagDouble : public NBTTag {
    public:
        double value;

        explicit NBTTagDouble(double val = 0.0) : NBTTag(NBTTagType::TAG_Double), value(val) {}
        void Print(std::ostream& os, int indent = 0) const override;
        std::string ToString() const override;
    };

    class NBTTagString : public NBTTag {
    public:
        std::string value;

        explicit NBTTagString(const std::string& val = "") : NBTTag(NBTTagType::TAG_String), value(val) {}
        void Print(std::ostream& os, int indent = 0) const override;
        std::string ToString() const override;
    };

    class NBTTagByteArray : public NBTTag {
    public:
        std::vector<int8_t> value;

        NBTTagByteArray() : NBTTag(NBTTagType::TAG_Byte_Array) {}
        void Print(std::ostream& os, int indent = 0) const override;
        std::string ToString() const override;
    };

    class NBTTagIntArray : public NBTTag {
    public:
        std::vector<int32_t> value;

        NBTTagIntArray() : NBTTag(NBTTagType::TAG_Int_Array) {}
        void Print(std::ostream& os, int indent = 0) const override;
        std::string ToString() const override;
    };

    class NBTTagLongArray : public NBTTag {
    public:
        std::vector<int64_t> value;

        NBTTagLongArray() : NBTTag(NBTTagType::TAG_Long_Array) {}
        void Print(std::ostream& os, int indent = 0) const override;
        std::string ToString() const override;
    };

    class NBTTagList : public NBTTag {
    public:
        NBTTagType listType;
        std::vector<NBTTagPtr> value;

        explicit NBTTagList(NBTTagType elemType = NBTTagType::TAG_End)
            : NBTTag(NBTTagType::TAG_List), listType(elemType) {}
        void Print(std::ostream& os, int indent = 0) const override;
        std::string ToString() const override;
    };

    class NBTTagCompound : public NBTTag {
    public:
        std::unordered_map<std::string, NBTTagPtr> value;

        NBTTagCompound() : NBTTag(NBTTagType::TAG_Compound) {}
        void Print(std::ostream& os, int indent = 0) const override;
        std::string ToString() const override;

        // Convenience methods for accessing child tags
        NBTTagPtr GetTag(const std::string& tagName) const;
        bool HasTag(const std::string& tagName) const;

        template<typename T>
        T GetValue(const std::string& tagName, T defaultValue = T{}) const {
            auto tag = GetTag(tagName);
            if (!tag) return defaultValue;

            // Cast to specific tag type and return value
            if constexpr (std::is_same_v<T, int8_t>) {
                auto byteTag = std::dynamic_pointer_cast<NBTTagByte>(tag);
                return byteTag ? byteTag->value : defaultValue;
            } else if constexpr (std::is_same_v<T, int16_t>) {
                auto shortTag = std::dynamic_pointer_cast<NBTTagShort>(tag);
                return shortTag ? shortTag->value : defaultValue;
            } else if constexpr (std::is_same_v<T, int32_t>) {
                auto intTag = std::dynamic_pointer_cast<NBTTagInt>(tag);
                return intTag ? intTag->value : defaultValue;
            } else if constexpr (std::is_same_v<T, int64_t>) {
                auto longTag = std::dynamic_pointer_cast<NBTTagLong>(tag);
                return longTag ? longTag->value : defaultValue;
            } else if constexpr (std::is_same_v<T, float>) {
                auto floatTag = std::dynamic_pointer_cast<NBTTagFloat>(tag);
                return floatTag ? floatTag->value : defaultValue;
            } else if constexpr (std::is_same_v<T, double>) {
                auto doubleTag = std::dynamic_pointer_cast<NBTTagDouble>(tag);
                return doubleTag ? doubleTag->value : defaultValue;
            } else if constexpr (std::is_same_v<T, std::string>) {
                auto stringTag = std::dynamic_pointer_cast<NBTTagString>(tag);
                return stringTag ? stringTag->value : defaultValue;
            }

            return defaultValue;
        }
    };

    // Utility class for parsing NBT data
    class NBTParser {
    public:
        // Parse NBT data from binary buffer
        static NBTTagPtr Parse(const std::vector<uint8_t>& data);

        // Parse NBT data from binary buffer starting at offset
        static NBTTagPtr Parse(const std::vector<uint8_t>& data, size_t& offset);

        // DEBUG: Parse NBT data with extensive debugging output
        static NBTTagPtr ParseWithDebug(const std::vector<uint8_t>& data);
        static NBTTagPtr ParseWithDebug(const std::vector<uint8_t>& data, size_t& offset);

        // Utility functions for reading big-endian data
        static int16_t ReadInt16BE(const std::vector<uint8_t>& data, size_t& offset);
        static int32_t ReadInt32BE(const std::vector<uint8_t>& data, size_t& offset);
        static int64_t ReadInt64BE(const std::vector<uint8_t>& data, size_t& offset);
        static float ReadFloatBE(const std::vector<uint8_t>& data, size_t& offset);
        static double ReadDoubleBE(const std::vector<uint8_t>& data, size_t& offset);
        static std::string ReadStringBE(const std::vector<uint8_t>& data, size_t& offset);

        // Helper to convert bytes to float/double with proper endianness
        static float BytesToFloat(uint32_t bytes);
        static double BytesToDouble(uint64_t bytes);

    private:
        // Enhanced debugging helpers
        static void LogDataContext(const std::vector<uint8_t>& data, size_t offset,
                                  const std::string& context = "");
    };

} // namespace World