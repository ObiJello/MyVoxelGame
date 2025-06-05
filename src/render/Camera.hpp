// File: src/render/Camera.hpp
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "../platform/Input.hpp"

namespace Render {

    class Camera {
    public:
        // Camera parameters
        glm::vec3 position{ 0.0f, 64.0f, 0.0f }; // Start above ground
        float yaw   = -90.0f;  // Facing down –Z by default
        float pitch =   0.0f;
        float fov   =  70.0f;  // Degrees

        // Movement settings
        float moveSpeed      =  10.0f;  // units per second
        float mouseSensitivity = 0.1f;   // degrees per pixel

        Camera() = default;

        // Returns a view matrix using glm::lookAt
        glm::mat4 GetViewMatrix() const {
            glm::vec3 front;
            front.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
            front.y = sin(glm::radians(pitch));
            front.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
            glm::vec3 dir = glm::normalize(front);
            glm::vec3 right = glm::normalize(glm::cross(dir, {0.0f, 1.0f, 0.0f}));
            glm::vec3 up    = glm::normalize(glm::cross(right, dir));
            return glm::lookAt(position, position + dir, up);
        }

        // Call once per frame with delta‐time in seconds.
        // Reads Input::IsKeyDown and Input::GetMouseDelta() to update pos and orientation.
        void Update(float dt) {
            // 1) Mouse look: get how far the cursor has moved since last frame
            auto [dx, dy] = Input::GetMouseDelta();
            yaw   += dx * mouseSensitivity;
            pitch += dy * mouseSensitivity;

            // Clamp pitch to avoid gimbal flip
            if (pitch >  89.0f) pitch =  89.0f;
            if (pitch < -89.0f) pitch = -89.0f;

            // Recalculate direction vectors
            glm::vec3 front;
            front.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
            front.y = sin(glm::radians(pitch));
            front.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
            front = glm::normalize(front);

            glm::vec3 right = glm::normalize(glm::cross(front, {0.0f, 1.0f, 0.0f}));
            glm::vec3 up    = glm::normalize(glm::cross(right, front));

            // 2) Keyboard movement
            glm::vec3 movement{ 0.0f };
            if (Input::IsKeyDown(Input::Key::W)) {
                movement += front;
            }
            if (Input::IsKeyDown(Input::Key::S)) {
                movement -= front;
            }
            if (Input::IsKeyDown(Input::Key::A)) {
                movement -= right;
            }
            if (Input::IsKeyDown(Input::Key::D)) {
                movement += right;
            }
            if (Input::IsKeyDown(Input::Key::Space)) {
                movement += glm::vec3{0.0f, 1.0f, 0.0f};
            }
            if (Input::IsKeyDown(Input::Key::LeftControl)) {
                movement -= glm::vec3{0.0f, 1.0f, 0.0f};
            }

            if (glm::length(movement) > 0.0f) {
                movement = glm::normalize(movement);
                position += movement * moveSpeed * dt;
            }

            // 3) Reset per-frame mouse deltas
            Input::ResetMouseDelta();
        }
    };

} // namespace Render
