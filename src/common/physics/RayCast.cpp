// File: src/common/physics/RayCast.cpp
#include "RayCast.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "../world/block/BlockRegistry.hpp"
#include "../core/Log.hpp"
#include "../core/Config.hpp"
#include "Physics.hpp"
#include <cmath>
#include <limits>
#include <algorithm>
#include <utility>
#include "../world/chunk/IBlockAccess.hpp"

namespace Game {

    // Global block access pointer for raycast system
    static const IBlockAccess* g_raycastBlockAccess = nullptr;

    // **NEW**: Set the global block access for raycast system
    void SetGlobalBlockAccess(const IBlockAccess* blockAccess) {
        g_raycastBlockAccess = blockAccess;
    }

    // **NEW**: Get block using global access
    static BlockID GetBlock(int worldX, int worldY, int worldZ) {
        if (g_raycastBlockAccess) {
            return g_raycastBlockAccess->GetBlock(worldX, worldY, worldZ);
        }
        Log::Warning("No global block access available for raycast");
        return BlockID::Air;
    }

    BlockState Raycast::GetBlockStateAt(int worldX, int worldY, int worldZ) {
        return g_raycastBlockAccess
                   ? g_raycastBlockAccess->GetBlockState(worldX, worldY, worldZ)
                   : BlockState{};
    }

    std::optional<RaycastHit> Raycast::CastRay(
        const glm::dvec3& origin,
        const glm::vec3& direction,
        float maxDistanceF,
        bool collisionOnly)
    {
        // DOUBLE throughout, as MC's BlockGetter.clip / traverseBlocks: the
        // origin is the eye, which at x = 300,000 a float puts on a 3 cm
        // grid — enough to pick the wrong face or the wrong half of a slab.
        const double maxDistance = maxDistanceF;
        // Normalize direction
        const glm::dvec3 dir = glm::normalize(glm::dvec3(direction));

        // DDA algorithm setup
        glm::dvec3 currentPos = origin;
        glm::ivec3 currentBlock = glm::ivec3(
            static_cast<int>(std::floor(currentPos.x)),
            static_cast<int>(std::floor(currentPos.y)),
            static_cast<int>(std::floor(currentPos.z))
        );

        // Calculate step direction for each axis
        glm::ivec3 step;
        step.x = (dir.x > 0) ? 1 : -1;
        step.y = (dir.y > 0) ? 1 : -1;
        step.z = (dir.z > 0) ? 1 : -1;

        // Calculate the distance to the next voxel boundary for each axis
        glm::dvec3 tMax;
        glm::dvec3 tDelta;

        // Small epsilon to avoid division by zero
        const double epsilon = 1e-6;

        // X axis
        if (std::abs(dir.x) > epsilon) {
            double voxelBoundary = (dir.x > 0) ?
                std::floor(currentPos.x) + 1.0 :
                std::floor(currentPos.x);
            tMax.x = (voxelBoundary - currentPos.x) / dir.x;
            tDelta.x = 1.0 / std::abs(dir.x);
        } else {
            tMax.x = 1e30;
            tDelta.x = 1e30;
        }

        // Y axis
        if (std::abs(dir.y) > epsilon) {
            double voxelBoundary = (dir.y > 0) ?
                std::floor(currentPos.y) + 1.0 :
                std::floor(currentPos.y);
            tMax.y = (voxelBoundary - currentPos.y) / dir.y;
            tDelta.y = 1.0 / std::abs(dir.y);
        } else {
            tMax.y = 1e30;
            tDelta.y = 1e30;
        }

        // Z axis
        if (std::abs(dir.z) > epsilon) {
            double voxelBoundary = (dir.z > 0) ?
                std::floor(currentPos.z) + 1.0 :
                std::floor(currentPos.z);
            tMax.z = (voxelBoundary - currentPos.z) / dir.z;
            tDelta.z = 1.0 / std::abs(dir.z);
        } else {
            tMax.z = 1e30;
            tDelta.z = 1e30;
        }

        // Track the previous block position for adjacent placement
        glm::ivec3 previousBlock = currentBlock;
        int lastStepAxis = -1; // 0=X, 1=Y, 2=Z

        double totalDistance = 0.0;
        
        // Check if we start inside a block
        bool startedInsideBlock = false;
        BlockID startBlockId = GetBlockAtWorldPos(origin);
        const auto stops = [&](BlockID id) {
            return IsBlockSolid(id) && (!collisionOnly || BlockRegistry::HasCollision(id));
        };
        if (stops(startBlockId)) {
            startedInsideBlock = true;
        }

        // Ray marching loop
        while (totalDistance < maxDistance) {
            // Check current block
            BlockID blockId = GetBlockAtWorldPos(glm::dvec3(currentBlock));

            if (stops(blockId)) {
                // Per-block shape refinement. MC's clip() walks every voxel the
                // ray crosses and tests against the block's actual VoxelShape —
                // so leaf litter (a flat plane at y=0.015625) only "hits" when
                // the ray actually intersects that plane, not whenever the ray
                // enters the full unit cell. Without this the player can target
                // air just above the leaf and break it, and the outline draws
                // around the entire surrounding cube.
                // …and against the shape of the block's actual STATE. A
                // rotated state occupies a different part of the cell (leaf
                // litter's quarter-plane swings to whichever corner `facing`
                // points at), so testing the default-state shape makes the
                // target box sit in the wrong corner — you aim at the visible
                // clump and hit nothing, then hit it from a corner where
                // nothing is drawn.
                const BlockState state =
                    GetBlockStateAt(currentBlock.x, currentBlock.y, currentBlock.z);
                // Every box of the shape, not just its bounds. MC's clip walks
                // the whole VoxelShape, which is what stops a stair from being
                // targetable through the empty half of its cell — aim above the
                // low step and the crosshair goes past it, exactly as vanilla.
                //
                // World-aware: a paired chest's half reaches the cell edge
                // toward its partner, so the seam is clickable instead of a
                // 2px dead strip between the two hitboxes.
                const auto shapes = g_raycastBlockAccess
                    ? BlockRegistry::GetBlockShapeSetAt(*g_raycastBlockAccess, currentBlock, state)
                    : BlockRegistry::GetBlockShapeSet(state);

                // The winning box: the one the ray enters FIRST. Boxes of one
                // shape can overlap in t (a stair's slab and its step share a
                // face), so "first hit wins" is the only ordering that gives
                // the same face MC would report.
                double bestHit  = std::numeric_limits<double>::infinity();
                int   nearAxis = -1;
                int   nearSign = 0;
                bool  insideShape = false;
                bool  anyHit   = false;

                for (const auto& shape : shapes) {
                    const glm::dvec3 boxMin = glm::dvec3(currentBlock) + glm::dvec3(shape.min);
                    const glm::dvec3 boxMax = glm::dvec3(currentBlock) + glm::dvec3(shape.max);

                    // Slab-test ray vs AABB. Returns tEnter / tExit relative to the
                    // unit-`dir` ray. We use the post-step `totalDistance` as the
                    // lower bound on tEnter so we never re-hit something the DDA
                    // already crossed.
                    double tNear = -std::numeric_limits<double>::infinity();
                    double tFar  =  std::numeric_limits<double>::infinity();
                    int   boxAxis = -1;
                    int   boxSign = 0;
                    bool  hitBox   = true;
                    for (int axis = 0; axis < 3; ++axis) {
                        const double d = dir[axis];
                        const double o = origin[axis];
                        if (std::abs(d) < epsilon) {
                            // Ray parallel to this slab — must already be inside it.
                            if (o < boxMin[axis] || o > boxMax[axis]) { hitBox = false; break; }
                            continue;
                        }
                        const double invD = 1.0 / d;
                        double t1 = (boxMin[axis] - o) * invD;
                        double t2 = (boxMax[axis] - o) * invD;
                        int   sign = -1; // hit the -axis face (entering through min)
                        if (t1 > t2) { std::swap(t1, t2); sign = +1; }
                        if (t1 > tNear) { tNear = t1; boxAxis = axis; boxSign = sign; }
                        if (t2 < tFar)  { tFar  = t2; }
                        if (tNear > tFar) { hitBox = false; break; }
                    }

                    // Also accept "started inside the shape" as a hit at t=0 (matches
                    // MC's `if (clipcontext.block().get(start) ...` behaviour on the
                    // origin voxel).
                    const bool insideBox =
                        (totalDistance == 0.0 &&
                         origin.x >= boxMin.x && origin.x <= boxMax.x &&
                         origin.y >= boxMin.y && origin.y <= boxMax.y &&
                         origin.z >= boxMin.z && origin.z <= boxMax.z);

                    if (hitBox && tFar >= 0.0 && tNear <= maxDistance) {
                        const double t = insideBox ? 0.0 : std::max(tNear, 0.0);
                        if (t <= maxDistance && t < bestHit) {
                            bestHit     = t;
                            nearAxis    = boxAxis;
                            nearSign    = boxSign;
                            insideShape = insideBox;
                            anyHit      = true;
                        }
                    }
                }   // shape boxes

                if (anyHit) {
                    // tHit is the entry distance (or 0 if the ray starts inside).
                    const double tHit = bestHit;
                    RaycastHit hit;
                    hit.blockPos = currentBlock;
                    hit.adjacentPos = previousBlock;
                    hit.blockId = blockId;
                    hit.state = state;
                    hit.distance = static_cast<float>(tHit);
                    hit.hitPoint = origin + dir * tHit;
                    hit.cursorPos = glm::vec3(hit.hitPoint - glm::dvec3(currentBlock));
                    hit.cursorPos = glm::clamp(hit.cursorPos, glm::vec3(0.0f), glm::vec3(0.999f));
                    hit.insideBlock = insideShape;

                    // Face from the slab axis we entered through.
                    if (insideShape || nearAxis < 0) {
                        // Fallback to the last DDA step axis so adjacent-placement
                        // (which uses `normal` to pick a neighbour) still works.
                        if (lastStepAxis == 0) {
                            hit.hitFace = (step.x > 0) ? 1 : 0;
                            hit.normal  = glm::vec3(-step.x, 0, 0);
                        } else if (lastStepAxis == 1) {
                            hit.hitFace = (step.y > 0) ? 3 : 2;
                            hit.normal  = glm::vec3(0, -step.y, 0);
                        } else if (lastStepAxis == 2) {
                            hit.hitFace = (step.z > 0) ? 5 : 4;
                            hit.normal  = glm::vec3(0, 0, -step.z);
                        } else {
                            hit.hitFace = 0;
                            hit.normal  = glm::vec3(0, 1, 0);
                        }
                    } else {
                        // nearSign is -1 if we entered through the min face
                        // (i.e. the normal points along -axis from the box's
                        // POV → +axis pointing outward). nearSign = +1 means
                        // entered through max face (normal points along +axis).
                        glm::vec3 n(0.0f);
                        n[nearAxis] = (nearSign < 0) ? -1.0f : 1.0f;
                        hit.normal  = n;
                        // hitFace encoding (matches SendUseItemOn's switch
                        // in PlayerController.cpp, which the server uses to
                        // pick the placement direction):
                        //   0 = +X face, 1 = -X face
                        //   2 = +Y face, 3 = -Y face
                        //   4 = +Z face, 5 = -Z face
                        // nearSign < 0 means we entered through the MIN
                        // face (= -axis face), so that maps to the odd
                        // numbers; +axis face maps to the even numbers.
                        // Inverting this puts placement on the opposite
                        // side of the targeted block.
                        if      (nearAxis == 0) hit.hitFace = (nearSign < 0) ? 1 : 0;
                        else if (nearAxis == 1) hit.hitFace = (nearSign < 0) ? 3 : 2;
                        else                    hit.hitFace = (nearSign < 0) ? 5 : 4;
                        // adjacentPos for partial shapes: the cell on the
                        // OTHER side of the hit face (so right-click places
                        // against the visible surface, not the full-cube
                        // neighbour the DDA happened to come from).
                        glm::ivec3 adj = currentBlock;
                        adj[nearAxis] += (nearSign < 0) ? -1 : 1;
                        hit.adjacentPos = adj;
                    }

                    return hit;
                }

                // Ray didn't actually intersect the block's shape inside this
                // cell — keep stepping as if the cell were empty.
            }

            // Store current block as previous before stepping
            previousBlock = currentBlock;

            // Find the next voxel boundary crossing
            if (tMax.x < tMax.y && tMax.x < tMax.z) {
                // Step in X direction
                currentBlock.x += step.x;
                totalDistance = tMax.x;
                tMax.x += tDelta.x;
                lastStepAxis = 0;
            } else if (tMax.y < tMax.z) {
                // Step in Y direction
                currentBlock.y += step.y;
                totalDistance = tMax.y;
                tMax.y += tDelta.y;
                lastStepAxis = 1;
            } else {
                // Step in Z direction
                currentBlock.z += step.z;
                totalDistance = tMax.z;
                tMax.z += tDelta.z;
                lastStepAxis = 2;
            }

            // Check world boundaries - use Config namespace
            if (currentBlock.y < Config::MinY || currentBlock.y > Config::MaxY) {
                break; // Out of world bounds
            }
        }

        // No block hit within range
        return std::nullopt;
    }

    BlockID Raycast::GetBlockAtWorldPos(const glm::dvec3& pos) {
        return GetBlock(
            static_cast<int>(std::floor(pos.x)),
            static_cast<int>(std::floor(pos.y)),
            static_cast<int>(std::floor(pos.z))
        );
    }

    bool Raycast::IsBlockSolid(BlockID id) {
        if (id == BlockID::Air) {
            return false;
        }
        // Raycast should hit all solid blocks, not just opaque ones.
        // Leaves (Cutout) are solid for interaction but not opaque for rendering.
        // Water and Lava are non-solid (can't target them); nor is Aurelith's
        // resonant water, which is water in every way but its look (MC's
        // bubble column: an empty shape, so the outline clip passes through).
        return id != BlockID::Water && id != BlockID::Lava && id != BlockID::ResonantWater;
    }

} // namespace Game