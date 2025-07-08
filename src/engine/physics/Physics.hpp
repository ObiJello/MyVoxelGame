// File: src/engine/physics/Physics.hpp
#pragma once

#include <glm/glm.hpp>
#include <functional>

namespace Game {

    // Forward declarations
    class Chunk;
    class ChunkManager;

    // AABB structure for collision detection
    struct AABB {
        glm::vec3 min;
        glm::vec3 max;

        AABB() = default;
        AABB(const glm::vec3& center, const glm::vec3& size)
            : min(center - size * 0.5f), max(center + size * 0.5f) {}

        bool Intersects(const AABB& other) const {
            return (min.x < other.max.x && max.x > other.min.x) &&
                   (min.y < other.max.y && max.y > other.min.y) &&
                   (min.z < other.max.z && max.z > other.min.z);
        }
    };

    // Player physics state and parameters
    struct PlayerPhysics {
        // Physical constants
        static constexpr float WALK_SPEED = 4.317f;        // Walking speed in blocks per second
        static constexpr float SPRINT_SPEED = 5.612f;      // Sprinting speed
        static constexpr float SNEAK_SPEED = 1.832f;       // Sneaking speed
        static constexpr float JUMP_VELOCITY = 9.04f;      // Velocity for a 1.25-block jump
        static constexpr float GRAVITY = -32.656f;         // Gravity acceleration
        static constexpr float TERMINAL_VELOCITY = -78.4f; // Terminal velocity

        static constexpr float WATER_WALK_SPEED = 1.85f;    // Reduced speed in water
        static constexpr float WATER_GRAVITY = -10.0f;     // Reduced gravity in water
        static constexpr float WATER_JUMP_VELOCITY = 4.0f; // Reduced jump in water

        static constexpr float OVERHANG_MARGIN = 0.125f; // Allowable overhang distance

        // Player dimensions
        static constexpr float WIDTH = 0.6f;
        static constexpr float HEIGHT_STANDING = 1.8f;
        static constexpr float HEIGHT_SNEAKING = 1.49f;
        static constexpr float EYE_HEIGHT_STANDING = 1.62f;
        static constexpr float EYE_HEIGHT_SNEAKING = 1.42f;

        // Movement momentum system
        static constexpr float CORRECT_JUMP_TIME_WINDOW = 0.1f; // 100 milliseconds window
        static constexpr float SPEED_INCREMENT = 0.5f; // Speed increase per correct jump
        static constexpr float MAX_SPEED_MULTIPLIER = 2.0f; // Max speed multiplier

        // Current player state
        glm::vec3 position{0.0f, 80.0f, 0.0f};
        glm::vec3 velocity{0.0f};
        bool isOnGround = false;
        bool isSneaking = false;
        bool isSprinting = false;
        bool isInWater = false;
        bool noclip = false;

        // Timing and momentum variables
        float totalTime = 0.0f;
        float lastLandingTime = 0.0f;
        float lastJumpTime = 0.0f;
        int consecutiveJumps = 0;
        float currentSpeed = WALK_SPEED;
        float baseSpeed = WALK_SPEED;
        bool wasOnGround = false;

        // Get current eye position
        glm::vec3 GetEyePosition() const {
            float eyeHeight = isSneaking ? EYE_HEIGHT_SNEAKING : EYE_HEIGHT_STANDING;
            return position + glm::vec3(0.0f, eyeHeight, 0.0f);
        }

        // Get current height
        float GetCurrentHeight() const {
            return isSneaking ? HEIGHT_SNEAKING : HEIGHT_STANDING;
        }

        // Get player's AABB
        AABB GetAABB() const {
            float height = GetCurrentHeight();
            return AABB(
                glm::vec3(position.x, position.y + height * 0.5f, position.z),
                glm::vec3(WIDTH, height, WIDTH)
            );
        }
    };

    // Function to check if a block is solid for collision
    using BlockCollisionFunction = std::function<bool(int x, int y, int z)>;

    // Main physics update function
    void UpdatePlayerPhysics(PlayerPhysics& physics,
                            const glm::vec3& movementInput,
                            bool jumpPressed,
                            bool sneakPressed,
                            float deltaTime,
                            BlockCollisionFunction blockCollisionCheck = nullptr);

    // Collision detection functions
    bool CheckCollision(const glm::vec3& position, const PlayerPhysics& physics,
                       BlockCollisionFunction blockCollisionCheck);

    bool HasSupportBelow(const glm::vec3& position, const PlayerPhysics& physics,
                        BlockCollisionFunction blockCollisionCheck);

    bool IsInWater(const glm::vec3& position, BlockCollisionFunction blockCollisionCheck);

    // Movement helper functions
    void HandleJump(PlayerPhysics& physics, bool jumpPressed,
                   BlockCollisionFunction blockCollisionCheck);

    void UpdateBaseSpeed(PlayerPhysics& physics);

    void ApplyGravity(PlayerPhysics& physics, float deltaTime);

    void HandleMovement(PlayerPhysics& physics, const glm::vec3& movementInput,
                       float deltaTime, BlockCollisionFunction blockCollisionCheck);

    // Default block collision checker (uses WorldAccess)
    bool DefaultBlockCollisionCheck(int x, int y, int z);

} // namespace Game