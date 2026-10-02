// File: src/server/commands/EntitySelector.cpp
#include "EntitySelector.hpp"
#include "../entity/ServerLevelBridge.hpp"   // PlayerEntityView

#include "../IntegratedServer.hpp"
#include "../entity/ItemEntityManager.hpp"
#include "../entity/ExperienceOrbManager.hpp"
#include "../level/ServerLevel.hpp"
#include "../entity/MobManager.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EntityType.hpp"
#include "common/entity/ItemEntity.hpp"
#include "common/entity/ExperienceOrb.hpp"
#include "common/entity/SelectableEntityTypes.hpp"
#include "common/entity/Mob.hpp"
#include "common/text/Language.hpp"
#include "common/world/tags/DataTags.hpp"
#include "common/core/SaveVersion.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "SnbtParser.hpp"
#include "NbtPath.hpp"
#include "../world/storage/NBTParser.hpp"
#include "../world/storage/anvil/EntityNbt.hpp"
#include "../world/storage/anvil/ItemStackNbt.hpp"
#include "../world/storage/anvil/PlayerDataStore.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <random>

namespace Server {

    namespace {

        // MC StringReader.readString: `"..."` or `'...'` with backslash
        // escapes of the quote and of the backslash itself; anything else is
        // taken as it stands (readUnquotedString's word).
        bool UnquoteString(const std::string& raw, std::string& out, std::string& error) {
            out.clear();
            if (raw.empty() || (raw[0] != '"' && raw[0] != '\'')) {
                out = raw;
                return true;
            }
            const char quote = raw[0];
            bool escaped = false;
            for (size_t i = 1; i < raw.size(); ++i) {
                const char c = raw[i];
                if (escaped) {
                    if (c != quote && c != '\\') {
                        error = std::string("Invalid escape sequence '") + c + "' in quoted string";
                        return false;
                    }
                    out += c;
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == quote) {
                    if (i + 1 != raw.size()) {
                        error = "Unexpected text after quoted string: " + raw;
                        return false;
                    }
                    return true;
                } else {
                    out += c;
                }
            }
            error = "Unclosed quoted string: " + raw;
            return false;
        }

        // MC Entity.getName().getString(): the custom name, else the type's
        // name ("Cow") — what `name=` compares against.
        std::string DisplayNameOf(const Game::Mob& mob) {
            if (const auto& custom = mob.GetCustomName()) return *custom;
            const std::string slug(mob.TypeInfo().slug);
            return Game::Language::GetOrDefault("entity.minecraft." + slug, slug);
        }

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
            // A positive `name=` (not `name=!x`): the engine's named-entity
            // lookup beyond the loaded chunks keys on it.
            std::optional<std::string> nameEquals;
            // The positive `type=` values and `type=#tag`s: one OR-set (see
            // the "type" option — a deliberate deviation from MC).
            std::vector<std::string> typeSlugsAny;
            std::vector<std::string> typeTagsAny;
            // MC's `nbt=` tests (EntitySelectorOptions "nbt"): each compound,
            // and whether it was `nbt=!{...}`. All must hold (AND).
            std::vector<std::pair<std::shared_ptr<::World::NBTTagCompound>, bool>> nbtTests;
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

        // A SelectedEntity's type slug as its registry id, for the tag files
        // ("cow" -> "minecraft:cow", a mod mob -> its own namespace).
        std::string FullEntityTypeId(const std::string& slug) {
            if (std::find(Game::kSelectorOnlyEntityTypes.begin(), Game::kSelectorOnlyEntityTypes.end(), slug) !=
                Game::kSelectorOnlyEntityTypes.end()) {
                return "minecraft:" + slug;
            }
            for (uint16_t i = 0; i < static_cast<uint16_t>(Game::EntityTypeId::Count); ++i) {
                const auto type = static_cast<Game::EntityTypeId>(i);
                if (Game::GetEntityTypeInfo(type).slug == slug) {
                    const std::string name = Game::Anvil::EntityName(type);
                    if (!name.empty()) return name;
                    break;
                }
            }
            return slug.find(':') == std::string::npos ? "minecraft:" + slug : slug;
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
            // The comma that ends a pair, skipping any inside a quoted
            // string (MC readString: `name="Smith, Jr."`).
            // `nbt={a:1,b:[2,3]}` holds commas too: only one at nesting depth
            // zero ends a pair.
            const auto nextComma = [&body](size_t from) {
                char quote = 0;
                int depth = 0;
                for (size_t k = from; k < body.size(); ++k) {
                    const char c = body[k];
                    if (quote) {
                        if (c == '\\' && k + 1 < body.size()) { ++k; continue; }
                        if (c == quote) quote = 0;
                    } else if (c == '"' || c == '\'') {
                        quote = c;
                    } else if (c == '{' || c == '[') {
                        ++depth;
                    } else if ((c == '}' || c == ']') && depth > 0) {
                        --depth;
                    } else if (c == ',' && depth == 0) {
                        return k;
                    }
                }
                return std::string::npos;
            };
            size_t i = 0;
            while (i < body.size()) {
                const size_t comma = nextComma(i);
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
            return key == "team" || key == "scores" ||
                   key == "advancements" || key == "predicate" ||
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
                // `type=#tag` (MC: EntityTypeTags through the entity_type
                // registry's tag files, the mod namespaces' included).
                if (!value.empty() && value[0] == '#') {
                    std::string tag = value.substr(1);
                    if (tag.find(':') == std::string::npos) tag = "minecraft:" + tag;
                    if (!Game::DataTags::TagExists(Game::DataTags::Registry::EntityType, tag)) {
                        error = "Unknown entity type tag '#" + tag + "'";
                        return false;
                    }
                    if (inverted) {
                        sel.predicates.push_back([tag](const SelectedEntity& e) {
                            return !Game::DataTags::HasTag(Game::DataTags::Registry::EntityType,
                                                           FullEntityTypeId(e.typeSlug), tag);
                        });
                    } else {
                        sel.typeTagsAny.push_back(tag);
                    }
                    return true;
                }
                const std::string slug = ToLower(StripNamespace(value));

                // Validate against the types this engine knows, plus MC's
                // registry entries outside EntityTypeId (players, dropped
                // items, experience orbs, and the ids with no entity here).
                bool known = std::find(Game::kSelectorOnlyEntityTypes.begin(),
                                       Game::kSelectorOnlyEntityTypes.end(), slug) !=
                             Game::kSelectorOnlyEntityTypes.end();
                if (!known) {
                    for (uint16_t i = 0; i < static_cast<uint16_t>(Game::EntityTypeId::Count); ++i) {
                        if (Game::GetEntityTypeInfo(static_cast<Game::EntityTypeId>(i)).slug == slug) {
                            known = true;
                            break;
                        }
                    }
                }
                if (!known) { error = "Unknown entity type: " + value; return false; }

                // DELIBERATE DEVIATION from MC (requested): MC allows one
                // positive `type=` (a second is "not applicable") and none
                // after a negated one. Here every positive type and positive
                // tag joins ONE OR-set — `@e[type=squid,type=glow_squid]`,
                // `@e[type=squid,type=#aquatic]` — applied after the options
                // are read (ResolveSelector); negated ones stay MC's AND of
                // exclusions, in any order.
                if (inverted) {
                    sel.typeLimitedInversely = true;
                    sel.predicates.push_back([slug](const SelectedEntity& e) { return e.typeSlug != slug; });
                } else {
                    sel.typeSlugsAny.push_back(slug);
                }
                return true;
            }

            if (key == "nbt") {
                // MC TagParser.parseCompoundAsArgument; any number of them.
                std::string parseError;
                auto tag = Snbt::ParseCompound(value, parseError);
                if (!tag) { error = parseError.empty() ? "Invalid NBT: " + value : parseError; return false; }
                sel.nbtTests.emplace_back(std::move(tag), inverted);
                return true;
            }

            if (key == "name") {
                if (!once(sel.nameSeen)) return false;
                // MC readString: a quoted string (either quote, with \ and
                // the quote escaped) or a bare word.
                std::string want;
                if (!UnquoteString(value, want, error)) return false;
                if (!inverted) sel.nameEquals = want;
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

            if (key == "tag") {
                // MC EntitySelectorOptions "tag": any number of them, each an
                // unquoted string; an empty one tests "has no tags at all"
                // (so `tag=!` means "has some tag").
                if (!value.empty() && !Game::IsValidEntityTag(value)) {
                    error = "Invalid tag: " + value;
                    return false;
                }
                sel.predicates.push_back([value, inverted](const SelectedEntity& e) {
                    const Game::EntityTags* tags = SelectedEntityTags(e);
                    const bool has = tags && (value.empty() ? tags->Empty() : tags->Has(value));
                    return has != inverted;
                });
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
                e.dimension = Game::DimensionFromRaw(session->GetDimensionId());
                e.typeSlug = "player";
                e.name     = p->getName();
                out.push_back(std::move(e));
            }
        }

        void CollectMobs(const CommandSource& src, std::vector<SelectedEntity>& out) {
            if (!g_integratedServer) return;
            // The SENDER's level, not the Overworld — see CommandSource.
            ServerLevel* level = g_integratedServer->GetLevel(src.dimension);
            MobManager* mobs = level ? level->Mobs() : nullptr;
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
                e.dimension = src.dimension;
                e.typeSlug = std::string(mob->TypeInfo().slug);
                e.name     = DisplayNameOf(*mob);
                out.push_back(std::move(e));
            }
        }

        void CollectItems(const CommandSource& src, std::vector<SelectedEntity>& out) {
            if (!g_integratedServer) return;
            ServerLevel* level = g_integratedServer->GetLevel(src.dimension);
            ItemEntityManager* items = level ? level->Items() : nullptr;
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
                e.dimension = src.dimension;
                e.typeSlug = "item";
                e.name     = "item";
                out.push_back(std::move(e));
            }
        }

        // MC's experience_orb entities (their own manager here).
        void CollectOrbs(const CommandSource& src, std::vector<SelectedEntity>& out) {
            if (!g_integratedServer) return;
            ServerLevel* level = g_integratedServer->GetLevel(src.dimension);
            ExperienceOrbManager* orbs = level ? level->Orbs() : nullptr;
            if (!orbs) return;
            for (const auto& [id, orb] : orbs->All()) {
                if (orb.pickedUp) continue;
                SelectedEntity e;
                e.kind     = SelectedEntity::Kind::Orb;
                e.id       = id;
                e.position = orb.pos;
                const float half = Game::ExperienceOrb::kWidth * 0.5f;
                e.box.min = glm::vec3(orb.pos) + glm::vec3(-half, 0.0f, -half);
                e.box.max = glm::vec3(orb.pos) + glm::vec3( half, Game::ExperienceOrb::kHeight, half);
                e.dimension = src.dimension;
                e.typeSlug = "experience_orb";
                e.name     = "Experience Orb";
                out.push_back(std::move(e));
            }
        }

        // ── nbt= ───────────────────────────────────────────────────────────

        // MC NbtUtils.compareNbt(expected, actual, partialListMatches = true)
        // — Nbt::Compare (NbtPath.hpp), which also carries the DELIBERATE
        // DEVIATION (requested) that two id-looking strings match with or
        // without "minecraft:": completion shows bare ids, so a player types
        // `nbt={variant:"temperate"}` where the entity saves
        // "minecraft:temperate".
        bool CompareNbt(const ::World::NBTTag* expected, const ::World::NBTTag* actual) {
            return Nbt::Compare(expected, actual);
        }

        std::shared_ptr<::World::NBTTagCompound> ParseRoot(const std::vector<uint8_t>& bytes) {
            try {
                return std::dynamic_pointer_cast<::World::NBTTagCompound>(::World::NBTParser::Parse(bytes));
            } catch (const std::exception&) {
                return nullptr;
            }
        }

        // One element written into a throwaway `E` list by an entity-list
        // writer (the Anvil entity save's own), read back as a tree.
        template <typename WriteFn>
        std::shared_ptr<::World::NBTTagCompound> WrittenElement(WriteFn&& write) {
            Game::Nbt::Writer w;
            w.BeginRootCompound();
            auto list = w.BeginList("E", Game::Nbt::TagType::Compound);
            const bool ok = write(w, list);
            w.EndList(list);
            w.EndRootCompound();
            if (!ok || !w.ok()) return nullptr;
            auto root = ParseRoot(w.Bytes());
            auto elements = root ? std::dynamic_pointer_cast<::World::NBTTagList>(root->GetTag("E")) : nullptr;
            if (!elements || elements->value.empty()) return nullptr;
            return std::dynamic_pointer_cast<::World::NBTTagCompound>(elements->value.front());
        }

        // MC's `entity.saveWithoutId(output)` (+ a player's "SelectedItem"):
        // the entity's full saved compound, from the same writers the world
        // save uses — EntityNbt for mobs and items, the player file for
        // players — minus the "id" (and the player file's DataVersion).
        std::shared_ptr<::World::NBTTagCompound> SavedNbtOf(const SelectedEntity& e) {
            std::shared_ptr<::World::NBTTagCompound> out;
            switch (e.kind) {
                case SelectedEntity::Kind::Mob:
                    if (!e.mob) return nullptr;
                    out = WrittenElement([&](Game::Nbt::Writer& w, Game::Nbt::Writer::ListScope& list) {
                        return Game::Anvil::WriteMob(w, list, *e.mob);
                    });
                    break;
                case SelectedEntity::Kind::Item: {
                    ServerLevel* level = g_integratedServer ? g_integratedServer->GetLevel(e.dimension) : nullptr;
                    ItemEntityManager* items = level ? level->Items() : nullptr;
                    const Game::ItemEntity* item = items ? items->Find(e.id) : nullptr;
                    if (!item) return nullptr;
                    out = WrittenElement([&](Game::Nbt::Writer& w, Game::Nbt::Writer::ListScope& list) {
                        return Game::Anvil::WriteItem(w, list, *item);
                    });
                    break;
                }
                case SelectedEntity::Kind::Orb: {
                    ServerLevel* level = g_integratedServer ? g_integratedServer->GetLevel(e.dimension) : nullptr;
                    ExperienceOrbManager* orbs = level ? level->Orbs() : nullptr;
                    if (!orbs) return nullptr;
                    const auto it = orbs->All().find(e.id);
                    if (it == orbs->All().end()) return nullptr;
                    const Game::ExperienceOrb& orb = it->second;
                    out = WrittenElement([&](Game::Nbt::Writer& w, Game::Nbt::Writer::ListScope& list) {
                        return Game::Anvil::WriteOrb(w, list, orb);
                    });
                    break;
                }
                case SelectedEntity::Kind::Player: {
                    if (!e.player) return nullptr;
                    std::vector<uint8_t> bytes;
                    std::string error;
                    if (!Game::Anvil::BuildPlayerNbt(*e.player, Game::Save::DataVersion(), bytes, error)) return nullptr;
                    out = ParseRoot(bytes);
                    if (!out) return nullptr;
                    out->value.erase("DataVersion");
                    const Game::ItemStack& selected = e.player->getInventory().GetSelectedStack();
                    if (!selected.IsEmpty()) {
                        Game::Nbt::Writer w;
                        w.BeginRootCompound();
                        w.BeginCompound("SelectedItem");
                        Game::Anvil::WriteItemStackBody(w, selected);
                        w.EndCompound();
                        w.EndRootCompound();
                        if (w.ok()) {
                            if (auto wrapper = ParseRoot(w.Bytes())) {
                                if (auto tag = wrapper->GetTag("SelectedItem")) out->value["SelectedItem"] = tag;
                            }
                        }
                    }
                    return out;
                }
            }
            if (out) out->value.erase("id");
            return out;
        }

    } // namespace

    std::shared_ptr<::World::NBTTagCompound> SavedEntityNbt(const SelectedEntity& entity) {
        return SavedNbtOf(entity);
    }

    NamedEntityDeferral& CurrentNamedEntityDeferral() {
        thread_local NamedEntityDeferral deferral;
        return deferral;
    }

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
            // MC: a name selector's findEntities is simply empty for an
            // offline player; only the non-optional argument types throw.
            if (kind == SelectorKind::OptionalEntities) return true;
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

        // The positive type set (types OR tags). MC: a positive
        // `type=player` alone keeps the selector to players; any other
        // positive type brings entities into scope.
        if (!sel.typeSlugsAny.empty() || !sel.typeTagsAny.empty()) {
            const bool playersOnly = sel.typeTagsAny.empty() &&
                std::all_of(sel.typeSlugsAny.begin(), sel.typeSlugsAny.end(),
                            [](const std::string& t) { return t == "player"; });
            if (!sel.currentEntity) sel.includesEntities = !playersOnly;
            sel.predicates.push_back([slugs = sel.typeSlugsAny, tags = sel.typeTagsAny](const SelectedEntity& e) {
                if (std::find(slugs.begin(), slugs.end(), e.typeSlug) != slugs.end()) return true;
                if (tags.empty()) return false;
                const std::string id = FullEntityTypeId(e.typeSlug);
                for (const std::string& tag : tags) {
                    if (Game::DataTags::HasTag(Game::DataTags::Registry::EntityType, id, tag)) return true;
                }
                return false;
            });
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
            // matches nothing. The source's entity when `/execute as` set
            // one (re-read, so a chain sees where it is NOW), else the sender.
            if (source.entity) {
                SelectedEntity e = *source.entity;
                if (RefreshSelectedEntity(source, e)) candidates.push_back(std::move(e));
            } else {
                std::vector<SelectedEntity> players;
                CollectPlayers(source, players);
                for (SelectedEntity& p : players) {
                    if (p.player == source.sender) { candidates.push_back(std::move(p)); break; }
                }
            }
        } else {
            CollectPlayers(source, candidates);
            if (sel.includesEntities) {
                CollectMobs(source, candidates);
                CollectItems(source, candidates);
                CollectOrbs(source, candidates);
            }
        }

        // ── Filter ──────────────────────────────────────────────────────────
        // `savedOnly`: an entity known only from the named-entity index (not
        // loaded) — its nbt= tests wait for the re-run that loads it.
        const auto passes = [&](const SelectedEntity& e, bool savedOnly = false) {
            if (selectOnlyAlive && e.kind == SelectedEntity::Kind::Mob &&
                e.mob && !e.mob->IsAlive()) {
                return false;
            }
            if (haveBox && !box.Intersects(e.box)) return false;
            if (!sel.distance.Any()) {
                const glm::dvec3 d = e.position - origin;
                if (!sel.distance.MatchesSq(glm::dot(d, d))) return false;
            }
            for (const Predicate& p : sel.predicates) {
                if (!p(e)) return false;
            }
            // nbt= last: building an entity's compound is the one costly test,
            // so only what every cheap filter let through pays for it.
            if (!sel.nbtTests.empty() && !savedOnly) {
                const auto actual = SavedNbtOf(e);
                if (!actual) return false;
                for (const auto& [expected, inverted] : sel.nbtTests) {
                    if (CompareNbt(expected.get(), actual.get()) == inverted) return false;
                }
            }
            return true;
        };

        // ── Named entities beyond the loaded chunks (engine deviation) ──────
        //
        // A positive `name=` also reaches custom-named entities saved in
        // chunks nobody has loaded (NamedEntityIndex.hpp), in every
        // dimension unless a position option limits it to this one (MC's
        // worldLimited: distance, x/y/z, dx/dy/dz) — MC's own non-limited
        // selectors walk every level too. Loaded mobs of the other levels
        // are candidates at once; saved ones make the command wait: their
        // chunks are held loaded and the dispatcher runs the command again
        // once their entities are in (CommandDispatcher::ProcessDeferred).
        if (sel.nameEquals && sel.includesEntities && !sel.currentEntity && g_integratedServer) {
            const bool worldLimited = sel.distanceSeen || sel.xSeen || sel.ySeen || sel.zSeen ||
                                      sel.dxSeen || sel.dySeen || sel.dzSeen;
            if (!worldLimited) {
                for (const Game::DimensionId d : Game::kAllDimensions) {
                    if (d == source.dimension || !g_integratedServer->GetLevel(d)) continue;
                    CommandSource scoped = source;
                    scoped.dimension = d;
                    CollectMobs(scoped, candidates);
                }
            }
            std::vector<NamedEntities::ChunkRef> waitFor;
            size_t waiting = 0;
            for (const Game::DimensionId d : Game::kAllDimensions) {
                if (worldLimited && d != source.dimension) continue;
                for (const NamedEntities::Entry& entry : NamedEntities::Find(d, *sel.nameEquals)) {
                    if (NamedEntities::EntitiesLive(d, entry.chunk)) continue;   // found live, or gone
                    SelectedEntity saved;
                    saved.kind      = SelectedEntity::Kind::Mob;
                    saved.position  = entry.position;
                    saved.box.min   = glm::vec3(entry.position) + glm::vec3(-0.3f, 0.0f, -0.3f);
                    saved.box.max   = glm::vec3(entry.position) + glm::vec3( 0.3f, 1.8f,  0.3f);
                    saved.dimension = d;
                    saved.typeSlug  = entry.typeSlug;
                    saved.name      = entry.name;
                    if (!passes(saved, /*savedOnly=*/true)) continue;
                    ++waiting;
                    const NamedEntities::ChunkRef ref{d, entry.chunk};
                    if (std::find(waitFor.begin(), waitFor.end(), ref) == waitFor.end()) waitFor.push_back(ref);
                }
            }
            NamedEntityDeferral& deferral = CurrentNamedEntityDeferral();
            if (!waitFor.empty() && deferral.allowed) {
                for (const NamedEntities::ChunkRef& ref : waitFor) {
                    if (std::find(deferral.chunks.begin(), deferral.chunks.end(), ref) == deferral.chunks.end()) {
                        deferral.chunks.push_back(ref);
                    }
                }
                error = "Loading " + std::to_string(waiting) + (waiting == 1 ? " named entity" : " named entities") +
                        " from unloaded chunks...";
                return false;
            }
        }

        std::vector<SelectedEntity> matched;
        for (SelectedEntity& e : candidates) {
            if (!passes(e)) continue;
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
        if (kind == SelectorKind::OptionalEntities) {
            out = std::move(matched);
            return true;
        }
        if (matched.empty()) {
            error = playersOnlyArgument ? "No player was found" : "No entity was found";
            return false;
        }
        // `@s` slipped past the includesEntities test above (MC lets it
        // through the parser too) but the argument type still rejects a
        // non-player result — EntityArgument.player() on a zombie source.
        if (playersOnlyArgument) {
            for (const SelectedEntity& e : matched) {
                if (e.kind != SelectedEntity::Kind::Player) {
                    error = "Only players may be affected by this command, but the "
                            "provided selector includes entities";
                    return false;
                }
            }
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

    // ── Source anchors and entity descriptions ──────────────────────────────

    namespace {
        double EyeHeightOf(const SelectedEntity& e) {
            switch (e.kind) {
                case SelectedEntity::Kind::Player: return 1.62;
                case SelectedEntity::Kind::Mob:    return e.mob ? e.mob->GetEyeHeight() : 0.0;
                case SelectedEntity::Kind::Item:   return 0.0;
                case SelectedEntity::Kind::Orb:    return Game::ExperienceOrb::kEyeHeight;
            }
            return 0.0;
        }
    }

    glm::dvec3 EntityAnchorPosition(const SelectedEntity& entity, bool eyes) {
        return eyes ? entity.position + glm::dvec3(0.0, EyeHeightOf(entity), 0.0) : entity.position;
    }

    glm::dvec3 SourceAnchorPosition(const CommandSource& source) {
        // MC Anchor.apply(CommandSourceStack): the SOURCE's position (which
        // `positioned` may have moved away from the entity), offset by the
        // entity's eye height — never the entity's own position.
        if (!source.anchorEyes || !source.entity) return source.position;
        return source.position + glm::dvec3(0.0, EyeHeightOf(*source.entity), 0.0);
    }

    bool DescribePlayer(ServerPlayer& player, const CommandSource& source, SelectedEntity& out) {
        std::vector<SelectedEntity> players;
        CollectPlayers(source, players);
        for (SelectedEntity& p : players) {
            if (p.player == &player) { out = std::move(p); return true; }
        }
        return false;
    }

    bool DescribeEntity(Game::Entity* entity, const CommandSource& source, SelectedEntity& out) {
        if (!entity || entity->IsRemoved()) return false;
        if (auto* view = dynamic_cast<PlayerEntityView*>(entity)) {
            return view->GetPlayer() && DescribePlayer(*view->GetPlayer(), source, out);
        }
        auto* mob = dynamic_cast<Game::Mob*>(entity);
        if (!mob || !g_integratedServer) return false;
        // Only a mob the source's level still owns: the pointer came from a
        // relation (target, vehicle, owner…) that may reach across a tick.
        ServerLevel* level = g_integratedServer->GetLevel(source.dimension);
        MobManager* mobs = level ? level->Mobs() : nullptr;
        if (!mobs || mobs->Find(mob->GetId()) != mob) return false;

        SelectedEntity e;
        e.kind      = SelectedEntity::Kind::Mob;
        e.id        = mob->GetId();
        e.mob       = mob;
        e.position  = mob->position;
        e.yRot      = mob->yRot;
        e.xRot      = mob->xRot;
        e.box       = mob->GetAABB();
        e.dimension = source.dimension;
        e.typeSlug  = std::string(mob->TypeInfo().slug);
        e.name      = DisplayNameOf(*mob);
        out = std::move(e);
        return true;
    }

    Game::EntityTags* SelectedEntityTags(const SelectedEntity& entity) {
        switch (entity.kind) {
            case SelectedEntity::Kind::Player:
                return entity.player ? &entity.player->getTags() : nullptr;
            case SelectedEntity::Kind::Mob:
                return entity.mob ? &entity.mob->Tags() : nullptr;
            case SelectedEntity::Kind::Item: {
                ServerLevel* level = g_integratedServer ? g_integratedServer->GetLevel(entity.dimension) : nullptr;
                ItemEntityManager* items = level ? level->Items() : nullptr;
                Game::ItemEntity* item = items ? items->Find(entity.id) : nullptr;
                return item ? &item->tags : nullptr;
            }
            case SelectedEntity::Kind::Orb: {
                ServerLevel* level = g_integratedServer ? g_integratedServer->GetLevel(entity.dimension) : nullptr;
                ExperienceOrbManager* orbs = level ? level->Orbs() : nullptr;
                if (!orbs) return nullptr;
                auto it = orbs->AllMutable().find(entity.id);
                return it == orbs->AllMutable().end() ? nullptr : &it->second.tags;
            }
        }
        return nullptr;
    }

    bool RefreshSelectedEntity(const CommandSource& source, SelectedEntity& entity) {
        CommandSource scoped = source;
        scoped.dimension = entity.dimension;
        std::vector<SelectedEntity> all;
        switch (entity.kind) {
            case SelectedEntity::Kind::Player: CollectPlayers(scoped, all); break;
            case SelectedEntity::Kind::Mob:    CollectMobs(scoped, all);    break;
            case SelectedEntity::Kind::Item:   CollectItems(scoped, all);   break;
            case SelectedEntity::Kind::Orb:    CollectOrbs(scoped, all);    break;
        }
        for (SelectedEntity& e : all) {
            if (e.kind == entity.kind && e.id == entity.id) { entity = std::move(e); return true; }
        }
        return false;
    }

} // namespace Server
