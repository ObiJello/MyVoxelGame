// File: src/tools/RegionDumperMain.cpp
#include "../world/RegionDumper.hpp"
#include "../world/RegionFileCache.hpp"
#include "../core/Log.hpp"
#include <iostream>
#include <string>
#include <vector>

void PrintUsage(const char* programName) {
    std::cout << "Region File Dumper - Minecraft .mca File Parser" << std::endl;
    std::cout << "================================================" << std::endl;
    std::cout << std::endl;
    std::cout << "Usage:" << std::endl;
    std::cout << "  " << programName << " <command> [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Commands:" << std::endl;
    std::cout << "  region <regionX> <regionZ> [worldPath]" << std::endl;
    std::cout << "    Dump entire region file by coordinates" << std::endl;
    std::cout << "    Example: " << programName << " region 0 0 ~/minecraft/saves/MyWorld" << std::endl;
    std::cout << std::endl;
    std::cout << "  file <path>" << std::endl;
    std::cout << "    Dump region file by direct path" << std::endl;
    std::cout << "    Example: " << programName << " file r.0.0.mca" << std::endl;
    std::cout << std::endl;
    std::cout << "  chunk <regionX> <regionZ> <localX> <localZ> [worldPath]" << std::endl;
    std::cout << "    Dump specific chunk from region" << std::endl;
    std::cout << "    Example: " << programName << " chunk 0 0 15 15 ~/minecraft/saves/MyWorld" << std::endl;
    std::cout << std::endl;
    std::cout << "  headers <path>" << std::endl;
    std::cout << "    Show only chunk headers (no NBT parsing)" << std::endl;
    std::cout << "    Example: " << programName << " headers r.0.0.mca" << std::endl;
    std::cout << std::endl;
    std::cout << "Notes:" << std::endl;
    std::cout << "  - regionX, regionZ: Region coordinates (not chunk coordinates)" << std::endl;
    std::cout << "  - localX, localZ: Local coordinates within region (0-31)" << std::endl;
    std::cout << "  - worldPath: Path to Minecraft world directory (contains 'region' folder)" << std::endl;
    std::cout << "  - If worldPath is omitted, current directory is used" << std::endl;
}

int main(int argc, char* argv[]) {
    // Initialize logging
    Log::Init();
    Log::SetLevel(Log::Level::Info);

    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    std::string command = argv[1];

    try {
        if (command == "region") {
            if (argc < 4) {
                std::cout << "Error: region command requires regionX and regionZ" << std::endl;
                PrintUsage(argv[0]);
                return 1;
            }

            int regionX = std::stoi(argv[2]);
            int regionZ = std::stoi(argv[3]);
            std::string worldPath = (argc >= 5) ? argv[4] : ".";

            std::cout << "Dumping region (" << regionX << ", " << regionZ << ") from " << worldPath << std::endl;

            bool success = World::RegionDumper::DumpRegion(regionX, regionZ, worldPath);
            return success ? 0 : 1;

        } else if (command == "file") {
            if (argc < 3) {
                std::cout << "Error: file command requires path" << std::endl;
                PrintUsage(argv[0]);
                return 1;
            }

            std::string filePath = argv[2];
            std::cout << "Dumping region file: " << filePath << std::endl;

            bool success = World::RegionDumper::DumpRegionFile(filePath);
            return success ? 0 : 1;

        } else if (command == "chunk") {
            if (argc < 6) {
                std::cout << "Error: chunk command requires regionX, regionZ, localX, and localZ" << std::endl;
                PrintUsage(argv[0]);
                return 1;
            }

            int regionX = std::stoi(argv[2]);
            int regionZ = std::stoi(argv[3]);
            int localX = std::stoi(argv[4]);
            int localZ = std::stoi(argv[5]);
            std::string worldPath = (argc >= 7) ? argv[6] : ".";

            std::cout << "Dumping chunk (" << localX << ", " << localZ << ") from region ("
                     << regionX << ", " << regionZ << ") in " << worldPath << std::endl;

            bool success = World::RegionDumper::DumpChunk(regionX, regionZ, localX, localZ, worldPath);
            return success ? 0 : 1;

        } else if (command == "headers") {
            if (argc < 3) {
                std::cout << "Error: headers command requires path" << std::endl;
                PrintUsage(argv[0]);
                return 1;
            }

            std::string filePath = argv[2];
            std::cout << "Showing headers for: " << filePath << std::endl;

            auto regionFile = World::RegionFileCache::Instance().GetRegionFile(filePath);
            if (!regionFile || !regionFile->IsValid()) {
                std::cout << "Error: Failed to load region file: " << filePath << std::endl;
                return 1;
            }

            World::RegionDumper::PrintChunkHeaders(*regionFile);
            return 0;

        } else if (command == "help" || command == "--help" || command == "-h") {
            PrintUsage(argv[0]);
            return 0;

        } else {
            std::cout << "Error: Unknown command '" << command << "'" << std::endl;
            PrintUsage(argv[0]);
            return 1;
        }

    } catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}