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
            throw std::runtime_error("Unexpected end of data while parsing NBT tag at offset " +
                                   std::to_string(offset));
        }

        NBTTagType tagType = static_cast<NBTTagType>(data[offset]);

        // Add validation for tag type
        if (static_cast<int>(tagType) < 0 || static_cast<int>(tagType) > 12) {
            std::ostringstream oss;
            oss << "Invalid NBT tag type: " << static_cast<int>(tagType)
                << " (0x" << std::hex << static_cast<unsigned>(tagType) << ") at offset " << std::dec << offset;
            Log::Error("NBT parsing error: %s", oss.str().c_str());
            throw std::runtime_error(oss.str());
        }

        offset++;

        if (tagType == NBTTagType::TAG_End) {
            return nullptr; // End tag has no name or payload
        }

        NBTTagPtr tag;

        // Read tag name if present
        std::string tagName;
        if (hasName) {
            try {
                tagName = NBTParser::ReadStringBE(data, offset);
            } catch (const std::exception& e) {
                Log::Error("Failed to read NBT tag name at offset %zu: %s", offset, e.what());
                throw;
            }
        }

        // Create appropriate tag type and parse payload
        try {
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

                    if (length < 0) {
                        throw std::runtime_error("Invalid byte array length: " + std::to_string(length));
                    }
                    if (offset + length > data.size()) {
                        throw std::runtime_error("Byte array extends beyond data bounds: offset=" +
                                               std::to_string(offset) + ", length=" + std::to_string(length) +
                                               ", data_size=" + std::to_string(data.size()));
                    }

                    arrayTag->value.resize(length);
                    for (int32_t i = 0; i < length; ++i) {
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

                    if (length < 0) {
                        throw std::runtime_error("Invalid list length: " + std::to_string(length));
                    }

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

                    if (length < 0) {
                        throw std::runtime_error("Invalid int array length: " + std::to_string(length));
                    }
                    if (offset + (length * 4) > data.size()) {
                        throw std::runtime_error("Int array extends beyond data bounds");
                    }

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

                    if (length < 0) {
                        throw std::runtime_error("Invalid long array length: " + std::to_string(length));
                    }
                    if (offset + (length * 8) > data.size()) {
                        throw std::runtime_error("Long array extends beyond data bounds: offset=" +
                                               std::to_string(offset) + ", length=" + std::to_string(length) +
                                               ", required_bytes=" + std::to_string(length * 8) +
                                               ", data_size=" + std::to_string(data.size()));
                    }

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
        } catch (const std::exception& e) {
            Log::Error("Failed to parse NBT tag of type %s (name: '%s') at offset %zu: %s",
                      GetTagTypeName(tagType).c_str(), tagName.c_str(), offset, e.what());
            throw;
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
            // Enhanced error message with more context
            std::ostringstream oss;
            oss << "Not enough data to read int64 at offset " << offset
                << " (need 8 bytes, have " << (data.size() - offset)
                << " bytes remaining, total data size: " << data.size() << ")";

            // Log the surrounding data for debugging
            Log::Error("NBT int64 read error: %s", oss.str().c_str());

            if (data.size() >= 16) {
                std::ostringstream hexDump;
                size_t start = (offset >= 8) ? offset - 8 : 0;
                size_t end = std::min(data.size(), offset + 16);

                for (size_t i = start; i < end; ++i) {
                    if (i == offset) hexDump << "[";
                    hexDump << std::hex << std::setw(2) << std::setfill('0')
                           << static_cast<unsigned>(data[i]);
                    if (i == offset + 7) hexDump << "]";
                    hexDump << " ";
                }
                Log::Error("Data context around offset %zu: %s", offset, hexDump.str().c_str());
            }

            throw std::runtime_error(oss.str());
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

     class NBTParserDebug {
    public:
        static NBTTagPtr ParseWithDebug(const std::vector<uint8_t>& data, size_t& offset) {
            Log::Debug("Starting NBT parsing at offset %zu, data size: %zu", offset, data.size());

            if (offset >= data.size()) {
                Log::Error("NBT parse error: offset %zu >= data size %zu", offset, data.size());
                return nullptr;
            }

            // Log first few bytes for debugging
            std::ostringstream hexDump;
            size_t dumpSize = std::min(data.size() - offset, size_t(32));
            for (size_t i = 0; i < dumpSize; ++i) {
                hexDump << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<unsigned>(data[offset + i]) << " ";
            }
            Log::Debug("NBT data hex dump (first %zu bytes): %s", dumpSize, hexDump.str().c_str());

            try {
                return ParseTagWithDebug(data, offset, true, 0);
            } catch (const std::exception& e) {
                Log::Error("NBT parsing exception at offset %zu: %s", offset, e.what());
                return nullptr;
            }
        }

        static NBTTagPtr ParseTagWithDebug(const std::vector<uint8_t>& data, size_t& offset,
                                          bool hasName, int depth) {
            const std::string indent(depth * 2, ' ');

            if (offset >= data.size()) {
                throw std::runtime_error("Unexpected end of data while parsing NBT tag at offset " +
                                       std::to_string(offset));
            }

            NBTTagType tagType = static_cast<NBTTagType>(data[offset]);
            Log::Debug("%sTag type at offset %zu: 0x%02X (%s)", indent.c_str(), offset,
                      static_cast<unsigned>(data[offset]), NBTTag::GetTagTypeName(tagType).c_str());
            offset++;

            if (tagType == NBTTagType::TAG_End) {
                Log::Debug("%sEncountered TAG_End", indent.c_str());
                return nullptr;
            }

            // Validate tag type
            if (static_cast<int>(tagType) < 0 || static_cast<int>(tagType) > 12) {
                throw std::runtime_error("Invalid NBT tag type: " + std::to_string(static_cast<int>(tagType)) +
                                       " at offset " + std::to_string(offset - 1));
            }

            NBTTagPtr tag;
            std::string tagName;

            // Read tag name if present
            if (hasName) {
                size_t nameOffset = offset;
                tagName = ReadStringBEWithDebug(data, offset, depth);
                Log::Debug("%sTag name at offset %zu: '%s'", indent.c_str(), nameOffset, tagName.c_str());
            }

            // Create appropriate tag type and parse payload
            switch (tagType) {
                case NBTTagType::TAG_Byte: {
                    if (offset >= data.size()) {
                        throw std::runtime_error("Unexpected end of data in TAG_Byte at offset " +
                                               std::to_string(offset));
                    }
                    auto byteTag = std::make_shared<NBTTagByte>();
                    byteTag->value = static_cast<int8_t>(data[offset]);
                    Log::Debug("%sByte value at offset %zu: %d", indent.c_str(), offset,
                              static_cast<int>(byteTag->value));
                    offset++;
                    tag = byteTag;
                    break;
                }

                case NBTTagType::TAG_Short: {
                    auto shortTag = std::make_shared<NBTTagShort>();
                    shortTag->value = ReadInt16BEWithDebug(data, offset, depth);
                    tag = shortTag;
                    break;
                }

                case NBTTagType::TAG_Int: {
                    auto intTag = std::make_shared<NBTTagInt>();
                    intTag->value = ReadInt32BEWithDebug(data, offset, depth);
                    tag = intTag;
                    break;
                }

                case NBTTagType::TAG_Long: {
                    auto longTag = std::make_shared<NBTTagLong>();
                    longTag->value = ReadInt64BEWithDebug(data, offset, depth);
                    tag = longTag;
                    break;
                }

                case NBTTagType::TAG_Float: {
                    auto floatTag = std::make_shared<NBTTagFloat>();
                    floatTag->value = ReadFloatBEWithDebug(data, offset, depth);
                    tag = floatTag;
                    break;
                }

                case NBTTagType::TAG_Double: {
                    auto doubleTag = std::make_shared<NBTTagDouble>();
                    doubleTag->value = ReadDoubleBEWithDebug(data, offset, depth);
                    tag = doubleTag;
                    break;
                }

                case NBTTagType::TAG_String: {
                    auto stringTag = std::make_shared<NBTTagString>();
                    stringTag->value = ReadStringBEWithDebug(data, offset, depth);
                    tag = stringTag;
                    break;
                }

                case NBTTagType::TAG_Byte_Array: {
                    auto arrayTag = std::make_shared<NBTTagByteArray>();
                    int32_t length = ReadInt32BEWithDebug(data, offset, depth);
                    Log::Debug("%sByte array length: %d", indent.c_str(), length);

                    if (length < 0) {
                        throw std::runtime_error("Invalid byte array length: " + std::to_string(length));
                    }
                    if (offset + length > data.size()) {
                        throw std::runtime_error("Byte array extends beyond data bounds: offset=" +
                                               std::to_string(offset) + ", length=" + std::to_string(length) +
                                               ", data_size=" + std::to_string(data.size()));
                    }

                    arrayTag->value.resize(length);
                    for (int32_t i = 0; i < length; ++i) {
                        arrayTag->value[i] = static_cast<int8_t>(data[offset++]);
                    }
                    tag = arrayTag;
                    break;
                }

                case NBTTagType::TAG_List: {
                    auto listTag = std::make_shared<NBTTagList>();
                    if (offset >= data.size()) {
                        throw std::runtime_error("Unexpected end of data in TAG_List header");
                    }

                    listTag->listType = static_cast<NBTTagType>(data[offset]);
                    Log::Debug("%sList type: %s", indent.c_str(),
                              NBTTag::GetTagTypeName(listTag->listType).c_str());
                    offset++;

                    int32_t length = ReadInt32BEWithDebug(data, offset, depth);
                    Log::Debug("%sList length: %d", indent.c_str(), length);

                    if (length < 0) {
                        throw std::runtime_error("Invalid list length: " + std::to_string(length));
                    }

                    for (int32_t i = 0; i < length; ++i) {
                        Log::Debug("%sParsing list element %d/%d", indent.c_str(), i + 1, length);

                        // Create temporary data with list type prefix for parsing
                        std::vector<uint8_t> elementData;
                        elementData.push_back(static_cast<uint8_t>(listTag->listType));
                        elementData.insert(elementData.end(), data.begin() + offset, data.end());

                        size_t elementOffset = 0;
                        NBTTagPtr element = ParseTagWithDebug(elementData, elementOffset, false, depth + 1);

                        if (element) {
                            listTag->value.push_back(element);
                        }

                        // Update main offset (subtract 1 because we added the type byte)
                        offset += elementOffset - 1;
                    }
                    tag = listTag;
                    break;
                }

                case NBTTagType::TAG_Compound: {
                    auto compoundTag = std::make_shared<NBTTagCompound>();
                    Log::Debug("%sStarting compound parsing", indent.c_str());

                    int childCount = 0;
                    while (true) {
                        if (offset >= data.size()) {
                            throw std::runtime_error("Unexpected end of data in TAG_Compound");
                        }

                        // Check for TAG_End
                        if (data[offset] == static_cast<uint8_t>(NBTTagType::TAG_End)) {
                            Log::Debug("%sEncountered TAG_End in compound after %d children",
                                      indent.c_str(), childCount);
                            offset++; // Consume the TAG_End
                            break;
                        }

                        Log::Debug("%sParsing compound child %d", indent.c_str(), childCount);
                        NBTTagPtr childTag = ParseTagWithDebug(data, offset, true, depth + 1);
                        if (childTag) {
                            compoundTag->value[childTag->name] = childTag;
                            childCount++;
                        }
                    }

                    Log::Debug("%sCompound parsing complete with %d children", indent.c_str(), childCount);
                    tag = compoundTag;
                    break;
                }

                case NBTTagType::TAG_Int_Array: {
                    auto arrayTag = std::make_shared<NBTTagIntArray>();
                    int32_t length = ReadInt32BEWithDebug(data, offset, depth);
                    Log::Debug("%sInt array length: %d", indent.c_str(), length);

                    if (length < 0) {
                        throw std::runtime_error("Invalid int array length: " + std::to_string(length));
                    }
                    if (offset + (length * 4) > data.size()) {
                        throw std::runtime_error("Int array extends beyond data bounds");
                    }

                    arrayTag->value.resize(length);
                    for (int32_t i = 0; i < length; ++i) {
                        arrayTag->value[i] = ReadInt32BEWithDebug(data, offset, depth + 1);
                    }
                    tag = arrayTag;
                    break;
                }

                case NBTTagType::TAG_Long_Array: {
                    auto arrayTag = std::make_shared<NBTTagLongArray>();
                    int32_t length = ReadInt32BEWithDebug(data, offset, depth);
                    Log::Debug("%sLong array length: %d", indent.c_str(), length);

                    if (length < 0) {
                        throw std::runtime_error("Invalid long array length: " + std::to_string(length));
                    }
                    if (offset + (length * 8) > data.size()) {
                        throw std::runtime_error("Long array extends beyond data bounds: offset=" +
                                               std::to_string(offset) + ", length=" + std::to_string(length) +
                                               ", required_bytes=" + std::to_string(length * 8) +
                                               ", data_size=" + std::to_string(data.size()));
                    }

                    arrayTag->value.resize(length);
                    for (int32_t i = 0; i < length; ++i) {
                        arrayTag->value[i] = ReadInt64BEWithDebug(data, offset, depth + 1);
                    }
                    tag = arrayTag;
                    break;
                }

                default:
                    throw std::runtime_error("Unknown NBT tag type: " + std::to_string(static_cast<int>(tagType)));
            }

            if (tag) {
                tag->name = tagName;
                Log::Debug("%sSuccessfully parsed tag '%s' of type %s", indent.c_str(),
                          tagName.c_str(), NBTTag::GetTagTypeName(tagType).c_str());
            }

            return tag;
        }

    private:
        static int16_t ReadInt16BEWithDebug(const std::vector<uint8_t>& data, size_t& offset, int depth) {
            const std::string indent(depth * 2, ' ');
            if (offset + 2 > data.size()) {
                throw std::runtime_error("Not enough data to read int16 at offset " + std::to_string(offset) +
                                       " (need 2 bytes, have " + std::to_string(data.size() - offset) + ")");
            }

            int16_t value = (static_cast<int16_t>(data[offset]) << 8) |
                           static_cast<int16_t>(data[offset + 1]);
            Log::Debug("%sRead int16 at offset %zu: %d (0x%04X)", indent.c_str(), offset, value,
                      static_cast<unsigned>(value) & 0xFFFF);
            offset += 2;
            return value;
        }

        static int32_t ReadInt32BEWithDebug(const std::vector<uint8_t>& data, size_t& offset, int depth) {
            const std::string indent(depth * 2, ' ');
            if (offset + 4 > data.size()) {
                throw std::runtime_error("Not enough data to read int32 at offset " + std::to_string(offset) +
                                       " (need 4 bytes, have " + std::to_string(data.size() - offset) + ")");
            }

            int32_t value = (static_cast<int32_t>(data[offset]) << 24) |
                           (static_cast<int32_t>(data[offset + 1]) << 16) |
                           (static_cast<int32_t>(data[offset + 2]) << 8) |
                           static_cast<int32_t>(data[offset + 3]);
            Log::Debug("%sRead int32 at offset %zu: %d (0x%08X)", indent.c_str(), offset, value,
                      static_cast<unsigned>(value));
            offset += 4;
            return value;
        }

        static int64_t ReadInt64BEWithDebug(const std::vector<uint8_t>& data, size_t& offset, int depth) {
            const std::string indent(depth * 2, ' ');
            if (offset + 8 > data.size()) {
                throw std::runtime_error("Not enough data to read int64 at offset " + std::to_string(offset) +
                                       " (need 8 bytes, have " + std::to_string(data.size() - offset) + ")");
            }

            int64_t value = (static_cast<int64_t>(data[offset]) << 56) |
                           (static_cast<int64_t>(data[offset + 1]) << 48) |
                           (static_cast<int64_t>(data[offset + 2]) << 40) |
                           (static_cast<int64_t>(data[offset + 3]) << 32) |
                           (static_cast<int64_t>(data[offset + 4]) << 24) |
                           (static_cast<int64_t>(data[offset + 5]) << 16) |
                           (static_cast<int64_t>(data[offset + 6]) << 8) |
                           static_cast<int64_t>(data[offset + 7]);
            Log::Debug("%sRead int64 at offset %zu: %lld (0x%016llX)", indent.c_str(), offset,
                      static_cast<long long>(value), static_cast<unsigned long long>(value));
            offset += 8;
            return value;
        }

        static float ReadFloatBEWithDebug(const std::vector<uint8_t>& data, size_t& offset, int depth) {
            uint32_t intValue = static_cast<uint32_t>(ReadInt32BEWithDebug(data, offset, depth));
            float result = NBTParser::BytesToFloat(intValue);
            const std::string indent(depth * 2, ' ');
            Log::Debug("%sFloat value: %f", indent.c_str(), result);
            return result;
        }

        static double ReadDoubleBEWithDebug(const std::vector<uint8_t>& data, size_t& offset, int depth) {
            uint64_t longValue = static_cast<uint64_t>(ReadInt64BEWithDebug(data, offset, depth));
            double result = NBTParser::BytesToDouble(longValue);
            const std::string indent(depth * 2, ' ');
            Log::Debug("%sDouble value: %f", indent.c_str(), result);
            return result;
        }

        static std::string ReadStringBEWithDebug(const std::vector<uint8_t>& data, size_t& offset, int depth) {
            const std::string indent(depth * 2, ' ');
            int16_t length = ReadInt16BEWithDebug(data, offset, depth);
            if (length < 0) {
                throw std::runtime_error("Invalid string length: " + std::to_string(length));
            }

            if (offset + length > data.size()) {
                throw std::runtime_error("Not enough data to read string of length " + std::to_string(length) +
                                       " at offset " + std::to_string(offset));
            }

            std::string result(reinterpret_cast<const char*>(&data[offset]), length);
            Log::Debug("%sString length: %d, value: '%s'", indent.c_str(), length, result.c_str());
            offset += length;
            return result;
        }
    };

    // Add debug method to NBTParser
    NBTTagPtr NBTParser::ParseWithDebug(const std::vector<uint8_t>& data) {
        size_t offset = 0;
        return NBTParserDebug::ParseWithDebug(data, offset);
    }

    NBTTagPtr NBTParser::ParseWithDebug(const std::vector<uint8_t>& data, size_t& offset) {
        return NBTParserDebug::ParseWithDebug(data, offset);
    }

} // namespace World