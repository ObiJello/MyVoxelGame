// File: src/world/NBTParser.cpp
#include "NBTParser.hpp"
#include "../core/Log.hpp"
#include <iomanip>
#include <sstream>
#include <cstring>

namespace World {

    // Helper function to create indentation
    static std::string Indent(int level) {
        return std::string(level * 2, ' ');
    }

    // NBTTag static methods
    std::string NBTTag::GetTagTypeName(NBTTagType type) {
        switch (type) {
            case NBTTagType::TAG_End: return "TAG_End";
            case NBTTagType::TAG_Byte: return "TAG_Byte";
            case NBTTagType::TAG_Short: return "TAG_Short";
            case NBTTagType::TAG_Int: return "TAG_Int";
            case NBTTagType::TAG_Long: return "TAG_Long";
            case NBTTagType::TAG_Float: return "TAG_Float";
            case NBTTagType::TAG_Double: return "TAG_Double";
            case NBTTagType::TAG_Byte_Array: return "TAG_Byte_Array";
            case NBTTagType::TAG_String: return "TAG_String";
            case NBTTagType::TAG_List: return "TAG_List";
            case NBTTagType::TAG_Compound: return "TAG_Compound";
            case NBTTagType::TAG_Int_Array: return "TAG_Int_Array";
            case NBTTagType::TAG_Long_Array: return "TAG_Long_Array";
            default: return "TAG_Unknown";
        }
    }

    NBTTagPtr NBTTag::ParseTag(const std::vector<uint8_t>& data, size_t& offset, bool hasName) {
        if (offset >= data.size()) {
            throw std::runtime_error("Unexpected end of data while parsing NBT tag");
        }

        NBTTagType tagType = static_cast<NBTTagType>(data[offset++]);

        if (tagType == NBTTagType::TAG_End) {
            return nullptr; // End tag has no name or payload
        }

        NBTTagPtr tag;

        // Read tag name if present
        std::string tagName;
        if (hasName) {
            tagName = NBTParser::ReadStringBE(data, offset);
        }

        // Create appropriate tag type and parse payload
        switch (tagType) {
            case NBTTagType::TAG_Byte: {
                auto byteTag = std::make_shared<NBTTagByte>();
                if (offset >= data.size()) throw std::runtime_error("Unexpected end of data in TAG_Byte");
                byteTag->value = static_cast<int8_t>(data[offset++]);
                tag = byteTag;
                break;
            }

            case NBTTagType::TAG_Short: {
                auto shortTag = std::make_shared<NBTTagShort>();
                shortTag->value = NBTParser::ReadInt16BE(data, offset);
                tag = shortTag;
                break;
            }

            case NBTTagType::TAG_Int: {
                auto intTag = std::make_shared<NBTTagInt>();
                intTag->value = NBTParser::ReadInt32BE(data, offset);
                tag = intTag;
                break;
            }

            case NBTTagType::TAG_Long: {
                auto longTag = std::make_shared<NBTTagLong>();
                longTag->value = NBTParser::ReadInt64BE(data, offset);
                tag = longTag;
                break;
            }

            case NBTTagType::TAG_Float: {
                auto floatTag = std::make_shared<NBTTagFloat>();
                floatTag->value = NBTParser::ReadFloatBE(data, offset);
                tag = floatTag;
                break;
            }

            case NBTTagType::TAG_Double: {
                auto doubleTag = std::make_shared<NBTTagDouble>();
                doubleTag->value = NBTParser::ReadDoubleBE(data, offset);
                tag = doubleTag;
                break;
            }

            case NBTTagType::TAG_String: {
                auto stringTag = std::make_shared<NBTTagString>();
                stringTag->value = NBTParser::ReadStringBE(data, offset);
                tag = stringTag;
                break;
            }

            case NBTTagType::TAG_Byte_Array: {
                auto arrayTag = std::make_shared<NBTTagByteArray>();
                int32_t length = NBTParser::ReadInt32BE(data, offset);
                arrayTag->value.resize(length);
                for (int32_t i = 0; i < length; ++i) {
                    if (offset >= data.size()) throw std::runtime_error("Unexpected end of data in TAG_Byte_Array");
                    arrayTag->value[i] = static_cast<int8_t>(data[offset++]);
                }
                tag = arrayTag;
                break;
            }

            case NBTTagType::TAG_List: {
                auto listTag = std::make_shared<NBTTagList>();
                if (offset >= data.size()) throw std::runtime_error("Unexpected end of data in TAG_List");
                listTag->listType = static_cast<NBTTagType>(data[offset++]);
                int32_t length = NBTParser::ReadInt32BE(data, offset);

                for (int32_t i = 0; i < length; ++i) {
                    // List elements don't have names
                    NBTTagPtr element = ParseTag(data, offset, false);
                    if (element) {
                        listTag->value.push_back(element);
                    }
                }
                tag = listTag;
                break;
            }

            case NBTTagType::TAG_Compound: {
                auto compoundTag = std::make_shared<NBTTagCompound>();

                while (true) {
                    if (offset >= data.size()) throw std::runtime_error("Unexpected end of data in TAG_Compound");

                    // Check for TAG_End
                    if (data[offset] == static_cast<uint8_t>(NBTTagType::TAG_End)) {
                        offset++; // Consume the TAG_End
                        break;
                    }

                    NBTTagPtr childTag = ParseTag(data, offset, true);
                    if (childTag) {
                        compoundTag->value[childTag->name] = childTag;
                    }
                }
                tag = compoundTag;
                break;
            }

            case NBTTagType::TAG_Int_Array: {
                auto arrayTag = std::make_shared<NBTTagIntArray>();
                int32_t length = NBTParser::ReadInt32BE(data, offset);
                arrayTag->value.resize(length);
                for (int32_t i = 0; i < length; ++i) {
                    arrayTag->value[i] = NBTParser::ReadInt32BE(data, offset);
                }
                tag = arrayTag;
                break;
            }

            case NBTTagType::TAG_Long_Array: {
                auto arrayTag = std::make_shared<NBTTagLongArray>();
                int32_t length = NBTParser::ReadInt32BE(data, offset);
                arrayTag->value.resize(length);
                for (int32_t i = 0; i < length; ++i) {
                    arrayTag->value[i] = NBTParser::ReadInt64BE(data, offset);
                }
                tag = arrayTag;
                break;
            }

            default:
                throw std::runtime_error("Unknown NBT tag type: " + std::to_string(static_cast<int>(tagType)));
        }

        if (tag) {
            tag->name = tagName;
        }

        return tag;
    }

    // Tag implementation methods
    void NBTTagByte::Print(std::ostream& os, int indent) const {
        os << Indent(indent) << "TAG_Byte('" << name << "'): " << static_cast<int>(value) << std::endl;
    }

    std::string NBTTagByte::ToString() const {
        return "TAG_Byte('" + name + "'): " + std::to_string(static_cast<int>(value));
    }

    void NBTTagShort::Print(std::ostream& os, int indent) const {
        os << Indent(indent) << "TAG_Short('" << name << "'): " << value << std::endl;
    }

    std::string NBTTagShort::ToString() const {
        return "TAG_Short('" + name + "'): " + std::to_string(value);
    }

    void NBTTagInt::Print(std::ostream& os, int indent) const {
        os << Indent(indent) << "TAG_Int('" << name << "'): " << value << std::endl;
    }

    std::string NBTTagInt::ToString() const {
        return "TAG_Int('" + name + "'): " + std::to_string(value);
    }

    void NBTTagLong::Print(std::ostream& os, int indent) const {
        os << Indent(indent) << "TAG_Long('" << name << "'): " << value << std::endl;
    }

    std::string NBTTagLong::ToString() const {
        return "TAG_Long('" + name + "'): " + std::to_string(value);
    }

    void NBTTagFloat::Print(std::ostream& os, int indent) const {
        os << Indent(indent) << "TAG_Float('" << name << "'): " << std::fixed << std::setprecision(6) << value << std::endl;
    }

    std::string NBTTagFloat::ToString() const {
        std::ostringstream oss;
        oss << "TAG_Float('" << name << "'): " << std::fixed << std::setprecision(6) << value;
        return oss.str();
    }

    void NBTTagDouble::Print(std::ostream& os, int indent) const {
        os << Indent(indent) << "TAG_Double('" << name << "'): " << std::fixed << std::setprecision(12) << value << std::endl;
    }

    std::string NBTTagDouble::ToString() const {
        std::ostringstream oss;
        oss << "TAG_Double('" << name << "'): " << std::fixed << std::setprecision(12) << value;
        return oss.str();
    }

    void NBTTagString::Print(std::ostream& os, int indent) const {
        os << Indent(indent) << "TAG_String('" << name << "'): '" << value << "'" << std::endl;
    }

    std::string NBTTagString::ToString() const {
        return "TAG_String('" + name + "'): '" + value + "'";
    }

    void NBTTagByteArray::Print(std::ostream& os, int indent) const {
        os << Indent(indent) << "TAG_Byte_Array('" << name << "'): [" << value.size() << " bytes]";
        if (value.size() <= 16) {
            os << " {";
            for (size_t i = 0; i < value.size(); ++i) {
                if (i > 0) os << ", ";
                os << static_cast<int>(value[i]);
            }
            os << "}";
        }
        os << std::endl;
    }

    std::string NBTTagByteArray::ToString() const {
        return "TAG_Byte_Array('" + name + "'): [" + std::to_string(value.size()) + " bytes]";
    }

    void NBTTagIntArray::Print(std::ostream& os, int indent) const {
        os << Indent(indent) << "TAG_Int_Array('" << name << "'): [" << value.size() << " ints]";
        if (value.size() <= 8) {
            os << " {";
            for (size_t i = 0; i < value.size(); ++i) {
                if (i > 0) os << ", ";
                os << value[i];
            }
            os << "}";
        }
        os << std::endl;
    }

    std::string NBTTagIntArray::ToString() const {
        return "TAG_Int_Array('" + name + "'): [" + std::to_string(value.size()) + " ints]";
    }

    void NBTTagLongArray::Print(std::ostream& os, int indent) const {
        os << Indent(indent) << "TAG_Long_Array('" << name << "'): [" << value.size() << " longs]";
        if (value.size() <= 8) {
            os << " {";
            for (size_t i = 0; i < value.size(); ++i) {
                if (i > 0) os << ", ";
                os << value[i];
            }
            os << "}";
        }
        os << std::endl;
    }

    std::string NBTTagLongArray::ToString() const {
        return "TAG_Long_Array('" + name + "'): [" + std::to_string(value.size()) + " longs]";
    }

    void NBTTagList::Print(std::ostream& os, int indent) const {
        os << Indent(indent) << "TAG_List('" << name << "'): " << value.size() << " entries of type "
           << GetTagTypeName(listType) << std::endl;
        os << Indent(indent) << "{" << std::endl;
        for (const auto& tag : value) {
            if (tag) {
                tag->Print(os, indent + 1);
            }
        }
        os << Indent(indent) << "}" << std::endl;
    }

    std::string NBTTagList::ToString() const {
        return "TAG_List('" + name + "'): " + std::to_string(value.size()) + " entries of type " + GetTagTypeName(listType);
    }

    void NBTTagCompound::Print(std::ostream& os, int indent) const {
        os << Indent(indent) << "TAG_Compound('" << name << "'): " << value.size() << " entries" << std::endl;
        os << Indent(indent) << "{" << std::endl;
        for (const auto& [tagName, tag] : value) {
            if (tag) {
                tag->Print(os, indent + 1);
            }
        }
        os << Indent(indent) << "}" << std::endl;
    }

    std::string NBTTagCompound::ToString() const {
        return "TAG_Compound('" + name + "'): " + std::to_string(value.size()) + " entries";
    }

    NBTTagPtr NBTTagCompound::GetTag(const std::string& tagName) const {
        auto it = value.find(tagName);
        return (it != value.end()) ? it->second : nullptr;
    }

    bool NBTTagCompound::HasTag(const std::string& tagName) const {
        return value.find(tagName) != value.end();
    }

    // NBTParser implementation
    NBTTagPtr NBTParser::Parse(const std::vector<uint8_t>& data) {
        size_t offset = 0;
        return Parse(data, offset);
    }

    NBTTagPtr NBTParser::Parse(const std::vector<uint8_t>& data, size_t& offset) {
        try {
            return NBTTag::ParseTag(data, offset, true);
        } catch (const std::exception& e) {
            Log::Error("NBT parsing failed: %s", e.what());
            return nullptr;
        }
    }

    int16_t NBTParser::ReadInt16BE(const std::vector<uint8_t>& data, size_t& offset) {
        if (offset + 2 > data.size()) {
            throw std::runtime_error("Not enough data to read int16");
        }

        int16_t value = (static_cast<int16_t>(data[offset]) << 8) |
                        static_cast<int16_t>(data[offset + 1]);
        offset += 2;
        return value;
    }

    int32_t NBTParser::ReadInt32BE(const std::vector<uint8_t>& data, size_t& offset) {
        if (offset + 4 > data.size()) {
            throw std::runtime_error("Not enough data to read int32");
        }

        int32_t value = (static_cast<int32_t>(data[offset]) << 24) |
                        (static_cast<int32_t>(data[offset + 1]) << 16) |
                        (static_cast<int32_t>(data[offset + 2]) << 8) |
                        static_cast<int32_t>(data[offset + 3]);
        offset += 4;
        return value;
    }

    int64_t NBTParser::ReadInt64BE(const std::vector<uint8_t>& data, size_t& offset) {
        if (offset + 8 > data.size()) {
            throw std::runtime_error("Not enough data to read int64");
        }

        int64_t value = (static_cast<int64_t>(data[offset]) << 56) |
                        (static_cast<int64_t>(data[offset + 1]) << 48) |
                        (static_cast<int64_t>(data[offset + 2]) << 40) |
                        (static_cast<int64_t>(data[offset + 3]) << 32) |
                        (static_cast<int64_t>(data[offset + 4]) << 24) |
                        (static_cast<int64_t>(data[offset + 5]) << 16) |
                        (static_cast<int64_t>(data[offset + 6]) << 8) |
                        static_cast<int64_t>(data[offset + 7]);
        offset += 8;
        return value;
    }

    float NBTParser::ReadFloatBE(const std::vector<uint8_t>& data, size_t& offset) {
        uint32_t intValue = static_cast<uint32_t>(ReadInt32BE(data, offset));
        return BytesToFloat(intValue);
    }

    double NBTParser::ReadDoubleBE(const std::vector<uint8_t>& data, size_t& offset) {
        uint64_t longValue = static_cast<uint64_t>(ReadInt64BE(data, offset));
        return BytesToDouble(longValue);
    }

    std::string NBTParser::ReadStringBE(const std::vector<uint8_t>& data, size_t& offset) {
        int16_t length = ReadInt16BE(data, offset);
        if (length < 0) {
            throw std::runtime_error("Invalid string length: " + std::to_string(length));
        }

        if (offset + length > data.size()) {
            throw std::runtime_error("Not enough data to read string of length " + std::to_string(length));
        }

        std::string result(reinterpret_cast<const char*>(&data[offset]), length);
        offset += length;
        return result;
    }

    float NBTParser::BytesToFloat(uint32_t bytes) {
        float result;
        std::memcpy(&result, &bytes, sizeof(float));
        return result;
    }

    double NBTParser::BytesToDouble(uint64_t bytes) {
        double result;
        std::memcpy(&result, &bytes, sizeof(double));
        return result;
    }

} // namespace World