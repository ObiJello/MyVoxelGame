// File: src/common/world/block/ExplosionTrigger.cpp
#include "common/world/block/ExplosionTrigger.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/sound/SoundType.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/block/RedstoneComponents.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"

#include <string>
#include <string_view>

namespace Game {

    namespace {

        bool NameHas(const std::string& n, const char* what) {
            return n.find(what) != std::string::npos;
        }

        // MC BlockSetType.canOpenByWindCharge — true for every WOODEN door and
        // trapdoor and false for iron and copper, which is the whole point: a
        // wind charge is a wooden-door key, not a lockpick.
        bool OpensByWindCharge(const std::string& modelName) {
            return !NameHas(modelName, "iron_") && !NameHas(modelName, "copper_");
        }

        bool IsPowered(BlockState s) {
            // BooleanProperty lists [true, false], so index 0 is true.
            return s.GetIndex(PropertyId::POWERED) == 0;
        }

        bool WriteState(ILevelWrite& level, const glm::ivec3& pos, BlockState next) {
            return level.SetBlock(pos.x, pos.y, pos.z, next, World::UpdateFlags::All);
        }

        // MC level.getRandom().nextFloat() * 0.1F + 0.9F — the door / gate
        // pitch jitter. Explosions only run on the server, which has a random.
        float JitterPitch(ILevelWrite& level) {
            JavaRandom* r = level.Random();
            return r ? r->NextFloat() * 0.1f + 0.9f : 1.0f;
        }

    } // namespace

    bool ExplosionTriggerBlock(ILevelWrite& level, const glm::ivec3& pos,
                               BlockState state) {
        const BlockID id = state.Block();
        if (id == BlockID::Air) return false;

        const std::string& model = BlockRegistry::Get(id).modelName;

        // ── ButtonBlock / LeverBlock.onExplosionHit → press / pull ────────
        //
        // The redstone port's own press and pull (RedstoneComponents), so a
        // wind-charged button powers the block it hangs on and releases on
        // its ticksToStayPressed, exactly as a clicked one.
        if (NameHas(model, "_button") || model == "lever") {
            return RedstoneComponentOnExplosionHit(level, pos, state);
        }

        // ── FenceGateBlock.onExplosionHit → toggle OPEN ───────────────────
        if (NameHas(model, "_fence_gate")) {
            if (IsPowered(state)) return false;
            const bool wasOpen = state.GetIndex(PropertyId::OPEN) == 0;
            const BlockState next =
                state.SetName(PropertyId::OPEN, wasOpen ? "false" : "true");
            if (!WriteState(level, pos, next)) return false;
            // MC FenceGateBlock.onExplosionHit:138 — the WoodType's gate sound.
            if (const WoodType* wood = WoodTypeOf(id)) {
                level.PlaySound(nullptr, pos, wasOpen ? wood->fenceGateClose : wood->fenceGateOpen,
                                SoundSource::Blocks, 1.0f, JitterPitch(level));
            }
            // :139 gameEvent(open ? BLOCK_CLOSE : BLOCK_OPEN, pos, Context.of(state)).
            level.GameEvent(wasOpen ? GameEventId::BlockClose : GameEventId::BlockOpen, pos,
                            GameEventContext::Of(state));
            return true;
        }

        // ── TrapDoorBlock.onExplosionHit → toggle OPEN ────────────────────
        if (NameHas(model, "_trapdoor")) {
            if (!OpensByWindCharge(model) || IsPowered(state)) return false;
            const bool wasOpen = state.GetIndex(PropertyId::OPEN) == 0;
            const BlockState next =
                state.SetName(PropertyId::OPEN, wasOpen ? "false" : "true");
            if (!WriteState(level, pos, next)) return false;
            // MC TrapDoorBlock.toggle → playSound(null, ..., opening).
            if (const BlockSetType* set = BlockSetTypeOf(id)) {
                level.PlaySound(nullptr, pos, wasOpen ? set->trapdoorClose : set->trapdoorOpen,
                                SoundSource::Blocks, 1.0f, JitterPitch(level));
            }
            // toggle → gameEvent(null, open ? BLOCK_OPEN : BLOCK_CLOSE, pos).
            level.GameEvent(nullptr, wasOpen ? GameEventId::BlockClose : GameEventId::BlockOpen, pos);
            return true;
        }

        // ── DoorBlock.onExplosionHit → toggle OPEN, LOWER half only ───────
        //
        // MC drives the toggle from the lower half and setOpen writes BOTH
        // halves, so the guard is what stops a two-block door being toggled
        // twice by one blast (once per half) and ending up back where it was.
        if (NameHas(model, "_door")) {
            if (!OpensByWindCharge(model) || IsPowered(state)) return false;
            if (state.GetValueByName("half") != "lower") return false;

            const bool wasOpen = state.GetIndex(PropertyId::OPEN) == 0;
            const std::string_view to = wasOpen ? "false" : "true";
            if (!WriteState(level, pos, state.SetName(PropertyId::OPEN, to))) {
                return false;
            }
            // MC setOpen mirrors the upper half; without this the door renders
            // half open.
            const glm::ivec3 upper = pos + glm::ivec3(0, 1, 0);
            const BlockState upperState =
                level.GetBlockState(upper.x, upper.y, upper.z);
            if (upperState.Block() == id &&
                upperState.GetValueByName("half") == "upper") {
                WriteState(level, upper, upperState.SetName(PropertyId::OPEN, to));
            }
            // MC DoorBlock.setOpen → playSound(null, ..., open).
            if (const BlockSetType* set = BlockSetTypeOf(id)) {
                level.PlaySound(nullptr, pos, wasOpen ? set->doorClose : set->doorOpen,
                                SoundSource::Blocks, 1.0f, JitterPitch(level));
            }
            // setOpen → gameEvent(null, open ? BLOCK_OPEN : BLOCK_CLOSE, pos).
            level.GameEvent(nullptr, wasOpen ? GameEventId::BlockClose : GameEventId::BlockOpen, pos);
            return true;
        }

        // ── AbstractCandleBlock.onExplosionHit → extinguish ───────────────
        if (NameHas(model, "candle")) {
            // BooleanProperty [true, false] — index 0 is lit.
            if (state.GetIndex(PropertyId::LIT) != 0) return false;
            if (!WriteState(level, pos, state.SetName(PropertyId::LIT, "false"))) {
                return false;
            }
            // MC AbstractCandleBlock.extinguish:81 — the sound, then
            // gameEvent(null, BLOCK_CHANGE, pos).
            level.PlaySound(nullptr, pos, SoundEvents::CANDLE_EXTINGUISH, SoundSource::Blocks, 1.0f, 1.0f);
            level.GameEvent(nullptr, GameEventId::BlockChange, pos);
            return true;
        }

        // ── BellBlock.onExplosionHit → ring ───────────────────────────────
        //
        // The bell's swing lives on its block entity (BellBlockEntity's
        // `shaking`/`clickDirection`), which this engine does not carry, so
        // only the sound is real: MC attemptToRing:144, BELL_BLOCK at 2.0.
        if (model == "bell") {
            level.PlaySound(nullptr, pos, SoundEvents::BELL_BLOCK, SoundSource::Blocks, 2.0f, 1.0f);
            // attemptToRing → gameEvent(null, BLOCK_CHANGE, pos).
            level.GameEvent(nullptr, GameEventId::BlockChange, pos);
            return false;
        }

        return false;
    }

} // namespace Game
