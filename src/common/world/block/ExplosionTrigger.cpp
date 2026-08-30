// File: src/common/world/block/ExplosionTrigger.cpp
#include "common/world/block/ExplosionTrigger.hpp"

#include "common/core/SoundEvents.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/Blocks.hpp"
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

    } // namespace

    bool ExplosionTriggerBlock(ILevelWrite& level, const glm::ivec3& pos,
                               BlockState state) {
        const BlockID id = state.Block();
        if (id == BlockID::Air) return false;

        const std::string& model = BlockRegistry::Get(id).modelName;

        // ── ButtonBlock.onExplosionHit → press ────────────────────────────
        if (NameHas(model, "_button")) {
            if (IsPowered(state)) return false;
            // MC press(): POWERED = true, notify neighbours, schedule the
            // release tick, play the sound.
            const BlockState next = state.SetName(PropertyId::POWERED, "true");
            if (!WriteState(level, pos, next)) return false;
            if (auto* ticks = level.Ticks()) {
                // MC ButtonBlock.ticksToStayPressed — 30 for a stone button,
                // 20 for wood. Wooden buttons are the ones an arrow can hit.
                const int hold = NameHas(model, "stone") ||
                                 NameHas(model, "blackstone") ? 20 : 30;
                ticks->ScheduleTick(pos, id, hold);
            }
            PlaySound("block.stone_button.click_on", pos);
            return true;
        }

        // ── LeverBlock.onExplosionHit → pull ──────────────────────────────
        if (model == "lever") {
            const bool nowOn = !IsPowered(state);
            const BlockState next =
                state.SetName(PropertyId::POWERED, nowOn ? "true" : "false");
            if (!WriteState(level, pos, next)) return false;
            PlaySound("block.lever.click", pos);
            return true;
        }

        // ── FenceGateBlock.onExplosionHit → toggle OPEN ───────────────────
        if (NameHas(model, "_fence_gate")) {
            if (IsPowered(state)) return false;
            const bool wasOpen = state.GetIndex(PropertyId::OPEN) == 0;
            const BlockState next =
                state.SetName(PropertyId::OPEN, wasOpen ? "false" : "true");
            if (!WriteState(level, pos, next)) return false;
            PlaySound(wasOpen ? "block.fence_gate.close"
                              : "block.fence_gate.open", pos);
            return true;
        }

        // ── TrapDoorBlock.onExplosionHit → toggle OPEN ────────────────────
        if (NameHas(model, "_trapdoor")) {
            if (!OpensByWindCharge(model) || IsPowered(state)) return false;
            const bool wasOpen = state.GetIndex(PropertyId::OPEN) == 0;
            const BlockState next =
                state.SetName(PropertyId::OPEN, wasOpen ? "false" : "true");
            if (!WriteState(level, pos, next)) return false;
            PlaySound(wasOpen ? "block.wooden_trapdoor.close"
                              : "block.wooden_trapdoor.open", pos);
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
            PlaySound(wasOpen ? "block.wooden_door.close"
                              : "block.wooden_door.open", pos);
            return true;
        }

        // ── AbstractCandleBlock.onExplosionHit → extinguish ───────────────
        if (NameHas(model, "candle")) {
            // BooleanProperty [true, false] — index 0 is lit.
            if (state.GetIndex(PropertyId::LIT) != 0) return false;
            if (!WriteState(level, pos, state.SetName(PropertyId::LIT, "false"))) {
                return false;
            }
            PlaySound("block.candle.extinguish", pos);
            return true;
        }

        // ── BellBlock.onExplosionHit → ring ───────────────────────────────
        //
        // The bell's swing lives on its block entity (BellBlockEntity's
        // `shaking`/`clickDirection`), which this engine does not carry, so
        // only the sound site is real. Named here rather than omitted so the
        // ring is a one-line addition the day that block entity exists.
        if (model == "bell") {
            PlaySound("block.bell.use", pos);
            return false;
        }

        return false;
    }

} // namespace Game
