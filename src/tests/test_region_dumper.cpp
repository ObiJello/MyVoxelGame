// File: src/tests/test_region_dumper.cpp
// Simple test to verify the Region File Dumper is working
#include "../world/RegionDumper.hpp"
#include "../world/RegionFileCache.hpp"
#include "../core/Log.hpp"
#include <iostream>

void TestNBTParser() {
    std::cout << "=== NBT Parser Test ===" << std::endl;

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
            std::cout << "✓ NBT parsing successful!" << std::endl;
            rootTag->Print(std::cout, 0);

            // Test compound access
            auto compound = std::dynamic_pointer_cast<World::NBTTagCompound>(rootTag);
            if (compound) {
                std::string hello = compound->GetValue<std::string>("hello", "");
                int32_t number = compound->GetValue<int32_t>("number", 0);
                std::cout << "✓ Extracted values: hello='" << hello << "', number=" << number << std::endl;
            }
        } else {
            std::cout << "✗ NBT parsing failed" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "✗ NBT parsing exception: " << e.what() << std::endl;
    }
}

void TestRegionFileLoading(const std::string& testFile) {
    std::cout << "\n=== Region File Loading Test ===" << std::endl;
    std::cout << "Test file: " << testFile << std::endl;

    try {
        auto regionFile = World::RegionFileCache::Instance().GetRegionFile(testFile);

        if (regionFile && regionFile->IsValid()) {
            std::cout << "✓ Region file loaded successfully!" << std::endl;

            // Print basic info
            World::RegionDumper::PrintChunkHeaders(*regionFile);

            // Try to read first non-empty chunk
            bool foundChunk = false;
            for (int z = 0; z < 32 && !foundChunk; ++z) {
                for (int x = 0; x < 32 && !foundChunk; ++x) {
                    auto location = regionFile->GetLocation(x, z);
                    if (!location.isEmpty()) {
                        std::cout << "\n✓ Found non-empty chunk at (" << x << ", " << z << ")" << std::endl;
                        std::cout << "  Attempting to read chunk data..." << std::endl;

                        auto chunkData = World::RegionDumper::ReadChunkData(*regionFile, x, z);
                        if (chunkData.isValid) {
                            std::cout << "✓ Chunk data read and parsed successfully!" << std::endl;
                            World::RegionDumper::PrintChunkNBT(chunkData, false);
                        } else {
                            std::cout << "✗ Failed to read chunk data" << std::endl;
                        }
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
    Log::SetLevel(Log::Level::Debug);

    std::cout << "Region File Dumper Test Suite" << std::endl;
    std::cout << "=============================" << std::endl;

    // Test 1: NBT Parser
    TestNBTParser();

    // Test 2: Region file loading (if provided)
    if (argc >= 2) {
        TestRegionFileLoading(argv[1]);
    } else {
        std::cout << "\n! No region file provided for testing" << std::endl;
        std::cout << "  Usage: " << argv[0] << " <path_to_region_file.mca>" << std::endl;
        std::cout << "  Example: " << argv[0] << " r.0.0.mca" << std::endl;
    }

    std::cout << "\n=== Test Complete ===" << std::endl;
    return 0;
}