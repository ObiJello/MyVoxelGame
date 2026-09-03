// File: src/client/renderer/entity/model/SetupAnimRunner.cpp
#include "client/renderer/entity/model/SetupAnimRunner.hpp"
#include "client/renderer/entity/model/EntityModels.hpp"
#include "client/renderer/entity/model/ModelPart.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace Render {

    namespace {

        constexpr float kPi = 3.14159265358979323846f;

        // MC Mth, for the handful setupAnim actually calls. These are ports,
        // not equivalents — MC's wrapDegrees and rotLerp have specific
        // behaviour at the wrap boundary that a naive version gets wrong by a
        // full turn exactly when a mob crosses due south.
        float WrapDegrees(float v) {
            float w = std::fmod(v, 360.0f);
            if (w >= 180.0f) w -= 360.0f;
            if (w < -180.0f) w += 360.0f;
            return w;
        }

        float RotLerp(float t, float a, float b) {
            return a + t * WrapDegrees(b - a);
        }

        float WrapRadians(float v) {
            float w = std::fmod(v, 2.0f * kPi);
            if (w >= kPi) w -= 2.0f * kPi;
            if (w < -kPi) w += 2.0f * kPi;
            return w;
        }

        float RotLerpRad(float t, float a, float b) {
            return a + t * WrapRadians(b - a);
        }

        // MC Mth.triangleWave — a linear zig-zag, not a sine.
        float TriangleWave(float v, float period) {
            return (std::abs(std::fmod(v, period) - period * 0.5f)
                    - period * 0.25f) / (period * 0.25f);
        }

    } // namespace

    // Name -> index, once per node at bake. The names are the generator's
    // (tools/gen_setup_anim.py) and this table is the only place they are
    // spelled at runtime; an unknown name resolves to None and reads as 0.
    SetupAnimProgram::StateRef SetupAnimProgram::ResolveStateRef(std::string_view n) {
        struct Entry { std::string_view name; StateRef ref; };
        static constexpr Entry kTable[] = {
            { "WalkPos",              StateRef::WalkPos },
            { "WalkSpeed",            StateRef::WalkSpeed },
            { "AgeInTicks",           StateRef::AgeInTicks },
            { "XRot",                 StateRef::XRot },
            { "YRot",                 StateRef::YRot },
            { "AttackTime",           StateRef::AttackTime },
            { "AgeScale",             StateRef::AgeScale },
            { "Flap",                 StateRef::Flap },
            { "FlapSpeed",            StateRef::FlapSpeed },
            { "SpeedValue",           StateRef::SpeedValue },
            { "SwimAmount",           StateRef::SwimAmount },
            { "MainArm",              StateRef::MainArm },
            { "RightArmPose",         StateRef::RightArmPose },
            { "LeftArmPose",          StateRef::LeftArmPose },
            { "Squish",               StateRef::Squish },
            { "FlapTime",             StateRef::FlapTime },
            { "TentacleAngle",        StateRef::TentacleAngle },
            { "EatAnim",              StateRef::EatAnim },
            { "StandAnim",            StateRef::StandAnim },
            { "FeedingAnim",          StateRef::FeedingAnim },
            { "PlayingDead",          StateRef::PlayingDead },
            { "InWaterFactor",        StateRef::InWaterFactor },
            { "OnGroundFactor",       StateRef::OnGroundFactor },
            { "MovingFactor",         StateRef::MovingFactor },
            { "StandScale",           StateRef::StandScale },
            { "RammingXHeadRot",      StateRef::RammingXHeadRot },
            { "AttackTicksRemaining", StateRef::AttackTicksRemaining },
            { "AttackAnimRemaining",  StateRef::AttackAnimRemaining },
            { "StunnedTicks",         StateRef::StunnedTicks },
            { "JumpCompletion",       StateRef::JumpCompletion },
            { "HoldingProgress",      StateRef::HoldingProgress },
            { "TendrilAnim",          StateRef::TendrilAnim },
            { "SpikesAnim",           StateRef::SpikesAnim },
            { "TailAnim",             StateRef::TailAnim },
            { "EntityId",             StateRef::EntityId },
            { "JumpCooldown",         StateRef::JumpCooldown },
            { "HeadRollAngle",        StateRef::HeadRollAngle },
            { "TailAngle",            StateRef::TailAngle },
            { "LieDown",              StateRef::LieDown },
            { "LieDownTail",          StateRef::LieDownTail },
            { "RelaxOne",             StateRef::RelaxOne },
            { "SitAmount",            StateRef::SitAmount },
            { "LieOnBack",            StateRef::LieOnBack },
            { "RollAmount",           StateRef::RollAmount },
            { "SneezeTime",           StateRef::SneezeTime },
            { "CrouchAmount",         StateRef::CrouchAmount },
            { "PeekAmount",           StateRef::PeekAmount },
            { "SpinningProgress",     StateRef::SpinningProgress },
            { "OfferFlowerTick",      StateRef::OfferFlowerTick },
            { "RoarAnim",             StateRef::RoarAnim },
            { "YHeadRotAbs",          StateRef::YHeadRotAbs },
            { "YBodyRotAbs",          StateRef::YBodyRotAbs },
            { "MobArmPose",           StateRef::MobArmPose },
            { "MobPose",              StateRef::MobPose },
            { "SwingAnimType",        StateRef::SwingAnimType },
            { "IsAggressive",         StateRef::IsAggressive },
            { "IsBaby",               StateRef::IsBaby },
            { "IsCrouching",          StateRef::IsCrouching },
            { "IsSprinting",          StateRef::IsSprinting },
            { "IsInWater",            StateRef::IsInWater },
            { "IsOnGround",           StateRef::IsOnGround },
            { "IsFallFlying",         StateRef::IsFallFlying },
            { "IsPassenger",          StateRef::IsPassenger },
            { "IsUsingItem",          StateRef::IsUsingItem },
            { "IsSitting",            StateRef::IsSitting },
            { "IsHoldingBow",         StateRef::IsHoldingBow },
            { "IsHidingInShell",      StateRef::IsHidingInShell },
            { "IsResting",            StateRef::IsResting },
            { "CanMove",              StateRef::CanMove },
            { "IsSearching",          StateRef::IsSearching },
            { "IsHoldingItem",        StateRef::IsHoldingItem },
            { "IsMoving",             StateRef::IsMoving },
            { "AnimateTail",          StateRef::AnimateTail },
            { "HasChest",             StateRef::HasChest },
            { "HasLeftHorn",          StateRef::HasLeftHorn },
            { "HasRightHorn",         StateRef::HasRightHorn },
            { "HasEgg",               StateRef::HasEgg },
            { "IsOnLand",             StateRef::IsOnLand },
            { "IsLayingEgg",          StateRef::IsLayingEgg },
            { "IsAngry",              StateRef::IsAngry },
            { "HasStinger",           StateRef::HasStinger },
            { "IsSheared",            StateRef::IsSheared },
            { "IsUnhappy",            StateRef::IsUnhappy },
            { "IsCharging",           StateRef::IsCharging },
            { "IsRidden",             StateRef::IsRidden },
            { "IsCreepy",             StateRef::IsCreepy },
            { "IsDancing",            StateRef::IsDancing },
            { "IsFaceplanted",        StateRef::IsFaceplanted },
            { "IsSwimming",           StateRef::IsSwimming },
            { "IsSleeping",           StateRef::IsSleeping },
            { "IsSpinning",           StateRef::IsSpinning },
            { "IsSneezing",           StateRef::IsSneezing },
            { "IsEating",             StateRef::IsEating },
            { "IsScared",             StateRef::IsScared },
            { "HasMainHandItem",      StateRef::HasMainHandItem },
        };
        for (const Entry& e : kTable) {
            if (e.name == n) return e.ref;
        }
        return StateRef::None;
    }

    float SetupAnimProgram::ReadState(const EntityRenderState& s, StateRef ref) {
        const auto b = [](bool v) { return v ? 1.0f : 0.0f; };
        switch (ref) {
            case StateRef::None:                 return 0.0f;
            case StateRef::WalkPos:              return s.walkAnimationPos;
            case StateRef::WalkSpeed:            return s.walkAnimationSpeed;
            case StateRef::AgeInTicks:           return s.ageInTicks;
            case StateRef::XRot:                 return s.xRot;
            case StateRef::YRot:                 return s.yRot;
            case StateRef::AttackTime:           return s.attackTime;
            case StateRef::AgeScale:             return s.ageScale;
            case StateRef::Flap:                 return s.flap;
            case StateRef::FlapSpeed:            return s.flapSpeed;
            case StateRef::SpeedValue:           return s.speedValue;
            case StateRef::SwimAmount:           return s.swimAmount;
            case StateRef::MainArm:              return s.mainArm;
            case StateRef::RightArmPose:         return static_cast<float>(s.rightArmPose);
            case StateRef::LeftArmPose:          return static_cast<float>(s.leftArmPose);
            case StateRef::Squish:               return s.squish;
            case StateRef::FlapTime:             return s.flapTime;
            case StateRef::TentacleAngle:        return s.tentacleAngle;
            case StateRef::EatAnim:              return s.eatAnimation;
            case StateRef::StandAnim:            return s.standAnimation;
            case StateRef::FeedingAnim:          return s.feedingAnimation;
            case StateRef::PlayingDead:          return s.playingDeadFactor;
            case StateRef::InWaterFactor:        return s.inWaterFactor;
            case StateRef::OnGroundFactor:       return s.onGroundFactor;
            case StateRef::MovingFactor:         return s.movingFactor;
            case StateRef::StandScale:           return s.standScale;
            case StateRef::RammingXHeadRot:      return s.rammingXHeadRot;
            case StateRef::AttackTicksRemaining: return s.attackTicksRemaining;
            case StateRef::AttackAnimRemaining:  return s.attackAnimationRemainingTicks;
            case StateRef::StunnedTicks:         return s.stunnedTicksRemaining;
            case StateRef::JumpCompletion:       return s.jumpCompletion;
            case StateRef::HoldingProgress:      return s.holdingAnimationProgress;
            case StateRef::TendrilAnim:          return s.tendrilAnimation;
            case StateRef::SpikesAnim:           return s.spikesAnimation;
            case StateRef::TailAnim:             return s.tailAnimation;
            case StateRef::EntityId:             return s.entityId;
            case StateRef::JumpCooldown:         return s.jumpCooldown;
            case StateRef::HeadRollAngle:        return s.headRollAngle;
            case StateRef::TailAngle:            return s.tailAngle;
            case StateRef::LieDown:              return s.lieDownAmount;
            case StateRef::LieDownTail:          return s.lieDownAmountTail;
            case StateRef::RelaxOne:             return s.relaxStateOneAmount;
            case StateRef::SitAmount:            return s.sitAmount;
            case StateRef::LieOnBack:            return s.lieOnBackAmount;
            case StateRef::RollAmount:           return s.rollAmount;
            case StateRef::SneezeTime:           return s.sneezeTime;
            case StateRef::CrouchAmount:         return s.crouchAmount;
            case StateRef::PeekAmount:           return s.peekAmount;
            case StateRef::SpinningProgress:     return s.spinningProgress;
            case StateRef::OfferFlowerTick:      return s.offerFlowerTick;
            case StateRef::RoarAnim:             return s.roarAnimation;
            case StateRef::YHeadRotAbs:          return s.yHeadRotAbs;
            case StateRef::YBodyRotAbs:          return s.yBodyRotAbs;
            case StateRef::MobArmPose:           return s.mobArmPose;
            case StateRef::MobPose:              return s.mobPose;
            case StateRef::SwingAnimType:        return s.swingAnimType;
            case StateRef::IsAggressive:         return b(s.isAggressive);
            case StateRef::IsBaby:               return b(s.isBaby);
            case StateRef::IsCrouching:          return b(s.isCrouching);
            case StateRef::IsSprinting:          return b(s.isSprinting);
            case StateRef::IsInWater:            return b(s.isInWater);
            case StateRef::IsOnGround:           return b(s.isOnGround);
            case StateRef::IsFallFlying:         return b(s.isFallFlying);
            case StateRef::IsPassenger:          return b(s.isPassenger);
            case StateRef::IsUsingItem:          return b(s.isUsingItem);
            case StateRef::IsSitting:            return b(s.isSitting);
            case StateRef::IsHoldingBow:         return b(s.isHoldingBow);
            case StateRef::IsHidingInShell:      return b(s.isHidingInShell);
            case StateRef::IsResting:            return b(s.isResting);
            case StateRef::CanMove:              return b(s.canMove);
            case StateRef::IsSearching:          return b(s.isSearching);
            case StateRef::IsHoldingItem:        return b(s.isHoldingItem);
            case StateRef::IsMoving:             return b(s.isMoving);
            case StateRef::AnimateTail:          return b(s.animateTail);
            case StateRef::HasChest:             return b(s.hasChest);
            case StateRef::HasLeftHorn:          return b(s.hasLeftHorn);
            case StateRef::HasRightHorn:         return b(s.hasRightHorn);
            case StateRef::HasEgg:               return b(s.hasEgg);
            case StateRef::IsOnLand:             return b(s.isOnLand);
            case StateRef::IsLayingEgg:          return b(s.isLayingEgg);
            case StateRef::IsAngry:              return b(s.isAngry);
            case StateRef::HasStinger:           return b(s.hasStinger);
            case StateRef::IsSheared:            return b(s.isSheared);
            case StateRef::IsUnhappy:            return b(s.isUnhappy);
            case StateRef::IsCharging:           return b(s.isCharging);
            case StateRef::IsRidden:             return b(s.isRidden);
            case StateRef::IsCreepy:             return b(s.isCreepy);
            case StateRef::IsDancing:            return b(s.isDancing);
            case StateRef::IsFaceplanted:        return b(s.isFaceplanted);
            case StateRef::IsSwimming:           return b(s.isSwimming);
            case StateRef::IsSleeping:           return b(s.isSleeping);
            case StateRef::IsSpinning:           return b(s.isSpinning);
            case StateRef::IsSneezing:           return b(s.isSneezing);
            case StateRef::IsEating:             return b(s.isEating);
            case StateRef::IsScared:             return b(s.isScared);
            case StateRef::HasMainHandItem:      return b(s.hasMainHandItem);
        }
        return 0.0f;
    }

    SetupAnimProgram SetupAnimProgram::Bake(ModelPart& root, const AnimProgram& prog) {
        SetupAnimProgram out;
        out.m_prog = &prog;
        out.m_parts.reserve(static_cast<size_t>(prog.statementCount));

        const auto find = [&root](std::string_view n) -> ModelPart* {
            return n == "root" ? &root : root.Find(std::string(n));
        };

        int lo = INT32_MAX, hi = 0;
        for (int i = 0; i < prog.statementCount; ++i) {
            const AnimStatement& st = kAnimStatements[prog.firstStatement + i];
            out.m_parts.push_back(
                (st.kind == AnimStmt::SetLocal || st.part.empty()) ? nullptr
                                                                  : find(st.part));
            if (st.nodeCount > 0) {
                lo = std::min(lo, st.firstNode);
                hi = std::max(hi, st.firstNode + st.nodeCount);
            }
            if (st.guardNodeCount > 0) {
                lo = std::min(lo, st.firstGuardNode);
                hi = std::max(hi, st.firstGuardNode + st.guardNodeCount);
            }
        }

        out.m_locals.assign(static_cast<size_t>(std::max(1, prog.localCount)), 0.0f);

        if (hi > lo) {
            out.m_nodeLo = lo;
            out.m_nodes.assign(static_cast<size_t>(hi - lo), NodeBind{});
            for (int i = lo; i < hi; ++i) {
                const AnimNode& n = kAnimNodes[i];
                NodeBind& bind = out.m_nodes[static_cast<size_t>(i - lo)];
                if (n.op == AnimOp::State || n.op == AnimOp::BState) {
                    bind.ref = ResolveStateRef(n.name);
                    continue;
                }
                if (n.op != AnimOp::Part) continue;
                const size_t bar = n.name.find('|');
                if (bar == std::string_view::npos) continue;
                const std::string_view pname = n.name.substr(0, bar);
                const std::string_view fname = n.name.substr(bar + 1);
                PartField f = PartField::XRot;
                if      (fname == "X")      f = PartField::X;
                else if (fname == "Y")      f = PartField::Y;
                else if (fname == "Z")      f = PartField::Z;
                else if (fname == "XRot")   f = PartField::XRot;
                else if (fname == "YRot")   f = PartField::YRot;
                else if (fname == "ZRot")   f = PartField::ZRot;
                else if (fname == "XScale") f = PartField::XScale;
                else if (fname == "YScale") f = PartField::YScale;
                else if (fname == "ZScale") f = PartField::ZScale;
                bind.part = find(pname);
                bind.field = f;
            }
        }
        return out;
    }

    void SetupAnimProgram::Run(const EntityRenderState& state) {
        if (!m_prog) return;

        // Deep enough for every generated program; MC's expressions nest four
        // or five operands at most.
        float stack[32];
        std::vector<float>& locals = m_locals;
        std::fill(locals.begin(), locals.end(), 0.0f);

        // Node k's bake-time binding, or a null bind for a node outside the
        // baked range (cannot happen for a well-formed program; the guard
        // keeps a malformed one reading zeros rather than off the end).
        static const NodeBind kUnbound{};
        const auto bindOf = [&](int nodeIndex) -> const NodeBind& {
            const int k = nodeIndex - m_nodeLo;
            return (k >= 0 && k < static_cast<int>(m_nodes.size()))
                ? m_nodes[static_cast<size_t>(k)] : kUnbound;
        };

        const auto eval = [&](int first, int count) -> float {
            int sp = 0;
            for (int i = 0; i < count; ++i) {
                const AnimNode& n = kAnimNodes[first + i];
                const auto push = [&](float v) { if (sp < 32) stack[sp++] = v; };
                const auto pop = [&]() -> float { return sp > 0 ? stack[--sp] : 0.0f; };
                switch (n.op) {
                    case AnimOp::Const:  push(n.value); break;
                    case AnimOp::State:
                    case AnimOp::BState:
                        push(ReadState(state, bindOf(first + i).ref));
                        break;
                    case AnimOp::Local:
                        push(n.arg >= 0 && n.arg < static_cast<int>(locals.size())
                                 ? locals[static_cast<size_t>(n.arg)] : 0.0f);
                        break;
                    case AnimOp::Part: {
                        float v = 0.0f;
                        {
                            const NodeBind& r = bindOf(first + i);
                            if (r.part) {
                                switch (r.field) {
                                    case PartField::X:      v = r.part->x; break;
                                    case PartField::Y:      v = r.part->y; break;
                                    case PartField::Z:      v = r.part->z; break;
                                    case PartField::XRot:   v = r.part->xRot; break;
                                    case PartField::YRot:   v = r.part->yRot; break;
                                    case PartField::ZRot:   v = r.part->zRot; break;
                                    case PartField::XScale: v = r.part->xScale; break;
                                    case PartField::YScale: v = r.part->yScale; break;
                                    case PartField::ZScale: v = r.part->zScale; break;
                                    default: break;
                                }
                            }
                        }
                        push(v);
                        break;
                    }
                    case AnimOp::Neg:    push(-pop()); break;
                    case AnimOp::Not:    push(pop() != 0.0f ? 0.0f : 1.0f); break;
                    case AnimOp::Add: { float b = pop(), a = pop(); push(a + b); break; }
                    case AnimOp::Sub: { float b = pop(), a = pop(); push(a - b); break; }
                    case AnimOp::Mul: { float b = pop(), a = pop(); push(a * b); break; }
                    case AnimOp::Div: { float b = pop(), a = pop();
                                        push(b != 0.0f ? a / b : 0.0f); break; }
                    case AnimOp::Mod: { float b = pop(), a = pop();
                                        push(b != 0.0f ? std::fmod(a, b) : 0.0f); break; }
                    case AnimOp::Gt:  { float b = pop(), a = pop(); push(a >  b ? 1.0f : 0.0f); break; }
                    case AnimOp::Lt:  { float b = pop(), a = pop(); push(a <  b ? 1.0f : 0.0f); break; }
                    case AnimOp::Ge:  { float b = pop(), a = pop(); push(a >= b ? 1.0f : 0.0f); break; }
                    case AnimOp::Le:  { float b = pop(), a = pop(); push(a <= b ? 1.0f : 0.0f); break; }
                    case AnimOp::Eq:  { float b = pop(), a = pop(); push(a == b ? 1.0f : 0.0f); break; }
                    case AnimOp::Ne:  { float b = pop(), a = pop(); push(a != b ? 1.0f : 0.0f); break; }
                    case AnimOp::And: { float b = pop(), a = pop();
                                        push((a != 0.0f && b != 0.0f) ? 1.0f : 0.0f); break; }
                    case AnimOp::Or:  { float b = pop(), a = pop();
                                        push((a != 0.0f || b != 0.0f) ? 1.0f : 0.0f); break; }
                    case AnimOp::Select: { float f = pop(), t = pop(), c = pop();
                                           push(c != 0.0f ? t : f); break; }
                    case AnimOp::Cos:    push(std::cos(pop())); break;
                    case AnimOp::Sin:    push(std::sin(pop())); break;
                    case AnimOp::Abs:    push(std::abs(pop())); break;
                    case AnimOp::Sqrt:   { float v = pop(); push(v > 0.0f ? std::sqrt(v) : 0.0f); break; }
                    case AnimOp::Floor:  push(std::floor(pop())); break;
                    case AnimOp::Signum: { float v = pop();
                                           push(v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f)); break; }
                    case AnimOp::Square: { float v = pop(); push(v * v); break; }
                    case AnimOp::Cube:   { float v = pop(); push(v * v * v); break; }
                    case AnimOp::Min: { float b = pop(), a = pop(); push(std::min(a, b)); break; }
                    case AnimOp::Max: { float b = pop(), a = pop(); push(std::max(a, b)); break; }
                    case AnimOp::Clamp: { float hi = pop(), lo = pop(), v = pop();
                                          push(std::clamp(v, lo, hi)); break; }
                    case AnimOp::Lerp:  { float b = pop(), a = pop(), t = pop();
                                          push(a + t * (b - a)); break; }
                    case AnimOp::RotLerp:    { float b = pop(), a = pop(), t = pop();
                                               push(RotLerp(t, a, b)); break; }
                    case AnimOp::RotLerpRad: { float b = pop(), a = pop(), t = pop();
                                               push(RotLerpRad(t, a, b)); break; }
                    case AnimOp::WrapDegrees: push(WrapDegrees(pop())); break;
                    case AnimOp::TriangleWave: { float p = pop(), v = pop();
                                                 push(TriangleWave(v, p)); break; }
                    case AnimOp::DegDiffAbs: { float b = pop(), a = pop();
                                               push(std::abs(WrapDegrees(b - a))); break; }
                }
            }
            return sp > 0 ? stack[sp - 1] : 0.0f;
        };

        for (int i = 0; i < m_prog->statementCount; ++i) {
            const AnimStatement& st = kAnimStatements[m_prog->firstStatement + i];

            if (st.guardNodeCount > 0
                && eval(st.firstGuardNode, st.guardNodeCount) == 0.0f) {
                continue;
            }

            const float v = eval(st.firstNode, st.nodeCount);

            if (st.kind == AnimStmt::SetLocal) {
                if (st.localIndex < static_cast<int>(locals.size())) {
                    locals[static_cast<size_t>(st.localIndex)] = v;
                }
                continue;
            }

            ModelPart* p = m_parts[static_cast<size_t>(i)];
            if (!p) continue;

            if (st.field == PartField::Visible) {
                p->visible = (v != 0.0f);
                continue;
            }
            if (st.field == PartField::SkipDraw) {
                p->skipDraw = (v != 0.0f);
                continue;
            }

            float* target = nullptr;
            switch (st.field) {
                case PartField::X:      target = &p->x; break;
                case PartField::Y:      target = &p->y; break;
                case PartField::Z:      target = &p->z; break;
                case PartField::XRot:   target = &p->xRot; break;
                case PartField::YRot:   target = &p->yRot; break;
                case PartField::ZRot:   target = &p->zRot; break;
                case PartField::XScale: target = &p->xScale; break;
                case PartField::YScale: target = &p->yScale; break;
                case PartField::ZScale: target = &p->zScale; break;
                default: break;
            }
            if (!target) continue;

            switch (st.kind) {
                case AnimStmt::Set:     *target  = v; break;
                case AnimStmt::AddTo:   *target += v; break;
                case AnimStmt::SubFrom: *target -= v; break;
                case AnimStmt::MulBy:   *target *= v; break;
                default: break;
            }
        }
    }

} // namespace Render
