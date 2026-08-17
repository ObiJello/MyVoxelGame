// File: src/server/entity/MobManager.cpp
#include "server/entity/MobManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/world/loot/GeneratedMobLoot.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/core/JavaRandom.hpp"

#include <algorithm>
#include <cmath>

namespace Server {

    namespace {
        uint64_t ChunkKey(int cx, int cz) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) |
                    static_cast<uint32_t>(cz);
        }
        uint64_t ChunkKeyOf(const Game::Entity& e) {
            return ChunkKey(static_cast<int>(std::floor(e.position.x)) >> 4,
                            static_cast<int>(std::floor(e.position.z)) >> 4);
        }
    }

    MobManager::MobManager(ServerLevelBridge* level) : m_level(level) {}
    MobManager::~MobManager() = default;

    int32_t MobManager::Add(std::unique_ptr<Game::Mob> mob) {
        if (!mob) return 0;
        const int32_t id = m_nextId++;
        mob->SetId(id);
        m_mobs.emplace(id, std::move(mob));
        return id;
    }

    Game::Mob* MobManager::Find(int32_t id) const {
        const auto it = m_mobs.find(id);
        return it == m_mobs.end() ? nullptr : it->second.get();
    }

    int MobManager::CountForCategory(int category) const {
        if (category < 0 || category >= 8) return 0;
        return m_categoryCounts[category].load(std::memory_order_relaxed);
    }

    int MobManager::CountForType(uint16_t type) const {
        if (type >= static_cast<uint16_t>(Game::EntityTypeId::Count)) return 0;
        return m_typeCounts[type].load(std::memory_order_relaxed);
    }

    void MobManager::RebuildIndex() {
        m_byChunk.clear();
        for (auto& [id, mob] : m_mobs) {
            m_byChunk[ChunkKeyOf(*mob)].push_back(mob.get());
        }
    }

    void MobManager::CollectInBox(const Game::AABB& box, const Game::Entity* except,
                                  std::vector<Game::Entity*>& out) const {
        // Chunk range the box spans. Queries are small (a few blocks to a
        // follow range), so this touches a handful of buckets even in a busy
        // world — which is the whole reason the index exists.
        const int minCX = static_cast<int>(std::floor(box.min.x)) >> 4;
        const int maxCX = static_cast<int>(std::floor(box.max.x)) >> 4;
        const int minCZ = static_cast<int>(std::floor(box.min.z)) >> 4;
        const int maxCZ = static_cast<int>(std::floor(box.max.z)) >> 4;

        for (int cx = minCX; cx <= maxCX; ++cx) {
            for (int cz = minCZ; cz <= maxCZ; ++cz) {
                const auto it = m_byChunk.find(ChunkKey(cx, cz));
                if (it == m_byChunk.end()) continue;

                for (Game::Mob* mob : it->second) {
                    if (mob == except || mob->IsRemoved()) continue;
                    if (!mob->GetAABB().Intersects(box)) continue;
                    out.push_back(mob);
                }
            }
        }
    }

    void MobManager::Tick(const std::unordered_set<uint64_t>& tickingChunks,
                          std::vector<int32_t>& outRemoved) {
        PROFILE_ZONE_N("MobTick");

        RebuildIndex();

        int categoryCounts[8] = {};
        int typeCounts[static_cast<size_t>(Game::EntityTypeId::Count)] = {};

        for (auto& [id, mobPtr] : m_mobs) {
            Game::Mob& mob = *mobPtr;
            if (mob.IsRemoved()) continue;

            const size_t category = static_cast<size_t>(mob.TypeInfo().category);
            if (category < 8) ++categoryCounts[category];
            const size_t typeIdx = static_cast<size_t>(mob.GetType());
            if (typeIdx < static_cast<size_t>(Game::EntityTypeId::Count)) ++typeCounts[typeIdx];

            // Phase 1 — despawn check runs for EVERY mob, in or out of ticking
            // range. See the header note.
            mob.CheckDespawn();
            if (mob.IsRemoved()) continue;

            // Phase 2 — simulation, only inside the ticking set.
            const uint64_t key = ChunkKeyOf(mob);
            if (tickingChunks.find(key) == tickingChunks.end()) continue;

            // MC ServerLevel.tickNonPassenger does this immediately before
            // tick(), never inside it: the walk animation and the body rotation
            // control both measure this tick's displacement against these
            // values, so capturing them anywhere else makes a standing mob
            // appear to be walking.
            mob.SetOldPosAndRot();
            ++mob.tickCount;
            mob.Tick();
        }

        // Absorb anything created during the tick (breeding, reinforcements).
        // Deferred by ServerLevelBridge::AddFreshEntity precisely so the loop
        // above never mutates the container it is iterating.
        if (m_level) {
            auto& spawned = m_level->DrainSpawned();
            for (auto& entity : spawned) {
                // Only mobs are accepted here; a non-mob entity would have no
                // manager to tick it, so dropping it is better than leaking it
                // into a container that will never run its logic.
                if (auto* mob = dynamic_cast<Game::Mob*>(entity.get())) {
                    // Ownership moves from the generic Entity pointer to the
                    // typed one; release() is the transfer, not a leak.
                    (void)entity.release();
                    Add(std::unique_ptr<Game::Mob>(mob));
                }
            }
            spawned.clear();
        }

        // Drops, before the sweep — the entity still exists here, which is
        // what its position and its on-fire state are read from.
        for (auto& [id, mobPtr] : m_mobs) {
            if (mobPtr->GetRemovalReason() != Game::RemovalReason::Killed) continue;
            DropLoot(*mobPtr);
        }

        // ── Clear references BEFORE freeing anything ───────────────────────
        //
        // Mobs cache raw pointers to each other across ticks — a breeding
        // partner, a followed parent, an attack target, the thing being looked
        // at — and re-validate them with IsAlive() on the next tick. That is
        // MC's design, and it is safe there only because the JVM keeps a
        // removed entity's object alive for as long as anything points at it.
        //
        // Here, freeing the mob first turns every one of those IsAlive() calls
        // into a virtual dispatch through a freed vptr. So each dying mob is
        // announced to every survivor first, and only then destroyed.
        //
        // This loop is O(dying x surviving), which is fine because `dying` is
        // normally zero and rarely more than a handful.
        std::vector<Game::Mob*> dying;
        for (auto& [id, mobPtr] : m_mobs) {
            if (mobPtr->IsRemoved()) dying.push_back(mobPtr.get());
        }

        if (!dying.empty()) {
            for (auto& [id, mobPtr] : m_mobs) {
                if (mobPtr->IsRemoved()) continue;
                for (Game::Mob* dead : dying) mobPtr->ClearReferenceTo(dead);
            }
            // Player views hold no entity pointers of their own, but they are
            // LivingEntities and a future one might — clearing them here keeps
            // the invariant "nothing points at a dying mob" total.
            if (m_level) {
                for (PlayerEntityView* view : m_level->PlayerViews()) {
                    for (Game::Mob* dead : dying) view->ClearReferenceTo(dead);
                }
            }
        }

        // Sweep. Deferred from the tick loop for the same reason MC defers it:
        // a mob's death can remove OTHER mobs (a creeper explosion), and erasing
        // mid-iteration would invalidate the iterator that is still running.
        for (auto it = m_mobs.begin(); it != m_mobs.end();) {
            if (it->second->IsRemoved()) {
                outRemoved.push_back(it->first);
                it = m_mobs.erase(it);
            } else {
                ++it;
            }
        }

        // Publish the counts last, so a reader never sees a tally that counts
        // mobs this tick already removed.
        for (int i = 0; i < 8; ++i) {
            m_categoryCounts[i].store(categoryCounts[i], std::memory_order_relaxed);
        }
        for (size_t i = 0; i < static_cast<size_t>(Game::EntityTypeId::Count); ++i) {
            m_typeCounts[i].store(typeCounts[i], std::memory_order_relaxed);
        }
    }

    void MobManager::DropLoot(Game::Mob& mob) {
        if (!m_level) return;

        Game::JavaRandom& rng = m_level->Random();

        // MC LivingEntity.shouldDropLoot: babies drop nothing, except that
        // Monster overrides this and drops regardless of age. Zombies are the
        // only baby monster here, and baby zombies do drop in vanilla.
        const bool isMonster = mob.TypeInfo().category == Game::MobCategory::Monster;
        if (mob.IsBaby() && !isMonster) return;

        const Game::MobLootTable* table = Game::FindMobLootTable(mob.GetType());
        if (table) {
            const bool onFire = mob.IsOnFire();
            for (int i = 0; i < table->count; ++i) {
                const Game::MobLootEntry& entry = table->entries[i];

                // Uniform over the inclusive range. A zero roll means nothing
                // dropped, which is how MC expresses "usually but not always".
                const int count = entry.minCount >= entry.maxCount
                    ? entry.minCount
                    : entry.minCount + rng.NextInt(entry.maxCount - entry.minCount + 1);
                if (count <= 0) continue;

                // furnace_smelt: a mob killed while burning drops the cooked
                // form. This is why setting a cow on fire before killing it
                // yields steak.
                const Game::ItemID item =
                    (onFire && entry.smeltedItem != Game::Items::Air) ? entry.smeltedItem
                                                                      : entry.item;
                m_level->SpawnItemDrop(mob.position, item, count);
            }
        }

        // Sheep wool is not in the generated table: MC expresses it as an
        // `alternatives` entry keyed on the sheep's dye colour, which cannot be
        // a static row. An unsheared sheep drops one wool of its colour.
        if (auto* sheep = dynamic_cast<Game::Sheep*>(&mob)) {
            if (!sheep->IsSheared()) {
                m_level->SpawnItemDrop(mob.position, Game::Sheep::WoolItemForColor(sheep->GetColor()), 1);
            }
        }
    }

    void MobManager::RemoveInChunk(Game::Math::ChunkPos chunk,
                                   std::vector<int32_t>& outRemoved) {
        const uint64_t key = ChunkKey(chunk.x, chunk.z);

        // Same ordering rule as Tick's sweep: announce every departure before
        // freeing anything. Chunk unload is in fact the COMMON way a mob that
        // something else is pointing at disappears — a herd straddling a chunk
        // border loses half its members here while the other half still hold
        // FollowParentGoal / BreedGoal pointers to them.
        std::vector<Game::Mob*> dying;
        for (auto& [id, mobPtr] : m_mobs) {
            if (ChunkKeyOf(*mobPtr) == key) dying.push_back(mobPtr.get());
        }
        if (dying.empty()) return;

        for (auto& [id, mobPtr] : m_mobs) {
            if (ChunkKeyOf(*mobPtr) == key) continue;
            for (Game::Mob* dead : dying) mobPtr->ClearReferenceTo(dead);
        }
        if (m_level) {
            for (PlayerEntityView* view : m_level->PlayerViews()) {
                for (Game::Mob* dead : dying) view->ClearReferenceTo(dead);
            }
        }

        for (auto it = m_mobs.begin(); it != m_mobs.end();) {
            if (ChunkKeyOf(*it->second) == key) {
                outRemoved.push_back(it->first);
                it = m_mobs.erase(it);
            } else {
                ++it;
            }
        }
    }

    void MobManager::Clear() {
        m_mobs.clear();
        m_byChunk.clear();
        m_nextId = Game::kMobEntityIdBase;
        for (auto& c : m_categoryCounts) c.store(0, std::memory_order_relaxed);
        for (auto& c : m_typeCounts) c.store(0, std::memory_order_relaxed);
    }

} // namespace Server
