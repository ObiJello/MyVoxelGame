// File: src/client/renderer/entity/model/SetupAnimRunner.hpp
//
// Executes a compiled MC setupAnim (see tools/gen_setup_anim.py).
//
// The program is a flat list of statements over a postfix expression tape.
// Baking resolves, ONCE, everything the tape names by string: each
// statement's part NAME to a live ModelPart, each `Part` node's "part|field"
// to a pointer and a field enum, and each `State`/`BState` node's render-
// state field name to a StateRef index. A frame is then a walk over pointers,
// small enums and a fixed float stack — no string compares, no allocation.
//
// (The header used to claim that while Run still resolved state names with
// a chain of up to sixty string_view compares per node and allocated its
// locals vector per call — at a hundred mobs that was the measurable part of
// the per-mob cost. Both are now bake-time.)
//
// THREADING: a SetupAnimProgram belongs to one EntityModel, and the models
// are shared per entity TYPE and posed serially from the render thread —
// Run mutates the model's parts, so it was never re-entrant. The locals
// scratch is a member for the same reason it is safe: one caller at a time.
#pragma once

#include "client/renderer/entity/model/GeneratedSetupAnim.hpp"

#include <cstdint>
#include <vector>

namespace Render {

    class ModelPart;
    struct EntityRenderState;

    class SetupAnimProgram {
    public:
        SetupAnimProgram() = default;

        // Statements naming a part the mesh does not have are dropped, not
        // fatal: the mesh and the model class can legitimately disagree when
        // MC builds the layer from a different class than it animates.
        static SetupAnimProgram Bake(ModelPart& root, const AnimProgram& prog);

        bool Valid() const { return m_prog != nullptr; }

        // Non-const: writes the parts it was baked against and uses the
        // member locals scratch. See the threading note above.
        void Run(const EntityRenderState& state);

    private:
        // Every render-state field a generated program can read, in one
        // index space for floats and booleans alike. Resolved from the tape's
        // name at bake; Run switches on it. `None` is a name the tape carries
        // that this port's state does not — it reads as 0, exactly what the
        // string path answered for an unknown name.
        enum class StateRef : uint8_t {
            None,
            // Floats (AnimOp::State).
            WalkPos, WalkSpeed, AgeInTicks, XRot, YRot, AttackTime, AgeScale,
            Flap, FlapSpeed, SpeedValue, SwimAmount, MainArm, RightArmPose,
            LeftArmPose, Squish, FlapTime, TentacleAngle, EatAnim, StandAnim,
            FeedingAnim, PlayingDead, InWaterFactor, OnGroundFactor,
            MovingFactor, StandScale, RammingXHeadRot, AttackTicksRemaining,
            AttackAnimRemaining, StunnedTicks, JumpCompletion, HoldingProgress,
            TendrilAnim, SpikesAnim, TailAnim, EntityId, JumpCooldown,
            HeadRollAngle, TailAngle, LieDown, LieDownTail, RelaxOne, SitAmount,
            LieOnBack, RollAmount, SneezeTime, CrouchAmount, PeekAmount,
            SpinningProgress, OfferFlowerTick, RoarAnim, YHeadRotAbs,
            YBodyRotAbs, MobArmPose, MobPose, SwingAnimType,
            // Booleans (AnimOp::BState), read as 1.0 / 0.0.
            IsAggressive, IsBaby, IsCrouching, IsSprinting, IsInWater,
            IsOnGround, IsFallFlying, IsPassenger, IsUsingItem, IsSitting,
            IsHoldingBow, IsHidingInShell, IsResting, CanMove, IsSearching,
            IsHoldingItem, IsMoving, AnimateTail, HasChest, HasLeftHorn,
            HasRightHorn, HasEgg, IsOnLand, IsLayingEgg, IsAngry, HasStinger,
            IsSheared, IsUnhappy, IsCharging, IsRidden, IsCreepy, IsDancing,
            IsFaceplanted, IsSwimming, IsSleeping, IsSpinning, IsSneezing,
            IsEating, IsScared, HasMainHandItem,
        };

        // What one tape node needs at run time, resolved at bake.
        //
        // A `Part` node READS a part's current value — MC's
        // `Mth.clamp(this.rightArm.xRot, -0.4F, 0.4F)` and every
        // `Mth.rotLerpRad(t, this.head.xRot, …)`. Resolving those at bake time
        // matters: evaluating them as zero silently turns a clamp into a
        // constant and a rotLerp into a snap.
        struct NodeBind {
            ModelPart* part = nullptr;          // Part nodes
            PartField  field = PartField::XRot; // Part nodes
            StateRef   ref = StateRef::None;    // State / BState nodes
        };

        static StateRef ResolveStateRef(std::string_view name);
        static float ReadState(const EntityRenderState& s, StateRef ref);

        const AnimProgram* m_prog = nullptr;
        std::vector<ModelPart*> m_parts;      // one per statement, null = skip
        std::vector<NodeBind>   m_nodes;      // one per node in [m_nodeLo, m_nodeHi)
        int m_nodeLo = 0;
        // Sized once at bake (program.localCount, at least 1) and reused by
        // every Run; a SetLocal always precedes any read of that local in a
        // generated program, so no per-call clear is needed — but Run clears
        // anyway, the cost is a handful of floats and it keeps a program that
        // reads a local before writing it at MC's Java default of 0.
        std::vector<float> m_locals;
    };

} // namespace Render
