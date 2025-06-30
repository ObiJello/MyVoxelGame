// File: src/game/Physics.hpp (Added Chunk Loading Check)
#pragma once

#include <glm/glm.hpp>
#include "Blocks.hpp"
#include "WorldMath.hpp"

namespace Game {

    // Axis-Aligned Bounding Box for collision detection
    struct AABB {
        glm::vec3 min;
        glm::vec3 max;

        AABB() = default;
        AABB(const glm::vec3& center, const glm::vec3& size) {
            glm::vec3 halfSize = size * 0.5f;
            min = center - halfSize;
            max = center + halfSize;
        }

        // Get center point
        glm::vec3 GetCenter() const {
            return (min + max) * 0.5f;
        }

        // Get size
        glm::vec3 GetSize() const {
            return max - min;
        }

        // Expand AABB by amount in all directions
        AABB Expanded(float amount) const {
            AABB result = *this;
            result.min -= glm::vec3(amount);
            result.max += glm::vec3(amount);
            return result;
        }

        // Check if this AABB intersects with another
        bool Intersects(const AABB& other) const {
            return (min.x < other.max.x && max.x > other.min.x) &&
                   (min.y < other.max.y && max.y > other.min.y) &&
                   (min.z < other.max.z && max.z > other.min.z);
        }

        // Check if a point is inside this AABB
        bool Contains(const glm::vec3& point) const {
            return point.x >= min.x && point.x <= max.x &&
                   point.y >= min.y && point.y <= max.y &&
                   point.z >= min.z && point.z <= max.z;
        }
    };

    // Result of a collision check
    struct CollisionResult {
        bool hasCollision = false;
        glm::vec3 penetration{0.0f};  // How much to move to resolve collision
        glm::vec3 normal{0.0f};       // Surface normal at collision point
        BlockID blockId = BlockID::Air;
        glm::ivec3 blockPos{0};
    };

    // Player physics state
    struct PlayerPhysics {
        // Player dimensions (matching Minecraft)
        static constexpr float WIDTH = 0.6f;
        static constexpr float HEIGHT_STANDING = 1.8f;
        static constexpr float HEIGHT_SNEAKING = 1.49f;
        static constexpr float EYE_HEIGHT_STANDING = 1.62f;
        static constexpr float EYE_HEIGHT_SNEAKING = 1.42f;

        // Physics constants
        static constexpr float GRAVITY = 32.656f;           // blocks/second²
        static constexpr float TERMINAL_VELOCITY = 78.4f; // blocks/second
        static constexpr float JUMP_VELOCITY = 9.04f;     // blocks/second
        static constexpr float MOVE_SPEED = 4.317f;       // blocks/second (walking)
        static constexpr float SPRINT_SPEED = 5.612f;     // blocks/second (sprinting)
        static constexpr float SNEAK_SPEED = 1.832f;      // blocks/second (sneaking)
        static constexpr float FRICTION = 1.0f; //0.6f;           // Ground friction
        static constexpr float AIR_RESISTANCE = 1.0f; //0.98f;    // Air resistance multiplier

        // Current state
        glm::vec3 position{0.0f, 80.0f, 0.0f};  // Foot position
        glm::vec3 velocity{0.0f};               // Current velocity
        bool isOnGround = false;                // Standing on solid block
        bool isSneaking = false;                // Sneaking state
        bool isSprinting = false;               // Sprinting state
        bool isInWater = false;                 // Swimming/submerged
        bool noclip = false;                    // Debug: no collision

        // Get player's AABB at current position
        AABB GetAABB() const {
            float height = isSneaking ? HEIGHT_SNEAKING : HEIGHT_STANDING;
            glm::vec3 center = position + glm::vec3(0.0f, height * 0.5f, 0.0f);
            glm::vec3 size = glm::vec3(WIDTH, height, WIDTH);
            return AABB(center, size);
        }

        // Get eye position for camera
        glm::vec3 GetEyePosition() const {
            float eyeHeight = isSneaking ? EYE_HEIGHT_SNEAKING : EYE_HEIGHT_STANDING;
            return position + glm::vec3(0.0f, eyeHeight, 0.0f);
        }

        // Get current movement speed based on state
        float GetMoveSpeed() const {
            if (isSneaking) return SNEAK_SPEED;
            if (isSprinting) return SPRINT_SPEED;
            return MOVE_SPEED;
        }
    };

    class Physics {
    public:
        // Check if a block is solid for collision purposes
        static bool IsBlockSolid(BlockID blockId);

        // Check collision between AABB and the world
        static CollisionResult CheckCollision(const AABB& aabb);

        // Perform collision detection and response for movement
        static glm::vec3 ResolveCollision(const AABB& aabb, const glm::vec3& movement);

        // Update player physics (gravity, movement, collision)
        static void UpdatePlayerPhysics(PlayerPhysics& physics, const glm::vec3& inputMovement,
                                       bool jumpPressed, bool sneakPressed, float deltaTime);

        // Check if player is standing on ground
        static bool CheckGroundCollision(const PlayerPhysics& physics);

        // Check if player is in water
        static bool CheckWaterCollision(const PlayerPhysics& physics);

        // NEW: Check if the chunk containing the player is fully loaded
        static bool IsPlayerChunkLoaded(const PlayerPhysics& physics);

        // Get all blocks that intersect with an AABB
        static std::vector<glm::ivec3> GetIntersectingBlocks(const AABB& aabb);

    private:
        // Resolve collision along a single axis
        static float ResolveAxisCollision(const AABB& aabb, const glm::vec3& movement, int axis);

        // Check collision along a specific axis
        static bool CheckAxisCollision(const AABB& aabb, float movement, int axis);
    };

} // namespace Game