// File: src/tests/test_region_dumper.cpp
// Enhanced test with debug NBT parsing for 1.18+ support
#include "../world/RegionDumper.hpp"
#include "../world/RegionFileCache.hpp"
#include "../core/Log.hpp"
#include <iostream>
#include <iomanip>

void TestNBTParserBasic() {
    std::cout << "=== NBT Parser Basic Test ===" << std::endl;

    // Create a simple NBT structure manually for testing
    std::vector<uint8_t> testData = {
        // TAG_Compound (root)
        0x0A,
        // Name length (2 bytes, big-endian)
        0x00, 0x04,
        // Name ("test")
        't', 'e', 's', 't',

        // TAG_String inside compound
        0x08,
        // Name length
        0x00, 0x05,
        // Name ("hello")
        'h', 'e', 'l', 'l', 'o',
        // String length
        0x00, 0x05,
        // String value ("world")
        'w', 'o', 'r', 'l', 'd',

        // TAG_Int inside compound
        0x03,
        // Name length
        0x00, 0x06,
        // Name ("number")
        'n', 'u', 'm', 'b', 'e', 'r',
        // Int value (42, big-endian)
        0x00, 0x00, 0x00, 0x2A,

        // TAG_End
        0x00
    };

    try {
        auto rootTag = World::NBTParser::Parse(testData);
        if (rootTag) {
            std::cout << "✓ Basic NBT parsing successful!" << std::endl;
            rootTag->Print(std::cout, 0);

            // Test compound access
            auto compound = std::dynamic_pointer_cast<World::NBTTagCompound>(rootTag);
            if (compound) {
                std::string hello = compound->GetValue<std::string>("hello", "");
                int32_t number = compound->GetValue<int32_t>("number", 0);
                std::cout << "✓ Extracted values: hello='" << hello << "', number=" << number << std::endl;
            }
        } else {
            std::cout << "✗ Basic NBT parsing failed" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "✗ Basic NBT parsing exception: " << e.what() << std::endl;
    }
}

void TestNBTParserLongArray() {
    std::cout << "\n=== NBT Long Array Test (1.18+ feature) ===" << std::endl;

    // Create a NBT structure with a long array (used in heightmaps)
    std::vector<uint8_t> longArrayData = {
        // TAG_Compound (root)
        0x0A,
        // Name length (2 bytes, big-endian)
        0x00, 0x0A,
        // Name ("heightmaps")
        'h', 'e', 'i', 'g', 'h', 't', 'm', 'a', 'p', 's',

        // TAG_Long_Array inside compound
        0x0C,  // TAG_Long_Array = 12
        // Name length
        0x00, 0x11,
        // Name ("MOTION_BLOCKING")
        'M', 'O', 'T', 'I', 'O', 'N', '_', 'B', 'L', 'O', 'C', 'K', 'I', 'N', 'G', '_', 'S',
        // Array length (2 longs)
        0x00, 0x00, 0x00, 0x02,
        // First long (big-endian)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,
        // Second long (big-endian)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,

        // TAG_End
        0x00
    };

    try {
        auto rootTag = World::NBTParser::Parse(longArrayData);
        if (rootTag) {
            std::cout << "✓ Long Array NBT parsing successful!" << std::endl;
            rootTag->Print(std::cout, 0);
        } else {
            std::cout << "✗ Long Array NBT parsing failed" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "✗ Long Array NBT parsing exception: " << e.what() << std::endl;
    }
}

void TestRegionFileWithDebug(const std::string& testFile) {
    std::cout << "\n=== Region File Debug Test ===" << std::endl;
    std::cout << "Test file: " << testFile << std::endl;

    try {
        auto regionFile = World::RegionFileCache::Instance().GetRegionFile(testFile);

        if (regionFile && regionFile->IsValid()) {
            std::cout << "✓ Region file loaded successfully!" << std::endl;

            // Find first non-empty chunk
            bool foundChunk = false;
            for (int z = 0; z < 32 && !foundChunk; ++z) {
                for (int x = 0; x < 32 && !foundChunk; ++x) {
                    auto location = regionFile->GetLocation(x, z);
                    if (!location.isEmpty()) {
                        std::cout << "\n✓ Found non-empty chunk at (" << x << ", " << z << ")" << std::endl;
                        std::cout << "  Offset: " << location.sectorOffset << ", Sectors: "
                                 << static_cast<int>(location.sectorCount) << std::endl;

                        // Enable debug logging for NBT parsing
                        Log::SetLevel(Log::Level::Debug);

                        std::cout << "\n--- Starting debug chunk read ---" << std::endl;
                        auto chunkData = World::RegionDumper::ReadChunkData(*regionFile, x, z);

                        if (chunkData.isValid) {
                            std::cout << "✓ Chunk data read and decompressed successfully!" << std::endl;
                            std::cout << "  Compressed size: " << chunkData.compressedData.size() << " bytes" << std::endl;
                            std::cout << "  Uncompressed size: " << chunkData.uncompressedData.size() << " bytes" << std::endl;
                            std::cout << "  Compression version: " << static_cast<int>(chunkData.version) << std::endl;

                            // Print first 32 bytes of uncompressed data as hex
                            std::cout << "\nFirst 32 bytes of uncompressed NBT data:" << std::endl;
                            for (size_t i = 0; i < std::min(size_t(32), chunkData.uncompressedData.size()); ++i) {
                                std::cout << std::hex << std::setw(2) << std::setfill('0')
                                         << static_cast<unsigned>(chunkData.uncompressedData[i]) << " ";
                                if ((i + 1) % 16 == 0) std::cout << std::endl;
                            }
                            std::cout << std::dec << std::endl;

                            // Try parsing with debug
                            std::cout << "\n--- Attempting NBT parsing with debug ---" << std::endl;
                            try {
                                auto debugTag = World::NBTParser::ParseWithDebug(chunkData.uncompressedData);
                                if (debugTag) {
                                    std::cout << "✓ Debug NBT parsing successful!" << std::endl;
                                    std::cout << "Root tag: " << debugTag->ToString() << std::endl;

                                    // Extract key information
                                    auto compound = std::dynamic_pointer_cast<World::NBTTagCompound>(debugTag);
                                    if (compound) {
                                        // Check for DataVersion (1.18+)
                                        int32_t dataVersion = compound->GetValue<int32_t>("DataVersion", -1);
                                        if (dataVersion != -1) {
                                            std::cout << "✓ Found DataVersion: " << dataVersion << std::endl;
                                        }

                                        // Check for Level compound
                                        auto levelTag = compound->GetTag("Level");
                                        if (levelTag) {
                                            std::cout << "✓ Found Level compound" << std::endl;
                                            auto levelCompound = std::dynamic_pointer_cast<World::NBTTagCompound>(levelTag);
                                            if (levelCompound) {
                                                // Check for yPos (1.18+)
                                                int32_t yPos = levelCompound->GetValue<int32_t>("yPos", INT32_MIN);
                                                if (yPos != INT32_MIN) {
                                                    std::cout << "✓ Found yPos (1.18+ feature): " << yPos << std::endl;
                                                }

                                                // Check coordinates
                                                int32_t xPos = levelCompound->GetValue<int32_t>("xPos", 0);
                                                int32_t zPos = levelCompound->GetValue<int32_t>("zPos", 0);
                                                std::cout << "✓ Chunk coordinates: (" << xPos << ", " << zPos << ")" << std::endl;

                                                // Check for status
                                                std::string status = levelCompound->GetValue<std::string>("Status", "unknown");
                                                std::cout << "✓ Chunk status: " << status << std::endl;
                                            }
                                        }
                                    }
                                } else {
                                    std::cout << "✗ Debug NBT parsing returned null" << std::endl;
                                }
                            } catch (const std::exception& e) {
                                std::cout << "✗ Debug NBT parsing exception: " << e.what() << std::endl;

                                // Additional debugging: check if data starts with expected bytes
                                if (!chunkData.uncompressedData.empty()) {
                                    uint8_t firstByte = chunkData.uncompressedData[0];
                                    std::cout << "First byte of NBT data: 0x" << std::hex << static_cast<unsigned>(firstByte) << std::dec;
                                    if (firstByte == 0x0A) {
                                        std::cout << " (TAG_Compound - expected)" << std::endl;
                                    } else {
                                        std::cout << " (unexpected - should be 0x0A for TAG_Compound)" << std::endl;
                                    }

                                    // Check if we have enough data for the root tag name
                                    if (chunkData.uncompressedData.size() >= 3) {
                                        uint16_t nameLength = (static_cast<uint16_t>(chunkData.uncompressedData[1]) << 8) |
                                                             static_cast<uint16_t>(chunkData.uncompressedData[2]);
                                        std::cout << "Root tag name length: " << nameLength << std::endl;

                                        if (nameLength > 0 && chunkData.uncompressedData.size() >= 3 + nameLength) {
                                            std::string rootName(reinterpret_cast<const char*>(&chunkData.uncompressedData[3]), nameLength);
                                            std::cout << "Root tag name: '" << rootName << "'" << std::endl;
                                        }
                                    }
                                }
                            }
                        } else {
                            std::cout << "✗ Failed to read/decompress chunk data" << std::endl;
                        }

                        // Reset log level
                        Log::SetLevel(Log::Level::Info);
                        foundChunk = true;
                    }
                }
            }

            if (!foundChunk) {
                std::cout << "! No non-empty chunks found in region file" << std::endl;
            }

        } else {
            std::cout << "✗ Failed to load region file" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "✗ Region file test exception: " << e.what() << std::endl;
    }
}

int main(int argc, char* argv[]) {
    // Initialize logging
    Log::Init();
    Log::SetLevel(Log::Level::Info);

    std::cout << "Enhanced Region File Dumper Test Suite" << std::endl;
    std::cout << "=======================================" << std::endl;

    // Test 1: Basic NBT Parser
    TestNBTParserBasic();

    // Test 2: Long Array NBT (1.18+ feature)
    TestNBTParserLongArray();

    // Test 3: Region file with debug parsing
    if (argc >= 2) {
        TestRegionFileWithDebug(argv[1]);
    } else {
        std::cout << "\n! No region file provided for debug testing" << std::endl;
        std::cout << "  Usage: " << argv[0] << " <path_to_region_file.mca>" << std::endl;
        std::cout << "  Example: " << argv[0] << " r.0.0.mca" << std::endl;
    }

    std::cout << "\n=== Enhanced Test Complete ===" << std::endl;
    return 0;
}