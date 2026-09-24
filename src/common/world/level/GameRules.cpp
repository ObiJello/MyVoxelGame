// File: src/common/world/level/GameRules.cpp
#include "GameRules.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>

namespace Game::Rules {

    namespace {

        constexpr Def kDefs[] = {
#define GAME_RULE(Enum, id, legacy, inverted, category, type, def, lo, hi, label, description) \
            Def{ Id::Enum, id, legacy, inverted, Category::category, Type::type, def, lo, hi, label, description },
#include "GeneratedGameRules.inc"
#undef GAME_RULE
        };
        static_assert(sizeof(kDefs) / sizeof(kDefs[0]) == kCount, "GeneratedGameRules.inc and Id disagree");

        // The rules whose gate exists in this engine. Everything else is
        // stored and shown, and says "Not implemented yet". Keep this list
        // honest: adding an id here without its hook makes the screen lie.
        constexpr Id kImplemented[] = {
            // World-field rules (Game::World, see the header)
            Id::AdvanceTime, Id::SpawnMobs, Id::MobGriefing, Id::RandomTickSpeed,
            Id::TntExplodes, Id::EntityDrops, Id::TntExplosionDropDecay,
            Id::BlockExplosionDropDecay, Id::MobExplosionDropDecay,
            // Player
            Id::NaturalHealthRegeneration,        // FoodData::tick
            Id::FallDamage, Id::FireDamage,       // ServerPlayer::damage (Player.isInvulnerableTo)
            Id::DrowningDamage,
            Id::KeepInventory,                    // PlayerSession death edge: inventory + XP drop
            Id::ImmediateRespawn,                 // mirrored to clients (WorldRulesS2C), death screen skipped
            Id::PlayerMovementCheck,              // ServerPlayer::setPosition "moved too fast"
            Id::PlayersNetherPortalDefaultDelay,  // Portals::GetTransitionTime
            Id::PlayersNetherPortalCreativeDelay,
            Id::Pvp,                              // PlayerEntityView::Hurt
            Id::RespawnRadius,                    // PlayerSessionManager::OnPlayerRespawn (PlayerSpawnFinder)
            Id::PlayersSleepingPercentage,        // IntegratedServer sleep status (SleepStatus port)
            // Mobs
            Id::MaxEntityCramming,                // LivingEntity::PushEntities
            Id::UniversalAnger, Id::ForgiveDeadPlayers,   // NeutralMob
            // Spawning
            Id::SpawnMonsters,                    // RunNaturalSpawner category gate
            // Drops
            Id::BlockDrops, Id::MobDrops,         // ItemEntityManager::PopResource + block XP; MobManager::DropDeathLoot
            // Chat
            Id::ShowDeathMessages,                // PlayerSession death broadcast
            // Updates
            Id::WaterSourceConversion,            // Fluids::GetNewLiquid (FlowingFluid.canConvertToSource)
            Id::LavaSourceConversion,
            // Misc
            Id::AllowEnteringNetherUsingPortals,  // PortalTravel + immersive crossing
            Id::MaxCommandForks,                  // ExecuteCommand fork limit
            Id::SpawnerBlocksWork,                // SpawnerBlockEntity::Tick, the spawn egg's spawner branch
            Id::ReducedDebugInfo,                 // mirrored to clients, F3 reduced view
        };

        // GameRuleMap. Defaults at static init so a value read before any
        // world opens (a client's mirrored flags, a unit test) is the vanilla
        // default rather than zero.
        struct Store {
            std::array<std::atomic<int>, kCount> values;
            Store() {
                for (size_t i = 0; i < kCount; ++i) values[i].store(kDefs[i].defaultValue, std::memory_order_relaxed);
            }
        };
        Store& TheStore() {
            static Store store;
            return store;
        }

        bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
            }
            return true;
        }

    } // namespace

    const Def& GetDef(Id id) { return kDefs[static_cast<size_t>(id)]; }
    const Def* AllDefs() { return kDefs; }

    const Def* Find(std::string_view name) {
        constexpr std::string_view kNamespace = "minecraft:";
        if (name.size() > kNamespace.size() && EqualsIgnoreCase(name.substr(0, kNamespace.size()), kNamespace)) {
            name.remove_prefix(kNamespace.size());
        }
        for (const Def& def : kDefs) {
            if (EqualsIgnoreCase(name, def.key)) return &def;
        }
        for (const Def& def : kDefs) {
            if (def.legacyName && EqualsIgnoreCase(name, def.legacyName)) return &def;
        }
        return nullptr;
    }

    const char* CategoryName(Category category) {
        switch (category) {
            case Category::Player:   return "Player";
            case Category::Mobs:     return "Mobs";
            case Category::Spawning: return "Spawning";
            case Category::Drops:    return "Drops";
            case Category::Updates:  return "World Updates";
            case Category::Chat:     return "Chat";
            case Category::Misc:     return "Miscellaneous";
        }
        return "Miscellaneous";
    }

    int GetInt(Id id) {
        return TheStore().values[static_cast<size_t>(id)].load(std::memory_order_relaxed);
    }
    bool GetBool(Id id) { return GetInt(id) != 0; }

    void Set(Id id, int value) {
        const Def& def = GetDef(id);
        value = std::clamp(value, def.minValue, def.maxValue);
        TheStore().values[static_cast<size_t>(id)].store(value, std::memory_order_relaxed);
    }
    void SetBool(Id id, bool value) { Set(id, value ? 1 : 0); }

    void ResetAll() {
        for (size_t i = 0; i < kCount; ++i) {
            TheStore().values[i].store(kDefs[i].defaultValue, std::memory_order_relaxed);
        }
    }

    std::string Serialize(Id id, int value) {
        if (GetDef(id).type == Type::Bool) return value != 0 ? "true" : "false";
        return std::to_string(value);
    }
    std::string Serialize(Id id) { return Serialize(id, GetInt(id)); }

    std::optional<int> Parse(Id id, std::string_view text) {
        const Def& def = GetDef(id);
        if (def.type == Type::Bool) {
            if (EqualsIgnoreCase(text, "true"))  return 1;
            if (EqualsIgnoreCase(text, "false")) return 0;
            return std::nullopt;
        }
        if (text.empty()) return std::nullopt;
        const std::string owned(text);
        char* end = nullptr;
        const long long value = std::strtoll(owned.c_str(), &end, 10);
        if (!end || *end != '\0' || end == owned.c_str()) return std::nullopt;   // trailing junk, or no digits
        if (value < def.minValue || value > def.maxValue) return std::nullopt;
        return static_cast<int>(value);
    }

    bool IsImplemented(Id id) {
        for (Id implemented : kImplemented) if (implemented == id) return true;
        return false;
    }

} // namespace Game::Rules
