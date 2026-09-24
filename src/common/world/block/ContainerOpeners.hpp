// File: src/common/world/block/ContainerOpeners.hpp
//
// MC ContainerOpenersCounter for the two lidless containers this engine has no
// block entity class of their own for: the BARREL (BarrelBlockEntity — the
// BarrelBlock.OPEN state and BARREL_OPEN/CLOSE from its face) and the SHULKER
// BOX (ShulkerBoxBlockEntity.startOpen/stopOpen — SHULKER_BOX_OPEN/CLOSE).
// The copper chests (a plain container here, no lid block entity) get their
// weathering-dependent hinge sounds the same way. The wooden chest family has
// ChestBlockEntity's own counter.
//
// Server thread only (the container menus open and close there). The first
// opener plays the open sound (and props a barrel open); the last one to leave
// plays the close. A spectator is never counted, as in MC.
#pragma once

#include <glm/glm.hpp>

namespace Game {

    class ILevelWrite;

    namespace ContainerOpeners {

        // True when the block at `pos` is a barrel or a shulker box, i.e. one
        // this counter handles.
        bool Handles(const ILevelWrite& level, const glm::ivec3& pos);

        void StartOpen(ILevelWrite& level, const glm::ivec3& pos);
        void StopOpen(ILevelWrite& level, const glm::ivec3& pos);

    } // namespace ContainerOpeners

} // namespace Game
