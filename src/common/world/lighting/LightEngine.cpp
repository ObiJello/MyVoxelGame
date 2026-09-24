// File: src/common/world/lighting/LightEngine.cpp
#include "common/world/lighting/LightEngine.hpp"

#include "common/world/lighting/BlockLightProperties.hpp"
#include "common/world/lighting/LightStateAccess.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <algorithm>
#include <array>

namespace Game::Lighting {

    namespace {
        constexpr Direction kPropagationDirections[6] = {
            Direction::Down, Direction::Up, Direction::North,
            Direction::South, Direction::West, Direction::East,
        };
        constexpr int64_t kNoChunk = INT64_MIN;

        inline int64_t ChunkKey(int cx, int cz) {
            return (static_cast<int64_t>(cx) << 32) ^ static_cast<int64_t>(static_cast<uint32_t>(cz));
        }

        // Missing chunks read as bedrock (MC LightEngine.getState).
        BlockState BedrockState() {
            static const BlockState s = BlockStates::Default(BlockID::Bedrock);
            return s;
        }

        // SkyLightEngine's constant entries.
        constexpr int64_t kRemoveTopSkySourceEntry = QueueEntry::DecreaseAllDirections(15);
        constexpr int64_t kRemoveSkySourceEntry    = QueueEntry::DecreaseSkipOneDirection(15, Direction::Up);
        constexpr int64_t kAddSkySourceEntry       = QueueEntry::IncreaseSkipOneDirection(15, false, Direction::Up);

        // World Y span of the light range.
        constexpr int kLightBottomY = kMinLightSectionY * 16;           // -80
        constexpr int kLightTopY    = (kMaxLightSectionY + 1) * 16 - 1; // 335
    }

    // ── LayerLightEngine ────────────────────────────────────────────────────

    LayerLightEngine::LayerLightEngine(LightLayer layer, LightChunkGetter* getter)
        : m_layer(layer), m_getter(getter) {
        ClearChunkCache();
    }

    void LayerLightEngine::ClearChunkCache() {
        m_lastChunkKey[0] = m_lastChunkKey[1] = kNoChunk;
        m_lastChunk[0] = m_lastChunk[1] = nullptr;
    }

    Chunk* LayerLightEngine::GetChunk(int chunkX, int chunkZ) {
        const int64_t key = ChunkKey(chunkX, chunkZ);
        if (key == m_lastChunkKey[0]) return m_lastChunk[0];
        if (key == m_lastChunkKey[1]) return m_lastChunk[1];
        Chunk* chunk = m_getter ? m_getter->GetChunkForLighting(chunkX, chunkZ) : nullptr;
        m_lastChunkKey[1] = m_lastChunkKey[0];
        m_lastChunk[1] = m_lastChunk[0];
        m_lastChunkKey[0] = key;
        m_lastChunk[0] = chunk;
        return chunk;
    }

    BlockState LayerLightEngine::GetState(int64_t pos) {
        const int x = Pos::X(pos), z = Pos::Z(pos);
        const Chunk* chunk = GetChunk(x >> 4, z >> 4);
        if (!chunk) return BedrockState();
        return BlockState::FromRawId(StateIdAt(*chunk, x & 15, Pos::Y(pos), z & 15));
    }

    DataLayer* LayerLightEngine::LayerAt(int64_t pos) {
        const int y = Pos::Y(pos);
        if (y < kLightBottomY || y > kLightTopY) return nullptr;
        Chunk* chunk = GetChunk(Pos::X(pos) >> 4, Pos::Z(pos) >> 4);
        if (!chunk) return nullptr;
        return &chunk->light.Layer(m_layer, LightIndexForY(y));
    }

    int LayerLightEngine::GetStoredLevel(int64_t pos) {
        const DataLayer* layer = LayerAt(pos);
        return layer ? layer->Get(Pos::X(pos) & 15, Pos::Y(pos) & 15, Pos::Z(pos) & 15) : 0;
    }

    int LayerLightEngine::GetStoredLevelOrZero(int64_t pos) { return GetStoredLevel(pos); }

    int LayerLightEngine::StoredLevelIn(Chunk& chunk, int x, int y, int z) const {
        if (y < kLightBottomY || y > kLightTopY) return 0;
        return chunk.light.Layer(m_layer, LightIndexForY(y)).Get(x & 15, y & 15, z & 15);
    }

    void LayerLightEngine::SetStoredLevel(int64_t pos, int level) {
        DataLayer* layer = LayerAt(pos);
        if (!layer) return;
        const int x = Pos::X(pos), y = Pos::Y(pos), z = Pos::Z(pos);
        layer->Set(x & 15, y & 15, z & 15, level);
        if (m_affected) {
            // MC SectionPos.aroundAndAtBlockPos. Almost every write lands
            // inside the section it was last in, which is one compare.
            const int64_t own = SectionKey::Pack(x >> 4, y >> 4, z >> 4);
            const int lx = x & 15, ly = y & 15, lz = z & 15;
            const bool interior = lx > 0 && lx < 15 && ly > 0 && ly < 15 && lz > 0 && lz < 15;
            if (interior) {
                if (own != m_lastAffected) {
                    m_affected->insert(own);
                    m_lastAffected = own;
                }
            } else {
                AroundAndAtBlockPos(x, y, z, [this](int64_t key) { m_affected->insert(key); });
                m_lastAffected = INT64_MIN;
            }
        }
    }

    int  LayerLightEngine::Opacity(BlockState s) { return BlockLightProperties::Opacity(s); }
    bool LayerLightEngine::IsEmptyShape(BlockState s) { return BlockLightProperties::IsEmptyShape(s); }
    bool LayerLightEngine::ShapeOccludes(BlockState from, BlockState to, Direction d) {
        return BlockLightProperties::ShapeOccludes(from, to, d);
    }

    void LayerLightEngine::EnqueueDecrease(int64_t fromNode, int64_t decreaseData) {
        m_decreaseQueue.Push(fromNode, decreaseData);
    }
    void LayerLightEngine::EnqueueIncrease(int64_t fromNode, int64_t increaseData) {
        m_increaseQueue.Push(fromNode, increaseData);
    }

    bool LayerLightEngine::HasLightWork() const {
        return !m_blockNodesToCheck.empty() || !m_decreaseQueue.Empty() || !m_increaseQueue.Empty();
    }

    int LayerLightEngine::RunLightUpdates() {
        if (!m_blockNodesToCheck.empty()) {
            // MC keeps these in a hash set; the order only moves intermediate
            // states, never the result, so a sorted unique vector serves.
            // Swapped out first: a check can register a chunk (the level
            // manager's lazy registration), whose deferred checks land in
            // the live vector for the next run.
            m_checkScratch.clear();
            m_checkScratch.swap(m_blockNodesToCheck);
            std::sort(m_checkScratch.begin(), m_checkScratch.end());
            m_checkScratch.erase(std::unique(m_checkScratch.begin(), m_checkScratch.end()),
                                 m_checkScratch.end());
            for (size_t i = 0; i < m_checkScratch.size(); ++i) CheckNode(m_checkScratch[i]);
            m_checkScratch.clear();
        }
        int count = 0;
        count += PropagateDecreases();
        count += PropagateIncreases();
        ClearChunkCache();
        m_lastAffected = INT64_MIN;
        return count;
    }

    int LayerLightEngine::PropagateIncreases() {
        int count = 0;
        while (!m_increaseQueue.Empty()) {
            int64_t fromNode, increaseData;
            m_increaseQueue.Pop(fromNode, increaseData);
            int fromLevel = GetStoredLevel(fromNode);
            const int fromTargetLevel = QueueEntry::GetFromLevel(increaseData);
            if (QueueEntry::IsIncreaseFromEmission(increaseData) && fromLevel < fromTargetLevel) {
                SetStoredLevel(fromNode, fromTargetLevel);
                fromLevel = fromTargetLevel;
            }
            if (fromLevel == fromTargetLevel) {
                PropagateIncrease(fromNode, increaseData, fromLevel);
            }
            ++count;
        }
        return count;
    }

    int LayerLightEngine::PropagateDecreases() {
        int count = 0;
        while (!m_decreaseQueue.Empty()) {
            int64_t fromNode, decreaseData;
            m_decreaseQueue.Pop(fromNode, decreaseData);
            PropagateDecrease(fromNode, decreaseData);
            ++count;
        }
        return count;
    }

    // ── BlockLightEngine ────────────────────────────────────────────────────

    int BlockLightEngine::GetEmission(int64_t blockNode, BlockState state) {
        const int emission = BlockLightProperties::Emission(state);
        return emission > 0 && LightOnAt(blockNode) ? emission : 0;
    }

    void BlockLightEngine::CheckNode(int64_t blockNode) {
        if (!StoringLightForSection(blockNode)) return;
        const BlockState state = GetState(blockNode);
        const int lightEmission = GetEmission(blockNode, state);
        const int oldLevel = GetStoredLevel(blockNode);
        if (lightEmission < oldLevel) {
            SetStoredLevel(blockNode, 0);
            EnqueueDecrease(blockNode, QueueEntry::DecreaseAllDirections(oldLevel));
        } else {
            EnqueueDecrease(blockNode, kPullLightInEntry);
        }
        if (lightEmission > 0) {
            EnqueueIncrease(blockNode, QueueEntry::IncreaseLightFromEmission(lightEmission, IsEmptyShape(state)));
        }
    }

    void BlockLightEngine::PropagateIncrease(int64_t fromNode, int64_t increaseData, int fromLevel) {
        BlockState fromState;
        bool haveFrom = false;
        for (Direction dir : kPropagationDirections) {
            if (!QueueEntry::ShouldPropagateInDirection(increaseData, dir)) continue;
            const int64_t toNode = Pos::Offset(fromNode, dir);
            if (!StoringLightForSection(toNode)) continue;
            const int toLevel = GetStoredLevel(toNode);
            const int maxPossibleNewToLevel = fromLevel - 1;
            if (maxPossibleNewToLevel <= toLevel) continue;
            const BlockState toState = GetState(toNode);
            const int newToLevel = fromLevel - Opacity(toState);
            if (newToLevel <= toLevel) continue;
            if (!haveFrom) {
                fromState = QueueEntry::IsFromEmptyShape(increaseData) ? BlockState{} : GetState(fromNode);
                haveFrom = true;
            }
            if (ShapeOccludes(fromState, toState, dir)) continue;
            SetStoredLevel(toNode, newToLevel);
            if (newToLevel > 1) {
                EnqueueIncrease(toNode, QueueEntry::IncreaseSkipOneDirection(newToLevel, IsEmptyShape(toState), Opposite(dir)));
            }
        }
    }

    void BlockLightEngine::PropagateDecrease(int64_t fromNode, int64_t decreaseData) {
        const int oldFromLevel = QueueEntry::GetFromLevel(decreaseData);
        for (Direction dir : kPropagationDirections) {
            if (!QueueEntry::ShouldPropagateInDirection(decreaseData, dir)) continue;
            const int64_t toNode = Pos::Offset(fromNode, dir);
            if (!StoringLightForSection(toNode)) continue;
            const int toLevel = GetStoredLevel(toNode);
            if (toLevel == 0) continue;
            if (toLevel <= oldFromLevel - 1) {
                const BlockState toState = GetState(toNode);
                const int toEmission = GetEmission(toNode, toState);
                SetStoredLevel(toNode, 0);
                if (toEmission < toLevel) {
                    EnqueueDecrease(toNode, QueueEntry::DecreaseSkipOneDirection(toLevel, Opposite(dir)));
                }
                if (toEmission > 0) {
                    EnqueueIncrease(toNode, QueueEntry::IncreaseLightFromEmission(toEmission, IsEmptyShape(toState)));
                }
            } else {
                EnqueueIncrease(toNode, QueueEntry::IncreaseOnlyOneDirection(toLevel, false, Opposite(dir)));
            }
        }
    }

    void BlockLightEngine::PropagateLightSources(Chunk& chunk) {
        // MC LightChunk.findBlockLightSources: every emitting state in the
        // chunk seeds an increase. A section whose palette holds no emitter is
        // skipped without reading its 4096 entries.
        const int baseX = chunk.pos.x * 16, baseZ = chunk.pos.z * 16;
        for (int si = 0; si < Math::SECTIONS_PER_CHUNK; ++si) {
            const ChunkSection* section = chunk.GetSection(si);
            if (section->IsAllAir()) continue;
            const PalettedContainer& states = section->States();
            if (!states.IsGlobalPalette()) {
                bool any = false;
                for (uint32_t id : states.Palette()) {
                    if (BlockLightProperties::Emission(BlockState::FromRawId(id)) > 0) { any = true; break; }
                }
                if (!any) continue;
            }
            const int baseY = Config::MinY + si * 16;
            for (int i = 0; i < 4096; ++i) {
                const BlockState state = BlockState::FromRawId(states.Get(static_cast<size_t>(i)));
                const int emission = BlockLightProperties::Emission(state);
                if (emission <= 0) continue;
                const int x = baseX + (i & 15), y = baseY + (i >> 8), z = baseZ + ((i >> 4) & 15);
                EnqueueIncrease(Pos::Pack(x, y, z),
                                QueueEntry::IncreaseLightFromEmission(emission, IsEmptyShape(state)));
            }
        }
    }

    // ── SkyLightEngine ──────────────────────────────────────────────────────

    int SkyLightEngine::GetLowestSourceY(int x, int z, int defaultValue) {
        const Chunk* chunk = GetChunk(x >> 4, z >> 4);
        return chunk ? chunk->light.skySources.GetLowestSourceY(x & 15, z & 15) : defaultValue;
    }

    void SkyLightEngine::CheckNode(int64_t blockNode) {
        const int x = Pos::X(blockNode), y = Pos::Y(blockNode), z = Pos::Z(blockNode);
        const int lowestSourceY = LightOnAt(blockNode) ? GetLowestSourceY(x, z, INT_MAX) : INT_MAX;
        if (lowestSourceY != INT_MAX) UpdateSourcesInColumn(x, z, lowestSourceY);
        if (!StoringLightForSection(blockNode)) return;
        const bool isSource = y >= lowestSourceY;
        if (isSource) {
            EnqueueDecrease(blockNode, kRemoveSkySourceEntry);
            EnqueueIncrease(blockNode, kAddSkySourceEntry);
        } else {
            const int oldLevel = GetStoredLevel(blockNode);
            if (oldLevel > 0) {
                SetStoredLevel(blockNode, 0);
                EnqueueDecrease(blockNode, QueueEntry::DecreaseAllDirections(oldLevel));
            } else {
                EnqueueDecrease(blockNode, kPullLightInEntry);
            }
        }
    }

    void SkyLightEngine::UpdateSourcesInColumn(int x, int z, int lowestSourceY) {
        RemoveSourcesBelow(x, z, lowestSourceY, kLightBottomY);
        AddSourcesAbove(x, z, lowestSourceY, kLightBottomY);
    }

    void SkyLightEngine::RemoveSourcesBelow(int x, int z, int lowestSourceY, int worldBottomY) {
        if (lowestSourceY <= worldBottomY) return;
        const int startY = lowestSourceY - 1;
        for (int y = std::min(startY, kLightTopY); y >= worldBottomY; --y) {
            const int64_t blockNode = Pos::Pack(x, y, z);
            if (!StoringLightForSection(blockNode)) return;
            if (GetStoredLevel(blockNode) != 15) return;
            SetStoredLevel(blockNode, 0);
            EnqueueDecrease(blockNode, y == lowestSourceY - 1 ? kRemoveTopSkySourceEntry : kRemoveSkySourceEntry);
        }
    }

    void SkyLightEngine::AddSourcesAbove(int x, int z, int lowestSourceY, int worldBottomY) {
        const int neighborLowestSourceY = std::max(
            std::max(GetLowestSourceY(x - 1, z, INT_MIN), GetLowestSourceY(x + 1, z, INT_MIN)),
            std::max(GetLowestSourceY(x, z - 1, INT_MIN), GetLowestSourceY(x, z + 1, INT_MIN)));
        const int startY = std::max(lowestSourceY, worldBottomY);
        for (int y = startY; y <= kLightTopY; ++y) {
            const int64_t blockNode = Pos::Pack(x, y, z);
            if (!StoringLightForSection(blockNode)) return;
            if (GetStoredLevel(blockNode) == 15) return;
            SetStoredLevel(blockNode, 15);
            if (y < neighborLowestSourceY || y == lowestSourceY) {
                EnqueueIncrease(blockNode, kAddSkySourceEntry);
            }
        }
    }

    void SkyLightEngine::PropagateIncrease(int64_t fromNode, int64_t increaseData, int fromLevel) {
        BlockState fromState;
        bool haveFrom = false;
        for (Direction dir : kPropagationDirections) {
            if (!QueueEntry::ShouldPropagateInDirection(increaseData, dir)) continue;
            const int64_t toNode = Pos::Offset(fromNode, dir);
            if (!StoringLightForSection(toNode)) continue;
            const int toLevel = GetStoredLevel(toNode);
            const int maxPossibleNewToLevel = fromLevel - 1;
            if (maxPossibleNewToLevel <= toLevel) continue;
            const BlockState toState = GetState(toNode);
            const int newToLevel = fromLevel - Opacity(toState);
            if (newToLevel <= toLevel) continue;
            if (!haveFrom) {
                fromState = QueueEntry::IsFromEmptyShape(increaseData) ? BlockState{} : GetState(fromNode);
                haveFrom = true;
            }
            if (ShapeOccludes(fromState, toState, dir)) continue;
            SetStoredLevel(toNode, newToLevel);
            if (newToLevel > 1) {
                EnqueueIncrease(toNode, QueueEntry::IncreaseSkipOneDirection(newToLevel, IsEmptyShape(toState), Opposite(dir)));
            }
        }
    }

    void SkyLightEngine::PropagateDecrease(int64_t fromNode, int64_t decreaseData) {
        const int oldFromLevel = QueueEntry::GetFromLevel(decreaseData);
        for (Direction dir : kPropagationDirections) {
            if (!QueueEntry::ShouldPropagateInDirection(decreaseData, dir)) continue;
            const int64_t toNode = Pos::Offset(fromNode, dir);
            if (!StoringLightForSection(toNode)) continue;
            const int toLevel = GetStoredLevel(toNode);
            if (toLevel == 0) continue;
            if (toLevel <= oldFromLevel - 1) {
                SetStoredLevel(toNode, 0);
                EnqueueDecrease(toNode, QueueEntry::DecreaseSkipOneDirection(toLevel, Opposite(dir)));
            } else {
                EnqueueIncrease(toNode, QueueEntry::IncreaseOnlyOneDirection(toLevel, false, Opposite(dir)));
            }
        }
    }

    void SkyLightEngine::FillFullySourcedSections(Chunk& chunk) {
        // MC setLightEnabled: highestNonSourceY = highest lowest-source - 1;
        // every section from the top down to the one above it is all sky.
        // A column open to the bottom of the world reports INT_MIN: then
        // every section is all sky (MC reaches the same end through
        // propagateLightSources; Java's INT_MIN - 1 wraps and skips the fill).
        const int highestLowest = chunk.light.skySources.GetHighestLowestSourceY();
        const int lowestFullySourceSectionY =
            highestLowest == ChunkSkyLightSources::kNegativeInfinity
                ? kMinLightSectionY
                : ((highestLowest - 1) >> 4) + 1;
        const int bottom = std::max(kMinLightSectionY, lowestFullySourceSectionY);
        for (int sy = kMaxLightSectionY; sy >= bottom; --sy) {
            DataLayer& layer = chunk.light.sky[static_cast<size_t>(sy - kMinLightSectionY)];
            if (layer.IsEmpty()) layer.Fill(15);
        }
    }

    void SkyLightEngine::PropagateLightSources(Chunk& chunk) {
        const int cx = chunk.pos.x, cz = chunk.pos.z;
        static const ChunkSkyLightSources kEmpty{};
        auto sourcesOf = [&](int x, int z) -> const ChunkSkyLightSources& {
            const Chunk* c = (x == cx && z == cz) ? &chunk : GetChunk(x, z);
            return c ? c->light.skySources : kEmpty;
        };
        const ChunkSkyLightSources& sources = chunk.light.skySources;
        const ChunkSkyLightSources& north = sourcesOf(cx, cz - 1);
        const ChunkSkyLightSources& south = sourcesOf(cx, cz + 1);
        const ChunkSkyLightSources& west  = sourcesOf(cx - 1, cz);
        const ChunkSkyLightSources& east  = sourcesOf(cx + 1, cz);
        const int minX = cx * 16, minZ = cz * 16;

        for (int sy = kMaxLightSectionY; sy >= kMinLightSectionY; --sy) {
            DataLayer& layer = chunk.light.sky[static_cast<size_t>(sy - kMinLightSectionY)];
            const int sectionMinY = sy * 16;
            const int sectionMaxY = sectionMinY + 15;
            bool sourcesBelow = false;
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    const int lowestSourceY = sources.GetLowestSourceY(x, z);
                    if (lowestSourceY > sectionMaxY) continue;
                    const int northY = z == 0  ? north.GetLowestSourceY(x, 15) : sources.GetLowestSourceY(x, z - 1);
                    const int southY = z == 15 ? south.GetLowestSourceY(x, 0)  : sources.GetLowestSourceY(x, z + 1);
                    const int westY  = x == 0  ? west.GetLowestSourceY(15, z)  : sources.GetLowestSourceY(x - 1, z);
                    const int eastY  = x == 15 ? east.GetLowestSourceY(0, z)   : sources.GetLowestSourceY(x + 1, z);
                    const int neighborLowestSourceY = std::max(std::max(northY, southY), std::max(westY, eastY));
                    // Nothing to write or seed: the whole section of this
                    // column is already sky above both its own and its
                    // neighbours' source tops (the common case far above the
                    // terrain, where the layer was filled with 15 whole).
                    if (sectionMinY > lowestSourceY && sectionMinY >= neighborLowestSourceY &&
                        layer.IsDefinitelyFilledWith(15)) {
                        sourcesBelow = true;
                        continue;
                    }
                    for (int y = sectionMaxY; y >= std::max(sectionMinY, lowestSourceY); --y) {
                        layer.Set(x, y & 15, z, 15);
                        if (y == lowestSourceY || y < neighborLowestSourceY) {
                            EnqueueIncrease(Pos::Pack(minX + x, y, minZ + z),
                                            QueueEntry::IncreaseSkySourceInDirections(
                                                y == lowestSourceY, y < northY, y < southY, y < westY, y < eastY));
                        }
                    }
                    if (lowestSourceY < sectionMinY) sourcesBelow = true;
                }
            }
            if (!sourcesBelow) break;
        }
    }

    // ── LevelLightEngine ────────────────────────────────────────────────────

    LevelLightEngine::LevelLightEngine(LightChunkGetter* getter, bool hasSkyLight)
        : m_hasSkyLight(hasSkyLight), m_block(getter), m_sky(getter) {}

    void LevelLightEngine::CheckBlock(int x, int y, int z) {
        const int64_t pos = Pos::Pack(x, y, z);
        m_block.CheckBlock(pos);
        if (m_hasSkyLight) m_sky.CheckBlock(pos);
    }

    bool LevelLightEngine::HasLightWork() const {
        return (m_hasSkyLight && m_sky.HasLightWork()) || m_block.HasLightWork();
    }

    int LevelLightEngine::RunLightUpdates() {
        int count = m_block.RunLightUpdates();
        if (m_hasSkyLight) count += m_sky.RunLightUpdates();
        return count;
    }

    // ── Entry points ────────────────────────────────────────────────────────

    namespace {
        class SingleChunkGetter final : public LightChunkGetter {
        public:
            Chunk* chunk = nullptr;
            Chunk* GetChunkForLighting(int chunkX, int chunkZ) override {
                return (chunk && chunk->pos.x == chunkX && chunk->pos.z == chunkZ) ? chunk : nullptr;
            }
        };
    }

    void LightChunk(Chunk& chunk, bool hasSkyLight) {
        PROFILE_ZONE_N("Light.InitialChunk");
        thread_local SingleChunkGetter getter;
        thread_local BlockLightEngine blockEngine(&getter);
        thread_local SkyLightEngine skyEngine(&getter);
        getter.chunk = &chunk;
        blockEngine.SetGetter(&getter);
        skyEngine.SetGetter(&getter);

        chunk.light.Reset();
        chunk.light.skySources.FillFrom(chunk);
        if (hasSkyLight) {
            skyEngine.FillFullySourcedSections(chunk);
            skyEngine.PropagateLightSources(chunk);
            skyEngine.RunLightUpdates();
        }
        blockEngine.PropagateLightSources(chunk);
        blockEngine.RunLightUpdates();

        chunk.light.Compact();
        chunk.light.lightCorrect = true;
        getter.chunk = nullptr;
    }

    int GetBrightness(const Chunk* chunk, LightLayer layer, int x, int y, int z, bool hasSkyLight) {
        if (layer == LightLayer::Sky) {
            if (!hasSkyLight) return 0;
            if (!chunk) return 15;
            return chunk->light.Get(LightLayer::Sky, x & 15, y, z & 15);
        }
        if (!chunk) return 0;
        return chunk->light.Get(LightLayer::Block, x & 15, y, z & 15);
    }

} // namespace Game::Lighting
