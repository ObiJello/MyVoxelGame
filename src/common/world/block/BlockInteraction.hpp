// File: src/common/world/block/BlockInteraction.hpp
#pragma once

#include <glm/glm.hpp>
#include "Blocks.hpp"

namespace Game {
    class World;
}

namespace Server {
    class ServerPlayer;
}

namespace Game {
    
    // Represents the result of a block hit/interaction
    struct BlockHitResult {
        glm::ivec3 blockPos;        // Position of the clicked block
        int face;                   // Face that was clicked (0-5: bottom, top, north, south, west, east)
        glm::vec3 hitPoint;         // Exact world-space hit point
        bool insideBlock;           // True if raycast started inside the block
        
        BlockHitResult() = default;
        BlockHitResult(const glm::ivec3& pos, int f, const glm::vec3& hit, bool inside)
            : blockPos(pos), face(f), hitPoint(hit), insideBlock(inside) {}
    };
    
    // Result of a block / item use action — mirrors MC's InteractionResult
    // (net.minecraft.world.InteractionResult, which is a sealed interface).
    //
    //   Pass                       — no opinion; caller falls through to next step
    //   Success                    — handled, swing arm client-side
    //   SuccessServer              — handled, swing arm server-side (rare; e.g. some particle items)
    //   Consume                    — handled, no arm swing
    //   Fail                       — explicitly rejected; STOP, do NOT fall through
    //   TryEmptyHandInteraction    — item declined; main hand should retry as if
    //                                empty (so block.useWithoutItem gets a turn even
    //                                though there's an item in hand)
    //
    // We keep a plain enum (not MC's sealed-interface-with-records) for the kind
    // because:
    //   • the "swing source" distinction (NONE vs CLIENT vs SERVER) folds into
    //     the three Success-flavour values above;
    //   • MC's `heldItemTransformedTo` Optional<ItemStack> is achieved by us via
    //     the mutable `ItemStack&` passed into useOn/use callbacks — they just
    //     mutate the stack directly to "transform" it (bucket → water_bucket,
    //     stew → bowl). If/when we need a non-mutating API path (e.g. dual-wield
    //     with a clean copy), upgrade this to a struct carrying an optional
    //     replacement stack — see InteractionResult.Success.heldItemTransformedTo
    //     in MC's InteractionResult.java.
    //   • `wasItemInteraction` (used by MC for ITEM_USED_ON_BLOCK criteria)
    //     defaults to true; the rare `withoutItem()` variant isn't needed until
    //     we have advancements.
    enum class UseResult {
        Pass,
        Success,
        SuccessServer,
        Consume,
        Fail,
        TryEmptyHandInteraction
    };

    // Mirrors MC InteractionResult.consumesAction(): true when the result means
    // "I handled it, stop dispatching" — Success / SuccessServer / Consume.
    inline bool ConsumesAction(UseResult r) {
        return r == UseResult::Success
            || r == UseResult::SuccessServer
            || r == UseResult::Consume;
    }
    
    // Context for block placement/interaction
    struct UseOnContext {
        World* world;
        Server::ServerPlayer* player;
        uint32_t hand;              // 0 = main hand, 1 = off hand
        BlockHitResult hitResult;
        float playerYaw;            // Player's yaw for orientation
        float playerPitch;          // Player's pitch for orientation
        bool  altInteract = false;  // true = left-click "use" (currently
                                    // only meaningful for PortalGun: left
                                    // = blue, right = orange).

        UseOnContext() = default;
        UseOnContext(World* w, Server::ServerPlayer* p, uint32_t h, const BlockHitResult& hit)
            : world(w), player(p), hand(h), hitResult(hit), playerYaw(0), playerPitch(0) {}
        
        // Calculate the position where a block would be placed
        glm::ivec3 getPlacementPos() const {
            glm::ivec3 offset(0);
            switch (hitResult.face) {
                case 0: offset.y = -1; break; // Bottom
                case 1: offset.y = 1; break;  // Top
                case 2: offset.z = -1; break; // North
                case 3: offset.z = 1; break;  // South
                case 4: offset.x = -1; break; // West
                case 5: offset.x = 1; break;  // East
            }
            return hitResult.blockPos + offset;
        }
        
        // Get the cursor position within the clicked face (0-1 range)
        glm::vec3 getCursorPos() const {
            // Calculate block-local coordinates
            glm::vec3 localPos = hitResult.hitPoint - glm::vec3(hitResult.blockPos);
            
            // Clamp to [0, 1) range
            localPos.x = glm::clamp(localPos.x - glm::floor(localPos.x), 0.0f, 0.999f);
            localPos.y = glm::clamp(localPos.y - glm::floor(localPos.y), 0.0f, 0.999f);
            localPos.z = glm::clamp(localPos.z - glm::floor(localPos.z), 0.0f, 0.999f);
            
            return localPos;
        }
        
        // Check if placing on top face
        bool isPlacingOnTop() const {
            return hitResult.face == 1;
        }
        
        // Check if placing on bottom face
        bool isPlacingOnBottom() const {
            return hitResult.face == 0;
        }
        
        // Check if placing on a horizontal face (sides)
        bool isPlacingOnSide() const {
            return hitResult.face >= 2 && hitResult.face <= 5;
        }
    };
    
    // Helper to convert packet cursor coordinates to world-space hit point
    inline glm::vec3 faceLocalUVToWorld(int face, float cursorX, float cursorY, float cursorZ, const glm::ivec3& blockPos) {
        // The cursor coordinates are in block-local space [0,1)
        // We need to convert them to world space based on which face was hit
        
        glm::vec3 worldPos = glm::vec3(blockPos);
        
        // Add the local offset
        // The cursor values represent the position on the face that was clicked
        // They're already in the correct block-local coordinates
        worldPos.x += cursorX;
        worldPos.y += cursorY;
        worldPos.z += cursorZ;
        
        return worldPos;
    }
    
} // namespace Game