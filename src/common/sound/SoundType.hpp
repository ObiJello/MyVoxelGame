// File: src/common/sound/SoundType.hpp
//
// MC world.level.block.SoundType, BlockSetType and WoodType — the sound half of
// a block — and BlockBehaviour.getSoundType(state).
//
//   SoundType     volume, pitch and the break / step / place / hit / fall
//                 events (SoundType.java; 131 of them, generated).
//   BlockSetType  a door family's door / trapdoor / pressure-plate / button
//                 events and the sound type its blocks get (BlockSetType.java).
//   WoodType      a wood's sound type, hanging-sign sound type and fence-gate
//                 events (WoodType.java).
//
// Which block has which comes from Blocks.java through tools/gen_sounds.py,
// keyed by REGISTRY SLUG: the per-BlockID table is built from
// BlockRegistry slugs on first use, so a block appended to BlockDefs.inc picks
// its row up by name — and one the generator has never seen (an engine block
// with no SLUG_ALIASES entry yet) sounds like STONE, which is MC's own
// Properties default (BlockBehaviour.Properties.soundType = SoundType.STONE).
//
// The events are id strings ("" is SoundEvents.EMPTY — no sound), ready for
// Level::PlaySound.
#pragma once

#include "common/world/block/BlockState.hpp"
#include "common/world/block/Blocks.hpp"

namespace Game {

    struct SoundType {
        float       volume;
        float       pitch;
        const char* breakSound;
        const char* stepSound;
        const char* placeSound;
        const char* hitSound;
        const char* fallSound;

        float       GetVolume() const     { return volume; }
        float       GetPitch() const      { return pitch; }
        const char* GetBreakSound() const { return breakSound; }
        const char* GetStepSound() const  { return stepSound; }
        const char* GetPlaceSound() const { return placeSound; }
        const char* GetHitSound() const   { return hitSound; }
        const char* GetFallSound() const  { return fallSound; }
    };

    // SoundType.STONE, SoundType.WOOD, ... as named constants.
    namespace SoundTypes {
#define SOUND_TYPE(NAME, V, P, B, S, PL, H, F) extern const SoundType NAME;
#include "common/sound/GeneratedSoundTypes.inc"
#undef SOUND_TYPE
    }

    struct BlockSetType {
        const char*      name;
        const SoundType* soundType;
        const char*      doorClose;
        const char*      doorOpen;
        const char*      trapdoorClose;
        const char*      trapdoorOpen;
        const char*      pressurePlateClickOff;
        const char*      pressurePlateClickOn;
        const char*      buttonClickOff;
        const char*      buttonClickOn;
    };

    struct WoodType {
        const char*         name;
        const BlockSetType* setType;
        const SoundType*    soundType;
        const SoundType*    hangingSignSoundType;
        const char*         fenceGateClose;
        const char*         fenceGateOpen;
    };

    namespace BlockSetTypes {
#define BLOCK_SET_TYPE(NAME, ST, DC, DO, TC, TO, PF, PN, BF, BN) extern const BlockSetType NAME;
#define WOOD_TYPE(NAME, SET, ST, HST, GC, GO)
#include "common/sound/GeneratedBlockSetTypes.inc"
#undef BLOCK_SET_TYPE
#undef WOOD_TYPE
    }

    namespace WoodTypes {
#define BLOCK_SET_TYPE(NAME, ST, DC, DO, TC, TO, PF, PN, BF, BN)
#define WOOD_TYPE(NAME, SET, ST, HST, GC, GO) extern const WoodType NAME;
#include "common/sound/GeneratedBlockSetTypes.inc"
#undef BLOCK_SET_TYPE
#undef WOOD_TYPE
    }

    // MC BlockState.getSoundType(). By state because one vanilla block varies
    // it by state (DecoratedPotBlock: cracked → DECORATED_POT_CRACKED).
    const SoundType& SoundTypeOf(BlockState state);
    // The block's Properties sound, ignoring state.
    const SoundType& SoundTypeOf(BlockID block);

    // The BlockSetType a door / trapdoor / button / pressure plate / fence
    // gate was built with; null for every other block. A fence gate answers
    // its WoodType's set.
    const BlockSetType* BlockSetTypeOf(BlockID block);
    // The WoodType a sign or fence gate was built with; null otherwise.
    const WoodType* WoodTypeOf(BlockID block);

    // "" (SoundEvents.EMPTY) means no sound; callers test before playing.
    inline bool IsEmptySound(const char* event) { return event == nullptr || event[0] == '\0'; }

} // namespace Game
