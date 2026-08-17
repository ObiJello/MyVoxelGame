// File: src/client/renderer/entity/model/SetupAnimRunner.cpp
#include "client/renderer/entity/model/SetupAnimRunner.hpp"
#include "client/renderer/entity/model/EntityModels.hpp"
#include "client/renderer/entity/model/ModelPart.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

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

        float StateValue(const EntityRenderState& s, std::string_view n) {
            if (n == "WalkPos")      return s.walkAnimationPos;
            if (n == "WalkSpeed")    return s.walkAnimationSpeed;
            if (n == "AgeInTicks")   return s.ageInTicks;
            if (n == "XRot")         return s.xRot;
            if (n == "YRot")         return s.yRot;
            if (n == "AttackTime")   return s.attackTime;
            if (n == "AgeScale")     return s.ageScale;
            if (n == "Flap")         return s.flap;
            if (n == "FlapSpeed")    return s.flapSpeed;
            if (n == "SpeedValue")   return s.speedValue;
            if (n == "SwimAmount")   return s.swimAmount;
            if (n == "MainArm")      return s.mainArm;
            if (n == "RightArmPose") return static_cast<float>(s.rightArmPose);
            if (n == "LeftArmPose")  return static_cast<float>(s.leftArmPose);
            return 0.0f;
        }

        float BoolValue(const EntityRenderState& s, std::string_view n) {
            if (n == "IsAggressive")  return s.isAggressive  ? 1.0f : 0.0f;
            if (n == "IsBaby")        return s.isBaby        ? 1.0f : 0.0f;
            if (n == "IsCrouching")   return s.isCrouching   ? 1.0f : 0.0f;
            if (n == "IsSprinting")   return s.isSprinting   ? 1.0f : 0.0f;
            if (n == "IsInWater")     return s.isInWater     ? 1.0f : 0.0f;
            if (n == "IsOnGround")    return s.isOnGround    ? 1.0f : 0.0f;
            if (n == "IsFallFlying")  return s.isFallFlying  ? 1.0f : 0.0f;
            if (n == "IsPassenger")   return s.isPassenger   ? 1.0f : 0.0f;
            if (n == "IsUsingItem")   return s.isUsingItem   ? 1.0f : 0.0f;
            if (n == "IsSitting")     return s.isSitting     ? 1.0f : 0.0f;
            if (n == "IsHoldingBow")  return s.isHoldingBow  ? 1.0f : 0.0f;
            if (n == "IsHidingInShell") return s.isHidingInShell ? 1.0f : 0.0f;
            return 0.0f;
        }

    } // namespace

    SetupAnimProgram SetupAnimProgram::Bake(ModelPart& root, const AnimProgram& prog) {
        SetupAnimProgram out;
        out.m_prog = &prog;
        out.m_localCount = prog.localCount;
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

        if (hi > lo) {
            out.m_nodeLo = lo;
            out.m_nodeParts.assign(static_cast<size_t>(hi - lo),
                                   PartRef{ nullptr, PartField::XRot });
            for (int i = lo; i < hi; ++i) {
                const AnimNode& n = kAnimNodes[i];
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
                out.m_nodeParts[static_cast<size_t>(i - lo)] = PartRef{ find(pname), f };
            }
        }
        return out;
    }

    void SetupAnimProgram::Run(const EntityRenderState& state) const {
        if (!m_prog) return;

        // Deep enough for every generated program; MC's expressions nest four
        // or five operands at most.
        float stack[32];
        std::vector<float> locals(static_cast<size_t>(std::max(1, m_localCount)), 0.0f);

        const auto eval = [&](int first, int count) -> float {
            int sp = 0;
            for (int i = 0; i < count; ++i) {
                const AnimNode& n = kAnimNodes[first + i];
                const auto push = [&](float v) { if (sp < 32) stack[sp++] = v; };
                const auto pop = [&]() -> float { return sp > 0 ? stack[--sp] : 0.0f; };
                switch (n.op) {
                    case AnimOp::Const:  push(n.value); break;
                    case AnimOp::State:  push(StateValue(state, n.name)); break;
                    case AnimOp::BState: push(BoolValue(state, n.name)); break;
                    case AnimOp::Local:
                        push(n.arg < static_cast<int>(locals.size())
                                 ? locals[static_cast<size_t>(n.arg)] : 0.0f);
                        break;
                    case AnimOp::Part: {
                        const int k = (first + i) - m_nodeLo;
                        float v = 0.0f;
                        if (k >= 0 && k < static_cast<int>(m_nodeParts.size())) {
                            const PartRef& r = m_nodeParts[static_cast<size_t>(k)];
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
