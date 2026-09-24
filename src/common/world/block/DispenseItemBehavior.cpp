// File: src/common/world/block/DispenseItemBehavior.cpp
#include "common/world/block/DispenseItemBehavior.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/SpawnEggs.hpp"
#include "common/entity/projectile/Arrow.hpp"
#include "common/entity/projectile/ThrowableProjectile.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/block/TntBlock.hpp"
#include "common/world/block/entity/DispenserBlockEntity.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/level/WorldMobSpawn.hpp"
#include "common/world/level/WorldPrimedTnt.hpp"

#include <memory>

namespace Game {

    namespace {

        constexpr int kDefaultAccuracy = 6;

        Direction FacingOfSource(const DispenseSource& source) { return FacingOf(source.state); }

        // MC DispenserBlock.getDispensePosition(source, 0.7, ZERO).
        glm::dvec3 DispensePosition(const DispenseSource& source, double scale = 0.7) {
            const Direction d = FacingOfSource(source);
            return source.Center() + glm::dvec3(scale * StepX(d), scale * StepY(d), scale * StepZ(d));
        }

        // MC level.levelEvent(1000 / 1001 / 1002): dispense, fail, shoot —
        // their sound half (LevelEventHandler: 1.0/1.0, 1.0/1.2, 1.0/1.2).
        void PlayDefaultSound(const DispenseSource& source, bool success) {
            PlayLevelEventSound(source.level, nullptr,
                                success ? LevelEvent::SOUND_DISPENSER_DISPENSE : LevelEvent::SOUND_DISPENSER_FAIL,
                                source.pos, 0, source.level.Random());
        }
        void PlayShootSound(const DispenseSource& source) {
            PlayLevelEventSound(source.level, nullptr, LevelEvent::SOUND_DISPENSER_PROJECTILE_LAUNCH,
                                source.pos, 0, source.level.Random());
        }

        // MC DefaultDispenseItemBehavior.spawnItem.
        void SpawnItem(ILevelWrite& level, const ItemStack& stack, int accuracy, Direction direction,
                       const glm::dvec3& position) {
            double x = position.x, y = position.y, z = position.z;
            if (AxisOf(direction) == Axis::Y) y -= 0.125;
            else                              y -= 0.15625;
            JavaRandom* random = level.Random();
            JavaRandom fallback(0);
            JavaRandom& r = random ? *random : fallback;
            const double pow = r.NextDouble() * 0.1 + 0.2;
            const glm::dvec3 velocity(
                r.Triangle(static_cast<double>(StepX(direction)) * pow, 0.0172275 * accuracy),
                r.Triangle(0.2, 0.0172275 * accuracy),
                r.Triangle(static_cast<double>(StepZ(direction)) * pow, 0.0172275 * accuracy));
            SpawnItemEntity(level.GetDimension(), glm::dvec3(x, y, z), velocity, stack, 10);
        }

        ItemStack SplitOne(ItemStack& stack) {
            ItemStack one = stack;
            one.count = 1;
            stack.count -= 1;
            if (stack.count <= 0) stack.Clear();
            return one;
        }

        // MC DefaultDispenseItemBehavior.execute.
        ItemStack ExecuteDefault(const DispenseSource& source, ItemStack dispensed) {
            const Direction direction = FacingOfSource(source);
            const glm::dvec3 position = DispensePosition(source);
            const ItemStack one = SplitOne(dispensed);
            SpawnItem(source.level, one, kDefaultAccuracy, direction, position);
            return dispensed;
        }

        // MC DefaultDispenseItemBehavior.consumeWithRemainder.
        ItemStack ConsumeWithRemainder(const DispenseSource& source, ItemStack dispensed, const ItemStack& remainder) {
            dispensed.count -= 1;
            if (dispensed.count <= 0) return remainder;
            // addToInventoryOrDispense
            const ItemStack rest = source.blockEntity.InsertItem(remainder);
            if (!rest.IsEmpty()) {
                SpawnItem(source.level, rest, kDefaultAccuracy, FacingOfSource(source), DispensePosition(source));
                PlayDefaultSound(source, true);
            }
            return dispensed;
        }

        // ── Projectiles (ProjectileDispenseBehavior, DispenseConfig.DEFAULT:
        //    uncertainty 6, power 1.1) ─────────────────────────────────────
        bool IsProjectileItem(ItemID id) {
            return id == Items::Arrow || id == Items::Snowball || id == Items::Egg ||
                   id == Items::BlueEgg || id == Items::BrownEgg ||
                   id == Items::TippedArrow ||
                   id == Items::SplashPotion || id == Items::LingeringPotion;
        }

        ItemStack ExecuteProjectile(const DispenseSource& source, ItemStack dispensed, bool& handled) {
            EntityLevel* entities = source.level.Entities();
            handled = false;
            if (!entities) return dispensed;
            const Direction direction = FacingOfSource(source);
            const glm::dvec3 position = DispensePosition(source);
            std::unique_ptr<Projectile> projectile;
            // ThrowablePotionItem.createDispenseConfig: uncertainty x0.5,
            // power x1.25 over the DEFAULT (6, 1.1).
            float power = 1.1f, uncertainty = 6.0f;
            if (dispensed.itemId == Items::Arrow || dispensed.itemId == Items::TippedArrow) {
                // ArrowItem / TippedArrowItem.asProjectile: an Arrow whose
                // pickup stack is the dispensed one (a tipped arrow's
                // contents and 1/8 duration scale ride on it).
                auto arrow = std::make_unique<Arrow>(entities);
                if (dispensed.itemId == Items::TippedArrow) arrow->SetPotionFromPickupStack(dispensed);
                projectile = std::move(arrow);
            } else if (dispensed.itemId == Items::SplashPotion ||
                       dispensed.itemId == Items::LingeringPotion) {
                auto potion = std::make_unique<ThrownSplashPotion>(entities);
                potion->SetItem(dispensed);
                projectile = std::move(potion);
                power *= 1.25f;
                uncertainty *= 0.5f;
            } else if (dispensed.itemId == Items::Snowball) {
                projectile = std::make_unique<Snowball>(entities);
            } else {
                projectile = std::make_unique<ThrownEgg>(entities);
            }
            projectile->position = position;
            projectile->Shoot(StepX(direction), StepY(direction), StepZ(direction), power, uncertainty);
            entities->AddFreshEntity(std::move(projectile));
            dispensed.count -= 1;
            if (dispensed.count <= 0) dispensed.Clear();
            handled = true;
            return dispensed;
        }

        // ── Buckets ──────────────────────────────────────────────────────
        ItemStack ExecuteFilledBucket(const DispenseSource& source, ItemStack dispensed) {
            const glm::ivec3 target = Relative(source.pos, FacingOfSource(source));
            ILevelWrite& level = source.level;
            const BlockState targetState = level.GetBlockState(target.x, target.y, target.z);
            const bool isLava = dispensed.itemId == Items::LavaBucket;
            // BucketItem.emptyContents: the cell must be air, replaceable, or
            // the same fluid; a waterloggable block takes water.
            const BlockID targetBlock = targetState.Block();
            bool placed = false;
            if (!isLava && BlockRegistry::IsWaterloggable(targetBlock) && !BlockRegistry::ContainsWater(targetState)) {
                placed = level.SetBlock(target.x, target.y, target.z,
                                        BlockRegistry::WithWaterlogged(targetState, true), World::UpdateFlags::All);
            } else if (targetBlock == BlockID::Air || BlockRegistry::Get(targetBlock).replaceable ||
                       targetBlock == BlockID::Water || targetBlock == BlockID::Lava) {
                placed = level.SetBlock(target.x, target.y, target.z,
                                        isLava ? BlockID::Lava : BlockID::Water, World::UpdateFlags::All);
            }
            if (!placed) {
                PlayDefaultSound(source, true);
                return ExecuteDefault(source, dispensed);
            }
            // MC BucketItem.emptyContents(null, ...) → playEmptySound(null,
            // level, pos): BUCKET_EMPTY(_LAVA), BLOCKS, 1.0, 1.0.
            level.PlaySound(nullptr, target, isLava ? SoundEvents::BUCKET_EMPTY_LAVA : SoundEvents::BUCKET_EMPTY,
                            SoundSource::Blocks, 1.0f, 1.0f);
            return ConsumeWithRemainder(source, dispensed, ItemStack(Items::Bucket, 1));
        }

        ItemStack ExecuteEmptyBucket(const DispenseSource& source, ItemStack dispensed) {
            const glm::ivec3 target = Relative(source.pos, FacingOfSource(source));
            ILevelWrite& level = source.level;
            const BlockState targetState = level.GetBlockState(target.x, target.y, target.z);
            const BlockID targetBlock = targetState.Block();
            ItemID pickup = Items::Air;
            if (targetBlock == BlockID::Water && BlockRegistry::IsWaterSource(targetState)) {
                level.SetBlock(target.x, target.y, target.z, BlockID::Air, World::UpdateFlags::All);
                pickup = Items::WaterBucket;
            } else if (targetBlock == BlockID::Lava) {
                level.SetBlock(target.x, target.y, target.z, BlockID::Air, World::UpdateFlags::All);
                pickup = Items::LavaBucket;
            } else if (BlockRegistry::IsWaterloggable(targetBlock) && BlockRegistry::ContainsWater(targetState)) {
                level.SetBlock(target.x, target.y, target.z,
                               BlockRegistry::WithWaterlogged(targetState, false), World::UpdateFlags::All);
                pickup = Items::WaterBucket;
            }
            if (pickup == Items::Air) return ExecuteDefault(source, dispensed);
            // MC's dispenser takes the fluid with bucket.pickupBlock(null, …)
            // and plays no pickup sound of its own (only BucketItem.use does);
            // what is heard is the dispense click the caller plays.
            return ConsumeWithRemainder(source, dispensed, ItemStack(pickup, 1));
        }

        // ── Flint and steel ──────────────────────────────────────────────
        // BaseFireBlock.canBePlacedAt: air, with a sturdy floor or a
        // flammable neighbour (FireBlock.canSurvive).
        bool CanFirePlaceAt(ILevelWrite& level, const glm::ivec3& pos) {
            if (level.GetBlock(pos.x, pos.y, pos.z) != BlockID::Air) return false;
            if (IsFaceSturdyAt(level, Below(pos), Direction::Up)) return true;
            for (Direction d : kAllDirections) {
                const glm::ivec3 n = Relative(pos, d);
                if (BlockRegistry::Get(level.GetBlock(n.x, n.y, n.z)).ignitedByLava) return true;
            }
            return false;
        }

        ItemStack ExecuteFlintAndSteel(const DispenseSource& source, ItemStack dispensed, bool& success) {
            ILevelWrite& level = source.level;
            success = true;
            const glm::ivec3 targetPos = Relative(source.pos, FacingOfSource(source));
            const BlockState target = level.GetBlockState(targetPos.x, targetPos.y, targetPos.z);
            const BlockID id = target.Block();
            if (CanFirePlaceAt(level, targetPos)) {
                level.SetBlock(targetPos.x, targetPos.y, targetPos.z, BlockID::Fire, World::UpdateFlags::All);
            } else if ((id == BlockID::Campfire || id == BlockID::SoulCampfire ||
                        BlockRegistry::Get(id).registrySlug.find("candle") != std::string::npos) &&
                       target.HasProperty(PropertyId::LIT) && !LitOf(target)) {
                level.SetBlock(targetPos.x, targetPos.y, targetPos.z, WithLit(target, true), World::UpdateFlags::All);
            } else if (id == BlockID::Tnt) {
                if (TntPrime(level, targetPos, nullptr)) {
                    if (auto* world = dynamic_cast<World*>(&level)) world->RemoveBlock(targetPos, false);
                } else {
                    success = false;
                }
            } else {
                success = false;
            }
            // MC: hurtAndBreak(1) — durability is not modelled.
            return dispensed;
        }

        // ── Bone meal ────────────────────────────────────────────────────
        ItemStack ExecuteBoneMeal(const DispenseSource& source, ItemStack dispensed, bool& success) {
            ILevelWrite& level = source.level;
            const glm::ivec3 target = Relative(source.pos, FacingOfSource(source));
            const BlockState state = level.GetBlockState(target.x, target.y, target.z);
            const Block& def = BlockRegistry::Get(state.Block());
            success = false;
            if (def.isValidBonemealTarget && def.performBonemeal &&
                def.isValidBonemealTarget(level, target, state)) {
                JavaRandom* random = level.Random();
                JavaRandom fallback(0);
                def.performBonemeal(level, target, state, random ? *random : fallback);
                success = true;
                dispensed.count -= 1;
                if (dispensed.count <= 0) dispensed.Clear();
                // MC DispenseItemBehavior:200 — levelEvent(1505, target, 15):
                // the growth particles and BONE_MEAL_USE.
                PlayLevelEventSound(level, nullptr, LevelEvent::PARTICLES_AND_SOUND_PLANT_GROWTH, target, 15,
                                    level.Random());
            }
            return dispensed;
        }

        // ── TNT ──────────────────────────────────────────────────────────
        ItemStack ExecuteTnt(const DispenseSource& source, ItemStack dispensed, bool& success) {
            const glm::ivec3 target = Relative(source.pos, FacingOfSource(source));
            success = SpawnPrimedTnt(source.level, target, nullptr);
            if (success) {
                // MC DispenseItemBehavior:219 — at the primed TNT (the cell's
                // bottom centre), BLOCKS, 1.0, 1.0.
                source.level.PlaySound(nullptr, glm::dvec3(target.x + 0.5, target.y, target.z + 0.5),
                                       SoundEvents::TNT_PRIMED, SoundSource::Blocks, 1.0f, 1.0f);
                dispensed.count -= 1;
                if (dispensed.count <= 0) dispensed.Clear();
            }
            return dispensed;
        }

        // ── Spawn eggs ───────────────────────────────────────────────────
        ItemStack ExecuteSpawnEgg(const DispenseSource& source, ItemStack dispensed, EntityTypeId type) {
            const Direction direction = FacingOfSource(source);
            const glm::ivec3 target = Relative(source.pos, direction);
            if (SpawnMobFromItem(type, target, /*tryMoveDown=*/direction != Direction::Up, false,
                                 source.level.GetDimension())) {
                dispensed.count -= 1;
                if (dispensed.count <= 0) dispensed.Clear();
            }
            return dispensed;
        }

    } // namespace

    ItemStack DispenseDefault(const DispenseSource& source, ItemStack stack) {
        const ItemStack result = ExecuteDefault(source, stack);
        PlayDefaultSound(source, true);
        return result;
    }

    ItemStack DispenseItem(const DispenseSource& source, ItemStack stack) {
        const ItemID id = stack.itemId;
        bool flag = true;

        if (IsProjectileItem(id)) {
            const ItemStack result = ExecuteProjectile(source, stack, flag);
            if (flag) { PlayShootSound(source); return result; }
            return DispenseDefault(source, stack);
        }
        // Both bucket behaviours are DefaultDispenseItemBehaviors: dispense()
        // runs execute() and then its own playSound (levelEvent 1000).
        if (id == Items::WaterBucket || id == Items::LavaBucket) {
            const ItemStack result = ExecuteFilledBucket(source, stack);
            PlayDefaultSound(source, true);
            return result;
        }
        if (id == Items::Bucket) {
            const ItemStack result = ExecuteEmptyBucket(source, stack);
            PlayDefaultSound(source, true);
            return result;
        }
        if (id == Items::FlintAndSteel) {
            const ItemStack result = ExecuteFlintAndSteel(source, stack, flag);
            PlayDefaultSound(source, flag);
            return result;
        }
        if (id == Items::BoneMeal) {
            const ItemStack result = ExecuteBoneMeal(source, stack, flag);
            PlayDefaultSound(source, flag);
            return result;
        }
        if (id == ItemRegistry::FromBlock(BlockID::Tnt)) {
            const ItemStack result = ExecuteTnt(source, stack, flag);
            PlayDefaultSound(source, flag);
            return result;
        }
        if (const EntityTypeId type = SpawnEggEntityType(id); type != EntityTypeId::Count) {
            const ItemStack result = ExecuteSpawnEgg(source, stack, type);
            PlayDefaultSound(source, true);
            return result;
        }
        return DispenseDefault(source, stack);
    }

} // namespace Game
