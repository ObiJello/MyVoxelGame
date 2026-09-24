// File: src/common/entity/ai/village/PoiManager.cpp
#include "common/entity/ai/village/PoiManager.hpp"

#include "common/world/block/BlockState.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/chunk/ChunkSection.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace Game {

    namespace {
        // MC SectionPos.blockToSectionCoord — floor division by 16.
        int BlockToSection(int v) { return v >> 4; }

        // The engine's chunk column: 24 sections from y = -64.
        constexpr int kMinSectionY = -4;

        double DistSqr(const glm::ivec3& a, const glm::ivec3& b) {
            const double dx = static_cast<double>(a.x) - b.x;
            const double dy = static_cast<double>(a.y) - b.y;
            const double dz = static_cast<double>(a.z) - b.z;
            return dx * dx + dy * dy + dz * dz;
        }

        bool PassesOccupancy(const PoiManager::Record& r, PoiManager::Occupancy o) {
            switch (o) {
                case PoiManager::Occupancy::HasSpace:   return r.HasSpace();
                case PoiManager::Occupancy::IsOccupied: return r.IsOccupied();
                case PoiManager::Occupancy::Any:        return true;
            }
            return true;
        }
    }

    // ── Keys ─────────────────────────────────────────────────────────────

    int64_t PoiManager::SectionKey(int sx, int sy, int sz) {
        // MC SectionPos.asLong: 22 bits X, 22 bits Z, 20 bits Y.
        return (static_cast<int64_t>(sx & 0x3FFFFF) << 42) |
               (static_cast<int64_t>(sy & 0xFFFFF)) |
               (static_cast<int64_t>(sz & 0x3FFFFF) << 20);
    }

    int64_t PoiManager::SectionKeyOf(const glm::ivec3& pos) {
        return SectionKey(BlockToSection(pos.x), BlockToSection(pos.y), BlockToSection(pos.z));
    }

    int64_t PoiManager::ChunkKey(int cx, int cz) {
        // MC ChunkPos.asLong.
        return (static_cast<int64_t>(cx) & 0xFFFFFFFFLL) |
               ((static_cast<int64_t>(cz) & 0xFFFFFFFFLL) << 32);
    }

    // ── Record access ────────────────────────────────────────────────────

    PoiManager::Record* PoiManager::FindRecord(const glm::ivec3& pos) {
        auto col = m_columns.find(ChunkKey(BlockToSection(pos.x), BlockToSection(pos.z)));
        if (col == m_columns.end()) return nullptr;
        auto sec = col->second.sections.find(BlockToSection(pos.y));
        if (sec == col->second.sections.end()) return nullptr;
        for (Record& r : sec->second.records) {
            if (r.pos == pos) return &r;
        }
        return nullptr;
    }

    const PoiManager::Record* PoiManager::FindRecord(const glm::ivec3& pos) const {
        return const_cast<PoiManager*>(this)->FindRecord(pos);
    }

    const PoiManager::Record* PoiManager::GetRecord(const glm::ivec3& pos) const {
        return FindRecord(pos);
    }

    std::optional<PoiType> PoiManager::GetType(const glm::ivec3& pos) const {
        if (const Record* r = FindRecord(pos)) return r->type;
        return std::nullopt;
    }

    bool PoiManager::Exists(const glm::ivec3& pos, const TypePredicate& predicate) const {
        const Record* r = FindRecord(pos);
        return r && (!predicate || predicate(r->type));
    }

    size_t PoiManager::RecordCount() const {
        size_t n = 0;
        for (const auto& [key, col] : m_columns) {
            (void)key;
            for (const auto& [sy, sec] : col.sections) { (void)sy; n += sec.records.size(); }
        }
        return n;
    }

    // ── Add / remove ─────────────────────────────────────────────────────

    bool PoiManager::Add(const glm::ivec3& pos, PoiType type) {
        // MC PoiSection.add: a record already there with the same type is a
        // no-op; a DIFFERENT type is MC's "POI data mismatch" and the new one
        // replaces it.
        if (Record* existing = FindRecord(pos)) {
            if (existing->type == type) return false;
            Remove(pos);
        }
        Column& col = m_columns[ChunkKey(BlockToSection(pos.x), BlockToSection(pos.z))];
        Section& sec = col.sections[BlockToSection(pos.y)];
        Record r;
        r.pos = pos;
        r.type = type;
        r.freeTickets = PoiMaxTickets(type);
        sec.records.push_back(std::move(r));
        return true;
    }

    void PoiManager::Remove(const glm::ivec3& pos) {
        auto col = m_columns.find(ChunkKey(BlockToSection(pos.x), BlockToSection(pos.z)));
        if (col == m_columns.end()) return;
        auto sec = col->second.sections.find(BlockToSection(pos.y));
        if (sec == col->second.sections.end()) return;
        auto& records = sec->second.records;
        for (auto it = records.begin(); it != records.end(); ++it) {
            if (it->pos != pos) continue;
            if (CountsForVillage(*it)) {
                auto c = m_villageCentres.find(SectionKeyOf(pos));
                if (c != m_villageCentres.end() && --c->second <= 0) m_villageCentres.erase(c);
            }
            records.erase(it);
            return;
        }
    }

    void PoiManager::OnBlockStateChange(const glm::ivec3& pos, BlockState oldState,
                                        BlockState newState) {
        const std::optional<PoiType> before = PoiTypeForState(oldState);
        const std::optional<PoiType> after  = PoiTypeForState(newState);
        if (before == after) return;
        if (before) Remove(pos);
        if (after)  Add(pos, *after);
    }

    // ── Chunk scan ───────────────────────────────────────────────────────

    bool PoiManager::IsChunkScanned(int chunkX, int chunkZ) const {
        return m_scannedChunks.count(ChunkKey(chunkX, chunkZ)) != 0;
    }

    void PoiManager::NoteChunkLoaded(Math::ChunkPos chunkPos, const Chunk& chunk) {
        if (!m_scannedChunks.insert(ChunkKey(chunkPos.x, chunkPos.z)).second) return;

        for (int si = 0; si < Math::SECTIONS_PER_CHUNK; ++si) {
            const ChunkSection* section = chunk.GetSection(si);
            if (!section || section->IsAllAir()) continue;

            // MC PoiManager.mayHavePoi → LevelChunkSection.maybeHas(hasPoi):
            // the palette answers "none" for almost every section. A global
            // palette carries no list and is walked.
            const auto& states = section->States();
            if (!states.IsGlobalPalette()) {
                bool may = false;
                for (uint32_t raw : states.Palette()) {
                    if (BlockMayHavePoi(BlockState::FromRawId(raw).Block())) { may = true; break; }
                }
                if (!may) continue;
            }

            const int baseY = (si + kMinSectionY) * 16;
            for (int y = 0; y < 16; ++y) {
                for (int z = 0; z < 16; ++z) {
                    for (int x = 0; x < 16; ++x) {
                        const BlockState state = section->StateAt(x, y, z);
                        if (!BlockMayHavePoi(state.Block())) continue;
                        if (const auto type = PoiTypeForState(state)) {
                            Add(glm::ivec3(chunkPos.x * 16 + x, baseY + y, chunkPos.z * 16 + z), *type);
                        }
                    }
                }
            }
        }
    }

    // ── Walks ────────────────────────────────────────────────────────────

    void PoiManager::Walk(const TypePredicate& predicate, const glm::ivec3& center, int radius,
                          Occupancy occupancy, bool circular,
                          const std::function<void(const Record&)>& visit) const {
        // MC getInSquare: chunkRadius = floorDiv(radius, 16) + 1 around the
        // centre's chunk, ChunkPos.rangeClosed order (x fastest, then z).
        const int chunkRadius = (radius >= 0 ? radius / 16 : -((-radius + 15) / 16)) + 1;
        const int ccx = BlockToSection(center.x);
        const int ccz = BlockToSection(center.z);
        const double radiusSqr = static_cast<double>(radius) * radius;
        for (int cz = ccz - chunkRadius; cz <= ccz + chunkRadius; ++cz) {
            for (int cx = ccx - chunkRadius; cx <= ccx + chunkRadius; ++cx) {
                auto col = m_columns.find(ChunkKey(cx, cz));
                if (col == m_columns.end()) continue;
                for (const auto& [sy, sec] : col->second.sections) {
                    (void)sy;
                    for (const Record& r : sec.records) {
                        if (predicate && !predicate(r.type)) continue;
                        if (!PassesOccupancy(r, occupancy)) continue;
                        if (std::abs(r.pos.x - center.x) > radius ||
                            std::abs(r.pos.z - center.z) > radius) continue;
                        if (circular && DistSqr(r.pos, center) > radiusSqr) continue;
                        visit(r);
                    }
                }
            }
        }
    }

    std::vector<const PoiManager::Record*>
    PoiManager::GetInRange(const TypePredicate& predicate, const glm::ivec3& center, int radius,
                           Occupancy occupancy) const {
        std::vector<const Record*> out;
        Walk(predicate, center, radius, occupancy, /*circular=*/true,
             [&](const Record& r) { out.push_back(&r); });
        return out;
    }

    std::vector<const PoiManager::Record*>
    PoiManager::GetInSquare(const TypePredicate& predicate, const glm::ivec3& center, int radius,
                            Occupancy occupancy) const {
        std::vector<const Record*> out;
        Walk(predicate, center, radius, occupancy, /*circular=*/false,
             [&](const Record& r) { out.push_back(&r); });
        return out;
    }

    std::optional<glm::ivec3> PoiManager::FindClosest(const TypePredicate& predicate,
                                                      const glm::ivec3& center, int radius,
                                                      Occupancy occupancy,
                                                      const PosPredicate& filter) const {
        // MC: getInRange(...).map(getPos).filter(filter).min(distSqr) — the
        // FIRST of equally close positions wins (Stream.min keeps the first).
        std::optional<glm::ivec3> best;
        double bestDist = std::numeric_limits<double>::max();
        Walk(predicate, center, radius, occupancy, true, [&](const Record& r) {
            if (filter && !filter(r.pos)) return;
            const double d = DistSqr(r.pos, center);
            if (!best || d < bestDist) { best = r.pos; bestDist = d; }
        });
        return best;
    }

    std::vector<std::pair<PoiType, glm::ivec3>>
    PoiManager::FindAllWithType(const TypePredicate& predicate, const PosPredicate& filter,
                                const glm::ivec3& center, int radius, Occupancy occupancy) const {
        std::vector<std::pair<PoiType, glm::ivec3>> out;
        Walk(predicate, center, radius, occupancy, true, [&](const Record& r) {
            if (filter && !filter(r.pos)) return;
            out.emplace_back(r.type, r.pos);
        });
        return out;
    }

    std::vector<std::pair<PoiType, glm::ivec3>>
    PoiManager::FindAllClosestFirstWithType(const TypePredicate& predicate, const PosPredicate& filter,
                                            const glm::ivec3& center, int radius,
                                            Occupancy occupancy) const {
        auto out = FindAllWithType(predicate, filter, center, radius, occupancy);
        // Stream.sorted is stable.
        std::stable_sort(out.begin(), out.end(), [&](const auto& a, const auto& b) {
            return DistSqr(a.second, center) < DistSqr(b.second, center);
        });
        return out;
    }

    // ── Tickets ──────────────────────────────────────────────────────────

    void PoiManager::NoteOccupancy(const Record& record, bool wasCounted) {
        const bool counts = CountsForVillage(record);
        if (counts == wasCounted) return;
        const int64_t key = SectionKeyOf(record.pos);
        if (counts) {
            ++m_villageCentres[key];
        } else {
            auto c = m_villageCentres.find(key);
            if (c != m_villageCentres.end() && --c->second <= 0) m_villageCentres.erase(c);
        }
    }

    bool PoiManager::AcquireTicket(Record& record, const Uuid& holder) {
        // MC PoiRecord.acquireTicket.
        if (record.freeTickets <= 0) return false;
        const bool was = CountsForVillage(record);
        --record.freeTickets;
        if (!UuidIsNil(holder) &&
            std::find(record.holders.begin(), record.holders.end(), holder) == record.holders.end()) {
            record.holders.push_back(holder);
        }
        NoteOccupancy(record, was);
        return true;
    }

    bool PoiManager::ReleaseTicket(Record& record, const Uuid& holder) {
        // MC PoiRecord.releaseTicket.
        if (record.freeTickets >= PoiMaxTickets(record.type)) return false;
        const bool was = CountsForVillage(record);
        ++record.freeTickets;
        auto it = std::find(record.holders.begin(), record.holders.end(), holder);
        if (it != record.holders.end()) {
            record.holders.erase(it);
        } else {
            // An anonymous release (MC's are all anonymous): keep the list no
            // longer than the tickets actually taken.
            const size_t taken = static_cast<size_t>(PoiMaxTickets(record.type) - record.freeTickets);
            while (record.holders.size() > taken) record.holders.pop_back();
        }
        NoteOccupancy(record, was);
        return true;
    }

    std::optional<glm::ivec3> PoiManager::Take(const TypePredicate& predicate,
                                               const std::function<bool(PoiType, const glm::ivec3&)>& filter,
                                               const glm::ivec3& center, int radius,
                                               const Uuid& holder) {
        // MC take: getInRange(HAS_SPACE).filter(filter).findFirst().
        std::optional<glm::ivec3> found;
        Walk(predicate, center, radius, Occupancy::HasSpace, true, [&](const Record& r) {
            if (found) return;
            if (filter && !filter(r.type, r.pos)) return;
            found = r.pos;
        });
        if (found) {
            if (Record* r = FindRecord(*found)) AcquireTicket(*r, holder);
        }
        return found;
    }

    bool PoiManager::Release(const glm::ivec3& pos, const Uuid& holder) {
        Record* r = FindRecord(pos);
        return r && ReleaseTicket(*r, holder);
    }

    bool PoiManager::Restore(const glm::ivec3& pos, PoiType expected, const Uuid& holder) {
        Record* r = FindRecord(pos);
        if (!r || r->type != expected) return false;
        if (!UuidIsNil(holder) &&
            std::find(r->holders.begin(), r->holders.end(), holder) != r->holders.end()) {
            return true;   // already ours this session
        }
        return AcquireTicket(*r, holder);
    }

    void PoiManager::TransferHolder(const glm::ivec3& pos, const Uuid& from, const Uuid& to) {
        Record* r = FindRecord(pos);
        if (!r) return;
        if (UuidIsNil(to)) return;
        auto it = std::find(r->holders.begin(), r->holders.end(), from);
        if (!UuidIsNil(from) && it != r->holders.end()) {
            *it = to;
            return;
        }
        // An anonymous ticket (taken with a nil holder) gains its holder:
        // name it while the named holders are fewer than the tickets taken.
        const size_t taken = static_cast<size_t>(PoiMaxTickets(r->type) - r->freeTickets);
        if (r->holders.size() < taken &&
            std::find(r->holders.begin(), r->holders.end(), to) == r->holders.end()) {
            r->holders.push_back(to);
        }
    }

    // ── Village distance ─────────────────────────────────────────────────

    int PoiManager::SectionsToVillage(int sx, int sy, int sz) const {
        // MC's DistanceTracker is a SectionTracker over the 26-neighbourhood
        // with unit cost, sourced at 0 in every village-centre section and
        // capped at 7: its level IS the Chebyshev distance to the nearest
        // centre, or 7 when there is none within 6.
        constexpr int kNone = kMaxVillageDistance + 1;
        int best = kNone;
        const size_t neighbourhood = static_cast<size_t>((2 * kMaxVillageDistance + 1) *
                                                         (2 * kMaxVillageDistance + 1) *
                                                         (2 * kMaxVillageDistance + 1));
        if (m_villageCentres.size() < neighbourhood) {
            for (const auto& [key, count] : m_villageCentres) {
                (void)count;
                // Unpack MC SectionPos.asLong.
                const int cx = static_cast<int>(key >> 42);
                const int cy = static_cast<int>((key << 44) >> 44);
                const int cz = static_cast<int>((key << 22) >> 42);
                const int d = std::max({ std::abs(cx - sx), std::abs(cy - sy), std::abs(cz - sz) });
                if (d < best) {
                    best = d;
                    if (best == 0) break;
                }
            }
        } else {
            for (int dy = -kMaxVillageDistance; dy <= kMaxVillageDistance; ++dy) {
                for (int dz = -kMaxVillageDistance; dz <= kMaxVillageDistance; ++dz) {
                    for (int dx = -kMaxVillageDistance; dx <= kMaxVillageDistance; ++dx) {
                        const int d = std::max({ std::abs(dx), std::abs(dy), std::abs(dz) });
                        if (d >= best) continue;
                        if (m_villageCentres.count(SectionKey(sx + dx, sy + dy, sz + dz))) best = d;
                    }
                }
            }
        }
        return best > kMaxVillageDistance ? kNone : best;
    }

    int PoiManager::SectionsToVillage(const glm::ivec3& blockPos) const {
        return SectionsToVillage(BlockToSection(blockPos.x), BlockToSection(blockPos.y),
                                 BlockToSection(blockPos.z));
    }

    bool PoiManager::IsCloseToVillage(const glm::ivec3& blockPos, int sectionDistance) const {
        // MC ServerLevel.isCloseToVillage.
        return sectionDistance <= kMaxVillageDistance &&
               SectionsToVillage(blockPos) <= sectionDistance;
    }

} // namespace Game
