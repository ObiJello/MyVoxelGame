// File: src/common/world/level/GameRules.hpp
//
// Port of net.minecraft.world.level.gamerules — the registry (GameRules.java's
// registration list, baked by tools/gen_game_rules.py into
// GeneratedGameRules.inc) and ONE server-wide value map (GameRuleMap).
//
// MC keeps a single GameRules on the server that every level reads; this is
// that object. Values are atomics because the readers are everywhere: the
// server thread (most rules), the mob workers (max_entity_cramming inside a
// parallel tick), a remote client's I/O thread (the rules the server mirrors
// to clients through WorldRulesS2C). Relaxed loads are enough — nothing
// orders a rule change against other state.
//
// WHICH RULES ACTUALLY DO SOMETHING
// ---------------------------------
// Every vanilla rule is registered, persisted (level.dat game_rules, read
// under the new id, the bare id or the pre-26 camelCase name) and settable
// through /gamerule, so a level.dat this engine writes is one Minecraft reads
// back whole. `IsImplemented` says whether the engine has the system the rule
// gates; the in-world Edit Game Rules screen shows the others with a greyed
// "Not implemented yet" control instead of a value, and /gamerule says so when
// one is set. A rule becomes implemented by adding its hook and moving its id
// into kImplemented in GameRules.cpp — nothing else changes.
//
// Nine rules (advance_time, spawn_mobs, mob_griefing, random_tick_speed,
// tnt_explodes, entity_drops and the three *_explosion_drop_decay) predate
// this map and live as fields on every Game::World; GameRuleCommand keeps
// those in step (it writes both), so this map is a correct read for them too,
// but the World getters remain their authoritative, per-level read.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Game::Rules {

    enum class Id : uint8_t {
#define GAME_RULE(Enum, id, legacy, inverted, category, type, def, lo, hi, label, description) Enum,
#include "GeneratedGameRules.inc"
#undef GAME_RULE
        Count
    };
    inline constexpr size_t kCount = static_cast<size_t>(Id::Count);

    // MC GameRuleCategory, in its declaration order (the order the Edit Game
    // Rules screen groups by).
    enum class Category : uint8_t { Player, Mobs, Spawning, Drops, Updates, Chat, Misc };

    // MC GameRuleType: the registry knows only these two kinds.
    enum class Type : uint8_t { Bool, Int };

    struct Def {
        Id          id;
        const char* key;            // MC GameRule.id(), snake_case
        const char* legacyName;     // pre-26 camelCase name (level.dat, old typing), or nullptr
        bool        legacyInverted; // disableRaids -> raids: the old value meant the opposite
        Category    category;
        Type        type;
        int         defaultValue;   // booleans 0/1
        int         minValue;       // MC IntegerArgumentType bounds (ints); 0/1 for bools
        int         maxValue;
        const char* label;          // lang gamerule.<name>
        const char* description;    // lang gamerule.<name>.description, or ""
    };

    const Def& GetDef(Id id);
    // All defs, in registration order (MC's: alphabetical by id).
    const Def* AllDefs();
    // MC registers each rule under its id and its `minecraft:`-qualified
    // identifier; the legacy camelCase spelling is accepted too so anything
    // already typing it keeps working. Case-insensitive. Null when unknown.
    const Def* Find(std::string_view name);

    // lang gamerule.category.<name>: "Player", "World Updates", ...
    const char* CategoryName(Category category);

    // ── the value map ──────────────────────────────────────────────────────
    int  GetInt(Id id);
    bool GetBool(Id id);
    // Clamped to the rule's bounds (bools to 0/1).
    void Set(Id id, int value);
    void SetBool(Id id, bool value);
    // Every rule back to its default — a new world, or a world whose level.dat
    // had no game_rules compound.
    void ResetAll();

    // MC GameRule.serialize / deserialize: "true" / "false" for booleans, the
    // decimal for integers. Parse rejects trailing characters and, for
    // integers, values outside [min, max] — what Brigadier's argument type
    // would have refused.
    std::string        Serialize(Id id, int value);
    std::string        Serialize(Id id);   // the live value
    std::optional<int> Parse(Id id, std::string_view text);

    // Whether the engine has the system this rule gates. See the header note.
    bool IsImplemented(Id id);

} // namespace Game::Rules
