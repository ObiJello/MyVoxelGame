// File: src/client/renderer/entity/model/KeyframeAnimation.cpp
#include "client/renderer/entity/model/KeyframeAnimation.hpp"
#include "client/renderer/entity/model/ModelPart.hpp"

#include <algorithm>
#include <cmath>

namespace Render {

    namespace {

        // MC Mth.catmullrom. Used by CATMULLROM channels, which is how MC gets
        // a smooth arc through sparse keyframes instead of the visible corners
        // linear interpolation would leave.
        float Catmullrom(float t, float p0, float p1, float p2, float p3) {
            return 0.5f * (2.0f * p1
                           + (p2 - p0) * t
                           + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t
                           + (3.0f * p1 - p0 - 3.0f * p2 + p3) * t * t * t);
        }

    } // namespace

    KeyframeAnimation KeyframeAnimation::Bake(ModelPart& root, const GenAnim& anim) {
        KeyframeAnimation out;
        out.m_anim = &anim;
        for (int i = 0; i < anim.channelCount; ++i) {
            const GenAnimChannel& ch = kGenAnimChannels[anim.firstChannel + i];
            // MC's part lookup maps "root" to the root part itself; every other
            // name is searched depth-first through the children.
            ModelPart* part = (ch.part == "root") ? &root
                                                  : root.Find(std::string(ch.part));
            if (!part) continue;
            out.m_entries.push_back(Entry{ part, ch.target, ch.firstKey, ch.keyCount });
        }
        return out;
    }

    void KeyframeAnimation::ApplyWalk(float animationPos, float animationSpeed,
                                      float speedFactor, float scaleFactor) const {
        if (!m_anim) return;
        // MC: `long time = (long)(animationPos * 50.0F * speedFactor)` in
        // MILLISECONDS, then `getElapsedSeconds` divides by 1000. Folding the
        // two gives seconds directly; keeping MC's integer-millisecond
        // truncation would only quantise the curve.
        const float seconds = animationPos * 0.05f * speedFactor;
        const float scale = std::min(animationSpeed * scaleFactor, 1.0f);
        Apply(seconds, scale);
    }

    void KeyframeAnimation::Apply(float secondsSinceStart, float targetScale) const {
        if (!m_anim) return;

        float t = secondsSinceStart;
        if (m_anim->looping && m_anim->length > 0.0f) {
            t = std::fmod(t, m_anim->length);
            if (t < 0.0f) t += m_anim->length;
        }

        for (const Entry& e : m_entries) {
            const GenAnimKey* keys = kGenAnimKeys + e.firstKey;

            // MC Entry.apply: the last keyframe at or before `t`, and the one
            // after it.
            int prev = 0;
            while (prev + 1 < e.keyCount && keys[prev + 1].t <= t) ++prev;
            const int next = std::min(e.keyCount - 1, prev + 1);

            const GenAnimKey& a = keys[prev];
            const GenAnimKey& b = keys[next];

            float alpha = 0.0f;
            if (next != prev && b.t > a.t) {
                alpha = std::clamp((t - a.t) / (b.t - a.t), 0.0f, 1.0f);
            }

            float vx, vy, vz;
            if (b.interp == AnimInterp::CatmullRom) {
                const GenAnimKey& p0 = keys[std::max(0, prev - 1)];
                const GenAnimKey& p3 = keys[std::min(e.keyCount - 1, next + 1)];
                vx = Catmullrom(alpha, p0.x, a.x, b.x, p3.x) * targetScale;
                vy = Catmullrom(alpha, p0.y, a.y, b.y, p3.y) * targetScale;
                vz = Catmullrom(alpha, p0.z, a.z, b.z, p3.z) * targetScale;
            } else {
                vx = (a.x + (b.x - a.x) * alpha) * targetScale;
                vy = (a.y + (b.y - a.y) * alpha) * targetScale;
                vz = (a.z + (b.z - a.z) * alpha) * targetScale;
            }

            // MC's targets are offsetPos / offsetRotation / offsetScale — they
            // ADD to whatever the rest pose (and any earlier animation) left,
            // which is why every caller does resetPose first.
            switch (e.target) {
                case AnimTarget::Position:
                    e.part->x += vx; e.part->y += vy; e.part->z += vz;
                    break;
                case AnimTarget::Rotation:
                    e.part->xRot += vx; e.part->yRot += vy; e.part->zRot += vz;
                    break;
                case AnimTarget::Scale:
                    e.part->xScale += vx; e.part->yScale += vy; e.part->zScale += vz;
                    break;
            }
        }
    }

} // namespace Render
