// File: src/client/renderer/core/Camera.hpp
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include "../../input/Input.hpp"
#include "../../input/KeyMapping.hpp"
#include "common/core/Mth.hpp"

namespace Render {

    // Camera perspective — MC CameraType. F5 cycles FIRST_PERSON →
    // THIRD_PERSON_BACK → THIRD_PERSON_FRONT → back to first person.
    enum class Perspective {
        FirstPerson,
        ThirdBack,
        ThirdFront,
    };

    class Camera {
    public:
        // Camera parameters
        glm::vec3 position{ 0.0f, 64.0f, 0.0f }; // This will be set by PlayerController

        // MINECRAFT'S CONVENTION — see the long note on Game::Mth::ViewVector.
        //   yaw   0 = facing +Z (south), increasing clockwise from above.
        //   pitch POSITIVE LOOKING DOWN; -90 is straight up.
        //
        // These are the same numbers the wire, the mobs, `facing=` blockstates
        // and /tp use, so nothing converts between the camera and the rest of
        // the game any more.
        float yaw   = 180.0f;  // Facing -Z (north) by default
        float pitch =   0.0f;
        float roll  =   0.0f;  // Camera roll around forward axis (degrees).
                               // Always 0 for the player's real camera.
                               // Set non-zero by the portal renderer when the
                               // virtual camera goes through a portal pair
                               // whose source/destination orientations differ
                               // (e.g. floor↔wall) — the camera basis must
                               // tilt to match the rotated "up" through the
                               // portal pair. Without it, lookAt's world-Y
                               // up would silently drop the roll component.
        float fov   =  70.0f;  // Degrees

        // Optional explicit view-matrix override. When set, GetViewMatrix()
        // returns this matrix verbatim instead of building one from
        // yaw/pitch/roll. Used by the portal renderer's virtual camera —
        // yaw/pitch decomposition is unstable for near-vertical forward
        // directions (atan2 of tiny near-zero components swings wildly),
        // so the portal renderer composes the view directly from the
        // src→dst transform and stores it here.
        bool      hasViewOverride = false;
        glm::mat4 viewOverride    {1.0f};

        // MC GameRenderer.bobHurt — the damage tilt and the death spin, as one
        // VIEW-SPACE rotation. MC pushes these onto the pose stack BEFORE the
        // camera's own pitch/yaw, which in matrix terms is a left multiply on
        // the view matrix, so that is exactly how it is applied below.
        //
        // Set per frame by the caller (it needs partialTick); identity means
        // "not hurt, not dying". Deliberately NOT applied to viewOverride: the
        // portal renderer derives its virtual views from the real view matrix,
        // so the tilt reaches them through that transform instead — applying it
        // twice would double the lean when looking through a portal.
        glm::mat4 viewTilt {1.0f};

        // MC GameRenderer.bobHurt, verbatim. `hurtTime`/`deathTime` are the
        // tick counters; partialTick is the render fraction between ticks, and
        // MC SUBTRACTS it from hurtTime (the flash decays) while ADDING it to
        // deathTime (the spin accumulates).
        static glm::mat4 MakeViewTilt(int hurtTime, int hurtDuration, float hurtDirDeg,
                                      bool dying, int deathTime, float partialTick) {
            glm::mat4 m{1.0f};

            if (dying) {
                const float duration = std::min(static_cast<float>(deathTime) + partialTick,
                                                20.0f);
                // Asymptotic, not linear: most of the roll happens in the first
                // few ticks and it eases into ~2.4 degrees short of 40.
                const float angle = 40.0f - 8000.0f / (duration + 200.0f);
                m = glm::rotate(m, glm::radians(angle), glm::vec3(0.0f, 0.0f, 1.0f));
            }

            float hurt = static_cast<float>(hurtTime) - partialTick;
            if (hurt < 0.0f || hurtDuration <= 0) return m;

            // sin(t^4 * pi): a sharp jolt at the start of the flash that fades
            // out — NOT a linear ramp, which would read as a slow sway.
            hurt /= static_cast<float>(hurtDuration);
            hurt = std::sin(hurt * hurt * hurt * hurt * 3.14159265358979323846f);

            // Conjugating the roll by the damage bearing is what aims it: a hit
            // from the front rolls the screen, a hit from the side pitches it.
            m = glm::rotate(m, glm::radians(-hurtDirDeg), glm::vec3(0.0f, 1.0f, 0.0f));
            m = glm::rotate(m, glm::radians(-hurt * 14.0f), glm::vec3(0.0f, 0.0f, 1.0f));
            m = glm::rotate(m, glm::radians(hurtDirDeg),  glm::vec3(0.0f, 1.0f, 0.0f));
            return m;
        }

        // Movement settings (not used when physics is enabled)
        float moveSpeed      =  10.0f;  // units per second
        float mouseSensitivity = 0.1f;   // degrees per pixel
        bool  invertY        = false;    // "Invert Mouse" option (pitch axis)

        // Control flags
        bool enableMouseLook = true;  // Whether mouse movement affects camera
        bool physicsControlled = false; // Whether position is controlled by physics

        // F5 camera perspective. `position`/yaw/pitch stay the LOGICAL eye
        // (raycast, interaction, audio); the render pass derives a pulled-
        // back view for the third-person modes (MC Camera.setup detached).
        Perspective perspective = Perspective::FirstPerson;
        void CyclePerspective() {
            perspective = static_cast<Perspective>(
                (static_cast<int>(perspective) + 1) % 3);
        }
        bool IsFirstPerson() const { return perspective == Perspective::FirstPerson; }

        Camera() = default;

        // Returns a view matrix using glm::lookAt
        glm::mat4 GetViewMatrix() const {
            if (hasViewOverride) return viewOverride;
            glm::vec3 dir = GetForward();
            glm::vec3 right = glm::normalize(glm::cross(dir, {0.0f, 1.0f, 0.0f}));
            glm::vec3 up    = glm::normalize(glm::cross(right, dir));
            if (roll != 0.0f) {
                const float r  = glm::radians(roll);
                const float cs = std::cos(r);
                const float sn = std::sin(r);
                const glm::vec3 newUp    =  cs * up    + sn * right;
                const glm::vec3 newRight = -sn * up    + cs * right;
                up    = newUp;
                right = newRight;
            }
            return viewTilt * glm::lookAt(position, position + dir, up);
        }

        // Get the camera's forward direction vector — MC Entity.getLookAngle().
        glm::vec3 GetForward() const {
            return Game::Mth::ViewVector(pitch, yaw);
        }

        // Get the camera's right direction vector.
        //
        // cross(forward, worldUp) really is the RIGHT hand in MC's convention:
        // facing south (+Z), it gives -X = west, which is where your right hand
        // points. The formula did not have to change with the convention, but
        // it is easy to "fix" it the wrong way round, so: it is correct.
        glm::vec3 GetRight() const {
            return glm::normalize(glm::cross(GetForward(), {0.0f, 1.0f, 0.0f}));
        }

        // Get horizontal-only forward direction (for movement)
        glm::vec3 GetHorizontalForward() const {
            return Game::Mth::HorizontalViewVector(yaw);
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

                // Input::GetMouseDelta already flips dy so that moving the
                // mouse UP is positive. MC's pitch is positive looking DOWN, so
                // the sign here is a SUBTRACTION — this is the one line that
                // decides whether the mouse feels inverted.
                yaw   += dx * mouseSensitivity;
                pitch -= (invertY ? -dy : dy) * mouseSensitivity;

                // MC clamps to +-90; we stop just short because
                // glm::lookAt's cross with world-up degenerates exactly there.
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
                if (Input::IsDown(*Input::Binds::Forward)) {
                    horizontalMovement += horizontalFront;
                }
                if (Input::IsDown(*Input::Binds::Back)) {
                    horizontalMovement -= horizontalFront;
                }
                if (Input::IsDown(*Input::Binds::Left)) {
                    horizontalMovement -= horizontalRight;
                }
                if (Input::IsDown(*Input::Binds::Right)) {
                    horizontalMovement += horizontalRight;
                }

                // Vertical movement (Space/Ctrl)
                glm::vec3 verticalMovement{ 0.0f };
                if (Input::IsDown(*Input::Binds::Jump)) {
                    verticalMovement += glm::vec3{0.0f, 1.0f, 0.0f};
                }
                if (Input::IsDown(*Input::Binds::Sneak)) {
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
            if (Input::IsDown(*Input::Binds::Forward)) {
                movement += horizontalFront;
            }
            if (Input::IsDown(*Input::Binds::Back)) {
                movement -= horizontalFront;
            }
            if (Input::IsDown(*Input::Binds::Left)) {
                movement -= horizontalRight;
            }
            if (Input::IsDown(*Input::Binds::Right)) {
                movement += horizontalRight;
            }

            // Vertical movement for noclip mode
            if (Input::IsDown(*Input::Binds::Jump)) {
                movement.y += 1.0f;  // Move up
            }
            if (Input::IsDown(*Input::Binds::Sneak)) {
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
            return Input::IsDown(*Input::Binds::Jump);
        }

        // Check if sprint key is pressed
        bool IsSprintPressed() const {
            return Input::IsDown(*Input::Binds::Sprint);
        }

        // Check if sneak key is pressed
        bool IsSneakPressed() const {
            return Input::IsDown(*Input::Binds::Sneak);
        }
    };

} // namespace Render