// File: src/common/entity/Entity.cpp
#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/Mth.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"

#include <cmath>

namespace Game {

    Entity::Entity(EntityTypeId type, EntityLevel* level)
        : m_type(type), m_level(level) {}

    void Entity::SetOldPosAndRot() {
        oldPosition = position;
        yRotO = yRot;
        xRotO = xRot;
    }

    glm::dvec3 Entity::GetInputVector(const glm::dvec3& input, float speed, float yRot) {
        const double lenSq = input.x * input.x + input.y * input.y + input.z * input.z;
        if (lenSq < 1.0e-7) return glm::dvec3(0.0);

        // MC normalises only when the input is LONGER than unit length. A
        // shorter input keeps its magnitude, which is how a mob that is only
        // partly turned toward its waypoint moves at less than full speed.
        glm::dvec3 movement = (lenSq > 1.0) ? input / std::sqrt(lenSq) : input;
        movement *= static_cast<double>(speed);

        const float sin = std::sin(yRot * Mth::kDegToRad);
        const float cos = std::cos(yRot * Mth::kDegToRad);
        return glm::dvec3(movement.x * cos - movement.z * sin,
                          movement.y,
                          movement.z * cos + movement.x * sin);
    }

    void Entity::MoveRelative(float speed, const glm::dvec3& input) {
        velocity += GetInputVector(input, speed, yRot);
    }

    void Entity::ApplyGravity() {
        velocity.y -= GetGravity();
    }

    void Entity::Move(const glm::dvec3& delta) {
        if (!m_level) return;

        const PhysicsContext ctx = m_level->Physics();

        // MoveEntity consumes the velocity vector it is handed, zeroing blocked
        // axes. MC passes deltaMovement itself, so the caller's velocity is the
        // thing that must be mutated — hand it the real member, not a copy.
        glm::dvec3 vel = delta;
        const EntityMoveResult result =
            MoveEntity(position, vel, HalfExtents(), MaxUpStep(), onGround, ctx);

        // MC zeroes the collided components of deltaMovement, which for the
        // usual call (move(SELF, getDeltaMovement())) is the same vector. Copy
        // the resolved components back so a caller that passed something else
        // (knockback, piston push) still sees the correct post-move velocity.
        velocity = vel;

        onGround            = result.onGround;
        horizontalCollision = result.horizontalCollision;
        verticalCollision   = result.verticalCollision;
    }

    bool Entity::IsInWater() const {
        if (!m_level) return false;
        const IBlockAccess* blocks = m_level->Blocks();
        if (!blocks) return false;

        // MC tests the fluid the entity's box overlaps. This engine has no
        // fluid-height model, so the test is "is the block at the entity's eye-
        // low / feet-high midpoint a fluid" — accurate enough for the two
        // things that read it (FloatGoal and the water movement branch) and
        // deliberately coarse rather than pretending to a precision the block
        // data cannot support.
        const glm::ivec3 p = BlockPosition();
        return blocks->IsBlockFluid(p.x, p.y, p.z);
    }

    void Entity::IgniteForSeconds(int seconds) {
        const int ticks = seconds * 20;
        if (ticks > m_remainingFireTicks) m_remainingFireTicks = ticks;
    }

    void Entity::BaseTick() {
        firstTick = false;

        if (m_remainingFireTicks > 0) {
            --m_remainingFireTicks;
        }

        // MC Entity.checkBelowWorld — anything that falls out of the world is
        // discarded rather than left falling forever. The threshold is the
        // world floor minus 64, matching MC.
        if (position.y < -64.0 - 64.0) {
            Discard();
        }
    }

    void Entity::Tick() {
        BaseTick();
    }

} // namespace Game
