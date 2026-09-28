// File: src/common/world/level/WorldDrops.cpp
#include "WorldDrops.hpp"
#include <algorithm>
#include <cmath>
#include "common/entity/Item.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/loot/LootTables.hpp"
#include "common/data/DataComponentMap.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/block/Direction.hpp"
#include "server/IntegratedServer.hpp"
#include "server/entity/ItemEntityManager.hpp"
#include "server/level/ServerLevel.hpp"

namespace Game {

    namespace {
        // The named dimension's item manager, or null with no server (a
        // client-side call, a torn-down world) or before that level exists.
        Server::ItemEntityManager* ItemsFor(DimensionId dimension) {
            auto* server = Server::g_integratedServer.get();
            if (!server) return nullptr;
            Server::ServerLevel* level = server->GetLevel(dimension);
            return level ? level->Items() : nullptr;
        }
    }

    bool DropItemStackNear(DimensionId dimension, const glm::ivec3& pos,
                           const ItemStack& stack) {
        if (stack.IsEmpty()) return true;   // nothing to deliver

        auto* items = ItemsFor(dimension);
        if (!items) return false;

        items->PopResource(pos, stack);
        return true;
    }

    bool DropItemStackAt(DimensionId dimension, const glm::dvec3& pos,
                         const ItemStack& stack) {
        if (stack.IsEmpty()) return true;

        auto* items = ItemsFor(dimension);
        if (!items) return false;

        items->SpawnAtLocation(pos, stack);
        return true;
    }

    bool SpawnItemEntity(DimensionId dimension, const glm::dvec3& pos, const glm::dvec3& velocity,
                         const ItemStack& stack, int pickupDelay) {
        if (stack.IsEmpty()) return true;
        auto* items = ItemsFor(dimension);
        if (!items) return false;
        items->Spawn(pos, velocity, stack, pickupDelay);
        return true;
    }

    void DropContainerItemStack(ILevelWrite& level, const glm::dvec3& pos, ItemStack stack) {
        if (stack.IsEmpty()) return;
        JavaRandom fallback(static_cast<int64_t>(pos.x * 3129871.0) ^ static_cast<int64_t>(pos.z * 116129781.0) ^
                            static_cast<int64_t>(pos.y));
        JavaRandom& random = level.Random() ? *level.Random() : fallback;
        // EntityTypes.ITEM: 0.25 wide.
        constexpr double kSize = 0.25;
        const double centerRange = 1.0 - kSize;
        const double halfSize = kSize / 2.0;
        const double xo = std::floor(pos.x) + random.NextDouble() * centerRange + halfSize;
        const double yo = std::floor(pos.y) + random.NextDouble() * centerRange;
        const double zo = std::floor(pos.z) + random.NextDouble() * centerRange + halfSize;
        while (!stack.IsEmpty()) {
            ItemStack part = stack;
            part.count = std::min(stack.count, random.NextInt(21) + 10);
            stack.count -= part.count;
            if (stack.count <= 0) stack.Clear();
            const double dx = random.Triangle(0.0, 0.11485000171139836);
            const double dy = random.Triangle(0.2, 0.11485000171139836);
            const double dz = random.Triangle(0.0, 0.11485000171139836);
            SpawnItemEntity(level.GetDimension(), glm::dvec3(xo, yo, zo), glm::dvec3(dx, dy, dz), part, 0);
        }
    }

    bool DropItemStackFromFace(DimensionId dimension, const glm::ivec3& pos,
                               int face, const ItemStack& stack) {
        if (stack.IsEmpty()) return true;

        auto* items = ItemsFor(dimension);
        if (!items) return false;

        // Face ordinals match Game::Direction (0=down .. 5=east) by
        // construction — Direction's own comment pins them to MC's enum order
        // so anything round-tripping through a numeric id lines up.
        if (face < 0 || face > 5) return false;
        items->PopResourceFromFace(pos, static_cast<Direction>(face), stack);
        return true;
    }

    void DropBlockLoot(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
        const BlockID id = state.Block();
        if (id == BlockID::Air) return;

        // The tables key on the block and on its state, so the caller passes
        // the state it is about to clear.
        JavaRandom rng(static_cast<uint64_t>(
            (static_cast<int64_t>(pos.x) * 3129871) ^
            (static_cast<int64_t>(pos.z) * 116129781) ^
             static_cast<int64_t>(pos.y)));
        LootContext ctx;
        ctx.block      = id;
        ctx.blockState = state.Index();
        // Block.dropResources(state, level, pos, blockEntity): the entity
        // still standing there, for copy_components.
        DataComponentMap entityComponents;
        if (BlockEntity* be = level.GetBlockEntity(pos)) {
            be->CollectComponents(entityComponents);
            ctx.blockEntityComponents = &entityComponents;
        }
        ctx.pos        = pos;
        ctx.rng        = &rng;
        // location_check conditions want the real world. Null for the client's
        // predicted level, which is correct — those conditions then pass, and
        // the client is not rolling authoritative loot anyway.
        ctx.blocks     = &level;
        // MC Block.dropResources(state, level, pos[, blockEntity]) builds its
        // LootParams without THIS_ENTITY: nothing "broke" the block — it
        // melted, lost its support, was washed away or pushed. The two block
        // tables that ask (`entity_properties` on snow and chorus_flower) then
        // drop nothing, which is why melting snow leaves no snowballs and a
        // chorus flower knocked off its plant is lost. A player's break builds
        // its own context (PlayerSession) with the entity present.
        ctx.brokenByEntity = false;

        // MC Block.popResource: nothing pops while block_drops is off.
        if (Rules::GetBool(Rules::Id::BlockDrops)) {
            for (const ItemStack& drop : LootTables::GetDrops(ctx)) {
                DropItemStackNear(level.GetDimension(), pos, drop);
            }
        }
    }

    void DestroyBlockWithDrops(ILevelWrite& level, const glm::ivec3& pos) {
        const BlockState state = level.GetBlockState(pos.x, pos.y, pos.z);
        if (state.Block() == BlockID::Air) return;
        // Roll the loot BEFORE clearing — see DropBlockLoot.
        DropBlockLoot(level, pos, state);
        level.SetBlock(pos.x, pos.y, pos.z, BlockID::Air, World::UpdateFlags::All);
    }

} // namespace Game
