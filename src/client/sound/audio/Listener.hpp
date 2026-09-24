// File: src/client/sound/audio/Listener.hpp
//
// MC com.mojang.blaze3d.audio.Listener + ListenerTransform: where the ears
// are. SoundEngine.updateSource sets it from the camera every frame; the AL
// calls run on the sound executor.
#pragma once

#include <glm/glm.hpp>

#include <mutex>

namespace Client::Audio {

    struct ListenerTransform {
        glm::dvec3 position{0.0};
        glm::dvec3 forward{0.0, 0.0, -1.0};
        glm::dvec3 up{0.0, 1.0, 0.0};

        glm::dvec3 Right() const { return glm::cross(forward, up); }
        static ListenerTransform Initial() { return ListenerTransform{}; }
    };

    class Listener {
    public:
        // Sound executor thread (AL).
        void SetTransform(const ListenerTransform& transform);
        void Reset() { SetTransform(ListenerTransform::Initial()); }
        // Any thread.
        ListenerTransform GetTransform() const;

    private:
        mutable std::mutex m_mutex;
        ListenerTransform  m_transform;
    };

} // namespace Client::Audio
