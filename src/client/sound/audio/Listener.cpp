// File: src/client/sound/audio/Listener.cpp
#include "client/sound/audio/Listener.hpp"

#include <AL/al.h>

namespace Client::Audio {

    void Listener::SetTransform(const ListenerTransform& transform) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_transform = transform;
        }
        const glm::dvec3& p = transform.position;
        const glm::dvec3& f = transform.forward;
        const glm::dvec3& u = transform.up;
        alListener3f(AL_POSITION, static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
        const ALfloat orientation[6] = {
            static_cast<float>(f.x), static_cast<float>(f.y), static_cast<float>(f.z),
            static_cast<float>(u.x), static_cast<float>(u.y), static_cast<float>(u.z)};
        alListenerfv(AL_ORIENTATION, orientation);
    }

    ListenerTransform Listener::GetTransform() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_transform;
    }

} // namespace Client::Audio
