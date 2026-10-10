// File: src/client/input/VeinMineClient.cpp
// See the header.

#include "VeinMineClient.hpp"

#include "Input.hpp"
#include "KeyMapping.hpp"
#include "client/renderer/debug/Gizmos.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/Item.hpp"
#include "common/network/packets/game/BlockActionC2SPacket.hpp"
#include "common/world/chunk/Chunk.hpp"   // IVec3Hash
#include "platform/GameDirectory.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <utility>
#include <unordered_set>
#include <vector>

namespace Client::VeinMineClient {

    namespace {

        using Game::VeinMine::Settings;
        using Game::VeinMine::Shape;

        // options.txt keys.
        constexpr const char* kShapeKey  = "veinMineShape";
        constexpr const char* kWidthKey  = "veinMineWidth";
        constexpr const char* kHeightKey = "veinMineHeight";
        constexpr const char* kLengthKey = "veinMineLength";

        Settings& Mutable() {
            static Settings s_settings = [] {
                const Settings defaults;
                Settings s;
                auto& opts = Platform::g_gameSettings;
                s.shape  = static_cast<Shape>(opts.GetInt(kShapeKey, static_cast<int>(defaults.shape)));
                s.width  = static_cast<uint8_t>(std::clamp(opts.GetInt(kWidthKey, defaults.width),
                                                           Game::VeinMine::kMinSize, Game::VeinMine::kMaxSize));
                s.height = static_cast<uint8_t>(std::clamp(opts.GetInt(kHeightKey, defaults.height),
                                                           Game::VeinMine::kMinSize, Game::VeinMine::kMaxSize));
                s.length = static_cast<uint8_t>(std::clamp(opts.GetInt(kLengthKey, defaults.length),
                                                           Game::VeinMine::kMinSize, Game::VeinMine::kMaxLength));
                return s.Clamped();
            }();
            return s_settings;
        }

        void Persist() {
            const Settings& s = Mutable();
            auto& opts = Platform::g_gameSettings;
            opts.SetInt(kShapeKey,  static_cast<int>(s.shape));
            opts.SetInt(kWidthKey,  s.width);
            opts.SetInt(kHeightKey, s.height);
            opts.SetInt(kLengthKey, s.length);
            opts.Save();
        }

        // Steps one size by `delta` in [lo, hi]; true when it moved.
        bool Step(uint8_t& value, int delta, int lo, int hi) {
            const int next = std::clamp(static_cast<int>(value) + delta, lo, hi);
            if (next == value) return false;
            value = static_cast<uint8_t>(next);
            return true;
        }

        bool s_shapeKeyWasDown = false;
        bool s_shapeKeyUsed    = false;   // a size key or the wheel was used while it was held
        bool s_keysWereHeld    = false;

        // The highlight's last answer, kept while nothing it depends on
        // changes and refreshed a few times a second for the world changing
        // under it (another player digging, water flowing in).
        struct HighlightCache {
            bool                                  valid = false;
            glm::ivec3                            origin{0};
            Game::BlockID                         kind = Game::BlockID::Air;
            int                                   face = -1;
            Game::Direction                       facing = Game::Direction::North;
            Settings                              settings;
            bool                                  creative = false;
            Game::ItemID                          tool{};
            std::chrono::steady_clock::time_point at{};
            std::vector<glm::ivec3>               cells;   // origin + targets
            std::unordered_set<glm::ivec3, Game::IVec3Hash> lookup;
            // What is drawn, built once per refresh (BuildOutline).
            std::vector<std::pair<glm::dvec3, glm::dvec3>> edges;
            std::vector<std::pair<glm::ivec3, Game::Direction>> faces;
        };
        HighlightCache s_highlight;
        constexpr auto kHighlightRefresh = std::chrono::milliseconds(100);

        bool SameSettings(const Settings& a, const Settings& b) {
            return a.shape == b.shape && a.width == b.width && a.height == b.height && a.length == b.length;
        }

        // A cool light blue — reads against stone, dirt, ores and the sky
        // alike, and is nothing else's colour (the hit outline is black, the
        // glowing effect white / team colours).
        constexpr uint32_t kOutline = 0xE055CCFFu;
        constexpr uint32_t kFill    = 0x3855CCFFu;

        constexpr Game::Direction kDirs[6] = {
            Game::Direction::Down, Game::Direction::Up, Game::Direction::North,
            Game::Direction::South, Game::Direction::West, Game::Direction::East,
        };

        // The set's outline as ONE shape: of every cell edge only the creases
        // of the union's surface — where it turns a corner (one or three of
        // the four cells around the edge taken) or two cells meet only along
        // the edge (diagonal pair). The seams inside a flat face (two side by
        // side) and edges buried in the set (all four) are dropped, and the
        // creases left along one grid line are merged into one line. The
        // faces the set shows to everything else carry the tint.
        void BuildOutline(HighlightCache& c) {
            c.edges.clear();
            c.faces.clear();
            const auto taken = [&](const glm::ivec3& p) { return c.lookup.count(p) != 0; };

            // Edge along axis a from grid point p to p + e_a, keyed on p.
            std::unordered_set<glm::ivec3, Game::IVec3Hash> visited[3];
            std::vector<glm::ivec3> creases[3];
            for (const glm::ivec3& cell : c.cells) {
                for (int a = 0; a < 3; ++a) {
                    const int b = (a + 1) % 3, d = (a + 2) % 3;
                    for (int ob = 0; ob <= 1; ++ob)
                    for (int od = 0; od <= 1; ++od) {
                        glm::ivec3 p = cell;
                        p[b] += ob;
                        p[d] += od;
                        if (!visited[a].insert(p).second) continue;
                        bool occ[2][2];
                        int n = 0;
                        for (int i = 0; i <= 1; ++i)
                        for (int j = 0; j <= 1; ++j) {
                            glm::ivec3 q = p;
                            q[b] += i - 1;
                            q[d] += j - 1;
                            occ[i][j] = taken(q);
                            n += occ[i][j] ? 1 : 0;
                        }
                        const bool crease = n == 1 || n == 3 || (n == 2 && occ[0][0] == occ[1][1]);
                        if (crease) creases[a].push_back(p);
                    }
                }
            }
            for (int a = 0; a < 3; ++a) {
                const int b = (a + 1) % 3, d = (a + 2) % 3;
                auto& list = creases[a];
                std::sort(list.begin(), list.end(), [&](const glm::ivec3& l, const glm::ivec3& r) {
                    if (l[b] != r[b]) return l[b] < r[b];
                    if (l[d] != r[d]) return l[d] < r[d];
                    return l[a] < r[a];
                });
                for (size_t i = 0; i < list.size();) {
                    size_t j = i + 1;
                    while (j < list.size() && list[j][b] == list[i][b] && list[j][d] == list[i][d] &&
                           list[j][a] == list[j - 1][a] + 1) {
                        ++j;
                    }
                    glm::dvec3 from(list[i]);
                    glm::dvec3 to(list[j - 1]);
                    to[a] += 1.0;
                    c.edges.emplace_back(from, to);
                    i = j;
                }
            }

            for (const glm::ivec3& cell : c.cells) {
                for (Game::Direction dir : kDirs) {
                    const glm::ivec3 n = cell + glm::ivec3(Game::StepX(dir), Game::StepY(dir), Game::StepZ(dir));
                    if (!taken(n)) c.faces.emplace_back(cell, dir);
                }
            }
        }

    } // namespace

    const Game::VeinMine::Settings& Current() { return Mutable(); }

    bool KeysHeld() {
        return Input::Binds::Sneak && Input::Binds::VeinMine &&
               Input::IsDown(*Input::Binds::Sneak) && Input::IsDown(*Input::Binds::VeinMine);
    }

    bool ResizeKeysActive() {
        const bool shapeKey = Input::Binds::VeinMineShape && Input::IsDown(*Input::Binds::VeinMineShape);
        return shapeKey || (KeysHeld() && Game::VeinMine::IsSized(Mutable().shape));
    }

    bool HandleKeys() {
        using namespace Input;
        if (!Binds::VeinMineShape) return false;
        // A screen took the keyboard: its opening releases every key, which
        // is not a tap of this one.
        if (IsUiActive()) {
            s_shapeKeyWasDown = false;
            return false;
        }
        // Held state only: a queued click of the shape key means nothing.
        while (ConsumeClick(*Binds::VeinMineShape)) {}

        Settings& s = Mutable();
        bool changed = false;
        const bool down = IsDown(*Binds::VeinMineShape);
        if (down && !s_shapeKeyWasDown) s_shapeKeyUsed = false;

        // The size keys act while the shape key is held, or while Sneak +
        // Vein Mine are (no shape key needed mid-dig); otherwise their
        // presses are dropped so none is waiting when it next goes down.
        const auto clicks = [](KeyMapping* key) {
            int n = 0;
            if (key) while (ConsumeClick(*key)) ++n;
            return n;
        };
        const int taller   = clicks(Binds::VeinMineTaller);
        const int shorter  = clicks(Binds::VeinMineShorter);
        const int wider    = clicks(Binds::VeinMineWider);
        const int narrower = clicks(Binds::VeinMineNarrower);
        if (down || KeysHeld()) {
            const int dh = taller - shorter;
            const int dw = wider - narrower;
            if (down && (taller || shorter || wider || narrower)) s_shapeKeyUsed = true;
            if (Game::VeinMine::IsSized(s.shape)) {
                changed |= Step(s.height, dh, Game::VeinMine::kMinSize, Game::VeinMine::kMaxSize);
                changed |= Step(s.width,  dw, Game::VeinMine::kMinSize, Game::VeinMine::kMaxSize);
            }
        }
        // A tap — down and up with nothing resized meanwhile — cycles.
        if (!down && s_shapeKeyWasDown && !s_shapeKeyUsed) {
            s.shape = Game::VeinMine::NextShape(s.shape);
            changed = true;
        }
        s_shapeKeyWasDown = down;
        if (changed) {
            Persist();
            s_highlight.valid = false;
        }
        return changed;
    }

    bool OnScroll(int wheel) {
        s_shapeKeyUsed = true;
        Settings& s = Mutable();
        if (!Game::VeinMine::IsSized(s.shape) || wheel == 0) return false;
        // Wheel up (away) = longer, as the hotbar's "previous" is up.
        if (!Step(s.length, wheel > 0 ? 1 : -1, Game::VeinMine::kMinSize, Game::VeinMine::kMaxLength)) return false;
        Persist();
        s_highlight.valid = false;
        return true;
    }

    std::string StatusText() {
        const Settings& s = Mutable();
        std::string text = "Vein Mine: ";
        text += Game::VeinMine::ShapeName(s.shape);
        if (Game::VeinMine::IsSized(s.shape)) {
            text += "  " + std::to_string(s.width) + " wide x " + std::to_string(s.height) + " tall, " +
                    std::to_string(s.length) + " long";
        }
        return text;
    }

    bool TakeReleaseEdge() {
        const bool held = KeysHeld();
        const bool released = s_keysWereHeld && !held;
        s_keysWereHeld = held;
        return released;
    }

    Game::Direction FacingFor(const Game::RaycastHit& hit) {
        const glm::vec3& d = hit.rayDirection;
        if (glm::dot(d, d) < 0.25f) return Game::Direction::North;
        return Game::FromYRot(Game::Mth::YRotFromVector(d));
    }

    void FillPacket(Network::BlockActionC2SPacket& packet, Game::Direction facing) {
        const Settings& s = Mutable();
        packet.veinShape  = static_cast<uint8_t>(s.shape);
        packet.veinWidth  = s.width;
        packet.veinHeight = s.height;
        packet.veinLength = s.length;
        packet.veinFacing = static_cast<uint8_t>(facing);
    }

    void QueueHighlight(const Game::RaycastHit& hit, const Game::ItemStack* harvestTool) {
        if (!KeysHeld() || !g_clientBlockAccess) return;
        const Game::BlockID kind = hit.blockId;
        if (kind == Game::BlockID::Air) return;

        const Settings& s = Mutable();
        const Game::Direction facing = FacingFor(hit);
        const auto now = std::chrono::steady_clock::now();
        HighlightCache& c = s_highlight;
        const Game::ItemID toolId = harvestTool ? harvestTool->itemId : Game::ItemID{};
        const bool stale = !c.valid || c.origin != hit.blockPos || c.kind != kind || c.face != hit.hitFace ||
                           c.facing != facing || !SameSettings(c.settings, s) ||
                           c.creative != (harvestTool == nullptr) || c.tool != toolId ||
                           now - c.at >= kHighlightRefresh;
        if (stale) {
            const auto targets = Game::VeinMine::CollectTargets(*g_clientBlockAccess, hit.blockPos, kind,
                                                                hit.hitFace, facing, s, harvestTool);
            c.creative  = harvestTool == nullptr;
            c.tool      = toolId;
            c.valid     = true;
            c.origin    = hit.blockPos;
            c.kind      = kind;
            c.face      = hit.hitFace;
            c.facing    = facing;
            c.settings  = s;
            c.at        = now;
            // Nothing taken (the held item cannot harvest the block, or the
            // shape finds nothing): no highlight — the dig is a plain one.
            std::vector<glm::ivec3> cells;
            cells.reserve(targets.size() + 1);
            if (!targets.empty()) cells.push_back(hit.blockPos);
            for (const auto& t : targets) cells.push_back(t.pos);
            // The timed refresh mostly finds the same set (a full-size
            // shape is 14,400 blocks): the outline is only rebuilt when it
            // changed.
            if (cells != c.cells) {
                c.cells = std::move(cells);
                c.lookup.clear();
                c.lookup.insert(c.cells.begin(), c.cells.end());
                BuildOutline(c);
            }
        }

        // The outline on top of everything (an ore vein is mostly buried —
        // the outline is how you see what it reaches), and the tint on the
        // outer faces, nudged off the block face along its normal only so
        // neighbouring faces meet without overlapping.
        for (const auto& [from, to] : c.edges) {
            Render::Gizmos::Line(from, to, kOutline, 2.5f, /*alwaysOnTop=*/true);
        }
        constexpr double kInflate = 0.002;
        for (const auto& [cell, dir] : c.faces) {
            const glm::dvec3 normal(std::abs(Game::StepX(dir)), std::abs(Game::StepY(dir)), std::abs(Game::StepZ(dir)));
            const glm::dvec3 min = glm::dvec3(cell) - normal * kInflate;
            const glm::dvec3 max = glm::dvec3(cell) + glm::dvec3(1.0) + normal * kInflate;
            Render::Gizmos::Rect(min, max, dir, kFill);
        }
    }

} // namespace Client::VeinMineClient
