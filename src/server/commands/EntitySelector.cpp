// File: src/server/commands/EntitySelector.cpp
#include "EntitySelector.hpp"

#include "../IntegratedServer.hpp"
#include "../entity/ItemEntityManager.hpp"
#include "../entity/MobManager.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EntityType.hpp"
#include "common/entity/ItemEntity.hpp"
#include "common/entity/Mob.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <random>

namespace Server {

    namespace {

        // ── MinMaxBounds ────────────────────────────────────────────────────
        //
        // MC's range syntax, used by `distance`, `x_rotation`, `y_rotation` and
        // `level`: "3" (exactly), "3.." (at least), "..7" (at most), "3..7".
        struct Bounds {
            std::optional<double> min, max;

            bool Any() const { return !min && !max; }

            // MC MinMaxBounds.Doubles.matchesSqr — the bounds are squared, not
            // the value square-rooted, so `distance=..64` stays exact and cheap.
            bool MatchesSq(double vSq) const {
                if (min && vSq < *min * *min) return false;
                if (max && vSq > *max * *max) return false;
                return true;
            }
        };

        bool ParseDouble(const std::string& s, double& out) {
            if (s.empty()) return false;
            try {
                size_t used = 0;
                out = std::stod(s, &used);
                return used == s.size();
            } catch (...) { return false; }
        }

        bool ParseBounds(const std::string& raw, Bounds& out, std::string& error) {
            const size_t dots = raw.find("..");
            if (dots == std::string::npos) {
                double v;
                if (!ParseDouble(raw, v)) { error = "Invalid range: " + raw; return false; }
                out.min = v;
                out.max = v;
                return true;
            }

            const std::string lo = raw.substr(0, dots);
            const std::string hi = raw.substr(dots + 2);
            if (lo.empty() && hi.empty()) { error = "Invalid range: " + raw; return false; }

            if (!lo.empty()) {
                double v;
                if (!ParseDouble(lo, v)) { error = "Invalid range: " + raw; return false; }
                out.min = v;
            }
            if (!hi.empty()) {
                double v;
                if (!ParseDouble(hi, v)) { error = "Invalid range: " + raw; return false; }
                out.max = v;
            }
            // MC MinMaxBounds.Bounds.areSwapped.
            if (out.min && out.max && *out.min > *out.max) {
                error = "Min cannot be bigger than max in range: " + raw;
                return false;
            }
            return true;
        }

        // ── Parsed selector state (MC EntitySelectorParser's fields) ────────

        enum class Order : uint8_t { Arbitrary, Nearest, Furthest, Random };

        using Predicate = std::function<bool(const SelectedEntity&)>;

        struct ParsedSelector {
            int   maxResults       = 1;
            bool  includesEntities = false;
            bool  currentEntity    = false;
            Order order            = Order::Arbitrary;

            // MC's `canUse` guards: each of these options may appear once, and
            // `sort` is inapplicable to @s at all.
            bool sorted      = false;
            bool limitSeen   = false;
            bool distanceSeen= false;
            bool rotXSeen    = false;
            bool rotYSeen    = false;
            bool xSeen = false, ySeen = false, zSeen = false;
            bool dxSeen = false, dySeen = false, dzSeen = false;
            bool nameSeen = false;
            // MC tracks the two gamemode forms separately: repeated
            // NEGATIVE tests are legal (`gamemode=!creative,gamemode=!spectator`),
            // a second positive one is not.
            bool gamemodeEquals = false, gamemodeNotEquals = false;
            bool typeLimited = false, typeLimitedInversely = false;

            std::optional<double> x, y, z, dx, dy, dz;
            Bounds distance, rotX, rotY;

            std::vector<Predicate> predicates;
        };

        std::string ToLower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        bool IEquals(const std::string& a, const std::string& b) {
            return a.size() == b.size() && ToLower(a) == ToLower(b);
        }

        // Strip the vanilla namespace so a command copied from the wiki works.
        std::string StripNamespace(const std::string& s) {
            return s.rfind("minecraft:", 0) == 0 ? s.substr(10) : s;
        }

        // ── Option splitting ────────────────────────────────────────────────
        //
        // `key=value` pairs separated by commas. Values never contain a comma
        // in the options this engine supports (MC's `nbt`, `scores` and
        // `advancements` take brace/bracket blobs and are rejected outright
        // below), so a flat split is exact here rather than merely convenient.
        bool SplitOptions(const std::string& body,
                          std::vector<std::pair<std::string, std::string>>& out,
                          std::string& error) {
            size_t i = 0;
            while (i < body.size()) {
                const size_t comma = body.find(',', i);
                const std::string pair = body.substr(i, comma == std::string::npos
                                                          ? std::string::npos : comma - i);
                i = (comma == std::string::npos) ? body.size() : comma + 1;

                if (pair.empty()) continue;

                const size_t eq = pair.find('=');
                if (eq == std::string::npos) {
                    error = "Expected key=value in selector: " + pair;
                    return false;
                }

                std::string key   = pair.substr(0, eq);
                std::string value = pair.substr(eq + 1);

                // Trim spaces — MC's reader skips whitespace around both.
                const auto trim = [](std::string& s) {
                    const size_t b = s.find_first_not_of(" \t");
                    const size_t e = s.find_last_not_of(" \t");
                    s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
                };
                trim(key);
                trim(value);

                out.emplace_back(std::move(key), std::move(value));
            }
            return true;
        }

        // MC EntitySelectorOptions options that exist in vanilla but have no
        // meaning here. Named explicitly so a player gets "not supported" rather
        // than "unknown option", which is the difference between "this build
        // can't do that" and "you made a typo".
        bool IsKnownUnsupportedOption(const std::string& key) {
            return key == "tag" || key == "team" || key == "scores" ||
                   key == "advancements" || key == "predicate" || key == "nbt" ||
                   key == "level";
        }

        bool ApplyOption(ParsedSelector& sel, const std::string& key,
                         const std::string& rawValue, std::string& error) {
            // MC shouldInvertValue: a leading '!' negates the whole test.
            bool inverted = false;
            std::string value = rawValue;
            if (!value.empty() && value[0] == '!') {
                inverted = true;
                value = value.substr(1);
            }

            const auto once = [&](bool& flag) {
                if (flag) { error = "Option '" + key + "' can only be used once"; return false; }
                flag = true;
                return true;
            };

            if (key == "x" || key == "y" || key == "z" ||
                key == "dx" || key == "dy" || key == "dz") {
                double v;
                if (!ParseDouble(value, v)) { error = "Invalid " + key + ": " + rawValue; return false; }
                if (key == "x")  { if (!once(sel.xSeen))  return false; sel.x  = v; }
                if (key == "y")  { if (!once(sel.ySeen))  return false; sel.y  = v; }
                if (key == "z")  { if (!once(sel.zSeen))  return false; sel.z  = v; }
                if (key == "dx") { if (!once(sel.dxSeen)) return false; sel.dx = v; }
                if (key == "dy") { if (!once(sel.dySeen)) return false; sel.dy = v; }
                if (key == "dz") { if (!once(sel.dzSeen)) return false; sel.dz = v; }
                return true;
            }

            if (key == "distance") {
                if (!once(sel.distanceSeen)) return false;
                if (!ParseBounds(value, sel.distance, error)) return false;
                // MC ERROR_RANGE_NEGATIVE.
                if ((sel.distance.min && *sel.distance.min < 0.0) ||
                    (sel.distance.max && *sel.distance.max < 0.0)) {
                    error = "Distance cannot be negative";
                    return false;
                }
                return true;
            }

            if (key == "x_rotation" || key == "y_rotation") {
                bool& seen = (key == "x_rotation") ? sel.rotXSeen : sel.rotYSeen;
                if (!once(seen)) return false;
                Bounds& b = (key == "x_rotation") ? sel.rotX : sel.rotY;
                return ParseBounds(value, b, error);
            }

            if (key == "limit") {
                if (!once(sel.limitSeen)) return false;
                // MC forbids `limit` on @s (ERROR_LIMIT_TOO_SMALL guards the
                // value; the canUse guard forbids the option itself).
                if (sel.currentEntity) {
                    error = "Option 'limit' is not applicable to @s";
                    return false;
                }
                double v;
                if (!ParseDouble(value, v) || v != std::floor(v)) {
                    error = "Invalid limit: " + rawValue;
                    return false;
                }
                if (v < 1.0) { error = "Limit must be at least 1"; return false; }
                sel.maxResults = static_cast<int>(v);
                return true;
            }

            if (key == "sort") {
                // MC: `!s.isCurrentEntity() && !s.isSorted()`.
                if (sel.currentEntity) {
                    error = "Option 'sort' is not applicable to @s";
                    return false;
                }
                if (!once(sel.sorted)) return false;
                const std::string v = ToLower(value);
                if      (v == "nearest")   sel.order = Order::Nearest;
                else if (v == "furthest")  sel.order = Order::Furthest;
                else if (v == "random")    sel.order = Order::Random;
                else if (v == "arbitrary") sel.order = Order::Arbitrary;
                else { error = "Invalid or unknown sort type: " + value; return false; }
                return true;
            }

            if (key == "type") {
                // MC's canUse guard is `!isTypeLimited()`, and @a/@p/@r set
                // that at parse time by pinning themselves to PLAYER. So
                // `@a[type=zombie]` is an INAPPLICABLE-OPTION error in vanilla,
                // not a selector that quietly matches nothing.
                if (sel.typeLimited) {
                    error = "Option 'type' isn't applicable here";
                    return false;
                }
                if (!value.empty() && value[0] == '#') {
                    error = "Entity type tags are not supported: " + rawValue;
                    return false;
                }
                // MC's second guard, inside the modifier: a positive type may
                // not follow a negative one (`type=!zombie,type=cow`).
                if (sel.typeLimitedInversely && !inverted) {
                    error = "Option 'type' isn't applicable here";
                    return false;
                }
                const std::string slug = ToLower(StripNamespace(value));

                // Validate against the types this engine knows, plus the two
                // that are not in EntityTypeId: players and dropped items.
                bool known = (slug == "player" || slug == "item");
                if (!known) {
                    for (uint16_t i = 0; i < static_cast<uint16_t>(Game::EntityTypeId::Count); ++i) {
                        if (Game::GetEntityTypeInfo(static_cast<Game::EntityTypeId>(i)).slug == slug) {
                            known = true;
                            break;
                        }
                    }
                }
                if (!known) { error = "Unknown entity type: " + value; return false; }

                if (inverted) {
                    sel.typeLimitedInversely = true;
                } else {
                    sel.typeLimited = true;
                    // MC: a positive non-player `type=` implies entities are in
                    // scope even for a selector that defaults to players only,
                    // and a positive `type=player` narrows @e back to players.
                    sel.includesEntities = (slug != "player");
                }
                sel.predicates.push_back([slug, inverted](const SelectedEntity& e) {
                    return (e.typeSlug == slug) != inverted;
                });
                return true;
            }

            if (key == "name") {
                if (!once(sel.nameSeen)) return false;
                const std::string want = value;
                sel.predicates.push_back([want, inverted](const SelectedEntity& e) {
                    return (e.name == want) != inverted;
                });
                return true;
            }

            if (key == "gamemode") {
                // MC canUse: `!hasGamemodeEquals()`, plus an inner guard that a
                // positive test may not follow a negative one.
                if (sel.gamemodeEquals || (sel.gamemodeNotEquals && !inverted)) {
                    error = "Option 'gamemode' isn't applicable here";
                    return false;
                }
                const std::string v = ToLower(value);
                GameMode want{};
                if      (v == "survival")  want = GameMode::SURVIVAL;
                else if (v == "creative")  want = GameMode::CREATIVE;
                else if (v == "adventure") want = GameMode::ADVENTURE;
                else if (v == "spectator") want = GameMode::SPECTATOR;
                else { error = "Unknown game mode: " + value; return false; }

                // MC: a gamemode test can only ever match a player, so the
                // selector stops considering anything else.
                sel.includesEntities = false;
                sel.predicates.push_back([want, inverted](const SelectedEntity& e) {
                    if (e.kind != SelectedEntity::Kind::Player || !e.player) return false;
                    return (e.player->getGameMode() == want) != inverted;
                });
                if (inverted) sel.gamemodeNotEquals = true;
                else          sel.gamemodeEquals = true;
                return true;
            }

            if (IsKnownUnsupportedOption(key)) {
                error = "Selector option '" + key + "' is not supported by this server";
                return false;
            }

            error = "Unknown selector option: " + key;
            return false;
        }

        // MC EntitySelectorParser.createRotationPredicate — the WRAPPING
        // comparison. Without it `y_rotation=170..-170` (a 20-degree arc across
        // the seam behind you) would select nothing instead of everything
        // except a narrow wedge.
        Predicate RotationPredicate(const Bounds& b, bool pitch) {
            const float min = Game::Mth::WrapDegrees(static_cast<float>(b.min.value_or(0.0)));
            const float max = Game::Mth::WrapDegrees(static_cast<float>(b.max.value_or(359.0)));
            return [min, max, pitch](const SelectedEntity& e) {
                const float r = Game::Mth::WrapDegrees(pitch ? e.xRot : e.yRot);
                return (min > max) ? (r >= min || r <= max) : (r >= min && r <= max);
            };
        }

        // ── Candidate gathering ─────────────────────────────────────────────

        void CollectPlayers(const CommandSource& src, std::vector<SelectedEntity>& out) {
            if (!src.sessions) return;
            for (const auto& session : src.sessions->GetAllSessions()) {
                if (!session || !session->GetPlayer()) continue;
                ServerPlayer* p = session->GetPlayer();

                SelectedEntity e;
                e.kind     = SelectedEntity::Kind::Player;
                e.id       = static_cast<int32_t>(session->GetConnectionId());
                e.player   = p;
                e.session  = session;
                e.position = p->getPosition();
                e.yRot     = p->getYaw();
                e.xRot     = p->getPitch();
                // PlayerEntityView's dimensions (0.6 x 1.8), built around FEET.
                e.box.min = glm::vec3(e.position) + glm::vec3(-0.3f, 0.0f, -0.3f);
                e.box.max = glm::vec3(e.position) + glm::vec3( 0.3f, 1.8f,  0.3f);
                e.typeSlug = "player";
                e.name     = p->getName();
                out.push_back(std::move(e));
            }
        }

        void CollectMobs(std::vector<SelectedEntity>& out) {
            if (!g_integratedServer) return;
            MobManager* mobs = g_integratedServer->GetMobs();
            if (!mobs) return;

            for (const auto& [id, mob] : mobs->All()) {
                if (!mob) continue;

                SelectedEntity e;
                e.kind     = SelectedEntity::Kind::Mob;
                e.id       = id;
                e.mob      = mob.get();
                e.position = mob->position;
                e.yRot     = mob->yRot;
                e.xRot     = mob->xRot;
                e.box      = mob->GetAABB();
                e.typeSlug = std::string(mob->TypeInfo().slug);
                e.name     = e.typeSlug;
                out.push_back(std::move(e));
            }
        }

        void CollectItems(std::vector<SelectedEntity>& out) {
            if (!g_integratedServer) return;
            ItemEntityManager* items = g_integratedServer->GetItemEntities();
            if (!items) return;

            for (const auto& [id, item] : items->All()) {
                SelectedEntity e;
                e.kind     = SelectedEntity::Kind::Item;
                e.id       = id;
                e.position = item.pos;
                e.yRot     = 0.0f;
                e.xRot     = 0.0f;
                const float half = Game::ItemEntity::kWidth * 0.5f;
                e.box.min = glm::vec3(item.pos) + glm::vec3(-half, 0.0f, -half);
                e.box.max = glm::vec3(item.pos) +
                            glm::vec3( half, Game::ItemEntity::kHeight, half);
                e.typeSlug = "item";
                e.name     = "item";
                out.push_back(std::move(e));
            }
        }

    } // namespace

    bool ResolveSelector(const std::string& token, SelectorKind kind,
                         const CommandSource& source,
                         std::vector<SelectedEntity>& out, std::string& error) {
        out.clear();

        const bool playersOnlyArgument =
            (kind == SelectorKind::Player || kind == SelectorKind::Players);
        const bool singleOnly =
            (kind == SelectorKind::Entity || kind == SelectorKind::Player);

        // ── Bare name (MC EntitySelectorParser.parseNameOrUUID) ─────────────
        if (token.empty()) { error = "Expected a player name or selector"; return false; }
        if (token[0] != '@') {
            std::vector<SelectedEntity> players;
            CollectPlayers(source, players);
            for (SelectedEntity& p : players) {
                if (IEquals(p.name, token)) {
                    out.push_back(std::move(p));
                    return true;
                }
            }
            error = "No player was found: " + token;
            return false;
        }

        // ── @x ──────────────────────────────────────────────────────────────
        if (token.size() < 2) { error = "Missing selector type"; return false; }

        ParsedSelector sel;
        // MC parseSelector adds an isAlive() predicate for the two selectors
        // that can return non-players; the player selectors deliberately do
        // not, so a dead-but-not-yet-respawned player is still addressable.
        bool selectOnlyAlive = false;

        switch (std::tolower(static_cast<unsigned char>(token[1]))) {
            case 'a':
                sel.maxResults = std::numeric_limits<int>::max();
                sel.includesEntities = false;
                sel.order = Order::Arbitrary;
                // MC limitToType(EntityType.PLAYER) — which is also what makes
                // `type=` inapplicable to this selector.
                sel.typeLimited = true;
                break;
            case 'e':
                sel.maxResults = std::numeric_limits<int>::max();
                sel.includesEntities = true;
                sel.order = Order::Arbitrary;
                selectOnlyAlive = true;
                break;
            case 'n':   // MC 1.21's "nearest entity"
                sel.maxResults = 1;
                sel.includesEntities = true;
                sel.order = Order::Nearest;
                selectOnlyAlive = true;
                break;
            case 'p':
                sel.maxResults = 1;
                sel.includesEntities = false;
                sel.order = Order::Nearest;
                sel.typeLimited = true;
                break;
            case 'r':
                sel.maxResults = 1;
                sel.includesEntities = false;
                sel.order = Order::Random;
                sel.typeLimited = true;
                break;
            case 's':
                sel.maxResults = 1;
                sel.includesEntities = true;
                sel.currentEntity = true;
                break;
            default:
                error = std::string("Unknown selector type: @") + token[1];
                return false;
        }

        // ── Options ─────────────────────────────────────────────────────────
        if (token.size() > 2) {
            if (token[2] != '[' || token.back() != ']') {
                error = "Expected [ ... ] after selector: " + token;
                return false;
            }
            const std::string body = token.substr(3, token.size() - 4);

            std::vector<std::pair<std::string, std::string>> pairs;
            if (!SplitOptions(body, pairs, error)) return false;
            for (const auto& [key, value] : pairs) {
                if (!ApplyOption(sel, ToLower(key), value, error)) return false;
            }
        }

        // MC finalizePredicates — rotation tests are added AFTER the options
        // are parsed, because both bounds must be known before the wrapping
        // comparison can be built.
        if (!sel.rotX.Any()) sel.predicates.push_back(RotationPredicate(sel.rotX, true));
        if (!sel.rotY.Any()) sel.predicates.push_back(RotationPredicate(sel.rotY, false));

        // EntityArgument.player()/players() reject a selector that could return
        // a non-player rather than filtering it away. MC does this so
        // `/tp @e Steve` fails loudly instead of teleporting a subset.
        if (playersOnlyArgument && sel.includesEntities && !sel.currentEntity) {
            error = "Only players may be affected by this command, but the "
                    "provided selector includes entities";
            return false;
        }

        // ── Origin ──────────────────────────────────────────────────────────
        glm::dvec3 origin = source.position;
        if (sel.x) origin.x = *sel.x;
        if (sel.y) origin.y = *sel.y;
        if (sel.z) origin.z = *sel.z;

        // ── Volume (MC EntitySelectorParser.getSelector) ────────────────────
        // dx/dy/dz make a BOX, and it is one block bigger than the numbers
        // suggest: `dx=0` still spans the cube the origin sits in.
        bool haveBox = false;
        Game::AABB box{};
        if (sel.dx || sel.dy || sel.dz) {
            const double dx = sel.dx.value_or(0.0);
            const double dy = sel.dy.value_or(0.0);
            const double dz = sel.dz.value_or(0.0);
            const auto lo = [](double d) { return d < 0.0 ? d : 0.0; };
            const auto hi = [](double d) { return (d < 0.0 ? 0.0 : d) + 1.0; };
            box.min = glm::vec3(origin) + glm::vec3(lo(dx), lo(dy), lo(dz));
            box.max = glm::vec3(origin) + glm::vec3(hi(dx), hi(dy), hi(dz));
            haveBox = true;
        } else if (sel.distance.max) {
            const double r = *sel.distance.max;
            box.min = glm::vec3(origin) - glm::vec3(static_cast<float>(r));
            box.max = glm::vec3(origin) + glm::vec3(static_cast<float>(r + 1.0));
            haveBox = true;
        }

        // ── Candidates ──────────────────────────────────────────────────────
        std::vector<SelectedEntity> candidates;
        if (sel.currentEntity) {
            // MC: @s is the command's own entity, tested against the same
            // predicates as anything else — `@s[type=zombie]` legitimately
            // matches nothing.
            std::vector<SelectedEntity> players;
            CollectPlayers(source, players);
            for (SelectedEntity& p : players) {
                if (p.player == source.sender) { candidates.push_back(std::move(p)); break; }
            }
        } else {
            CollectPlayers(source, candidates);
            if (sel.includesEntities) {
                CollectMobs(candidates);
                CollectItems(candidates);
            }
        }

        // ── Filter ──────────────────────────────────────────────────────────
        std::vector<SelectedEntity> matched;
        for (SelectedEntity& e : candidates) {
            if (selectOnlyAlive && e.kind == SelectedEntity::Kind::Mob &&
                e.mob && !e.mob->IsAlive()) {
                continue;
            }
            if (haveBox && !box.Intersects(e.box)) continue;

            if (!sel.distance.Any()) {
                const glm::dvec3 d = e.position - origin;
                if (!sel.distance.MatchesSq(glm::dot(d, d))) continue;
            }

            bool ok = true;
            for (const Predicate& p : sel.predicates) {
                if (!p(e)) { ok = false; break; }
            }
            if (!ok) continue;

            matched.push_back(std::move(e));
        }

        // ── Sort, then limit (MC EntitySelector.sortAndLimit) ────────────────
        //
        // MC applies the limit DURING collection when the order is arbitrary,
        // so `@e[limit=3]` stops at whichever three it meets first. Collecting
        // everything and truncating afterwards picks a different arbitrary
        // three; both are equally arbitrary, and doing it here keeps one code
        // path for all four orders.
        if (matched.size() > 1) {
            const auto distSq = [&origin](const SelectedEntity& e) {
                const glm::dvec3 d = e.position - origin;
                return glm::dot(d, d);
            };
            switch (sel.order) {
                case Order::Nearest:
                    std::sort(matched.begin(), matched.end(),
                              [&](const SelectedEntity& a, const SelectedEntity& b) {
                                  return distSq(a) < distSq(b);
                              });
                    break;
                case Order::Furthest:
                    std::sort(matched.begin(), matched.end(),
                              [&](const SelectedEntity& a, const SelectedEntity& b) {
                                  return distSq(b) < distSq(a);
                              });
                    break;
                case Order::Random: {
                    static std::mt19937 rng{std::random_device{}()};
                    std::shuffle(matched.begin(), matched.end(), rng);
                    break;
                }
                case Order::Arbitrary:
                    break;
            }
        }

        if (static_cast<int>(matched.size()) > sel.maxResults) {
            matched.resize(static_cast<size_t>(sel.maxResults));
        }

        // ── Result-count rules (MC EntityArgument) ───────────────────────────
        if (matched.empty()) {
            error = playersOnlyArgument ? "No player was found" : "No entity was found";
            return false;
        }
        if (singleOnly && matched.size() > 1) {
            error = playersOnlyArgument
                ? "Only one player is allowed, but the provided selector allows more than one"
                : "Only one entity is allowed, but the provided selector allows more than one";
            return false;
        }

        out = std::move(matched);
        return true;
    }

} // namespace Server
