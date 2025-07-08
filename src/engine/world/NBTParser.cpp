// File: src/engine/world/NBTParser.cpp
// Enhanced version with better 1.18+ chunk support and error handling
#include "NBTParser.hpp"
#include "../../core/Log.hpp"
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

            // Enhanced error context
            oss << "\nData context around offset " << offset << ": ";
            size_t start = (offset >= 8) ? offset - 8 : 0;
            size_t end = std::min(data.size(), offset + 16);
            for (size_t i = start; i < end; ++i) {
                if (i == offset) oss << "[";
                oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(data[i]);
                if (i == offset) oss << "]";
                oss << " ";
            }

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

                    // Enhanced bounds checking with better error message
                    if (offset + static_cast<size_t>(length) > data.size()) {
                        std::ostringstream oss;
                        oss << "Byte array extends beyond data bounds: "
                            << "offset=" << offset << ", length=" << length
                            << ", required_end=" << (offset + length)
                            << ", data_size=" << data.size()
                            << ", remaining_bytes=" << (data.size() - offset);
                        throw std::runtime_error(oss.str());
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

                    // Sanity check for extremely large lists (common in corrupted data)
                    if (length > 10000000) { // 10M elements seems excessive
                        std::ostringstream oss;
                        oss << "Suspiciously large list length: " << length
                            << " at offset " << (offset - 4) << " (tag name: '" << tagName << "')";
                        Log::Warning("%s", oss.str().c_str());

                        // Still try to parse, but with extra caution
                        if (offset + (length * 8) > data.size()) { // Rough estimate assuming 8 bytes per element
                            throw std::runtime_error("List would extend far beyond data bounds: " + oss.str());
                        }
                    }

                    for (int32_t i = 0; i < length; ++i) {
                        // Check if we have enough data left for at least one more tag
                        if (offset >= data.size()) {
                            throw std::runtime_error("Unexpected end of data while parsing list element " +
                                                   std::to_string(i) + "/" + std::to_string(length));
                        }

                        // List elements don't have names, but we need to inject the type
                        size_t elementStart = offset;

                        // Save current position and create a temporary buffer with the type prefix
                        std::vector<uint8_t> elementData;
                        elementData.push_back(static_cast<uint8_t>(listTag->listType));
                        elementData.insert(elementData.end(), data.begin() + offset, data.end());

                        size_t elementOffset = 0;
                        try {
                            NBTTagPtr element = ParseTag(elementData, elementOffset, false);
                            if (element) {
                                listTag->value.push_back(element);
                            }

                            // Update main offset (subtract 1 because we added the type byte)
                            offset = elementStart + (elementOffset - 1);
                        } catch (const std::exception& e) {
                            Log::Error("Failed to parse list element %d/%d in tag '%s': %s",
                                      i, length, tagName.c_str(), e.what());
                            throw;
                        }
                    }
                    tag = listTag;
                    break;
                }

                case NBTTagType::TAG_Compound: {
                    auto compoundTag = std::make_shared<NBTTagCompound>();

                    while (true) {
                        if (offset >= data.size()) {
                            throw std::runtime_error("Unexpected end of data in TAG_Compound '" + tagName + "'");
                        }

                        // Check for TAG_End
                        if (data[offset] == static_cast<uint8_t>(NBTTagType::TAG_End)) {
                            offset++; // Consume the TAG_End
                            break;
                        }

                        try {
                            NBTTagPtr childTag = ParseTag(data, offset, true);
                            if (childTag) {
                                compoundTag->value[childTag->name] = childTag;
                            }
                        } catch (const std::exception& e) {
                            Log::Error("Failed to parse child tag in compound '%s' at offset %zu: %s",
                                      tagName.c_str(), offset, e.what());
                            throw;
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

                    // Enhanced bounds checking
                    size_t requiredBytes = static_cast<size_t>(length) * 4;
                    if (offset + requiredBytes > data.size()) {
                        std::ostringstream oss;
                        oss << "Int array extends beyond data bounds: "
                            << "offset=" << offset << ", length=" << length
                            << ", required_bytes=" << requiredBytes
                            << ", data_size=" << data.size()
                            << ", remaining_bytes=" << (data.size() - offset)
                            << " (tag: '" << tagName << "')";
                        throw std::runtime_error(oss.str());
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

                    // Read length with extra validation
                    size_t lengthOffset = offset;
                    int32_t length = NBTParser::ReadInt32BE(data, offset);

                    if (length < 0) {
                        throw std::runtime_error("Invalid long array length: " + std::to_string(length) +
                                               " at offset " + std::to_string(lengthOffset) +
                                               " (tag: '" + tagName + "')");
                    }

                    // Sanity check - long arrays in Minecraft are typically small to medium sized
                    // Heightmaps are usually 37 longs, block data is 256 longs, etc.
                    if (length > 100000) { // 100k longs = 800KB, seems like a reasonable upper bound
                        std::ostringstream oss;
                        oss << "Suspiciously large long array length: " << length
                            << " at offset " << lengthOffset
                            << " (tag: '" << tagName << "')"
                            << "\nThis might indicate corrupted data or a parsing error.";
                        Log::Warning("%s", oss.str().c_str());
                    }

                    // Enhanced bounds checking with detailed error info
                    size_t requiredBytes = static_cast<size_t>(length) * 8;
                    if (offset + requiredBytes > data.size()) {
                        std::ostringstream oss;
                        oss << "Long array extends beyond data bounds: "
                            << "offset=" << offset << ", length=" << length
                            << ", required_bytes=" << requiredBytes
                            << ", data_size=" << data.size()
                            << ", remaining_bytes=" << (data.size() - offset)
                            << " (tag: '" << tagName << "')";

                        // Add hex dump of the problematic area
                        oss << "\nHex dump around length field:";
                        size_t start = (lengthOffset >= 8) ? lengthOffset - 8 : 0;
                        size_t end = std::min(data.size(), lengthOffset + 16);
                        for (size_t i = start; i < end; ++i) {
                            if (i == lengthOffset) oss << " [";
                            oss << " " << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(data[i]);
                            if (i == lengthOffset + 3) oss << "]";
                        }

                        throw std::runtime_error(oss.str());
                    }

                    arrayTag->value.resize(length);
                    for (int32_t i = 0; i < length; ++i) {
                        try {
                            arrayTag->value[i] = NBTParser::ReadInt64BE(data, offset);
                        } catch (const std::exception& e) {
                            Log::Error("Failed to read long array element %d/%d in tag '%s': %s",
                                      i, length, tagName.c_str(), e.what());
                            throw;
                        }
                    }
                    tag = arrayTag;
                    break;
                }

                default:
                    throw std::runtime_error("Unknown NBT tag type: " + std::to_string(static_cast<int>(tagType)) +
                                           " (tag name: '" + tagName + "')");
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
            throw std::runtime_error("Not enough data to read int16 at offset " + std::to_string(offset) +
                                   " (need 2 bytes, have " + std::to_string(data.size() - offset) + ")");
        }

        int16_t value = (static_cast<int16_t>(data[offset]) << 8) |
                        static_cast<int16_t>(data[offset + 1]);
        offset += 2;
        return value;
    }

    int32_t NBTParser::ReadInt32BE(const std::vector<uint8_t>& data, size_t& offset) {
        if (offset + 4 > data.size()) {
            throw std::runtime_error("Not enough data to read int32 at offset " + std::to_string(offset) +
                                   " (need 4 bytes, have " + std::to_string(data.size() - offset) + ")");
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
            throw std::runtime_error("Invalid string length: " + std::to_string(length) +
                                   " at offset " + std::to_string(offset - 2));
        }

        if (offset + length > data.size()) {
            throw std::runtime_error("Not enough data to read string of length " + std::to_string(length) +
                                   " at offset " + std::to_string(offset) +
                                   " (need " + std::to_string(length) + " bytes, have " +
                                   std::to_string(data.size() - offset) + ")");
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

    // Debug parsing implementation remains the same but uses the enhanced ParseTag
    NBTTagPtr NBTParser::ParseWithDebug(const std::vector<uint8_t>& data) {
        size_t offset = 0;
        Log::Debug("Starting debug NBT parsing at offset %zu, data size: %zu", offset, data.size());

        try {
            return NBTTag::ParseTag(data, offset, true);
        } catch (const std::exception& e) {
            Log::Error("Debug NBT parsing failed: %s", e.what());
            return nullptr;
        }
    }

    NBTTagPtr NBTParser::ParseWithDebug(const std::vector<uint8_t>& data, size_t& offset) {
        Log::Debug("Starting debug NBT parsing at offset %zu, data size: %zu", offset, data.size());

        try {
            return NBTTag::ParseTag(data, offset, true);
        } catch (const std::exception& e) {
            Log::Error("Debug NBT parsing failed: %s", e.what());
            return nullptr;
        }
    }

} // namespace World