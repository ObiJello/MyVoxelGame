// File: src/client/renderer/entity/model/KeyframeAnimation.hpp
//
// MC net.minecraft.client.animation.KeyframeAnimation, ported.
//
// MC has two entirely separate ways of animating a mob and this is the one the
// modern ones use. The old way is `setupAnim` writing limb rotations by hand —
// `cos(walkAnimationPos * 0.6662) * 1.4 * speed` and friends. The new way is a
// declarative AnimationDefinition: per-part channels of position, rotation and
// scale keyframes, sampled against the walk position. Frog, armadillo, camel,
// sniffer, creaking and copper golem have NO limb swing at all — apply the old
// one to them and they slide along perfectly rigid, because MC never wrote
// those rotations.
//
// Baking resolves the definition's part NAMES against a live ModelPart tree
// once, so a frame is a walk over resolved pointers rather than a string
// lookup per channel.
#pragma once

#include "client/renderer/entity/model/GeneratedAnimations.hpp"

#include <string_view>
#include <vector>

namespace Render {

    class ModelPart;

    class KeyframeAnimation {
    public:
        KeyframeAnimation() = default;

        // MC KeyframeAnimation.bake. Channels naming a part the model does not
        // have are dropped rather than throwing (MC throws): a mesh we
        // generated from a different ModelLayer than the animation expects is
        // a generator bug worth surviving, not a crash in the render loop.
        static KeyframeAnimation Bake(ModelPart& root, const GenAnim& anim);

        bool Valid() const { return m_anim != nullptr; }

        // MC KeyframeAnimation.applyWalk. `animationPos` is the distance-walked
        // accumulator, so the animation advances with the mob's travel rather
        // than with wall-clock time — that is what keeps it in step at any
        // speed, and what makes it freeze when the mob does.
        void ApplyWalk(float animationPos, float animationSpeed,
                       float speedFactor, float scaleFactor) const;

        // MC KeyframeAnimation.apply(millisSinceStart, targetScale).
        void Apply(float secondsSinceStart, float targetScale) const;

    private:
        struct Entry {
            ModelPart* part;
            AnimTarget target;
            int firstKey, keyCount;
        };

        const GenAnim* m_anim = nullptr;
        std::vector<Entry> m_entries;
    };

} // namespace Render
