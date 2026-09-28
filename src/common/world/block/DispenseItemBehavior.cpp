// File: src/common/world/block/DispenseItemBehavior.cpp
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/world/block/DispenseItemBehavior.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/ArmorStand.hpp"
#include "common/entity/HorseTaming.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/MountInventory.hpp"
#include "common/physics/Physics.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/SpawnEggs.hpp"
#include "common/entity/vehicle/VehicleItems.hpp"
#include "common/entity/projectile/Arrow.hpp"
#include "common/entity/projectile/ThrownTrident.hpp"
#include "common/entity/projectile/FireworkRocket.hpp"
#include "common/entity/projectile/ThrowableProjectile.hpp"
#include "common/entity/projectile/HurtingProjectile.hpp"
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
#include <vector>

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
        // MC DefaultDispenseItemBehavior.dispense: playSound, then
        // playAnimation — levelEvent 2000, the smoke puff out of the face
        // (data = the facing's 3D data value). Every behaviour runs both.
        void PlayDispenseAnimation(const DispenseSource& source) {
            PlayLevelEventSound(source.level, nullptr, LevelEvent::PARTICLES_SHOOT_SMOKE, source.pos,
                                static_cast<int>(FacingOfSource(source)), source.level.Random());
        }
        void PlayDefaultSound(const DispenseSource& source, bool success) {
            PlayLevelEventSound(source.level, nullptr,
                                success ? LevelEvent::SOUND_DISPENSER_DISPENSE : LevelEvent::SOUND_DISPENSER_FAIL,
                                source.pos, 0, source.level.Random());
            PlayDispenseAnimation(source);
        }
        void PlayShootSound(const DispenseSource& source) {
            PlayLevelEventSound(source.level, nullptr, LevelEvent::SOUND_DISPENSER_PROJECTILE_LAUNCH,
                                source.pos, 0, source.level.Random());
            PlayDispenseAnimation(source);
        }

        // ── Wind charge (WindChargeItem as a ProjectileItem) ─────────────
        //
        // DispenseConfig: positionFunction getDispensePosition(source, 1.0,
        // ZERO), power 1.0, uncertainty 6.6666665 — but WindChargeItem.shoot
        // is EMPTY, so neither is applied: the charge flies along asProjectile's
        // direction, each axis random.triangle(step, 0.11485), at that
        // vector's own length. The dispense event is 1051 (the throw sound)
        // instead of 1002.
        ItemStack ExecuteWindCharge(const DispenseSource& source, ItemStack dispensed, bool& handled) {
            EntityLevel* entities = source.level.Entities();
            handled = false;
            if (!entities) return dispensed;
            const Direction direction = FacingOfSource(source);
            const glm::dvec3 position = DispensePosition(source, 1.0);
            JavaRandom* random = source.level.Random();
            const auto triangle = [random](double mode) {
                return random ? random->Triangle(mode, 0.11485000000000001) : mode;
            };
            const double dirX = triangle(static_cast<double>(StepX(direction)));
            const double dirY = triangle(static_cast<double>(StepY(direction)));
            const double dirZ = triangle(static_cast<double>(StepZ(direction)));
            auto charge = std::make_unique<WindCharge>(entities);
            charge->position    = position;
            charge->oldPosition = position;
            charge->velocity    = glm::dvec3(dirX, dirY, dirZ);
            charge->needsSync   = true;
            entities->AddFreshEntity(std::move(charge));
            dispensed.count -= 1;
            if (dispensed.count <= 0) dispensed.Clear();
            handled = true;
            return dispensed;
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
                   id == Items::TippedArrow || id == Items::Trident ||
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
                arrow->SetPickupItemStack(dispensed);
                arrow->SetPickup(Arrow::Pickup::Allowed);
                projectile = std::move(arrow);
            } else if (dispensed.itemId == Items::Trident) {
                // TridentItem.asProjectile: a ThrownTrident of the stack
                // (copyWithCount(1) — its enchantments and wear), ALLOWED.
                auto trident = std::make_unique<ThrownTrident>(entities);
                trident->SetTridentItem(dispensed);
                trident->SetPickup(Arrow::Pickup::Allowed);
                projectile = std::move(trident);
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
            // playEmptySound's tail: gameEvent(null, FLUID_PLACE, pos).
            level.GameEvent(static_cast<Entity*>(nullptr), GameEventId::FluidPlace, target);
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
            // DispenseItemBehavior (BUCKET) — gameEvent(null, FLUID_PICKUP, target).
            level.GameEvent(static_cast<Entity*>(nullptr), GameEventId::FluidPickup, target);
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
                // FlintAndSteelDispenseItemBehavior:33 — gameEvent(null, BLOCK_PLACE, targetPos).
                level.GameEvent(static_cast<Entity*>(nullptr), GameEventId::BlockPlace, targetPos);
            } else if ((id == BlockID::Campfire || id == BlockID::SoulCampfire ||
                        BlockRegistry::Get(id).registrySlug.find("candle") != std::string::npos) &&
                       target.HasProperty(PropertyId::LIT) && !LitOf(target)) {
                level.SetBlock(targetPos.x, targetPos.y, targetPos.z, WithLit(target, true), World::UpdateFlags::All);
                // FlintAndSteelDispenseItemBehavior:44 — gameEvent(null, BLOCK_CHANGE, targetPos).
                level.GameEvent(static_cast<Entity*>(nullptr), GameEventId::BlockChange, targetPos);
            } else if (id == BlockID::Tnt) {
                if (TntPrime(level, targetPos, nullptr)) {
                    if (auto* world = dynamic_cast<World*>(&level)) world->RemoveBlock(targetPos, false);
                } else {
                    success = false;
                }
            } else {
                success = false;
            }
            // MC FlintAndSteelDispenseItemBehavior: `if (isSuccess())
            // dispensed.hurtAndBreak(1, level, null, item -> {})` — no player,
            // so no creative exemption and no break effects; a stack that
            // breaks just leaves the slot empty.
            if (success && !level.IsClientSide()) {
                if (JavaRandom* random = level.Random()) HurtAndBreak(dispensed, 1, *random, false, nullptr);
            }
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
                JavaRandom& rng = random ? *random : fallback;
                // BoneMealItem.growCrop, as the dispenser runs it: the item
                // is spent on any valid target, the growth only when
                // isBonemealSuccess rolls (a sapling's 45 %, a mushroom's 40 %).
                if (!def.isBonemealSuccess || def.isBonemealSuccess(level, target, state, rng)) {
                    def.performBonemeal(level, target, state, rng);
                }
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
                // SpawnEggItemBehavior:34 — gameEvent(null, ENTITY_PLACE, source.pos()).
                source.level.GameEvent(static_cast<Entity*>(nullptr), GameEventId::EntityPlace, source.pos);
            }
            return dispensed;
        }

        // MC EquipmentDispenseItemBehavior.dispenseEquipment: the first
        // living entity in the block the dispenser faces that
        // canEquipWithDispenser(dispensed) takes one of it into the stack's
        // EQUIPPABLE slot — a mob as a guaranteed drop and made persistent.
        // Players (their inventory's armour and offhand slots) and armor
        // stands (their own six slots) are LivingEntities too.
        bool DispenseEquipment(const DispenseSource& source, ItemStack& dispensed) {
            EntityLevel* entities = source.level.Entities();
            if (!entities || dispensed.IsEmpty()) return false;
            const auto equippable = dispensed.get(DataComponents::EQUIPPABLE);
            if (!equippable || !equippable->dispensable) return false;
            const EquipmentSlot slot = equippable->slot;
            const glm::ivec3 target = source.pos + glm::ivec3(StepX(FacingOfSource(source)),
                                                                StepY(FacingOfSource(source)),
                                                                StepZ(FacingOfSource(source)));
            std::vector<Entity*> found;
            entities->GetEntitiesInBox(AABB::FromMinMax(glm::vec3(target), glm::vec3(target) + glm::vec3(1.0f)),
                                       nullptr, found);
            for (Entity* entity : found) {
                LivingEntity* living = entity ? entity->AsLiving() : nullptr;
                if (!living || !living->IsAlive()) continue;
                ItemStack one = dispensed;
                one.count = 1;
                if (auto* stand = dynamic_cast<ArmorStand*>(living)) {
                    // ArmorStand: canUseSlot (never BODY / SADDLE, nor a
                    // disabled slot), the slot empty.
                    if (!stand->CanUseSlot(slot) || !equippable->CanBeEquippedBy(stand->TypeInfo().slug) ||
                        stand->HasItemInSlot(slot)) {
                        continue;
                    }
                    stand->SetItemSlot(slot, one);
                } else if (auto* mob = dynamic_cast<Mob*>(living)) {
                    if (!mob->CanEquipWithDispenser(dispensed)) continue;
                    // getEquipmentSlotForItem — the component's slot, which
                    // canEquipWithDispenser has just allowed.
                    mob->SetEquipment(slot, one);
                    mob->SetGuaranteedDrop(slot);
                    mob->SetPersistenceRequired(true);
                } else if (living->IsPlayer()) {
                    // A player (Avatar): the four armour slots and the
                    // offhand — the rest are no inventory slot here. The
                    // session's inventory diff sends it and the player's tick
                    // sounds the equip.
                    if (InventoryIndexFor(slot) < 0 || !equippable->CanBeEquippedBy("minecraft:player")) continue;
                    ItemStack* worn = living->EquipmentInSlot(slot);
                    if (!worn || !worn->IsEmpty()) continue;
                    *worn = one;
                } else {
                    continue;
                }
                dispensed.count -= 1;   // dispensed.split(1)
                if (dispensed.count <= 0) dispensed.Clear();
                return true;
            }
            return false;
        }

        // MC DispenseItemBehavior's CHEST behaviour: a tamed donkey, mule or
        // llama in front without a chest takes one (getSlot(499).set).
        bool DispenseChestOntoChestedHorse(const DispenseSource& source, ItemStack& dispensed) {
            EntityLevel* entities = source.level.Entities();
            if (!entities) return false;
            const Direction facing = FacingOfSource(source);
            const glm::ivec3 target = source.pos + glm::ivec3(StepX(facing), StepY(facing), StepZ(facing));
            std::vector<Entity*> found;
            entities->GetEntitiesInBox(AABB::FromMinMax(glm::vec3(target), glm::vec3(target) + glm::vec3(1.0f)),
                                       nullptr, found);
            for (Entity* entity : found) {
                auto* mob = dynamic_cast<Mob*>(entity);
                MountInventory* inventory = mob ? mob->GetMountInventory() : nullptr;
                if (!inventory || !inventory->CanCarryChest() || !mob->IsAlive() || inventory->HasChest()) continue;
                const auto* taming = dynamic_cast<const HorseTaming*>(mob);
                if (!taming || !taming->IsTamed()) continue;
                if (MountInventory::SetChestSlot(*mob, dispensed)) {
                    dispensed.count -= 1;
                    if (dispensed.count <= 0) dispensed.Clear();
                    return true;
                }
            }
            return false;
        }

    } // namespace

    void DispenseSpawnItem(ILevelWrite& level, const ItemStack& stack, int accuracy, int direction,
                           const glm::dvec3& position) {
        SpawnItem(level, stack, accuracy, static_cast<Direction>(direction), position);
    }

    ItemStack DispenseDefault(const DispenseSource& source, ItemStack stack) {
        const ItemStack result = ExecuteDefault(source, stack);
        PlayDefaultSound(source, true);
        return result;
    }

    namespace {
        // ── Firework rocket (FireworkRocketItem as a ProjectileItem) ─────
        //
        // DispenseConfig: positionFunction getEntityJustOutsideOfBlockPos
        // (the centre plus 0.5000099999997474 along the facing), power 0.5,
        // uncertainty 1.0, dispense event 1004 (the rocket's shoot sound).
        // asProjectile: a copy of one rocket, shot at an angle.
        ItemStack ExecuteFireworkRocket(const DispenseSource& source, ItemStack dispensed, bool& handled) {
            EntityLevel* entities = source.level.Entities();
            handled = false;
            if (!entities) return dispensed;
            const Direction direction = FacingOfSource(source);
            constexpr double kOutside = 0.5000099999997474;
            const glm::dvec3 position = source.Center() +
                glm::dvec3(StepX(direction), StepY(direction), StepZ(direction)) * kOutside;
            ItemStack one = dispensed;
            one.count = 1;
            auto rocket = std::make_unique<FireworkRocket>(entities);
            rocket->InitLaunch(position, one);
            rocket->SetShotAtAngle(true);
            rocket->Shoot(StepX(direction), StepY(direction), StepZ(direction), 0.5f, 1.0f);
            entities->AddFreshEntity(std::move(rocket));
            dispensed.count -= 1;
            if (dispensed.count <= 0) dispensed.Clear();
            handled = true;
            return dispensed;
        }
    } // namespace

    ItemStack DispenseItem(const DispenseSource& source, ItemStack stack) {
        const ItemID id = stack.itemId;
        bool flag = true;

        if (id == Items::FireworkRocket) {
            const ItemStack result = ExecuteFireworkRocket(source, stack, flag);
            if (flag) {
                // ProjectileDispenseBehavior.playSound: the override event
                // (1004, SOUND_FIREWORK_SHOOT) instead of 1002, then the
                // smoke out of the face.
                PlayLevelEventSound(source.level, nullptr, LevelEvent::SOUND_FIREWORK_SHOOT, source.pos, 0,
                                    source.level.Random());
                PlayDispenseAnimation(source);
                return result;
            }
            return DispenseDefault(source, stack);
        }

        if (IsProjectileItem(id)) {
            const ItemStack result = ExecuteProjectile(source, stack, flag);
            if (flag) { PlayShootSound(source); return result; }
            return DispenseDefault(source, stack);
        }
        if (id == Items::WindCharge) {
            const ItemStack result = ExecuteWindCharge(source, stack, flag);
            if (flag) {
                PlayLevelEventSound(source.level, nullptr, LevelEvent::SOUND_WIND_CHARGE_SHOOT,
                                    source.pos, 0, source.level.Random());
                PlayDispenseAnimation(source);
                return result;
            }
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
        // BoatDispenseItemBehavior / MinecartDispenseItemBehavior: their
        // playSound is levelEvent 1000 either way (after a fallback's own).
        if (IsVehicleItem(id)) {
            const ItemStack result = DispenseVehicleItem(source, stack);
            PlayDefaultSound(source, true);
            return result;
        }
        // The chest's OptionalDispenseItemBehavior: onto a chested horse, else
        // the default throw (success stays true — the 1000 event either way).
        if (id == ItemRegistry::FromBlock(BlockID::Chest)) {
            if (DispenseChestOntoChestedHorse(source, stack)) {
                PlayDefaultSound(source, true);
                return stack;
            }
            return DispenseDefault(source, stack);
        }
        // DispenserBlock.getDefaultDispenseMethod: anything EQUIPPABLE is
        // EquipmentDispenseItemBehavior — put on the entity in front, else
        // the default throw; dispense's sound is 1000 either way.
        if (!stack.IsEmpty() && stack.get(DataComponents::EQUIPPABLE)) {
            if (DispenseEquipment(source, stack)) {
                PlayDefaultSound(source, true);
                return stack;
            }
            return DispenseDefault(source, stack);
        }
        return DispenseDefault(source, stack);
    }

} // namespace Game
