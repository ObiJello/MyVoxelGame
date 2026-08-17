// File: src/common/entity/ai/brain/Memory.hpp
//
// MC's brain memories: net.minecraft.world.entity.ai.memory.
//
// A memory is a typed key, an optional value, and a time-to-live. Behaviours
// communicate ONLY through memories — one writes WALK_TARGET, another reads it
// and walks — so the whole brain is a blackboard, and this file is the
// blackboard's cell.
//
// THE ONE ADAPTATION. MC keys memories by `MemoryModuleType<T>`, a typed
// singleton, and the compiler enforces that WALK_TARGET holds a WalkTarget.
// C++ has no equivalent without templating the entire brain on the value type,
// so the key is an enum and the value is a variant, with the declared kind
// generated from MC's own generic parameter and CHECKED on every write. That
// keeps the failure mode loud: a behaviour that writes the wrong type asserts,
// rather than storing a value the reader will never find.
#pragma once

#include "common/entity/ai/brain/GeneratedMemoryModules.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <variant>
#include <vector>

namespace Game {

    class Entity;
    class LivingEntity;
    class Path;

    // MC MemoryStatus — what a behaviour REQUIRES of a memory before it runs.
    //
    // The three are not interchangeable. VALUE_ABSENT is how MC expresses "only
    // when the mob is not already doing this" (the frog croaks only with no
    // WALK_TARGET); REGISTERED is how it expresses "this mob has the memory at
    // all", regardless of whether it currently holds anything.
    enum class MemoryStatus : uint8_t {
        ValuePresent,
        ValueAbsent,
        Registered,
    };

    // MC PositionTracker — a look/walk target that may be a fixed block or a
    // moving entity. MC has two implementations; the difference that matters is
    // that an EntityTracker's position follows its entity, so a mob told to
    // look at a player keeps looking as the player walks.
    struct PositionTracker {
        // Null entity = a fixed block position.
        Entity*    entity = nullptr;
        glm::ivec3 blockPos{0};
        // MC EntityTracker's `trackEyeHeight`: look at the eyes, not the feet.
        bool       trackEyeHeight = false;

        static PositionTracker OfBlock(const glm::ivec3& pos) {
            PositionTracker t; t.blockPos = pos; return t;
        }
        static PositionTracker OfEntity(Entity* e, bool eyeHeight) {
            PositionTracker t; t.entity = e; t.trackEyeHeight = eyeHeight; return t;
        }

        // Defined in Brain.cpp — Entity is only forward-declared here.
        glm::dvec3 CurrentPosition() const;
        glm::ivec3 CurrentBlockPosition() const;
        bool       IsVisibleBy(const LivingEntity& viewer) const;
    };

    // MC WalkTarget.
    struct WalkTarget {
        PositionTracker target;
        float speedModifier = 1.0f;
        int   closeEnoughDist = 1;

        WalkTarget() = default;
        WalkTarget(const PositionTracker& t, float speed, int closeEnough)
            : target(t), speedModifier(speed), closeEnoughDist(closeEnough) {}
        WalkTarget(const glm::ivec3& pos, float speed, int closeEnough)
            : target(PositionTracker::OfBlock(pos)), speedModifier(speed),
              closeEnoughDist(closeEnough) {}
    };

    // MC NearestVisibleLivingEntities — the sensor's snapshot, kept as its own
    // type because behaviours query it (`findClosest`, `contains`) rather than
    // iterating it.
    struct NearestVisibleLivingEntities {
        std::vector<LivingEntity*> entities;

        bool Contains(const LivingEntity* e) const;
        // First entity matching `pred`, nearest first — the list is kept sorted
        // by distance by the sensor that fills it.
        template <typename Pred>
        LivingEntity* FindClosest(Pred pred) const {
            for (LivingEntity* e : entities) {
                if (pred(e)) return e;
            }
            return nullptr;
        }
    };

    // The value a memory holds. The alternatives correspond one-for-one with
    // MemoryKind, which is generated from MC's own generic parameters.
    using MemoryValue = std::variant<
        std::monostate,              // Unit / Void — presence IS the value
        bool,                        // Bool
        int,                         // Int
        int64_t,                     // Long
        float,                       // Float
        glm::ivec3,                  // BlockPos / GlobalPos
        glm::dvec3,                  // Vec3
        WalkTarget,                  // WalkTarget
        PositionTracker,             // PositionTracker
        Entity*,                     // Entity (every concrete MC entity class)
        std::vector<Entity*>,        // EntityList (List<…> / Set<…>)
        NearestVisibleLivingEntities // VisibleEntities
        >;

    // Which variant index a MemoryKind occupies. Kept next to the variant so a
    // reordering of one is caught by the static_asserts below rather than by a
    // behaviour silently reading nothing.
    inline constexpr size_t MemoryKindIndex(MemoryKind k) {
        switch (k) {
            case MemoryKind::Unit:            return 0;
            case MemoryKind::Bool:            return 1;
            case MemoryKind::Int:             return 2;
            case MemoryKind::Long:            return 3;
            case MemoryKind::Float:           return 4;
            case MemoryKind::BlockPos:        return 5;
            case MemoryKind::Vec3:            return 6;
            case MemoryKind::WalkTarget:      return 7;
            case MemoryKind::PositionTracker: return 8;
            case MemoryKind::Path:            return 0;  // not modelled; see below
            case MemoryKind::VisibleEntities: return 11;
            case MemoryKind::EntityList:      return 10;
            case MemoryKind::Entity:          return 9;
        }
        return 0;
    }

    static_assert(std::variant_size_v<MemoryValue> == 12,
                  "MemoryKindIndex must be updated alongside MemoryValue");

    // MC ExpirableValue — a value plus a countdown. `kNoExpiry` is MC's
    // Long.MAX_VALUE sentinel: a memory that never decays.
    struct ExpirableValue {
        static constexpr int64_t kNoExpiry = std::numeric_limits<int64_t>::max();

        MemoryValue value;
        int64_t     timeToLive = kNoExpiry;

        bool CanExpire()  const { return timeToLive != kNoExpiry; }
        bool HasExpired() const { return timeToLive <= 0; }
        void Tick()             { if (CanExpire()) --timeToLive; }
    };

} // namespace Game
