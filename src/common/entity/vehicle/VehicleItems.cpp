// File: src/common/entity/vehicle/VehicleItems.cpp
#include "common/entity/vehicle/VehicleItems.hpp"

#include "common/core/Mth.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/vehicle/Boat.hpp"
#include "common/entity/vehicle/Minecart.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/DispenseItemBehavior.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/block/Rails.hpp"
#include "common/world/block/RedstoneFamilies.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace Game {

    namespace {

        // The slab-test entry distance of a ray into a box, or nothing.
        std::optional<double> RayBoxEntry(const glm::dvec3& origin, const glm::dvec3& dir,
                                          const glm::dvec3& mn, const glm::dvec3& mx, double maxDistance) {
            double tNear = 0.0, tFar = maxDistance;
            for (int axis = 0; axis < 3; ++axis) {
                if (std::abs(dir[axis]) < 1e-9) {
                    if (origin[axis] < mn[axis] || origin[axis] > mx[axis]) return std::nullopt;
                    continue;
                }
                double t1 = (mn[axis] - origin[axis]) / dir[axis];
                double t2 = (mx[axis] - origin[axis]) / dir[axis];
                if (t1 > t2) std::swap(t1, t2);
                tNear = std::max(tNear, t1);
                tFar  = std::min(tFar, t2);
                if (tNear > tFar) return std::nullopt;
            }
            return tNear;
        }

        struct VehicleClipHit {
            glm::ivec3 cell;
            glm::dvec3 location;
        };

        // MC Item.getPlayerPOVHitResult(level, player, ClipContext.Fluid.ANY)
        // with an OUTLINE block clip: walk the cells along the look ray and
        // take, in each, the nearer of the block's shape and the fluid's
        // (a water cell's box is as tall as its fluid, FluidState.getShape).
        std::optional<VehicleClipHit> ClipAnyFluid(const IBlockAccess& level, const glm::dvec3& eye,
                                                   const glm::dvec3& dir, double reach) {
            glm::ivec3 cell(static_cast<int>(std::floor(eye.x)), static_cast<int>(std::floor(eye.y)),
                            static_cast<int>(std::floor(eye.z)));
            const glm::ivec3 step(dir.x > 0.0 ? 1 : -1, dir.y > 0.0 ? 1 : -1, dir.z > 0.0 ? 1 : -1);
            glm::dvec3 tMax(0.0), tDelta(0.0);
            for (int axis = 0; axis < 3; ++axis) {
                if (std::abs(dir[axis]) < 1e-9) {
                    tMax[axis] = tDelta[axis] = std::numeric_limits<double>::infinity();
                } else {
                    const double bound = dir[axis] > 0.0 ? std::floor(eye[axis]) + 1.0 : std::floor(eye[axis]);
                    tMax[axis] = (bound - eye[axis]) / dir[axis];
                    tDelta[axis] = 1.0 / std::abs(dir[axis]);
                }
            }
            double travelled = 0.0;
            while (travelled <= reach) {
                if (!level.IsValidPosition(cell.x, cell.y, cell.z)) return std::nullopt;
                const BlockState state = level.GetBlockState(cell.x, cell.y, cell.z);
                std::optional<double> best;
                if (state.Block() != BlockID::Air) {
                    for (const auto& box : BlockRegistry::GetBlockShapeSet(state)) {
                        const auto t = RayBoxEntry(eye, dir, glm::dvec3(cell) + glm::dvec3(box.min),
                                                   glm::dvec3(cell) + glm::dvec3(box.max), reach);
                        if (t && (!best || *t < *best)) best = t;
                    }
                }
                const FluidState fluid = FluidStateOf(state);
                if (!fluid.IsEmpty()) {
                    const double height = FluidHeight(level, cell, fluid);
                    const auto t = RayBoxEntry(eye, dir, glm::dvec3(cell),
                                               glm::dvec3(cell) + glm::dvec3(1.0, height, 1.0), reach);
                    if (t && (!best || *t < *best)) best = t;
                }
                if (best) return VehicleClipHit{cell, eye + dir * *best};
                if (tMax.x < tMax.y && tMax.x < tMax.z) {
                    cell.x += step.x; travelled = tMax.x; tMax.x += tDelta.x;
                } else if (tMax.y < tMax.z) {
                    cell.y += step.y; travelled = tMax.y; tMax.y += tDelta.y;
                } else {
                    cell.z += step.z; travelled = tMax.z; tMax.z += tDelta.z;
                }
            }
            return std::nullopt;
        }

        // EntityType.createDefaultStackConfig's custom-name half.
        void ApplyStackConfig(Entity& entity, const ItemStack& stack) {
            if (auto name = stack.components.get(DataComponents::CUSTOM_NAME)) entity.SetCustomName(*name);
        }

        // ── BoatItem.use ───────────────────────────────────────────────────
        UseResult Use_Boat(ILevelWrite* world, IUsePlayer* player, uint32_t hand, ItemStack& stack) {
            if (!world || !player) return UseResult::Pass;
            const EntityTypeId type = BoatTypeForItem(stack.itemId);
            if (type == EntityTypeId::Count) return UseResult::Pass;

            Entity* self = player->GameEventSource();
            const glm::dvec3 eye = self ? self->GetEyePosition()
                                        : player->getPosition() + glm::dvec3(0.0, 1.62, 0.0);
            const glm::dvec3 view(Mth::ViewVector(player->getPitch(), player->getYaw()));
            constexpr double kReach = 5.0;   // BLOCK_INTERACTION_RANGE's default
            const auto hit = ClipAnyFluid(*world, eye, view, kReach);
            if (!hit) return UseResult::Pass;   // HitResult.Type.MISS

            EntityLevel* entities = world->Entities();
            if (world->IsClientSide() || !entities) return UseResult::Success;

            // An entity whose pick box holds the eye blocks the placement
            // (BoatItem.ENTITY_PREDICATE: not a spectator, pickable).
            {
                const glm::dvec3 feet = player->getPosition();
                AABBd playerBox = self ? self->GetAABBd()
                                       : AABBd::FromMinMax(feet - glm::dvec3(0.3, 0.0, 0.3),
                                                           feet + glm::dvec3(0.3, 1.8, 0.3));
                const glm::dvec3 reachVec = view * kReach;
                glm::dvec3 lo = glm::min(playerBox.min, playerBox.min + reachVec) - glm::dvec3(1.0);
                glm::dvec3 hi = glm::max(playerBox.max, playerBox.max + reachVec) + glm::dvec3(1.0);
                std::vector<Entity*> nearby;
                entities->GetEntitiesInBox(AABB::FromMinMax(glm::vec3(lo), glm::vec3(hi)), self, nearby);
                for (Entity* e : nearby) {
                    if (!e || e->IsSpectator() || !e->IsPickable()) continue;
                    AABBd box = e->GetAABBd();
                    const double r = e->GetPickRadius();
                    box.min -= glm::dvec3(r);
                    box.max += glm::dvec3(r);
                    if (eye.x >= box.min.x && eye.x < box.max.x && eye.y >= box.min.y && eye.y < box.max.y &&
                        eye.z >= box.min.z && eye.z < box.max.z) {
                        return UseResult::Pass;
                    }
                }
            }

            // getBoat: the entity at the hit location, the stack's config,
            // facing the player; it must fit there.
            auto boat = std::make_unique<Boat>(type, entities);
            boat->SetInitialPos(hit->location);
            ApplyStackConfig(*boat, stack);
            boat->yRot = player->getYaw();
            boat->yRotO = boat->yRot;
            if (!boat->NoCollisionHere()) return UseResult::Fail;

            entities->AddFreshEntity(std::move(boat));
            world->GameEvent(self, GameEventId::EntityPlace, hit->location);
            // itemStack.consume(1, player): creative keeps it.
            if (!player->isCreative()) {
                stack.count -= 1;
                if (stack.count <= 0) stack.Clear();
            }
            player->markSlotDirty(player->handSlotIndex(hand));
            return UseResult::Success;
        }

        // ── MinecartItem.useOn ─────────────────────────────────────────────
        UseResult UseOn_Minecart(const UseOnContext& ctx, ItemStack& stack) {
            if (!ctx.world) return UseResult::Pass;
            const glm::ivec3 pos = ctx.hitResult.blockPos;
            const BlockState blockState = ctx.world->GetBlockState(pos.x, pos.y, pos.z);
            if (!IsRailBlock(blockState.Block())) return UseResult::Fail;   // BlockTags.RAILS
            const EntityTypeId type = MinecartTypeForItem(stack.itemId);
            if (type == EntityTypeId::Count) return UseResult::Fail;

            const double offset = RailShapeIsSlope(RailShapeOf(blockState)) ? 0.5 : 0.0;
            const glm::dvec3 spawnPos(pos.x + 0.5, pos.y + 0.0625 + offset, pos.z + 0.5);

            EntityLevel* entities = ctx.world->Entities();
            if (!ctx.world->IsClientSide() && entities) {
                std::unique_ptr<AbstractMinecart> cart = CreateMinecart(type, entities);
                if (!cart) return UseResult::Fail;
                cart->SetInitialPos(spawnPos);
                ApplyStackConfig(*cart, stack);
                entities->AddFreshEntity(std::move(cart));
                Entity* source = ctx.player ? ctx.player->GameEventSource() : nullptr;
                ctx.world->GameEvent(GameEventId::EntityPlace, pos,
                                     GameEventContext::Of(source, ctx.world->GetBlockState(pos.x, pos.y - 1, pos.z)));
            }
            // itemStack.shrink(1) — a creative player's count is restored by
            // the use dispatch, as for every useOn.
            stack.count -= 1;
            if (stack.count <= 0) stack.Clear();
            return UseResult::Success;
        }

        // MinecartDispenseItemBehavior / BoatDispenseItemBehavior.getRailShape.
        RailShape RailShapeAt(const ILevelWrite& level, const glm::ivec3& p) {
            return RailShapeOf(level.GetBlockState(p.x, p.y, p.z));
        }

    } // namespace

    bool IsVehicleItem(ItemID item) {
        return BoatTypeForItem(item) != EntityTypeId::Count || MinecartTypeForItem(item) != EntityTypeId::Count;
    }

    void ItemRegistry_RegisterVehicleItems(std::unordered_map<ItemID, Item>& pureItems) {
        for (auto& [id, item] : pureItems) {
            if (BoatTypeForItem(id) != EntityTypeId::Count) {
                // Items.java: BoatItem, stacksTo(1).
                item.maxStackSize = 1;
                item.use = &Use_Boat;
            } else if (MinecartTypeForItem(id) != EntityTypeId::Count) {
                // Items.java: MinecartItem, stacksTo(1).
                item.maxStackSize = 1;
                item.useOn = &UseOn_Minecart;
            }
        }
    }

    ItemStack DispenseVehicleItem(const DispenseSource& source, ItemStack dispensed) {
        ILevelWrite& level = source.level;
        EntityLevel* entities = level.Entities();
        const Direction direction = FacingOf(source.state);
        const glm::dvec3 center = source.Center();
        const glm::ivec3 front = Relative(source.pos, direction);

        if (const EntityTypeId boatType = BoatTypeForItem(dispensed.itemId); boatType != EntityTypeId::Count) {
            // BoatDispenseItemBehavior.execute.
            const double justOutside = 0.5625 + static_cast<double>(GetEntityTypeInfo(boatType).width) / 2.0;
            const double spawnX = center.x + StepX(direction) * justOutside;
            const double spawnY = center.y + static_cast<double>(static_cast<float>(StepY(direction)) * 1.125f);
            const double spawnZ = center.z + StepZ(direction) * justOutside;
            double yOffset;
            if (GetFluidState(level, front).Is(FluidType::Water)) {
                yOffset = 1.0;
            } else {
                if (level.GetBlock(front.x, front.y, front.z) != BlockID::Air ||
                    !GetFluidState(level, front.x, front.y - 1, front.z).Is(FluidType::Water)) {
                    return DispenseDefault(source, dispensed);
                }
                yOffset = 0.0;
            }
            if (!entities) return dispensed;
            auto boat = std::make_unique<Boat>(boatType, entities);
            boat->SetInitialPos(glm::dvec3(spawnX, spawnY + yOffset, spawnZ));
            ApplyStackConfig(*boat, dispensed);
            boat->yRot = ToYRot(direction);
            boat->yRotO = boat->yRot;
            entities->AddFreshEntity(std::move(boat));
            dispensed.count -= 1;
            if (dispensed.count <= 0) dispensed.Clear();
            return dispensed;
        }

        const EntityTypeId cartType = MinecartTypeForItem(dispensed.itemId);
        if (cartType == EntityTypeId::Count) return DispenseDefault(source, dispensed);
        // MinecartDispenseItemBehavior.execute.
        const double spawnX = center.x + StepX(direction) * 1.125;
        const double spawnY = std::floor(center.y) + StepY(direction);
        const double spawnZ = center.z + StepZ(direction) * 1.125;
        const BlockState blockFront = level.GetBlockState(front.x, front.y, front.z);
        double yOffset;
        if (IsRailBlock(blockFront.Block())) {
            yOffset = RailShapeIsSlope(RailShapeOf(blockFront)) ? 0.6 : 0.1;
        } else {
            if (blockFront.Block() != BlockID::Air) return DispenseDefault(source, dispensed);
            const glm::ivec3 below(front.x, front.y - 1, front.z);
            if (!IsRailBlock(level.GetBlock(below.x, below.y, below.z))) return DispenseDefault(source, dispensed);
            yOffset = (direction != Direction::Down && RailShapeIsSlope(RailShapeAt(level, below))) ? -0.4 : -0.9;
        }
        if (!entities) return dispensed;
        std::unique_ptr<AbstractMinecart> cart = CreateMinecart(cartType, entities);
        if (cart) {
            cart->SetInitialPos(glm::dvec3(spawnX, spawnY + yOffset, spawnZ));
            ApplyStackConfig(*cart, dispensed);
            entities->AddFreshEntity(std::move(cart));
            dispensed.count -= 1;
            if (dispensed.count <= 0) dispensed.Clear();
        }
        return dispensed;
    }

} // namespace Game
