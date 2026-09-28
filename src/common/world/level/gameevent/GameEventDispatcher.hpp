// File: src/common/world/level/gameevent/GameEventDispatcher.hpp
//
// Port of MC 26.3's game-event plumbing (net.minecraft.world.level.gameevent):
//
//   PositionSource / BlockPositionSource / EntityPositionSource
//   GameEventListener (+ .Provider, .DeliveryMode)
//   EuclideanGameEventListenerRegistry — one per chunk section
//   GameEventDispatcher                — ServerLevel.gameEvent's back end
//   DynamicGameEventListener           — an entity's moving registration
//
// HOW REGISTRATION MAPS ONTO THIS ENGINE
// --------------------------------------
// MC keeps a registry per LevelChunk section and registers a block entity's
// listener when the entity joins the chunk (LevelChunk.addGameEventListener)
// and unregisters it when the entity is removed. The engine's chunks are
// shared with the IO thread and know nothing of levels, so the registries
// live here, keyed by SectionPos, and a block listener is registered by
// POSITION: the block entity asserts its registration from its server tick
// (idempotent — a hash lookup and a scan of a handful of entries) and every
// visit resolves the position back to the block entity through the level.
// A position whose block entity has gone (broken, replaced, its chunk
// unloaded) is dropped from the registry on the visit that finds it gone,
// which is MC's unregister-on-removal without a pointer that could dangle.
//
// Entity listeners (the warden) are real pointers: DynamicGameEventListener
// moves the registration between sections as the entity walks, exactly as
// MC's does, and its destructor unregisters — through a weak_ptr, so an
// entity that outlives its level's dispatcher never touches a dead one.
//
// THREADS
// -------
// Everything here is server-thread state. A post from anywhere else (a
// parallel mob batch) is queued and delivered by the server thread at the
// next post or the next block-entity tick (see Post / FlushDeferred).
#pragma once

#include "GameEvent.hpp"

#include "common/core/Uuid.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Game {

    class World;
    class Entity;
    struct ParticleOptions;

    // MC PositionSource — where a listener hears from. A block listener is
    // pinned to its block's centre; an entity listener rides the entity (at
    // `yOffset` above its feet — the warden listens from its eyes).
    struct PositionSource {
        enum class Kind : uint8_t { Block, Entity };

        static PositionSource OfBlock(const glm::ivec3& pos) {
            PositionSource s;
            s.kind = Kind::Block;
            s.blockPos = pos;
            return s;
        }
        // `entity` must outlive the source (the entity owns it).
        static PositionSource OfEntity(const Entity* entity, float yOffset) {
            PositionSource s;
            s.kind = Kind::Entity;
            s.entity = entity;
            s.yOffset = yOffset;
            return s;
        }

        // MC PositionSource.getPosition(level): Vec3.atCenterOf(pos) for a
        // block, entity.position() + (0, yOffset, 0) for an entity. Empty for
        // an entity source whose entity has been removed.
        std::optional<glm::dvec3> GetPosition() const;

        // MC VibrationParticleOption(this, arrivalInTicks) — the particle that
        // flies to this source.
        ParticleOptions VibrationParticle(int arrivalInTicks) const;

        Kind          kind = Kind::Block;
        glm::ivec3    blockPos{0};
        const Entity* entity = nullptr;
        float         yOffset = 0.0f;
    };

    // MC GameEventListener.
    class GameEventListener {
    public:
        // MC GameEventListener.DeliveryMode: UNSPECIFIED listeners are told
        // as they are visited; BY_DISTANCE ones (the sculk catalyst) are
        // collected and told nearest first once every section is visited.
        enum class DeliveryMode : uint8_t { Unspecified, ByDistance };

        virtual ~GameEventListener() = default;

        virtual const PositionSource& GetListenerSource() const = 0;
        virtual int GetListenerRadius() const = 0;
        virtual bool HandleGameEvent(World& level, GameEventId event, const GameEventContext& context,
                                     const glm::dvec3& sourcePosition) = 0;
        virtual DeliveryMode GetDeliveryMode() const { return DeliveryMode::Unspecified; }
    };

    // MC GameEventListener.Provider — what a listening block entity is. The
    // dispatcher finds a registered block position's listener through this.
    class GameEventListenerProvider {
    public:
        virtual ~GameEventListenerProvider() = default;
        virtual GameEventListener* GetGameEventListener() = 0;
    };

    // MC SectionPos.asLong packing (x 22 bits, z 22 bits, y 20 bits).
    inline int64_t PackSectionPos(int sx, int sy, int sz) {
        return ((static_cast<int64_t>(sx) & 4194303LL) << 42) |
               (static_cast<int64_t>(sy) & 1048575LL) |
               ((static_cast<int64_t>(sz) & 4194303LL) << 20);
    }
    inline int64_t SectionPosOfBlock(const glm::ivec3& p) {
        return PackSectionPos(p.x >> 4, p.y >> 4, p.z >> 4);
    }

    class GameEventDispatcher : public std::enable_shared_from_this<GameEventDispatcher> {
    public:
        explicit GameEventDispatcher(World& level) : m_level(level) {}
        GameEventDispatcher(const GameEventDispatcher&) = delete;
        GameEventDispatcher& operator=(const GameEventDispatcher&) = delete;

        // MC GameEventDispatcher.post: every registered listener in the
        // sections the event's notification radius touches, in loaded chunks,
        // that is itself within ITS listener radius of the source.
        void Post(GameEventId event, const glm::dvec3& position, const GameEventContext& context);

        // Deliver the posts queued from other threads. Server thread only.
        void FlushDeferred();

        // Drop the block-listener registrations of sections whose chunk is
        // no longer resident (their block entities re-register when the
        // chunk returns). Dynamic registrations are their owners' to remove.
        // Called now and then from the level's block-entity tick.
        void PruneUnloadedSections();

        // ── Block listeners (by position — see the header note) ─────────────
        // Idempotent. Server thread.
        void EnsureBlockListener(const glm::ivec3& pos);

        // ── Dynamic listeners (entities) ────────────────────────────────────
        void RegisterDynamic(GameEventListener* listener, int64_t sectionKey);
        void UnregisterDynamic(GameEventListener* listener, int64_t sectionKey);

        World& Level() { return m_level; }

    private:
        struct Entry {
            GameEventListener* dynamic = nullptr;   // null = a block listener at `pos`
            glm::ivec3         pos{0};
            bool               removed = false;     // unregistered mid-visit
        };
        struct Section {
            std::vector<Entry> listeners;
            // MC EuclideanGameEventListenerRegistry.processing + the two
            // pending lists: a listener registered or removed while its
            // section is being visited lands after the visit.
            bool               processing = false;
            std::vector<Entry> toAdd;
        };

        // MC EuclideanGameEventListenerRegistry.visitInRangeListeners.
        struct Visit {
            GameEventListener* listener;
            glm::dvec3         listenerPos;
            bool               block;   // resolved from `pos` — see Revalidate
            glm::ivec3         pos;
        };
        GameEventListener* Revalidate(const Visit& visit);
        bool VisitSection(int64_t key, Section& section, const glm::dvec3& source,
                          std::vector<Visit>& out);

        void DeliverNow(GameEventId event, const glm::dvec3& position, const GameEventContext& context);

        World& m_level;
        std::unordered_map<int64_t, Section> m_sections;

        // Posts from threads other than the server thread (see THREADS).
        struct Deferred {
            GameEventId               event;
            glm::dvec3                position;
            Uuid                      sourceEntity{};
            bool                      hasSourceEntity = false;
            std::optional<BlockState> affectedState;
        };
        std::mutex            m_deferredMutex;
        std::vector<Deferred> m_deferred;
        bool                  m_flushing = false;
    };

    // MC DynamicGameEventListener — an entity's listener registration, moved
    // between sections as the entity walks (ServerLevel's entity callbacks
    // call add / move / remove; here the owning entity calls Move from its
    // server tick and Remove when it leaves the level).
    class DynamicGameEventListener {
    public:
        explicit DynamicGameEventListener(GameEventListener& listener) : m_listener(listener) {}
        ~DynamicGameEventListener() { Remove(); }
        DynamicGameEventListener(const DynamicGameEventListener&) = delete;
        DynamicGameEventListener& operator=(const DynamicGameEventListener&) = delete;

        GameEventListener& GetListener() { return m_listener; }

        // MC move(level): re-register in the section the listener's source
        // now sits in, if that changed (or the dispatcher did — a new level).
        void Move(GameEventDispatcher* dispatcher);
        // MC remove(level).
        void Remove();

    private:
        GameEventListener&                 m_listener;
        std::weak_ptr<GameEventDispatcher> m_registeredWith;
        GameEventDispatcher*               m_registeredRaw = nullptr;
        int64_t                            m_lastSection = 0;
        bool                               m_registered = false;
    };

} // namespace Game
