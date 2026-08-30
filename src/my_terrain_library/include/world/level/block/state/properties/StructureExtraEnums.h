#pragma once

#include <string>
#include <vector>

// Compact enum types needed by structure-placed blocks (B7): note blocks,
// trial spawners and vaults. Reference: the same-named Java enums under
// net/minecraft/world/level/block/state/properties/.

namespace minecraft {
namespace world {
namespace level {
namespace block {
namespace state {
namespace properties {

#define MC_ENUM_COMMON(TYPE, DEFAULT)                                          \
    Value getValue() const { return m_value; }                                 \
    std::string getSerializedName() const { return m_name; }                   \
    bool operator==(const TYPE& other) const { return m_value == other.m_value; } \
    bool operator!=(const TYPE& other) const { return m_value != other.m_value; }

/** Reference: NoteBlockInstrument.java - declaration order. */
class NoteBlockInstrument {
public:
    enum Value {
        HARP, BASEDRUM, SNARE, HAT, BASS, FLUTE, BELL, GUITAR, CHIME,
        XYLOPHONE, IRON_XYLOPHONE, COW_BELL, DIDGERIDOO, BIT, BANJO, PLING,
        ZOMBIE, SKELETON, CREEPER, DRAGON, WITHER_SKELETON, PIGLIN, CUSTOM_HEAD
    };

private:
    Value m_value;
    std::string m_name;

public:
    NoteBlockInstrument() : NoteBlockInstrument(HARP) {}
    NoteBlockInstrument(Value value) : m_value(value) {
        static const char* names[] = {
            "harp", "basedrum", "snare", "hat", "bass", "flute", "bell",
            "guitar", "chime", "xylophone", "iron_xylophone", "cow_bell",
            "didgeridoo", "bit", "banjo", "pling", "zombie", "skeleton",
            "creeper", "dragon", "wither_skeleton", "piglin", "custom_head"};
        m_name = names[static_cast<int>(value)];
    }
    MC_ENUM_COMMON(NoteBlockInstrument, HARP)
    static std::vector<NoteBlockInstrument> values() {
        std::vector<NoteBlockInstrument> result;
        for (int i = 0; i <= static_cast<int>(CUSTOM_HEAD); ++i) {
            result.push_back(NoteBlockInstrument(static_cast<Value>(i)));
        }
        return result;
    }
};

/** Reference: TrialSpawnerState.java. */
class TrialSpawnerState {
public:
    enum Value {
        INACTIVE, WAITING_FOR_PLAYERS, ACTIVE, WAITING_FOR_REWARD_EJECTION,
        EJECTING_REWARD, COOLDOWN
    };

private:
    Value m_value;
    std::string m_name;

public:
    TrialSpawnerState() : TrialSpawnerState(INACTIVE) {}
    TrialSpawnerState(Value value) : m_value(value) {
        static const char* names[] = {
            "inactive", "waiting_for_players", "active",
            "waiting_for_reward_ejection", "ejecting_reward", "cooldown"};
        m_name = names[static_cast<int>(value)];
    }
    MC_ENUM_COMMON(TrialSpawnerState, INACTIVE)
    static std::vector<TrialSpawnerState> values() {
        std::vector<TrialSpawnerState> result;
        for (int i = 0; i <= static_cast<int>(COOLDOWN); ++i) {
            result.push_back(TrialSpawnerState(static_cast<Value>(i)));
        }
        return result;
    }
};

/** Reference: VaultState.java. */
class VaultState {
public:
    enum Value { INACTIVE, ACTIVE, UNLOCKING, EJECTING };

private:
    Value m_value;
    std::string m_name;

public:
    VaultState() : VaultState(INACTIVE) {}
    VaultState(Value value) : m_value(value) {
        static const char* names[] = {"inactive", "active", "unlocking", "ejecting"};
        m_name = names[static_cast<int>(value)];
    }
    MC_ENUM_COMMON(VaultState, INACTIVE)
    static std::vector<VaultState> values() {
        std::vector<VaultState> result;
        for (int i = 0; i <= static_cast<int>(EJECTING); ++i) {
            result.push_back(VaultState(static_cast<Value>(i)));
        }
        return result;
    }
};

#undef MC_ENUM_COMMON

} // namespace properties
} // namespace state
} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft

namespace std {
template<>
struct hash<minecraft::world::level::block::state::properties::NoteBlockInstrument> {
    size_t operator()(const minecraft::world::level::block::state::properties::NoteBlockInstrument& v) const {
        return std::hash<int>()(static_cast<int>(v.getValue()));
    }
};
template<>
struct hash<minecraft::world::level::block::state::properties::TrialSpawnerState> {
    size_t operator()(const minecraft::world::level::block::state::properties::TrialSpawnerState& v) const {
        return std::hash<int>()(static_cast<int>(v.getValue()));
    }
};
template<>
struct hash<minecraft::world::level::block::state::properties::VaultState> {
    size_t operator()(const minecraft::world::level::block::state::properties::VaultState& v) const {
        return std::hash<int>()(static_cast<int>(v.getValue()));
    }
};
}
