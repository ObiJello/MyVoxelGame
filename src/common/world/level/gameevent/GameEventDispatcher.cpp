// File: src/common/world/level/gameevent/GameEventDispatcher.cpp
#include "GameEventDispatcher.hpp"

#include "common/core/Assert.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/level/World.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

namespace Game {

    namespace {

        glm::ivec3 BlockContaining(const glm::dvec3& p) {
            return glm::ivec3(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)),
                              static_cast<int>(std::floor(p.z)));
        }

        bool OnServerThread() {
            // No server thread yet (a tool, a test): whoever calls owns it.
            return Server::g_serverThreadId == std::thread::id() ||
                   std::this_thread::get_id() == Server::g_serverThreadId;
        }

    } // namespace

    // ── PositionSource ──────────────────────────────────────────────────────

    std::optional<glm::dvec3> PositionSource::GetPosition() const {
        if (kind == Kind::Block) {
            return glm::dvec3(blockPos.x + 0.5, blockPos.y + 0.5, blockPos.z + 0.5);
        }
        if (!entity || entity->IsRemoved()) return std::nullopt;
        return entity->position + glm::dvec3(0.0, static_cast<double>(yOffset), 0.0);
    }

    ParticleOptions PositionSource::VibrationParticle(int arrivalInTicks) const {
        if (kind == Kind::Entity && entity) {
            return ParticleOptions::VibrationToEntity(entity->GetId(), yOffset, arrivalInTicks);
        }
        return ParticleOptions::VibrationToBlock(blockPos, arrivalInTicks);
    }

    // ── GameEventDispatcher ─────────────────────────────────────────────────

    void GameEventDispatcher::Post(GameEventId event, const glm::dvec3& position,
                                   const GameEventContext& context) {
        if (!OnServerThread()) {
            // A parallel batch: hold the post for the server thread. The
            // source entity travels as its UUID — by delivery time the
            // pointer may name a freed entity.
            Deferred d;
            d.event = event;
            d.position = position;
            d.affectedState = context.affectedState;
            if (context.sourceEntity) {
                d.sourceEntity = context.sourceEntity->GetUuid();
                d.hasSourceEntity = true;
            }
            std::lock_guard<std::mutex> lock(m_deferredMutex);
            m_deferred.push_back(d);
            return;
        }
        FlushDeferred();
        DeliverNow(event, position, context);
    }

    void GameEventDispatcher::FlushDeferred() {
        if (m_flushing) return;
        std::vector<Deferred> pending;
        {
            std::lock_guard<std::mutex> lock(m_deferredMutex);
            if (m_deferred.empty()) return;
            pending.swap(m_deferred);
        }
        m_flushing = true;
        EntityLevel* entities = m_level.Entities();
        for (const Deferred& d : pending) {
            GameEventContext context;
            context.affectedState = d.affectedState;
            if (d.hasSourceEntity && entities) context.sourceEntity = entities->ResolveEntity(d.sourceEntity);
            DeliverNow(d.event, d.position, context);
        }
        m_flushing = false;
    }

    void GameEventDispatcher::DeliverNow(GameEventId event, const glm::dvec3& position,
                                         const GameEventContext& context) {
        // MC GameEventDispatcher.post.
        const int radius = GameEvents::NotificationRadius(event);
        const glm::ivec3 center = BlockContaining(position);
        const int sectionMinX = (center.x - radius) >> 4;
        const int sectionMinY = (center.y - radius) >> 4;
        const int sectionMinZ = (center.z - radius) >> 4;
        const int sectionMaxX = (center.x + radius) >> 4;
        const int sectionMaxY = (center.y + radius) >> 4;
        const int sectionMaxZ = (center.z + radius) >> 4;

        if (m_sections.empty()) return;

        struct ListenerInfo {
            Visit  visit;
            double distanceToRecipient;
        };
        std::vector<ListenerInfo> toHandleByDistance;
        std::vector<Visit> visits;

        for (int chunkX = sectionMinX; chunkX <= sectionMaxX; ++chunkX) {
            for (int chunkZ = sectionMinZ; chunkZ <= sectionMaxZ; ++chunkZ) {
                // getChunkNow: only a resident chunk's registries are visited.
                bool chunkChecked = false;
                bool chunkLoaded = false;
                for (int sectionY = sectionMinY; sectionY <= sectionMaxY; ++sectionY) {
                    const int64_t key = PackSectionPos(chunkX, sectionY, chunkZ);
                    auto it = m_sections.find(key);
                    if (it == m_sections.end()) continue;
                    if (!chunkChecked) {
                        chunkChecked = true;
                        chunkLoaded = m_level.IsChunkLoaded(chunkX, chunkZ);
                    }
                    if (!chunkLoaded) continue;
                    visits.clear();
                    VisitSection(key, it->second, position, visits);
                    for (const Visit& v : visits) {
                        if (v.listener->GetDeliveryMode() == GameEventListener::DeliveryMode::ByDistance) {
                            const glm::dvec3 d = position - v.listenerPos;
                            toHandleByDistance.push_back({v, glm::dot(d, d)});
                        } else if (GameEventListener* l = Revalidate(v)) {
                            l->HandleGameEvent(m_level, event, context, position);
                        }
                    }
                }
            }
        }

        // MC handleGameEventMessagesInQueue: nearest first (a stable sort, as
        // Collections.sort is).
        if (!toHandleByDistance.empty()) {
            std::stable_sort(toHandleByDistance.begin(), toHandleByDistance.end(),
                             [](const ListenerInfo& a, const ListenerInfo& b) {
                                 return a.distanceToRecipient < b.distanceToRecipient;
                             });
            for (const ListenerInfo& info : toHandleByDistance) {
                if (GameEventListener* l = Revalidate(info.visit)) {
                    l->HandleGameEvent(m_level, event, context, position);
                }
            }
        }
    }

    GameEventListener* GameEventDispatcher::Revalidate(const Visit& visit) {
        // A handler earlier in this post may have written blocks (a sensor
        // powering its neighbours): a block listener is re-resolved before
        // it is told, so one whose block entity went meanwhile is skipped
        // rather than reached through a freed pointer.
        if (!visit.block) return visit.listener;
        auto* provider = dynamic_cast<GameEventListenerProvider*>(m_level.GetBlockEntity(visit.pos));
        GameEventListener* listener = provider ? provider->GetGameEventListener() : nullptr;
        return listener == visit.listener ? listener : nullptr;
    }

    bool GameEventDispatcher::VisitSection(int64_t key, Section& section, const glm::dvec3& source,
                                           std::vector<Visit>& out) {
        (void)key;
        // MC visitInRangeListeners, with the processing guard: the visit
        // collects first and the caller delivers afterwards, so a handler
        // that registers or removes listeners edits pending lists, not the
        // vector being walked.
        section.processing = true;
        bool applicable = false;
        const glm::ivec3 sourceBlock = BlockContaining(source);
        for (size_t i = 0; i < section.listeners.size();) {
            Entry& entry = section.listeners[i];
            if (entry.removed) {
                section.listeners.erase(section.listeners.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            GameEventListener* listener = entry.dynamic;
            if (!listener) {
                // A block listener: resolve its block entity now.
                BlockEntity* be = m_level.GetBlockEntity(entry.pos);
                auto* provider = dynamic_cast<GameEventListenerProvider*>(be);
                listener = provider ? provider->GetGameEventListener() : nullptr;
                if (!listener) {
                    section.listeners.erase(section.listeners.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
            }
            // MC getPostableListenerPosition: block-integer distance against
            // the listener's own radius.
            const std::optional<glm::dvec3> position = listener->GetListenerSource().GetPosition();
            if (position) {
                const glm::ivec3 lp = BlockContaining(*position);
                const double dx = static_cast<double>(lp.x - sourceBlock.x);
                const double dy = static_cast<double>(lp.y - sourceBlock.y);
                const double dz = static_cast<double>(lp.z - sourceBlock.z);
                const int r = listener->GetListenerRadius();
                if (dx * dx + dy * dy + dz * dz <= static_cast<double>(r * r)) {
                    out.push_back(Visit{listener, *position, entry.dynamic == nullptr, entry.pos});
                    applicable = true;
                }
            }
            ++i;
        }
        section.processing = false;
        if (!section.toAdd.empty()) {
            for (Entry& e : section.toAdd) section.listeners.push_back(e);
            section.toAdd.clear();
        }
        return applicable;
    }

    void GameEventDispatcher::PruneUnloadedSections() {
        for (auto it = m_sections.begin(); it != m_sections.end();) {
            Section& section = it->second;
            if (section.processing) { ++it; continue; }
            // SectionPos.asLong unpacked: x in the top 22 bits, z the next 22.
            const int sx = static_cast<int>(it->first >> 42);
            const int sz = static_cast<int>((it->first << 22) >> 42);
            if (!m_level.IsChunkLoaded(sx, sz)) {
                section.listeners.erase(std::remove_if(section.listeners.begin(), section.listeners.end(),
                                                       [](const Entry& e) { return e.dynamic == nullptr; }),
                                        section.listeners.end());
            }
            if (section.listeners.empty() && section.toAdd.empty()) it = m_sections.erase(it);
            else ++it;
        }
    }

    void GameEventDispatcher::EnsureBlockListener(const glm::ivec3& pos) {
        Section& section = m_sections[SectionPosOfBlock(pos)];
        for (const Entry& e : section.listeners) {
            if (!e.dynamic && !e.removed && e.pos == pos) return;
        }
        for (const Entry& e : section.toAdd) {
            if (!e.dynamic && e.pos == pos) return;
        }
        Entry entry;
        entry.pos = pos;
        if (section.processing) section.toAdd.push_back(entry);
        else section.listeners.push_back(entry);
    }

    void GameEventDispatcher::RegisterDynamic(GameEventListener* listener, int64_t sectionKey) {
        if (!listener) return;
        Section& section = m_sections[sectionKey];
        Entry entry;
        entry.dynamic = listener;
        if (section.processing) section.toAdd.push_back(entry);
        else section.listeners.push_back(entry);
    }

    void GameEventDispatcher::UnregisterDynamic(GameEventListener* listener, int64_t sectionKey) {
        auto it = m_sections.find(sectionKey);
        if (it == m_sections.end()) return;
        Section& section = it->second;
        for (auto a = section.toAdd.begin(); a != section.toAdd.end(); ++a) {
            if (a->dynamic == listener) { section.toAdd.erase(a); break; }
        }
        for (auto e = section.listeners.begin(); e != section.listeners.end(); ++e) {
            if (e->dynamic != listener || e->removed) continue;
            if (section.processing) e->removed = true;
            else section.listeners.erase(e);
            break;
        }
        // MC OnEmptyAction: an emptied registry is dropped with its section.
        if (!section.processing && section.listeners.empty() && section.toAdd.empty()) {
            m_sections.erase(it);
        }
    }

    // ── DynamicGameEventListener ────────────────────────────────────────────

    void DynamicGameEventListener::Move(GameEventDispatcher* dispatcher) {
        if (!dispatcher) {
            Remove();
            return;
        }
        const std::optional<glm::dvec3> pos = m_listener.GetListenerSource().GetPosition();
        if (!pos) return;
        const int64_t section = SectionPosOfBlock(BlockContaining(*pos));
        if (m_registered && m_registeredRaw == dispatcher && !m_registeredWith.expired() &&
            m_lastSection == section) {
            return;
        }
        Remove();
        dispatcher->RegisterDynamic(&m_listener, section);
        m_registeredWith = dispatcher->weak_from_this();
        m_registeredRaw = dispatcher;
        m_lastSection = section;
        m_registered = true;
    }

    void DynamicGameEventListener::Remove() {
        if (!m_registered) return;
        m_registered = false;
        if (auto dispatcher = m_registeredWith.lock()) {
            dispatcher->UnregisterDynamic(&m_listener, m_lastSection);
        }
        m_registeredWith.reset();
        m_registeredRaw = nullptr;
    }

} // namespace Game
