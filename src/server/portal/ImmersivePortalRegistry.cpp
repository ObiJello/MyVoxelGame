// File: src/server/portal/ImmersivePortalRegistry.cpp
#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "ImmersivePortalRegistry.hpp"

#include "server/IntegratedServer.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"
#include "common/network/packets/game/ImmersivePortalPackets.hpp"
#include "common/core/Log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace Server {

    using Game::Immersive::kInvalidPortalId;
    using Game::Immersive::PortalKind;
    using Game::Immersive::PortalShape;

    namespace {

        constexpr int kSaveVersion = 1;

        nlohmann::json Vec3Json(const glm::dvec3& v) { return { v.x, v.y, v.z }; }

        bool ReadVec3(const nlohmann::json& j, const char* key, glm::dvec3& out) {
            if (!j.contains(key) || !j[key].is_array() || j[key].size() != 3) return false;
            out = glm::dvec3(j[key][0].get<double>(), j[key][1].get<double>(), j[key][2].get<double>());
            return true;
        }

        nlohmann::json PortalToJson(const Game::Immersive::Portal& p) {
            nlohmann::json j;
            j["id"]               = p.id;
            j["kind"]             = static_cast<int>(p.kind);
            j["flags"]            = p.flags;
            j["dimension"]        = Game::DimensionToRaw(p.dimension);
            j["origin"]           = Vec3Json(p.origin);
            j["axisW"]            = Vec3Json(p.axisW);
            j["axisH"]            = Vec3Json(p.axisH);
            j["width"]            = p.width;
            j["height"]           = p.height;
            j["destDimension"]    = Game::DimensionToRaw(p.destDimension);
            j["destination"]      = Vec3Json(p.destination);
            j["rotation"]         = { p.rotation.w, p.rotation.x, p.rotation.y, p.rotation.z };
            j["scale"]            = p.scale;
            j["specificPlayerId"] = p.specificPlayerId;
            j["reverse"]          = p.reversePortalId;
            j["flipped"]          = p.flippedPortalId;
            j["parallel"]         = p.parallelPortalId;
            j["tag"]              = p.tag;
            nlohmann::json shape;
            shape["type"] = p.shape.IsRectangle() ? "rectangle" : "mesh";
            if (!p.shape.IsRectangle()) {
                nlohmann::json verts = nlohmann::json::array();
                for (const auto& v : p.shape.vertices) verts.push_back({ v.x, v.y });
                shape["vertices"] = std::move(verts);
                shape["indices"]  = p.shape.indices;
            }
            j["shape"] = std::move(shape);
            return j;
        }

        bool PortalFromJson(const nlohmann::json& j, Game::Immersive::Portal& p) {
            p.id    = j.value("id", 0u);
            p.kind  = static_cast<PortalKind>(j.value("kind", 0));
            p.flags = j.value("flags", Game::Immersive::PortalFlag::Default);
            p.dimension     = Game::DimensionFromRaw(j.value("dimension", 0));
            p.destDimension = Game::DimensionFromRaw(j.value("destDimension", 0));
            if (!ReadVec3(j, "origin", p.origin)) return false;
            if (!ReadVec3(j, "axisW", p.axisW)) return false;
            if (!ReadVec3(j, "axisH", p.axisH)) return false;
            if (!ReadVec3(j, "destination", p.destination)) return false;
            p.width  = j.value("width", 1.0);
            p.height = j.value("height", 2.0);
            if (j.contains("rotation") && j["rotation"].is_array() && j["rotation"].size() == 4) {
                p.rotation = glm::dquat(j["rotation"][0].get<double>(), j["rotation"][1].get<double>(),
                                        j["rotation"][2].get<double>(), j["rotation"][3].get<double>());
            }
            p.scale            = j.value("scale", 1.0);
            p.specificPlayerId = j.value("specificPlayerId", 0u);
            p.reversePortalId  = j.value("reverse", 0u);
            p.flippedPortalId  = j.value("flipped", 0u);
            p.parallelPortalId = j.value("parallel", 0u);
            p.tag              = j.value("tag", std::string());
            p.shape = PortalShape{};
            if (j.contains("shape") && j["shape"].is_object()) {
                const auto& s = j["shape"];
                if (s.value("type", "rectangle") == "mesh") {
                    p.shape.type = PortalShape::Type::Mesh;
                    if (s.contains("vertices") && s["vertices"].is_array()) {
                        for (const auto& v : s["vertices"]) {
                            if (v.is_array() && v.size() == 2) {
                                p.shape.vertices.emplace_back(v[0].get<float>(), v[1].get<float>());
                            }
                        }
                    }
                    if (s.contains("indices") && s["indices"].is_array()) {
                        for (const auto& i : s["indices"]) p.shape.indices.push_back(i.get<uint32_t>());
                    }
                }
            }
            return p.Orthonormalize() && p.IsValidGeometry();
        }

    } // namespace

    ImmersivePortalRegistry::ImmersivePortalRegistry(IntegratedServer& server)
        : m_server(server) {}

    // ── Keys / index ───────────────────────────────────────────────────────

    ImmersivePortalRegistry::ChunkKey ImmersivePortalRegistry::KeyOf(const Portal& p) {
        return KeyOf(p.dimension, p.OriginChunk());
    }

    ImmersivePortalRegistry::ChunkKey ImmersivePortalRegistry::KeyOf(Game::DimensionId dimension,
                                                                    Game::Math::ChunkPos chunk) {
        return ChunkKey{ static_cast<int8_t>(Game::DimensionToRaw(dimension)), chunk.x, chunk.z };
    }

    void ImmersivePortalRegistry::Index(const Portal& portal) {
        if (portal.Has(Game::Immersive::PortalFlag::Global)) return;   // chunkless
        auto& ids = m_byChunk[KeyOf(portal)];
        if (std::find(ids.begin(), ids.end(), portal.id) == ids.end()) ids.push_back(portal.id);
    }

    void ImmersivePortalRegistry::Unindex(const Portal& portal) {
        if (portal.Has(Game::Immersive::PortalFlag::Global)) return;
        auto it = m_byChunk.find(KeyOf(portal));
        if (it == m_byChunk.end()) return;
        auto& ids = it->second;
        ids.erase(std::remove(ids.begin(), ids.end(), portal.id), ids.end());
        if (ids.empty()) m_byChunk.erase(it);
    }

    // ── Mutation ───────────────────────────────────────────────────────────

    ImmersivePortalRegistry::PortalId ImmersivePortalRegistry::Add(Portal portal) {
        if (!portal.Orthonormalize() || !portal.IsValidGeometry()) {
            Log::Warning("[ImmersivePortals] Rejected portal with invalid geometry: %s",
                         portal.Describe().c_str());
            return kInvalidPortalId;
        }
        if (portal.id == kInvalidPortalId) {
            portal.id = m_nextId++;
        } else {
            // A caller-chosen id (a load) must not be handed out again.
            m_nextId = std::max(m_nextId, portal.id + 1);
        }
        auto [it, inserted] = m_portals.insert_or_assign(portal.id, std::move(portal));
        if (!inserted) {
            Log::Warning("[ImmersivePortals] Add replaced existing portal #%u", it->first);
        }
        Index(it->second);
        m_dirty = true;
        BroadcastSync(it->second);
        Log::Info("[ImmersivePortals] Added %s", it->second.Describe().c_str());
        return it->second.id;
    }

    bool ImmersivePortalRegistry::Update(const Portal& portal) {
        auto it = m_portals.find(portal.id);
        if (it == m_portals.end()) return false;
        Portal next = portal;
        if (!next.Orthonormalize() || !next.IsValidGeometry()) {
            Log::Warning("[ImmersivePortals] Rejected update with invalid geometry: %s",
                         next.Describe().c_str());
            return false;
        }
        if (it->second == next) return true;
        // Anchor chunk may move; clients that only held the old chunk must
        // be told to drop it, clients of the new chunk to add it.
        const bool moved = KeyOf(it->second) != KeyOf(next);
        if (moved) BroadcastRemove(it->second);
        Unindex(it->second);
        it->second = std::move(next);
        Index(it->second);
        m_dirty = true;
        BroadcastSync(it->second);
        return true;
    }

    bool ImmersivePortalRegistry::Remove(PortalId id) {
        auto it = m_portals.find(id);
        if (it == m_portals.end()) return false;
        BroadcastRemove(it->second);
        Unindex(it->second);
        Log::Info("[ImmersivePortals] Removed %s", it->second.Describe().c_str());
        m_portals.erase(it);
        m_dirty = true;
        return true;
    }

    size_t ImmersivePortalRegistry::RemoveCluster(PortalId id) {
        // Walk the link graph first, then remove: removing while walking
        // would drop the records the walk still has to read.
        std::unordered_set<PortalId> cluster;
        std::vector<PortalId> stack{ id };
        while (!stack.empty()) {
            const PortalId cur = stack.back();
            stack.pop_back();
            if (cur == kInvalidPortalId || cluster.count(cur)) continue;
            const Portal* p = Get(cur);
            if (!p) continue;
            cluster.insert(cur);
            stack.push_back(p->reversePortalId);
            stack.push_back(p->flippedPortalId);
            stack.push_back(p->parallelPortalId);
        }
        // Partners that point at a member but are not reachable from it
        // (a half-linked cluster from an interrupted save) go too.
        for (const auto& [otherId, other] : m_portals) {
            if (cluster.count(otherId)) continue;
            if (cluster.count(other.reversePortalId) || cluster.count(other.flippedPortalId) ||
                cluster.count(other.parallelPortalId)) {
                stack.push_back(otherId);
            }
        }
        for (PortalId extra : stack) cluster.insert(extra);

        size_t removed = 0;
        for (PortalId member : cluster) {
            if (Remove(member)) ++removed;
        }
        return removed;
    }

    ImmersivePortalRegistry::PortalId ImmersivePortalRegistry::AddBiWay(Portal front) {
        Portal back = front.MakeReverse();
        const PortalId frontId = Add(front);
        if (frontId == kInvalidPortalId) return kInvalidPortalId;
        const PortalId backId = Add(back);
        if (backId == kInvalidPortalId) {
            Remove(frontId);
            return kInvalidPortalId;
        }
        Portal f = *Get(frontId);
        Portal b = *Get(backId);
        f.reversePortalId = backId;
        b.reversePortalId = frontId;
        Update(f);
        Update(b);
        return frontId;
    }

    ImmersivePortalRegistry::PortalId ImmersivePortalRegistry::AddBiWayBiFaced(Portal front) {
        // The mod's generateBiWayBiFacedPortal: f1 (front), f2 = flipped(f1),
        // t1 = reverse(f1), t2 = flipped(t1); reverse links f1<->t1, f2<->t2;
        // flipped links f1<->f2, t1<->t2; parallel links f1<->t2, f2<->t1.
        Portal f1 = front;
        Portal f2 = f1.MakeFlipped();
        Portal t1 = f1.MakeReverse();
        Portal t2 = t1.MakeFlipped();

        const PortalId idF1 = Add(f1);
        if (idF1 == kInvalidPortalId) return kInvalidPortalId;
        const PortalId idF2 = Add(f2);
        const PortalId idT1 = Add(t1);
        const PortalId idT2 = Add(t2);
        if (idF2 == kInvalidPortalId || idT1 == kInvalidPortalId || idT2 == kInvalidPortalId) {
            for (PortalId id : { idF1, idF2, idT1, idT2 }) {
                if (id != kInvalidPortalId) Remove(id);
            }
            return kInvalidPortalId;
        }

        auto link = [&](PortalId id, PortalId reverse, PortalId flipped, PortalId parallel) {
            Portal p = *Get(id);
            p.reversePortalId  = reverse;
            p.flippedPortalId  = flipped;
            p.parallelPortalId = parallel;
            Update(p);
        };
        link(idF1, idT1, idF2, idT2);
        link(idF2, idT2, idF1, idT1);
        link(idT1, idF1, idT2, idF2);
        link(idT2, idF2, idT1, idF1);
        return idF1;
    }

    // ── Query ──────────────────────────────────────────────────────────────

    const ImmersivePortalRegistry::Portal* ImmersivePortalRegistry::Get(PortalId id) const {
        auto it = m_portals.find(id);
        return it == m_portals.end() ? nullptr : &it->second;
    }

    void ImmersivePortalRegistry::ForEach(const std::function<void(const Portal&)>& fn) const {
        for (const auto& [id, portal] : m_portals) fn(portal);
    }

    void ImmersivePortalRegistry::ForEachInDimension(
            Game::DimensionId dimension, const std::function<void(const Portal&)>& fn) const {
        for (const auto& [id, portal] : m_portals) {
            if (portal.dimension == dimension) fn(portal);
        }
    }

    void ImmersivePortalRegistry::ForEachInChunk(
            Game::DimensionId dimension, Game::Math::ChunkPos chunk,
            const std::function<void(const Portal&)>& fn) const {
        auto it = m_byChunk.find(KeyOf(dimension, chunk));
        if (it == m_byChunk.end()) return;
        for (PortalId id : it->second) {
            auto p = m_portals.find(id);
            if (p != m_portals.end()) fn(p->second);
        }
    }

    std::vector<const ImmersivePortalRegistry::Portal*> ImmersivePortalRegistry::CollectNear(
            Game::DimensionId dimension, const glm::dvec3& pos, double radius) const {
        std::vector<const Portal*> out;
        // The surface's box padded by `radius` in EVERY axis —
        // BoundingBox's own parameter pads along the normal only, which is
        // right for a crossing test and wrong here: a frame block beside
        // the surface, or a player standing off to its side, is "near" too.
        auto within = [&](const Portal& p) {
            glm::dvec3 mn, mx;
            p.BoundingBox(mn, mx, 0.0);
            mn -= glm::dvec3(radius);
            mx += glm::dvec3(radius);
            return pos.x >= mn.x && pos.x <= mx.x && pos.y >= mn.y && pos.y <= mx.y &&
                   pos.z >= mn.z && pos.z <= mx.z;
        };

        // A portal's origin chunk is where it is indexed, but its surface may
        // reach into neighbours; widen the chunk walk by the largest extent a
        // portal here has, plus the query radius.
        //
        // Global surfaces are chunkless (never indexed) and kilometres wide:
        // they are answered by this linear pass instead, and they must NOT
        // widen the chunk walk — a 200,000-block stack seam made `cr` twelve
        // thousand chunks, and the walk below visited every one of the 625
        // million keys per call, once per player per watch pass.
        double reach = radius;
        for (const auto& [id, portal] : m_portals) {
            if (portal.Has(Game::Immersive::PortalFlag::Global)) {
                if (portal.dimension == dimension && within(portal)) out.push_back(&portal);
                continue;
            }
            reach = std::max(reach, radius + std::max(portal.width, portal.height));
        }
        if (m_byChunk.empty()) return out;
        const int cr = static_cast<int>(std::ceil(reach / 16.0));
        const int cx = static_cast<int>(std::floor(pos.x)) >> 4;
        const int cz = static_cast<int>(std::floor(pos.z)) >> 4;
        for (int dz = -cr; dz <= cr; ++dz) {
            for (int dx = -cr; dx <= cr; ++dx) {
                auto it = m_byChunk.find(KeyOf(dimension, Game::Math::ChunkPos{ cx + dx, cz + dz }));
                if (it == m_byChunk.end()) continue;
                for (PortalId id : it->second) {
                    auto p = m_portals.find(id);
                    if (p == m_portals.end()) continue;
                    if (within(p->second)) out.push_back(&p->second);
                }
            }
        }
        return out;
    }

    // ── Client sync ────────────────────────────────────────────────────────

    void ImmersivePortalRegistry::SyncChunkToClient(ServerConnection& connection,
                                                    Game::DimensionId dimension,
                                                    Game::Math::ChunkPos chunk) const {
        ForEachInChunk(dimension, chunk, [&](const Portal& portal) {
            Network::ImmersivePortalSyncS2CPacket packet;
            packet.portal = portal;
            connection.SendPacketIn(dimension,
                                    static_cast<uint8_t>(Network::PacketId::ImmersivePortalSyncS2C),
                                    Network::Serialization::Serialize(packet));
        });
    }

    void ImmersivePortalRegistry::SyncGlobalsToClient(ServerConnection& connection,
                                                      Game::DimensionId dimension) const {
        ForEachInDimension(dimension, [&](const Portal& portal) {
            if (!portal.Has(Game::Immersive::PortalFlag::Global)) return;
            Network::ImmersivePortalSyncS2CPacket packet;
            packet.portal = portal;
            connection.SendPacketIn(dimension,
                                    static_cast<uint8_t>(Network::PacketId::ImmersivePortalSyncS2C),
                                    Network::Serialization::Serialize(packet));
        });
    }

    void ImmersivePortalRegistry::BroadcastSync(const Portal& portal) const {
        auto* sessions = m_server.GetSessionManager();
        if (!sessions) return;
        Network::ImmersivePortalSyncS2CPacket packet;
        packet.portal = portal;
        const auto data = Network::Serialization::Serialize(packet);
        if (portal.Has(Game::Immersive::PortalFlag::Global)) {
            // Every client holding the dimension, whatever chunks it has.
            for (const auto& session : sessions->GetAllSessions()) {
                if (!session || !session->LoadsDimension(portal.dimension)) continue;
                if (auto* conn = session->GetConnection()) {
                    conn->SendPacketIn(portal.dimension,
                                       static_cast<uint8_t>(Network::PacketId::ImmersivePortalSyncS2C), data);
                }
            }
            return;
        }
        const auto chunk = portal.OriginChunk();
        sessions->ForEachSessionWatching(portal.dimension, chunk, [&](PlayerSession& session) {
            // Watching is not holding: a session whose tracking view covers
            // the chunk but has not received it yet gets the portal with the
            // chunk (SyncChunkToClient), not now — otherwise a client would
            // carry a portal for terrain it never got and never unloads.
            if (!session.HasSentChunk(portal.dimension, chunk)) return;
            if (auto* conn = session.GetConnection()) {
                conn->SendPacketIn(portal.dimension,
                                   static_cast<uint8_t>(Network::PacketId::ImmersivePortalSyncS2C), data);
            }
        });
    }

    void ImmersivePortalRegistry::BroadcastRemove(const Portal& portal) const {
        auto* sessions = m_server.GetSessionManager();
        if (!sessions) return;
        Network::ImmersivePortalRemoveS2CPacket packet;
        packet.portalId = portal.id;
        const auto data = Network::Serialization::Serialize(packet);
        if (portal.Has(Game::Immersive::PortalFlag::Global)) {
            for (const auto& session : sessions->GetAllSessions()) {
                if (!session) continue;
                if (auto* conn = session->GetConnection()) {
                    conn->SendPacketIn(portal.dimension,
                                       static_cast<uint8_t>(Network::PacketId::ImmersivePortalRemoveS2C), data);
                }
            }
            return;
        }
        const auto chunk = portal.OriginChunk();
        // Every client that HOLDS the chunk, not only the ones watching it:
        // a level a player left through a portal stays resident on their
        // client (keepPrevious) with its chunks unwatched, and a portal
        // removed there — the pair a gun clears from the other dimension —
        // would otherwise live on in that client's copy.
        for (const auto& session : sessions->GetAllSessions()) {
            if (!session || !session->HasSentChunk(portal.dimension, chunk)) continue;
            if (auto* conn = session->GetConnection()) {
                conn->SendPacketIn(portal.dimension,
                                   static_cast<uint8_t>(Network::PacketId::ImmersivePortalRemoveS2C), data);
            }
        }
    }

    // ── Persistence ────────────────────────────────────────────────────────

    std::string ImmersivePortalRegistry::SavePath() const {
        ServerLevel* overworld = m_server.GetLevel(Game::DimensionId::Overworld);
        if (!overworld) return {};
        const std::string& root = overworld->Config().savePath;
        if (root.empty()) return {};
        return (std::filesystem::path(root) / "data" / "immersive_portals.json").string();
    }

    bool ImmersivePortalRegistry::Load() {
        const std::string path = SavePath();
        if (path.empty()) return false;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return false;
        try {
            std::ifstream f(path);
            nlohmann::json j;
            f >> j;
            const int version = j.value("version", 0);
            if (version > kSaveVersion) {
                Log::Warning("[ImmersivePortals] %s is version %d, newer than this build's %d; "
                             "loading anyway", path.c_str(), version, kSaveVersion);
            }
            size_t loaded = 0, rejected = 0;
            if (j.contains("portals") && j["portals"].is_array()) {
                for (const auto& pj : j["portals"]) {
                    Portal p;
                    if (!PortalFromJson(pj, p) || p.id == kInvalidPortalId) { ++rejected; continue; }
                    m_nextId = std::max(m_nextId, p.id + 1);
                    auto [it, inserted] = m_portals.insert_or_assign(p.id, std::move(p));
                    Index(it->second);
                    ++loaded;
                }
            }
            m_nextId = std::max(m_nextId, static_cast<PortalId>(j.value("nextId", 1u)));
            m_dirty = false;
            Log::Info("[ImmersivePortals] Loaded %zu portal(s) from %s%s", loaded, path.c_str(),
                      rejected ? " (some entries were invalid and skipped)" : "");
            return true;
        } catch (const std::exception& e) {
            Log::Warning("[ImmersivePortals] Could not read %s: %s", path.c_str(), e.what());
            return false;
        }
    }

    bool ImmersivePortalRegistry::Save() {
        if (!m_dirty) return true;
        const std::string path = SavePath();
        if (path.empty()) return false;
        ServerLevel* overworld = m_server.GetLevel(Game::DimensionId::Overworld);
        if (overworld && overworld->Config().readOnly) return false;
        try {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
            nlohmann::json j;
            j["version"] = kSaveVersion;
            j["nextId"]  = m_nextId;
            nlohmann::json portals = nlohmann::json::array();
            // Sorted by id so the file diffs cleanly between saves.
            std::vector<const Portal*> ordered;
            ordered.reserve(m_portals.size());
            for (const auto& [id, portal] : m_portals) {
                // Global surfaces come from the world options, not the file.
                if (portal.Has(Game::Immersive::PortalFlag::Global)) continue;
                ordered.push_back(&portal);
            }
            std::sort(ordered.begin(), ordered.end(),
                      [](const Portal* a, const Portal* b) { return a->id < b->id; });
            for (const Portal* p : ordered) portals.push_back(PortalToJson(*p));
            j["portals"] = std::move(portals);

            // Write-then-rename so a crash mid-write leaves the old file.
            const std::string tmp = path + ".tmp";
            {
                std::ofstream f(tmp, std::ios::trunc);
                if (!f) {
                    Log::Warning("[ImmersivePortals] Could not open %s for writing", tmp.c_str());
                    return false;
                }
                f << j.dump(2);
            }
            std::filesystem::rename(tmp, path, ec);
            if (ec) {
                Log::Warning("[ImmersivePortals] Could not move %s into place: %s",
                             tmp.c_str(), ec.message().c_str());
                return false;
            }
            m_dirty = false;
            Log::Info("[ImmersivePortals] Saved %zu portal(s) to %s", m_portals.size(), path.c_str());
            return true;
        } catch (const std::exception& e) {
            Log::Warning("[ImmersivePortals] Could not write %s: %s", path.c_str(), e.what());
            return false;
        }
    }

} // namespace Server

#endif // ENABLE_IMMERSIVE_PORTALS
