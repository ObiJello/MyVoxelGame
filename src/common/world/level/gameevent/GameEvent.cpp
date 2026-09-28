// File: src/common/world/level/gameevent/GameEvent.cpp
#include "GameEvent.hpp"

#include "common/world/tags/DataTags.hpp"

#include <array>
#include <mutex>
#include <string>

namespace Game::GameEvents {

    namespace {

        struct Row {
            std::string_view name;
            int              radius;
        };

        constexpr std::array<Row, kGameEventCount> kRows = {{
#define GAME_EVENT_ROW(Enum, name, radius) Row{name, radius},
            GAME_EVENT_LIST(GAME_EVENT_ROW)
#undef GAME_EVENT_ROW
        }};

        constexpr size_t Index(GameEventId e) { return static_cast<size_t>(e); }

        // MC VibrationSystem.VIBRATION_FREQUENCY_FOR_EVENT — every event not
        // listed maps to NO_VIBRATION_FREQUENCY (0); the fifteen resonate_N
        // events map to N.
        constexpr std::array<uint8_t, kGameEventCount> MakeFrequencies() {
            std::array<uint8_t, kGameEventCount> f{};
            using E = GameEventId;
            f[Index(E::Step)]               = 1;
            f[Index(E::Swim)]               = 1;
            f[Index(E::Flap)]               = 1;
            f[Index(E::ProjectileLand)]     = 2;
            f[Index(E::HitGround)]          = 2;
            f[Index(E::Splash)]             = 2;
            f[Index(E::Bounce)]             = 2;
            f[Index(E::ItemInteractFinish)] = 3;
            f[Index(E::ProjectileShoot)]    = 3;
            f[Index(E::InstrumentPlay)]     = 3;
            f[Index(E::EntityAction)]       = 4;
            f[Index(E::ElytraGlide)]        = 4;
            f[Index(E::Unequip)]            = 4;
            f[Index(E::EntityDismount)]     = 5;
            f[Index(E::Equip)]              = 5;
            f[Index(E::EntityInteract)]     = 6;
            f[Index(E::Shear)]              = 6;
            f[Index(E::EntityMount)]        = 6;
            f[Index(E::EntityDamage)]       = 7;
            f[Index(E::Drink)]              = 8;
            f[Index(E::Eat)]                = 8;
            f[Index(E::ContainerClose)]     = 9;
            f[Index(E::BlockClose)]         = 9;
            f[Index(E::BlockDeactivate)]    = 9;
            f[Index(E::BlockDetach)]        = 9;
            f[Index(E::ContainerOpen)]      = 10;
            f[Index(E::BlockOpen)]          = 10;
            f[Index(E::BlockActivate)]      = 10;
            f[Index(E::BlockAttach)]        = 10;
            f[Index(E::PrimeFuse)]          = 10;
            f[Index(E::NoteBlockPlay)]      = 10;
            f[Index(E::BlockChange)]        = 11;
            f[Index(E::BlockDestroy)]       = 12;
            f[Index(E::FluidPickup)]        = 12;
            f[Index(E::BlockPlace)]         = 13;
            f[Index(E::FluidPlace)]         = 13;
            f[Index(E::EntityPlace)]        = 14;
            f[Index(E::LightningStrike)]    = 14;
            f[Index(E::Teleport)]           = 14;
            f[Index(E::EntityDie)]          = 15;
            f[Index(E::Explode)]            = 15;
            for (int i = 1; i <= 15; ++i) {
                f[Index(E::Resonate1) + static_cast<size_t>(i - 1)] = static_cast<uint8_t>(i);
            }
            return f;
        }
        constexpr std::array<uint8_t, kGameEventCount> kFrequencies = MakeFrequencies();

        // Tag membership, one bit per GameEvents::Tag, resolved from the data
        // pack on first use.
        std::array<uint8_t, kGameEventCount> BuildTagTable() {
            std::array<uint8_t, kGameEventCount> t{};
            struct TagName { Tag tag; const char* name; };
            static constexpr TagName kTags[] = {
                { Tag::Vibrations,               "minecraft:vibrations" },
                { Tag::WardenCanListen,          "minecraft:warden_can_listen" },
                { Tag::ShriekerCanListen,        "minecraft:shrieker_can_listen" },
                { Tag::IgnoreVibrationsSneaking, "minecraft:ignore_vibrations_sneaking" },
                { Tag::AllayCanListen,           "minecraft:allay_can_listen" },
            };
            for (size_t i = 0; i < kGameEventCount; ++i) {
                const std::string id = "minecraft:" + std::string(kRows[i].name);
                for (const TagName& tn : kTags) {
                    if (DataTags::HasTag(DataTags::Registry::GameEvent, id, tn.name)) {
                        t[i] |= static_cast<uint8_t>(1u << static_cast<unsigned>(tn.tag));
                    }
                }
            }
            return t;
        }

        const std::array<uint8_t, kGameEventCount>& TagTable() {
            static const std::array<uint8_t, kGameEventCount> table = BuildTagTable();
            return table;
        }

    } // namespace

    int NotificationRadius(GameEventId event) {
        const size_t i = Index(event);
        return i < kGameEventCount ? kRows[i].radius : kDefaultGameEventRadius;
    }

    std::string_view Name(GameEventId event) {
        const size_t i = Index(event);
        return i < kGameEventCount ? kRows[i].name : std::string_view("unknown");
    }

    std::optional<GameEventId> FromName(std::string_view name) {
        if (name.rfind("minecraft:", 0) == 0) name.remove_prefix(10);
        for (size_t i = 0; i < kGameEventCount; ++i) {
            if (kRows[i].name == name) return static_cast<GameEventId>(i);
        }
        return std::nullopt;
    }

    bool Is(GameEventId event, Tag tag) {
        const size_t i = Index(event);
        if (i >= kGameEventCount) return false;
        return (TagTable()[i] & static_cast<uint8_t>(1u << static_cast<unsigned>(tag))) != 0;
    }

    int Frequency(GameEventId event) {
        const size_t i = Index(event);
        return i < kGameEventCount ? kFrequencies[i] : 0;
    }

    GameEventId ResonanceEventForFrequency(int frequency) {
        if (frequency < 1) frequency = 1;
        if (frequency > 15) frequency = 15;
        return static_cast<GameEventId>(static_cast<size_t>(GameEventId::Resonate1) +
                                        static_cast<size_t>(frequency - 1));
    }

} // namespace Game::GameEvents
