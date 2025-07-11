// File: src/render/gfx/Camera.hpp
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "../../platform/Input.hpp"

namespace Render {

    class Camera {
    public:
        // Camera parameters
        glm::vec3 position{ 0.0f, 64.0f, 0.0f }; // This will be set by PlayerController
        float yaw   = -90.0f;  // Facing down –Z by default
        float pitch =   0.0f;
        float fov   =  70.0f;  // Degrees

        // Movement settings (not used when physics is enabled)
        float moveSpeed      =  10.0f;  // units per second
        float mouseSensitivity = 0.1f;   // degrees per pixel

        // Control flags
        bool enableMouseLook = true;  // Whether mouse movement affects camera
        bool physicsControlled = false; // Whether position is controlled by physics

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

        // Get the camera's forward direction vector
        glm::vec3 GetForward() const {
            glm::vec3 front;
            front.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
            front.y = sin(glm::radians(pitch));
            front.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
            return glm::normalize(front);
        }

        // Get the camera's right direction vector
        glm::vec3 GetRight() const {
            return glm::normalize(glm::cross(GetForward(), {0.0f, 1.0f, 0.0f}));
        }

        // Get horizontal-only forward direction (for movement)
        glm::vec3 GetHorizontalForward() const {
            glm::vec3 front;
            front.x = cos(glm::radians(yaw));
            front.y = 0.0f;  // Keep Y at 0 for horizontal movement
            front.z = sin(glm::radians(yaw));
            return glm::normalize(front);
        }

        // Get horizontal-only right direction (for movement)
        glm::vec3 GetHorizontalRight() const {
            return glm::normalize(glm::cross(GetHorizontalForward(), {0.0f, 1.0f, 0.0f}));
        }

        // Update camera orientation and position (if not physics controlled)
        void Update(float dt) {
            // Mouse look: get how far the cursor has moved since last frame
            if (enableMouseLook) {
                auto [dx, dy] = Input::GetMouseDelta();

                yaw   += dx * mouseSensitivity;
                pitch += dy * mouseSensitivity;

                // Clamp pitch to avoid gimbal flip
                if (pitch >  89.0f) pitch =  89.0f;
                if (pitch < -89.0f) pitch = -89.0f;
            }

            // Only handle movement if not physics controlled
            if (!physicsControlled) {
                // Recalculate direction vectors
                glm::vec3 horizontalFront = GetHorizontalForward();
                glm::vec3 horizontalRight = GetHorizontalRight();

                // Horizontal movement (WASD)
                glm::vec3 horizontalMovement{ 0.0f };
                if (Input::IsKeyDown(Input::Key::W)) {
                    horizontalMovement += horizontalFront;
                }
                if (Input::IsKeyDown(Input::Key::S)) {
                    horizontalMovement -= horizontalFront;
                }
                if (Input::IsKeyDown(Input::Key::A)) {
                    horizontalMovement -= horizontalRight;
                }
                if (Input::IsKeyDown(Input::Key::D)) {
                    horizontalMovement += horizontalRight;
                }

                // Vertical movement (Space/Ctrl)
                glm::vec3 verticalMovement{ 0.0f };
                if (Input::IsKeyDown(Input::Key::Space)) {
                    verticalMovement += glm::vec3{0.0f, 1.0f, 0.0f};
                }
                if (Input::IsKeyDown(Input::Key::LeftShift)) {
                    verticalMovement -= glm::vec3{0.0f, 1.0f, 0.0f};
                }

                // Apply horizontal movement
                if (glm::length(horizontalMovement) > 0.0f) {
                    horizontalMovement = glm::normalize(horizontalMovement);
                    position += horizontalMovement * moveSpeed * dt;
                }

                // Apply vertical movement
                if (glm::length(verticalMovement) > 0.0f) {
                    position += verticalMovement * moveSpeed * dt;
                }
            }
        }

        // Calculate movement input vector for physics system
        // Returns full 3D movement for noclip mode
        glm::vec3 CalculateMovementInput() const {
            if (!physicsControlled) {
                return glm::vec3(0.0f); // Not using physics
            }

            glm::vec3 horizontalFront = GetHorizontalForward();
            glm::vec3 horizontalRight = GetHorizontalRight();

            glm::vec3 movement{ 0.0f };

            // Horizontal movement (WASD)
            if (Input::IsKeyDown(Input::Key::W)) {
                movement += horizontalFront;
            }
            if (Input::IsKeyDown(Input::Key::S)) {
                movement -= horizontalFront;
            }
            if (Input::IsKeyDown(Input::Key::A)) {
                movement -= horizontalRight;
            }
            if (Input::IsKeyDown(Input::Key::D)) {
                movement += horizontalRight;
            }

            // Vertical movement for noclip mode
            if (Input::IsKeyDown(Input::Key::Space)) {
                movement.y += 1.0f;  // Move up
            }
            if (Input::IsKeyDown(Input::Key::LeftShift)) {
                movement.y -= 1.0f;  // Move down
            }

            // Normalize to prevent faster diagonal movement
            if (glm::length(movement) > 0.0f) {
                movement = glm::normalize(movement);
            }

            return movement;
        }

        // Check if jump key is pressed
        bool IsJumpPressed() const {
            return Input::IsKeyDown(Input::Key::Space);
        }

        // Check if sprint key is pressed
        bool IsSprintPressed() const {
            return Input::IsKeyDown(Input::Key::LeftControl);
        }

        // Check if sneak key is pressed
        bool IsSneakPressed() const {
            return Input::IsKeyDown(Input::Key::LeftShift);
        }
    };

} // namespace Render