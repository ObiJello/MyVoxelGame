// File: src/common/world/lighting/LightEngine.hpp
//
// MC's light engine (net.minecraft.world.level.lighting): LightEngine,
// BlockLightEngine, SkyLightEngine and LevelLightEngine, ported line for line
// in their propagation — the same queue-entry bit packing, the same
// increase/decrease passes, the same sky-source column maintenance — over
// light layers that live on the chunks themselves (Chunk::light) instead of
// MC's level-wide section maps.
//
// WHERE IT RUNS
//   * Initial lighting of a chunk (MC's LIGHT status) runs on the worker that
//     generated or loaded the chunk, on a private engine that sees only that
//     chunk: LightChunk(). Light that would cross into a neighbour stops at
//     the border (the neighbour reads as BEDROCK and stores nothing — exactly
//     MC's view of a chunk that is not there).
//   * The level's engine (one per World, server thread only) owns every chunk
//     registered with it. Registering a chunk reconciles its four borders
//     with the registered neighbours: every border cell whose light can
//     improve the cell across the boundary seeds an increase — the chunk-local
//     light is a lower bound of the true light and increases alone reach the
//     full fixed point. Block changes queue checkBlock; RunLightUpdates()
//     drains everything once per tick, before block changes are broadcast
//     (MC ChunkHolder.broadcastChanges sends light before blocks).
//
// STORAGE DIFFERENCES FROM MC (values are identical wherever a surface is)
//   * Every light section of a lit chunk has both layers (homogeneous layers
//     cost nothing), so storingLightForSection is "the chunk is registered and
//     the section is in the light range", and SkyLightEngine's empty-section
//     machinery (countEmptySectionsBelowIfAtBorder / propagateFromEmptySections)
//     never has anything to do and is not carried.
//   * lightOnInSection is "the chunk is registered": registration only ever
//     happens for chunks that finished their initial lighting.
#pragma once

#include "common/world/lighting/ChunkLight.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/Direction.hpp"

#include <climits>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace Game {
    class Chunk;
}

namespace Game::Lighting {

    // MC BlockPos.asLong: x 26 bits << 38, z 26 bits << 12, y 12 bits.
    namespace Pos {
        inline constexpr int64_t kXMask = (int64_t{1} << 26) - 1;
        inline constexpr int64_t kZMask = (int64_t{1} << 26) - 1;
        inline constexpr int64_t kYMask = (int64_t{1} << 12) - 1;
        inline constexpr int64_t Pack(int x, int y, int z) {
            return ((static_cast<int64_t>(x) & kXMask) << 38) |
                   ((static_cast<int64_t>(z) & kZMask) << 12) |
                   (static_cast<int64_t>(y) & kYMask);
        }
        inline constexpr int X(int64_t p) { return static_cast<int>(p >> 38); }
        inline constexpr int Y(int64_t p) { return static_cast<int>(static_cast<int64_t>(static_cast<uint64_t>(p) << 52) >> 52); }
        inline constexpr int Z(int64_t p) { return static_cast<int>(static_cast<int64_t>(static_cast<uint64_t>(p) << 26) >> 38); }
        inline constexpr int64_t Offset(int64_t p, Direction d) {
            return Pack(X(p) + StepX(d), Y(p) + StepY(d), Z(p) + StepZ(d));
        }
    }

    // A light section's key (chunk x, world section y, chunk z).
    namespace SectionKey {
        inline constexpr int64_t Pack(int cx, int sy, int cz) {
            return ((static_cast<int64_t>(cx) & 0x3FFFFF) << 42) |
                   ((static_cast<int64_t>(cz) & 0x3FFFFF) << 20) |
                   (static_cast<int64_t>(sy) & 0xFFFFF);
        }
        inline constexpr int X(int64_t k) { return static_cast<int>(k >> 42); }
        inline constexpr int Y(int64_t k) { return static_cast<int>(static_cast<int64_t>(static_cast<uint64_t>(k) << 44) >> 44); }
        inline constexpr int Z(int64_t k) { return static_cast<int>(static_cast<int64_t>(static_cast<uint64_t>(k) << 22) >> 42); }
    }

    // MC LightEngine.QueueEntry — bits 0-3 level, 4-9 directions (ordinal+4),
    // 10 from-empty-shape, 11 increase-from-emission.
    namespace QueueEntry {
        inline constexpr int64_t kLevelMask = 15;
        inline constexpr int64_t kDirectionsMask = 1008;
        inline constexpr int64_t kFlagFromEmptyShape = 1024;
        inline constexpr int64_t kFlagIncreaseFromEmission = 2048;
        inline constexpr int64_t WithLevel(int64_t e, int level) { return (e & ~int64_t{15}) | (static_cast<int64_t>(level) & 15); }
        inline constexpr int64_t WithDirection(int64_t e, Direction d) { return e | (int64_t{1} << (static_cast<int>(d) + 4)); }
        inline constexpr int64_t WithoutDirection(int64_t e, Direction d) { return e & ~(int64_t{1} << (static_cast<int>(d) + 4)); }
        inline constexpr int64_t DecreaseSkipOneDirection(int oldLevel, Direction skip) { return WithLevel(WithoutDirection(kDirectionsMask, skip), oldLevel); }
        inline constexpr int64_t DecreaseAllDirections(int oldLevel) { return WithLevel(kDirectionsMask, oldLevel); }
        inline constexpr int64_t IncreaseLightFromEmission(int newLevel, bool fromEmptyShape) {
            return WithLevel(kDirectionsMask | kFlagIncreaseFromEmission | (fromEmptyShape ? kFlagFromEmptyShape : 0), newLevel);
        }
        inline constexpr int64_t IncreaseSkipOneDirection(int newLevel, bool fromEmptyShape, Direction skip) {
            return WithLevel(WithoutDirection(kDirectionsMask, skip) | (fromEmptyShape ? kFlagFromEmptyShape : 0), newLevel);
        }
        inline constexpr int64_t IncreaseOnlyOneDirection(int newLevel, bool fromEmptyShape, Direction d) {
            return WithLevel(WithDirection(fromEmptyShape ? kFlagFromEmptyShape : 0, d), newLevel);
        }
        inline constexpr int64_t IncreaseSkySourceInDirections(bool down, bool north, bool south, bool west, bool east) {
            int64_t e = WithLevel(0, 15);
            if (down)  e = WithDirection(e, Direction::Down);
            if (north) e = WithDirection(e, Direction::North);
            if (south) e = WithDirection(e, Direction::South);
            if (west)  e = WithDirection(e, Direction::West);
            if (east)  e = WithDirection(e, Direction::East);
            return e;
        }
        inline constexpr int  GetFromLevel(int64_t e) { return static_cast<int>(e & 15); }
        inline constexpr bool IsFromEmptyShape(int64_t e) { return (e & kFlagFromEmptyShape) != 0; }
        inline constexpr bool IsIncreaseFromEmission(int64_t e) { return (e & kFlagIncreaseFromEmission) != 0; }
        inline constexpr bool ShouldPropagateInDirection(int64_t e, Direction d) { return (e & (int64_t{1} << (static_cast<int>(d) + 4))) != 0; }
    }

    // The engine's view of the world: a chunk takes part in lighting when
    // this returns it. Called at most a couple of times per chunk per run
    // (the engine keeps MC's two-entry cache in front of it).
    class LightChunkGetter {
    public:
        virtual ~LightChunkGetter() = default;
        virtual Chunk* GetChunkForLighting(int chunkX, int chunkZ) = 0;
    };

    // Sections whose light changed or whose mesh reads a changed cell
    // (MC LayerLightSectionStorage.sectionsAffectedByLightUpdates).
    using SectionSet = std::unordered_set<int64_t>;

    class LayerLightEngine {
    public:
        static constexpr int kMaxLevel = 15;

        LayerLightEngine(LightLayer layer, LightChunkGetter* getter);
        virtual ~LayerLightEngine() = default;
        LayerLightEngine(const LayerLightEngine&) = delete;
        LayerLightEngine& operator=(const LayerLightEngine&) = delete;

        LightLayer Layer() const { return m_layer; }
        void SetGetter(LightChunkGetter* getter) { m_getter = getter; ClearChunkCache(); }
        // Where setStoredLevel reports affected sections; null = no tracking
        // (the worker's initial lighting, which sends the whole chunk anyway).
        void SetAffectedSink(SectionSet* sink) { m_affected = sink; }

        void CheckBlock(int64_t pos) { m_blockNodesToCheck.push_back(pos); }
        bool HasLightWork() const;
        int  RunLightUpdates();

        void EnqueueDecrease(int64_t fromNode, int64_t decreaseData);
        void EnqueueIncrease(int64_t fromNode, int64_t increaseData);

        // MC propagateLightSources(ChunkPos): seed a freshly lit chunk.
        virtual void PropagateLightSources(Chunk& chunk) = 0;

        // Raw stored level (0 when the section is not stored).
        int GetStoredLevelOrZero(int64_t pos);

        // Direct access for border reconciliation.
        int  StoredLevelIn(Chunk& chunk, int x, int y, int z) const;
        BlockState GetState(int64_t pos);

    protected:
        virtual void CheckNode(int64_t blockNode) = 0;
        virtual void PropagateIncrease(int64_t fromNode, int64_t increaseData, int fromLevel) = 0;
        virtual void PropagateDecrease(int64_t fromNode, int64_t decreaseData) = 0;

        Chunk* GetChunk(int chunkX, int chunkZ);
        void   ClearChunkCache();

        // MC LayerLightSectionStorage.
        DataLayer* LayerAt(int64_t pos);
        bool StoringLightForSection(int64_t pos) { return LayerAt(pos) != nullptr; }
        int  GetStoredLevel(int64_t pos);
        void SetStoredLevel(int64_t pos, int level);
        bool LightOnAt(int64_t pos) { return GetChunk(Pos::X(pos) >> 4, Pos::Z(pos) >> 4) != nullptr; }

        static int  Opacity(BlockState s);
        static bool IsEmptyShape(BlockState s);
        static bool ShapeOccludes(BlockState from, BlockState to, Direction d);

        // PULL_LIGHT_IN_ENTRY = decreaseAllDirections(1).
        static constexpr int64_t kPullLightInEntry = QueueEntry::DecreaseAllDirections(1);

        LightLayer        m_layer;
        LightChunkGetter* m_getter = nullptr;
        SectionSet*       m_affected = nullptr;

    private:
        int PropagateIncreases();
        int PropagateDecreases();

        // FIFO of (node, data) pairs — LongArrayFIFOQueue.
        struct PairQueue {
            std::vector<int64_t> buf;
            size_t head = 0;
            bool Empty() const { return head >= buf.size(); }
            void Push(int64_t a, int64_t b) { buf.push_back(a); buf.push_back(b); }
            void Pop(int64_t& a, int64_t& b) {
                a = buf[head]; b = buf[head + 1]; head += 2;
                if (head >= buf.size()) { buf.clear(); head = 0; }
            }
            void Clear() { buf.clear(); head = 0; }
        };
        PairQueue m_decreaseQueue;
        PairQueue m_increaseQueue;
        std::vector<int64_t> m_blockNodesToCheck;
        std::vector<int64_t> m_checkScratch;

        int64_t m_lastChunkKey[2];
        Chunk*  m_lastChunk[2];
        int64_t m_lastAffected = INT64_MIN;
    };

    class BlockLightEngine final : public LayerLightEngine {
    public:
        explicit BlockLightEngine(LightChunkGetter* getter) : LayerLightEngine(LightLayer::Block, getter) {}
        void PropagateLightSources(Chunk& chunk) override;

    protected:
        void CheckNode(int64_t blockNode) override;
        void PropagateIncrease(int64_t fromNode, int64_t increaseData, int fromLevel) override;
        void PropagateDecrease(int64_t fromNode, int64_t decreaseData) override;

    private:
        int GetEmission(int64_t blockNode, BlockState state);
    };

    class SkyLightEngine final : public LayerLightEngine {
    public:
        explicit SkyLightEngine(LightChunkGetter* getter) : LayerLightEngine(LightLayer::Sky, getter) {}
        void PropagateLightSources(Chunk& chunk) override;
        // MC SkyLightEngine.setLightEnabled(pos, true): every section wholly
        // above the column tops starts at 15.
        void FillFullySourcedSections(Chunk& chunk);

    protected:
        void CheckNode(int64_t blockNode) override;
        void PropagateIncrease(int64_t fromNode, int64_t increaseData, int fromLevel) override;
        void PropagateDecrease(int64_t fromNode, int64_t decreaseData) override;

    private:
        int  GetLowestSourceY(int x, int z, int defaultValue);
        void UpdateSourcesInColumn(int x, int z, int lowestSourceY);
        void RemoveSourcesBelow(int x, int z, int lowestSourceY, int worldBottomY);
        void AddSourcesAbove(int x, int z, int lowestSourceY, int worldBottomY);
    };

    // MC LevelLightEngine: both layers, one per level.
    class LevelLightEngine {
    public:
        LevelLightEngine(LightChunkGetter* getter, bool hasSkyLight);

        bool HasSkyLight() const { return m_hasSkyLight; }
        // Set before any chunk is lit (World::Initialize, from the dimension).
        void SetHasSkyLight(bool hasSkyLight) { m_hasSkyLight = hasSkyLight; }
        void SetGetter(LightChunkGetter* getter) { m_block.SetGetter(getter); m_sky.SetGetter(getter); }
        void SetAffectedSink(SectionSet* sink) { m_block.SetAffectedSink(sink); m_sky.SetAffectedSink(sink); }

        void CheckBlock(int x, int y, int z);
        bool HasLightWork() const;
        int  RunLightUpdates();

        BlockLightEngine& Block() { return m_block; }
        SkyLightEngine&   Sky()   { return m_sky; }

    private:
        bool m_hasSkyLight;
        BlockLightEngine m_block;
        SkyLightEngine   m_sky;
    };

    // ── Entry points ────────────────────────────────────────────────────────

    // MC's LIGHT status for one chunk, isolated from its neighbours: sky
    // sources, both layers, lightCorrect = true. Any thread; the chunk must
    // not be shared yet. Thread-local engine, no allocation after warm-up.
    void LightChunk(Chunk& chunk, bool hasSkyLight);

    // Level light query shared by server and client: MC getBrightness(layer)
    // over one chunk's layers (null chunk = not loaded: sky 15, block 0, as
    // MC answers for a missing column). `hasSkyLight` false reads sky 0.
    int  GetBrightness(const Chunk* chunk, LightLayer layer, int x, int y, int z, bool hasSkyLight);

    // MC SectionPos.aroundAndAtBlockPos — every section the 3x3x3 block
    // neighbourhood of (x, y, z) touches.
    template <class Fn>
    inline void AroundAndAtBlockPos(int x, int y, int z, Fn&& fn) {
        const int x0 = (x - 1) >> 4, x1 = (x + 1) >> 4;
        const int y0 = (y - 1) >> 4, y1 = (y + 1) >> 4;
        const int z0 = (z - 1) >> 4, z1 = (z + 1) >> 4;
        for (int sx = x0; sx <= x1; ++sx)
            for (int sy = y0; sy <= y1; ++sy)
                for (int sz = z0; sz <= z1; ++sz)
                    fn(SectionKey::Pack(sx, sy, sz));
    }

} // namespace Game::Lighting
