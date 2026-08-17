// File: src/common/world/pathfinder/PathTypeTable.cpp
#include "common/world/pathfinder/PathTypeTable.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/core/Log.hpp"

#include <array>
#include <string>
#include <string_view>

namespace Game {

    namespace {

        std::array<PathType, static_cast<size_t>(BlockID::Count)> g_table{};
        bool g_initialised = false;

        bool EndsWith(std::string_view s, std::string_view suffix) {
            return s.size() >= suffix.size() &&
                   s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        bool Contains(std::string_view s, std::string_view needle) {
            return s.find(needle) != std::string_view::npos;
        }

        // The MC chain from getPathTypeFromState, in the SAME order — the order
        // matters because several tests overlap (a lit campfire is both a
        // burning block and a collidable one, and must answer DAMAGE_FIRE).
        PathType ClassifySlug(std::string_view slug, BlockID id) {
            if (id == BlockID::Air) return PathType::Open;

            // Trapdoors, lily pads and big dripleaf: things a mob can stand on
            // whose collision shape says otherwise.
            if (EndsWith(slug, "_trapdoor") || slug == "lily_pad" || slug == "big_dripleaf") {
                return PathType::Trapdoor;
            }

            if (slug == "powder_snow") return PathType::PowderSnow;

            // Contact damage — mobs refuse these outright (malus -1).
            if (slug == "cactus" || slug == "sweet_berry_bush") return PathType::DamageOther;

            if (slug == "honey_block") return PathType::StickyHoney;
            if (slug == "cocoa")       return PathType::Cocoa;

            // Damage a mob will path near but not into.
            if (slug == "wither_rose" || slug == "pointed_dripstone") {
                return PathType::DamageCautious;
            }

            if (slug == "lava" || slug == "flowing_lava") return PathType::Lava;

            // MC NodeEvaluator.isBurningBlock. Campfires are only dangerous
            // when lit, but lit is the default state and the state index is not
            // available here — treating them as burning is the safe direction
            // to be wrong in, since it only makes mobs more cautious.
            if (slug == "fire" || slug == "soul_fire" || slug == "magma_block" ||
                slug == "lava_cauldron" || EndsWith(slug, "campfire")) {
                return PathType::DamageFire;
            }

            if (EndsWith(slug, "_door")) {
                // See the header: state is not keyed here, so doors read closed.
                return (slug == "iron_door") ? PathType::DoorIronClosed
                                             : PathType::DoorWoodClosed;
            }

            if (EndsWith(slug, "rail")) return PathType::Rail;
            if (EndsWith(slug, "_leaves")) return PathType::Leaves;

            // Fences, walls and closed fence gates are the "partial collision"
            // family: a mob cannot walk through them but CAN see past them, so
            // MC gives them their own type rather than BLOCKED.
            if (EndsWith(slug, "_fence") || EndsWith(slug, "_wall") ||
                EndsWith(slug, "_fence_gate") || slug == "nether_brick_fence") {
                return PathType::Fence;
            }

            const bool isWater = (slug == "water" || slug == "flowing_water");
            if (isWater) return PathType::Water;

            // MC's final branch is isPathfindable(LAND), which for practically
            // every block is "does it have a collision shape".
            return BlockRegistry::HasCollision(id) ? PathType::Blocked : PathType::Open;
        }

    } // namespace

    void InitPathTypeTable() {
        for (size_t i = 0; i < g_table.size(); ++i) {
            const BlockID id = static_cast<BlockID>(i);
            const std::string& slug = BlockRegistry::Get(id).registrySlug;
            g_table[i] = ClassifySlug(slug, id);
        }
        g_initialised = true;

        Log::Info("[Pathfinder] Classified %zu block ids into path types", g_table.size());
    }

    PathType GetPathTypeFromBlock(BlockID id) {
        const size_t idx = static_cast<size_t>(id);
        if (!g_initialised || idx >= g_table.size()) {
            // Failing closed (BLOCKED) would wall every mob in place if the
            // table were ever read before init; OPEN degrades to "mobs walk
            // through everything", which is louder and easier to spot.
            return PathType::Open;
        }
        return g_table[idx];
    }

} // namespace Game
