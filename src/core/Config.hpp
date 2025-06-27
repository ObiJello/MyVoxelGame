#pragma once

namespace Config {
    // Window settings
    inline constexpr int  WindowWidth  = 1280;
    inline constexpr int  WindowHeight = 720;
    inline constexpr const char* WindowTitle = "MyVoxelGame v0.1";

    // OpenGL version
    inline constexpr int OpenGLMajor = 3;
    inline constexpr int OpenGLMinor = 3;

    // World / Chunk settings (matching vanilla Minecraft)
    inline constexpr int ChunkSizeX = 16;
    inline constexpr int ChunkSizeY = 384;
    inline constexpr int ChunkSizeZ = 16;
    inline constexpr int SubChunkHeight = 16;   // each chunk is split into 16×16×16 sub-chunks
    inline constexpr int MinY = -64;
    inline constexpr int MaxY = 319;
}
