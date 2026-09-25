// File: src/common/entity/effect/MobEffects.cpp
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/EntityType.hpp"
#include "common/entity/mobs/Slime.hpp"
#include "common/world/level/Explosion.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/sound/SoundEvents.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace Game {

    // ── Registry (MC MobEffects.java static block, verbatim) ───────────────
    //
    // Colours are MC's decimal literals. Attribute amounts are MC's doubles,
    // including the float-widening noise (0.20000000298023224 is how the
    // Java source spells 0.2F widened).
    namespace {
        using AO = AttributeOperation;
        using Cat = MobEffectCategory;
        using P = MobEffectParticle;

        constexpr EffectAttributeTemplate kNoModifier{Attribute::MaxHealth, ModifierId::EffectSpeed,
                                                      0.0, AO::AddValue};

        constexpr MobEffectInfo Plain(const char* name, const char* display, Cat cat, uint32_t color,
                                      P particle = P::EntityEffect) {
            return MobEffectInfo{name, display, cat, color, particle, false, 0, 0, 0, 0, kNoModifier};
        }
        constexpr MobEffectInfo Instant(const char* name, const char* display, Cat cat, uint32_t color) {
            return MobEffectInfo{name, display, cat, color, P::EntityEffect, true, 0, 0, 0, 0, kNoModifier};
        }
        constexpr MobEffectInfo Mod(const char* name, const char* display, Cat cat, uint32_t color,
                                    Attribute attr, ModifierId id, double amount, AO op) {
            return MobEffectInfo{name, display, cat, color, P::EntityEffect, false, 0, 0, 0,
                                 1, EffectAttributeTemplate{attr, id, amount, op}};
        }
        constexpr MobEffectInfo Blend(MobEffectInfo info, int in, int out, int outAdvance) {
            info.blendInTicks = in;
            info.blendOutTicks = out;
            info.blendOutAdvanceTicks = outAdvance;
            return info;
        }

        const MobEffectInfo kEffects[kMobEffectCount] = {
            /* SPEED */ Mod("speed", "Speed", Cat::Beneficial, 3402751,
                Attribute::MovementSpeed, ModifierId::EffectSpeed, 0.20000000298023224, AO::AddMultipliedTotal),
            /* SLOWNESS */ Mod("slowness", "Slowness", Cat::Harmful, 9154528,
                Attribute::MovementSpeed, ModifierId::EffectSlowness, -0.15000000596046448, AO::AddMultipliedTotal),
            /* HASTE */ Mod("haste", "Haste", Cat::Beneficial, 14270531,
                Attribute::AttackSpeed, ModifierId::EffectHaste, 0.10000000149011612, AO::AddMultipliedTotal),
            /* MINING_FATIGUE */ Mod("mining_fatigue", "Mining Fatigue", Cat::Harmful, 4866583,
                Attribute::AttackSpeed, ModifierId::EffectMiningFatigue, -0.10000000149011612, AO::AddMultipliedTotal),
            /* STRENGTH */ Mod("strength", "Strength", Cat::Beneficial, 16762624,
                Attribute::AttackDamage, ModifierId::EffectStrength, 3.0, AO::AddValue),
            /* INSTANT_HEALTH */ Instant("instant_health", "Instant Health", Cat::Beneficial, 16262179),
            /* INSTANT_DAMAGE */ Instant("instant_damage", "Instant Damage", Cat::Harmful, 11101546),
            /* JUMP_BOOST */ Mod("jump_boost", "Jump Boost", Cat::Beneficial, 16646020,
                Attribute::SafeFallDistance, ModifierId::EffectJumpBoost, 1.0, AO::AddValue),
            /* NAUSEA */ Blend(Plain("nausea", "Nausea", Cat::Harmful, 5578058), 150, 20, 60),
            /* REGENERATION */ Plain("regeneration", "Regeneration", Cat::Beneficial, 13458603),
            /* RESISTANCE */ Plain("resistance", "Resistance", Cat::Beneficial, 9520880),
            /* FIRE_RESISTANCE */ Plain("fire_resistance", "Fire Resistance", Cat::Beneficial, 16750848),
            /* WATER_BREATHING */ Plain("water_breathing", "Water Breathing", Cat::Beneficial, 10017472),
            // MC also gives INVISIBILITY a WAYPOINT_TRANSMIT_RANGE ×0 modifier
            // ("effect.waypoint_transmit_range_hide") — it hides you from the
            // locator bar. There is no locator bar / waypoint attribute here.
            /* INVISIBILITY */ Plain("invisibility", "Invisibility", Cat::Beneficial, 16185078),
            /* BLINDNESS */ Plain("blindness", "Blindness", Cat::Harmful, 2039587),
            /* NIGHT_VISION */ Plain("night_vision", "Night Vision", Cat::Beneficial, 12779366),
            /* HUNGER */ Plain("hunger", "Hunger", Cat::Harmful, 5797459),
            /* WEAKNESS */ Mod("weakness", "Weakness", Cat::Harmful, 4738376,
                Attribute::AttackDamage, ModifierId::EffectWeakness, -4.0, AO::AddValue),
            /* POISON */ Plain("poison", "Poison", Cat::Harmful, 8889187),
            /* WITHER */ Plain("wither", "Wither", Cat::Harmful, 7561558),
            /* HEALTH_BOOST */ Mod("health_boost", "Health Boost", Cat::Beneficial, 16284963,
                Attribute::MaxHealth, ModifierId::EffectHealthBoost, 4.0, AO::AddValue),
            /* ABSORPTION */ Mod("absorption", "Absorption", Cat::Beneficial, 2445989,
                Attribute::MaxAbsorption, ModifierId::EffectAbsorption, 4.0, AO::AddValue),
            /* SATURATION */ Instant("saturation", "Saturation", Cat::Beneficial, 16262179),
            /* GLOWING */ Plain("glowing", "Glowing", Cat::Neutral, 9740385),
            /* LEVITATION */ Plain("levitation", "Levitation", Cat::Harmful, 13565951),
            /* LUCK */ Mod("luck", "Luck", Cat::Beneficial, 5882118,
                Attribute::Luck, ModifierId::EffectLuck, 1.0, AO::AddValue),
            /* UNLUCK */ Mod("unluck", "Bad Luck", Cat::Harmful, 12624973,
                Attribute::Luck, ModifierId::EffectUnluck, -1.0, AO::AddValue),
            /* SLOW_FALLING */ Plain("slow_falling", "Slow Falling", Cat::Beneficial, 15978425),
            /* CONDUIT_POWER */ Plain("conduit_power", "Conduit Power", Cat::Beneficial, 1950417),
            /* DOLPHINS_GRACE */ Plain("dolphins_grace", "Dolphin's Grace", Cat::Beneficial, 8954814),
            // .withSoundOnAdded(APPLY_EFFECT_BAD_OMEN) — see GetEffectSoundOnAdded.
            /* BAD_OMEN */ Plain("bad_omen", "Bad Omen", Cat::Neutral, 745784),
            /* HERO_OF_THE_VILLAGE */ Plain("hero_of_the_village", "Hero of the Village", Cat::Beneficial, 4521796),
            /* DARKNESS */ Blend(Plain("darkness", "Darkness", Cat::Harmful, 2696993), 22, 22, 22),
            /* TRIAL_OMEN */ Plain("trial_omen", "Trial Omen", Cat::Neutral, 1484454, P::TrialOmen),
            /* RAID_OMEN */ Plain("raid_omen", "Raid Omen", Cat::Neutral, 14565464, P::RaidOmen),
            /* WIND_CHARGED */ Plain("wind_charged", "Wind Charged", Cat::Harmful, 12438015, P::SmallGust),
            /* WEAVING */ Plain("weaving", "Weaving", Cat::Harmful, 7891290, P::ItemCobweb),
            /* OOZING */ Plain("oozing", "Oozing", Cat::Harmful, 10092451, P::ItemSlime),
            /* INFESTED */ Plain("infested", "Infested", Cat::Harmful, 9214860, P::Infested),
            /* BREATH_OF_THE_NAUTILUS */ Plain("breath_of_the_nautilus", "Breath of the Nautilus",
                                               Cat::Beneficial, 65518),
        };

        // MC MobEffect.AMBIENT_ALPHA = Mth.floor(38.25F).
        constexpr uint32_t kAmbientAlpha = 38;

        EffectMobFactory g_effectMobFactory = nullptr;

        // Mth.randomBetweenInclusive(random, min, max).
        int RandomBetweenInclusive(JavaRandom& r, int min, int max) {
            return r.NextInt(max - min + 1) + min;
        }

        // Java's int shifts mask the distance to its low five bits
        // (JLS 15.19), so `50 >> 32` is 50 again and `4 << 30` wraps to 0.
        // /effect accepts amplifiers up to 255; a raw C++ shift by 32+ is
        // undefined. These keep MC's exact numbers for every amplifier.
        int JavaShr(int value, int distance) { return value >> (distance & 31); }
        int JavaShl(int value, int distance) {
            return static_cast<int>(static_cast<uint32_t>(value) << (distance & 31));
        }
    } // namespace

    const MobEffectInfo& GetEffectInfo(MobEffectId id) {
        const int i = static_cast<int>(id);
        return kEffects[(i >= 0 && i < kMobEffectCount) ? i : 0];
    }

    MobEffectCategory GetEffectCategory(MobEffectId id) { return GetEffectInfo(id).category; }
    bool IsBeneficialEffect(MobEffectId id) { return GetEffectInfo(id).category == MobEffectCategory::Beneficial; }
    bool IsInstantenousEffect(MobEffectId id) { return GetEffectInfo(id).instantaneous; }
    const char* GetEffectName(MobEffectId id) {
        const int i = static_cast<int>(id);
        return (i >= 0 && i < kMobEffectCount) ? kEffects[i].name : "unknown";
    }
    const char* GetEffectDisplayName(MobEffectId id) { return GetEffectInfo(id).displayName; }
    uint32_t GetEffectColor(MobEffectId id) { return GetEffectInfo(id).color; }
    bool IsValidEffectId(int raw) { return raw >= 0 && raw < kMobEffectCount; }

    bool ParseEffectId(std::string_view name, MobEffectId& out) {
        // MC Identifier.parse: a bare path defaults to the minecraft namespace;
        // any other namespace names an effect this registry does not have.
        constexpr std::string_view kNs = "minecraft:";
        if (name.substr(0, kNs.size()) == kNs) name.remove_prefix(kNs.size());
        else if (name.find(':') != std::string_view::npos) return false;
        for (int i = 0; i < kMobEffectCount; ++i) {
            if (name == kEffects[i].name) { out = static_cast<MobEffectId>(i); return true; }
        }
        return false;
    }

    void SetEffectMobFactory(EffectMobFactory factory) { g_effectMobFactory = factory; }

    const char* GetEffectSoundOnAdded(MobEffectId id) {
        // MobEffects.java's three .withSoundOnAdded(...) registrations.
        switch (id) {
            case MobEffectId::BadOmen:   return "event.mob_effect.bad_omen";
            case MobEffectId::TrialOmen: return "event.mob_effect.trial_omen";
            case MobEffectId::RaidOmen:  return "event.mob_effect.raid_omen";
            default:                     return nullptr;
        }
    }

    std::vector<LivingEntity*> AddEffectToPlayersAround(EntityLevel& level, Entity* source,
                                                        const glm::dvec3& position, double radius,
                                                        const MobEffectInstance& effect,
                                                        int displayEffectLimit) {
        // MC MobEffectUtil.addEffectToPlayersAround. The filter, term by term:
        //   gameMode.isSurvival()  — survival or adventure (not creative /
        //                            spectator);
        //   !source.isAlliedTo(p)  — scoreboard teams; none exist, never allied;
        //   position.closerThan(p.position(), radius) — strict, squared;
        //   and the player does not already carry an equal-or-stronger
        //   instance that will outlast the display limit.
        std::vector<LivingEntity*> players;
        level.GetPlayers(players);
        std::vector<LivingEntity*> affected;
        for (LivingEntity* player : players) {
            if (!player || player->IsCreative() || player->IsSpectator()) continue;
            const glm::dvec3 d = player->position - position;
            if (!(d.x * d.x + d.y * d.y + d.z * d.z < radius * radius)) continue;
            if (const MobEffectInstance* existing = player->GetEffect(effect.effect)) {
                if (existing->amplifier >= effect.amplifier &&
                    !existing->EndsWithin(displayEffectLimit - 1)) {
                    continue;
                }
            }
            affected.push_back(player);
        }
        // players.forEach(p -> p.addEffect(new MobEffectInstance(effect), source)).
        for (LivingEntity* player : affected) {
            player->AddEffect(MobEffectInstance(effect), source);
        }
        return affected;
    }

    // ── Tick cadence (MC MobEffect.shouldApplyEffectTickThisTick) ──────────

    bool ShouldApplyEffectTickThisTick(MobEffectId id, int tickCount, int amplifier) {
        switch (id) {
            case MobEffectId::Regeneration: {
                // RegenerationMobEffect: every 50 >> amp ticks; every tick once
                // the shift reaches zero.
                const int interval = JavaShr(50, amplifier);
                return interval > 0 ? tickCount % interval == 0 : true;
            }
            case MobEffectId::Poison: {
                const int interval = JavaShr(25, amplifier);   // PoisonMobEffect.DAMAGE_INTERVAL
                return interval > 0 ? tickCount % interval == 0 : true;
            }
            case MobEffectId::Wither: {
                const int interval = JavaShr(40, amplifier);   // WitherMobEffect.DAMAGE_INTERVAL
                return interval > 0 ? tickCount % interval == 0 : true;
            }
            case MobEffectId::Hunger:        // HungerMobEffect
            case MobEffectId::Absorption:    // AbsorptionMobEffect
            case MobEffectId::BadOmen:       // BadOmenMobEffect
                return true;
            case MobEffectId::RaidOmen:
                // RaidOmenMobEffect: the tick its countdown reaches 1.
                return tickCount == 1;
            case MobEffectId::InstantHealth:
            case MobEffectId::InstantDamage:
            case MobEffectId::Saturation:
                // InstantaneousMobEffect: fire exactly once, on the tick the
                // 1-tick duration is still standing.
                return tickCount >= 1;
            default:
                return false;                // MobEffect base
        }
    }

    // ── Per-tick application (MC *MobEffect.applyEffectTick) ───────────────

    bool ApplyEffectTick(LivingEntity& mob, MobEffectId id, int amplifier) {
        switch (id) {
            case MobEffectId::Regeneration:
                // RegenerationMobEffect.
                if (mob.GetHealth() < mob.GetMaxHealth()) mob.Heal(1.0f);
                return true;
            case MobEffectId::Poison:
                // PoisonMobEffect: cannot kill — the hurt is skipped at 1 HP.
                if (mob.GetHealth() > 1.0f) {
                    mob.Hurt(MobDamageSource::Magic, 1.0f, nullptr);
                }
                return true;
            case MobEffectId::Wither:
                // WitherMobEffect: CAN kill.
                mob.Hurt(MobDamageSource::Wither, 1.0f, nullptr);
                return true;
            case MobEffectId::InstantHealth:
            case MobEffectId::InstantDamage: {
                // HealOrHarmMobEffect.applyEffectTick — the drink/direct path
                // (no distance scale).
                const bool isHarm = id == MobEffectId::InstantDamage;
                if (isHarm == mob.IsInvertedHealAndHarm()) {
                    mob.Heal(static_cast<float>(std::max(JavaShl(4, amplifier), 0)));
                } else {
                    mob.Hurt(MobDamageSource::Magic,
                             static_cast<float>(JavaShl(6, amplifier)), nullptr);
                }
                return true;
            }
            case MobEffectId::Hunger:
                // HungerMobEffect — `if (mob instanceof Player)`: the hook is
                // a no-op on every LivingEntity but the player view.
                mob.CauseFoodExhaustion(0.005f * static_cast<float>(amplifier + 1));
                return true;
            case MobEffectId::Saturation:
                // SaturationMobEffect — player-only, same hook pattern
                // (FoodData.eat(amp + 1, 1.0F)).
                mob.EatFood(amplifier + 1, 1.0f);
                return true;
            case MobEffectId::Absorption:
                // AbsorptionMobEffect: the effect ends once the hearts are gone.
                return mob.GetAbsorptionAmount() > 0.0f;
            case MobEffectId::BadOmen:
                // BadOmenMobEffect: a survival player standing in a VILLAGE on
                // a non-peaceful level trades it for RAID_OMEN (600 ticks) and
                // the effect ends. Level.isVillage needs village POIs and
                // raids need the raid manager — neither exists, so no player
                // is ever "in a village" and Bad Omen simply stays, as it
                // does in vanilla outside villages.
                return true;
            case MobEffectId::RaidOmen:
                // RaidOmenMobEffect: on its last tick, start the raid at the
                // stored raid-omen position and end. The position is only set
                // by BadOmen's village branch above, which cannot fire, so
                // there is never a position and the effect runs out.
                return true;
            default:
                return true;   // MobEffect base: nothing to do, keep the effect
        }
    }

    void ApplyInstantenousEffect(Entity* source, Entity* owner, LivingEntity& mob,
                                 MobEffectId id, int amplifier, double scale) {
        switch (id) {
            case MobEffectId::InstantHealth:
            case MobEffectId::InstantDamage: {
                // HealOrHarmMobEffect.applyInstantaneousEffect — the splash
                // falloff rounds through (int)(scale * base + 0.5).
                const bool isHarm = id == MobEffectId::InstantDamage;
                if (isHarm == mob.IsInvertedHealAndHarm()) {
                    const int amount = static_cast<int>(
                        scale * static_cast<double>(JavaShl(4, amplifier)) + 0.5);
                    mob.Heal(static_cast<float>(amount));
                } else {
                    const int amount = static_cast<int>(
                        scale * static_cast<double>(JavaShl(6, amplifier)) + 0.5);
                    // MC damageSources().indirectMagic(source, owner) — the
                    // attribution goes to the OWNER so retaliation targets the
                    // thrower, with the projectile as fallback; magic() when
                    // there is no source at all.
                    mob.Hurt(MobDamageSource::Magic, static_cast<float>(amount),
                             source ? (owner ? owner : source) : nullptr);
                }
                break;
            }
            default:
                // MobEffect base: applyInstantaneousEffect falls through to
                // applyEffectTick (Saturation takes this path).
                ApplyEffectTick(mob, id, amplifier);
                break;
        }
    }

    // ── onEffectStarted / onMobRemoved / onMobHurt ─────────────────────────

    void OnEffectStarted(LivingEntity& mob, MobEffectId id, int amplifier) {
        if (id == MobEffectId::Absorption) {
            // AbsorptionMobEffect.onEffectStarted: top the hearts up to
            // 4 * (1 + amp), never down; setAbsorptionAmount clamps to the
            // MAX_ABSORPTION the effect's own modifier just raised.
            mob.SetAbsorptionAmountClamped(std::max(mob.GetAbsorptionAmount(),
                                                    static_cast<float>(4 * (1 + amplifier))));
        }
    }

    namespace {
        // WindChargedMobEffect.onMobRemoved: a wind-charge burst centred on the
        // body — radius 3 + nextFloat() * 2, TRIGGER interaction, no entity
        // damage (AbstractWindCharge.EXPLOSION_DAMAGE_CALCULATOR: blocks yes,
        // entities no, no knockback multiplier).
        void WindChargedBurst(LivingEntity& mob) {
            EntityLevel* level = mob.Level();
            if (!level) return;
            ExplosionParams p;
            p.center = glm::dvec3(mob.position.x,
                                  mob.position.y + static_cast<double>(mob.GetBbHeight() / 2.0f),
                                  mob.position.z);
            p.radius              = 3.0f + level->Random().NextFloat() * 2.0f;
            p.source              = &mob;
            p.attributedTo        = nullptr;
            p.interaction         = ExplosionInteraction::Trigger;
            p.damageEntities      = false;
            p.knockbackMultiplier = 1.0f;
            Explode(*level, p);
        }

        // WeavingMobEffect.spawnCobwebsRandomlyAround: 15 random cells of the
        // 3x3x3 cube round the body; each replaceable cell on a sturdy floor
        // becomes a cobweb, up to 2-3 of them (Mth.randomBetweenInclusive).
        void WeavingCobwebs(LivingEntity& mob) {
            EntityLevel* level = mob.Level();
            if (!level || !level->Blocks()) return;
            JavaRandom& random = level->Random();
            const int cobwebCount = RandomBetweenInclusive(random, 2, 3);
            const glm::ivec3 center(static_cast<int>(std::floor(mob.position.x)),
                                    static_cast<int>(std::floor(mob.position.y)),
                                    static_cast<int>(std::floor(mob.position.z)));
            std::vector<glm::ivec3> positions;
            // BlockPos.randomInCube(random, 15, pos, 1) — randomBetweenClosed:
            // x, y, z drawn in that order, width 3 on each axis.
            for (int n = 0; n < 15; ++n) {
                const glm::ivec3 p(center.x - 1 + random.NextInt(3),
                                   center.y - 1 + random.NextInt(3),
                                   center.z - 1 + random.NextInt(3));
                if (std::find(positions.begin(), positions.end(), p) != positions.end()) continue;
                const BlockState state = level->Blocks()->GetBlockState(p.x, p.y, p.z);
                if (!BlockRegistry::Get(state.Block()).replaceable) continue;
                if (!IsFaceSturdyAt(*level->Blocks(), p - glm::ivec3(0, 1, 0), Direction::Up)) continue;
                positions.push_back(p);
                if (static_cast<int>(positions.size()) >= cobwebCount) break;
            }
            // levelEvent 3018 (the cobweb poof) has no client particle here.
            for (const glm::ivec3& p : positions) level->SetBlock(p, BlockID::Cobweb);
        }

        // OozingMobEffect.onMobRemoved: two size-2 slimes at y + 0.5, capped
        // so the slimes within 2 blocks never exceed max_entity_cramming.
        void OozingSlimes(LivingEntity& mob) {
            EntityLevel* level = mob.Level();
            if (!level || !g_effectMobFactory) return;
            const int requested = 2;   // (random) -> 2
            const int maxEntityCramming = Rules::GetInt(Rules::Id::MaxEntityCramming);
            int toSpawn = requested;
            if (maxEntityCramming >= 1) {
                // NearbySlimes.closeTo(mob).count(maxEntityCramming): slimes in
                // the box inflated by 2, the dying mob excluded, capped.
                AABB box = mob.GetAABB();
                box.min -= glm::vec3(2.0f);
                box.max += glm::vec3(2.0f);
                std::vector<Entity*> nearby;
                level->GetEntitiesInBox(box, &mob, nearby);
                int slimes = 0;
                for (Entity* e : nearby) {
                    if (e && !e->IsRemoved() && e->GetType() == EntityTypeId::Slime) {
                        if (++slimes >= maxEntityCramming) break;
                    }
                }
                toSpawn = std::clamp(maxEntityCramming - slimes, 0, requested);
            }
            for (int i = 0; i < toSpawn; ++i) {
                std::unique_ptr<Mob> spawned = g_effectMobFactory(EntityTypeId::Slime, level);
                if (!spawned) continue;
                if (auto* slime = dynamic_cast<Slime*>(spawned.get())) slime->SetSize(2, true);
                spawned->position = glm::dvec3(mob.position.x, mob.position.y + 0.5, mob.position.z);
                spawned->oldPosition = spawned->position;
                spawned->yRot = level->Random().NextFloat() * 360.0f;
                spawned->xRot = 0.0f;
                level->AddFreshEntity(std::move(spawned));
            }
        }
    } // namespace

    void OnEffectMobRemoved(LivingEntity& mob, MobEffectId id, int amplifier,
                            RemovalReason reason) {
        (void)amplifier;
        if (reason != RemovalReason::Killed) return;
        EntityLevel* level = mob.Level();
        if (!level || level->IsClientSide()) return;
        switch (id) {
            case MobEffectId::WindCharged:
                WindChargedBurst(mob);
                break;
            case MobEffectId::Weaving:
                // Players always weave; anything else only under mobGriefing.
                if (mob.IsPlayer() || level->MobGriefing()) WeavingCobwebs(mob);
                break;
            case MobEffectId::Oozing:
                OozingSlimes(mob);
                break;
            default:
                break;
        }
    }

    void OnEffectMobHurt(LivingEntity& mob, MobEffectId id, int amplifier) {
        (void)amplifier;
        if (id != MobEffectId::Infested) return;
        EntityLevel* level = mob.Level();
        if (!level || level->IsClientSide() || !g_effectMobFactory) return;
        // InfestedMobEffect.onMobHurt: 10 % to release 1-2 silverfish from the
        // body's middle, thrown along the look direction (x0.3, y x1.5)
        // rotated by a random ±90°.
        JavaRandom& random = level->Random();
        constexpr float kChanceToSpawn = 0.1f;
        if (random.NextFloat() > kChanceToSpawn) return;
        const int count = RandomBetweenInclusive(random, 1, 2);
        const double x = mob.position.x;
        const double y = mob.position.y + static_cast<double>(mob.GetBbHeight()) / 2.0;
        const double z = mob.position.z;
        for (int i = 0; i < count; ++i) {
            std::unique_ptr<Mob> silverfish = g_effectMobFactory(EntityTypeId::Silverfish, level);
            if (!silverfish) continue;
            // Mth.randomBetween(random, -PI/2, PI/2).
            const float randomAngle = random.NextFloat() * 3.1415927f - 1.5707964f;
            glm::vec3 dir = Mth::ViewVector(mob.xRot, mob.yRot) * 0.3f;
            dir.y *= 1.5f;
            // JOML Vector3f.rotateY(angle).
            const float c = std::cos(randomAngle), s = std::sin(randomAngle);
            const glm::vec3 rotated(dir.x * c + dir.z * s, dir.y, -dir.x * s + dir.z * c);
            silverfish->position = glm::dvec3(x, y, z);
            silverfish->oldPosition = silverfish->position;
            silverfish->yRot = level->Random().NextFloat() * 360.0f;
            silverfish->xRot = 0.0f;
            silverfish->velocity = glm::dvec3(rotated);
            silverfish->PlaySound(SoundEvents::SILVERFISH_HURT, 1.0f, 1.0f);
            level->AddFreshEntity(std::move(silverfish));
        }
    }

    // ── Attribute templates (MC MobEffect.add/removeAttributeModifiers) ────

    void AddEffectAttributeModifiers(AttributeMap& attributes, MobEffectId id,
                                     int amplifier) {
        const MobEffectInfo& info = GetEffectInfo(id);
        if (info.modifierCount == 0) return;
        const EffectAttributeTemplate& t = info.modifier;
        // MC: `if (attribute != null)` — an attribute the entity lacks is
        // skipped rather than created.
        AttributeInstance* inst = attributes.Find(t.attribute);
        if (!inst) return;
        // MC removes then re-adds, so an upgrade replaces rather than stacks.
        inst->RemoveModifier(t.id);
        AttributeModifier mod;
        mod.id = static_cast<uint32_t>(t.id);
        mod.amount = t.amount * static_cast<double>(amplifier + 1);
        mod.operation = t.operation;
        inst->AddModifier(mod);
    }

    void RemoveEffectAttributeModifiers(AttributeMap& attributes, MobEffectId id) {
        const MobEffectInfo& info = GetEffectInfo(id);
        if (info.modifierCount == 0) return;
        attributes.RemoveModifier(info.modifier.attribute, info.modifier.id);
    }

    double ComputeAttributeWithEffects(Attribute attribute, double base,
                                       const std::vector<MobEffectInstance>& effects) {
        AttributeInstance inst(attribute, base);
        AddEffectAttributeModifiers(inst, effects);
        return inst.GetValue();
    }

    void AddEffectAttributeModifiers(AttributeInstance& instance,
                                     const std::vector<MobEffectInstance>& effects) {
        for (const MobEffectInstance& e : effects) {
            const MobEffectInfo& info = GetEffectInfo(e.effect);
            if (info.modifierCount == 0 || info.modifier.attribute != instance.GetAttribute()) continue;
            AttributeModifier mod;
            mod.id = static_cast<uint32_t>(info.modifier.id);
            mod.amount = info.modifier.amount * static_cast<double>(e.amplifier + 1);
            mod.operation = info.modifier.operation;
            instance.AddModifier(mod);
        }
    }

    // ── Undead membership ──────────────────────────────────────────────────

    bool IsUndeadEntityType(EntityTypeId type) {
        // Vanilla data: #minecraft:undead = #zombies + #skeletons + phantom +
        // wither. Transcribed from the 1.21 data pack (the tag JSONs are not
        // part of the decompiled source tree).
        switch (type) {
            // #minecraft:zombies
            case EntityTypeId::Zombie:
            case EntityTypeId::ZombieVillager:
            case EntityTypeId::ZombifiedPiglin:
            case EntityTypeId::ZombieHorse:
            case EntityTypeId::Zoglin:
            case EntityTypeId::Husk:
            case EntityTypeId::Drowned:
            case EntityTypeId::ZombieNautilus:
            case EntityTypeId::CamelHusk:
            // #minecraft:skeletons
            case EntityTypeId::Skeleton:
            case EntityTypeId::Stray:
            case EntityTypeId::WitherSkeleton:
            case EntityTypeId::SkeletonHorse:
            case EntityTypeId::Bogged:
            case EntityTypeId::Parched:
            // direct members
            case EntityTypeId::Phantom:
            case EntityTypeId::Wither:
                return true;
            default:
                return false;
        }
    }

    bool CanBreatheUnderWaterEntityType(EntityTypeId type) {
        // Vanilla data: #minecraft:can_breathe_under_water = #undead + the
        // list below, transcribed from the 1.21 data pack
        // (data/minecraft/tags/entity_type/can_breathe_under_water.json,
        // vendored under data/). armor_stand is in the tag but is not an
        // entity type here. Note the tag is also the AXOLOTL's water half —
        // its 6000-tick land dry-out is Axolotl.handleAirSupply, which waits
        // on the axolotl's own class (still a generic Animal). The dolphin is
        // deliberately ABSENT: dolphins drown underwater (4800 ticks of air).
        // A baby happy ghast also breathes underwater in MC
        // (HappyGhast.canBreatheUnderwater); the adult does not — that
        // isBaby() split rides the happy ghast's own class if it ever
        // matters (ghastlings avoid water entirely).
        if (IsUndeadEntityType(type)) return true;
        switch (type) {
            case EntityTypeId::Axolotl:
            case EntityTypeId::Frog:
            case EntityTypeId::Guardian:
            case EntityTypeId::ElderGuardian:
            case EntityTypeId::Turtle:
            case EntityTypeId::GlowSquid:
            case EntityTypeId::Cod:
            case EntityTypeId::Pufferfish:
            case EntityTypeId::Salmon:
            case EntityTypeId::Squid:
            case EntityTypeId::TropicalFish:
            case EntityTypeId::Tadpole:
            case EntityTypeId::CopperGolem:
            case EntityTypeId::Nautilus:
                return true;
            default:
                return false;
        }
    }

    // ── MobEffectInstance ──────────────────────────────────────────────────

    MobEffectInstance MobEffectInstance::WithScaledDuration(float scale) const {
        MobEffectInstance copy(*this);
        if (!copy.IsInfiniteDuration() && copy.duration != 0) {
            copy.duration = std::max(static_cast<int>(std::floor(static_cast<float>(copy.duration) * scale)), 1);
        }
        return copy;
    }

    bool MobEffectInstance::Update(const MobEffectInstance& takeOver) {
        // MC MobEffectInstance.update, transcribed.
        bool changed = false;
        if (takeOver.amplifier > amplifier) {
            if (takeOver.IsShorterDurationThan(*this)) {
                // Stash the current (weaker, longer) effect underneath.
                std::unique_ptr<MobEffectInstance> prevHidden = std::move(hiddenEffect);
                hiddenEffect = std::make_unique<MobEffectInstance>(*this);
                hiddenEffect->hiddenEffect = std::move(prevHidden);
            }
            amplifier = takeOver.amplifier;
            duration = takeOver.duration;
            changed = true;
        } else if (IsShorterDurationThan(takeOver)) {
            if (takeOver.amplifier == amplifier) {
                duration = takeOver.duration;
                changed = true;
            } else if (!hiddenEffect) {
                hiddenEffect = std::make_unique<MobEffectInstance>(takeOver);
            } else {
                hiddenEffect->Update(takeOver);
            }
        }

        if ((!takeOver.ambient && ambient) || changed) {
            ambient = takeOver.ambient;
            changed = true;
        }
        if (takeOver.visible != visible) {
            visible = takeOver.visible;
            changed = true;
        }
        if (takeOver.showIcon != showIcon) {
            showIcon = takeOver.showIcon;
            changed = true;
        }
        return changed;
    }

    void MobEffectInstance::TickDownDuration() {
        // MC tickDownDuration: the whole hidden chain counts down in step, so
        // a surfaced hidden effect has already paid for the time it spent
        // buried.
        if (hiddenEffect) hiddenEffect->TickDownDuration();
        if (!IsInfiniteDuration() && duration != 0) --duration;
    }

    bool MobEffectInstance::DowngradeToHiddenEffect() {
        if (duration != 0 || !hiddenEffect) return false;
        SetDetailsFrom(*hiddenEffect);
        std::unique_ptr<MobEffectInstance> next = std::move(hiddenEffect->hiddenEffect);
        hiddenEffect = std::move(next);
        return true;
    }

    bool MobEffectInstance::TickServer(LivingEntity& target, bool& downgraded) {
        downgraded = false;
        if (!HasRemainingDuration()) return false;

        // MC: a finite effect keys its cadence on the remaining duration, an
        // infinite one on the entity's own tick count.
        const int tickCount = CadenceTickCount(target.tickCount);
        if (ShouldApplyEffectTickThisTick(effect, tickCount, amplifier) &&
            !ApplyEffectTick(target, effect, amplifier)) {
            return false;
        }
        return FinishServerTick(downgraded);
    }

    void MobEffectInstance::TickBlend() {
        // MC BlendState.tick.
        blendFactorPrevFrame = blendFactor;
        const MobEffectInfo& info = GetEffectInfo(effect);
        const bool hasEffect = !EndsWithin(info.blendOutAdvanceTicks);
        const float target = hasEffect ? 1.0f : 0.0f;
        if (blendFactor == target) return;
        const int blendDuration = hasEffect ? info.blendInTicks : info.blendOutTicks;
        if (blendDuration == 0) {
            blendFactor = target;
        } else {
            const float maxDeltaPerTick = 1.0f / static_cast<float>(blendDuration);
            blendFactor += std::clamp(target - blendFactor, -maxDeltaPerTick, maxDeltaPerTick);
        }
    }

    void MobEffectInstance::TickClient() {
        // MC MobEffectInstance.tickClient.
        if (HasRemainingDuration()) {
            TickDownDuration();
            DowngradeToHiddenEffect();
        }
        TickBlend();
    }

    void MobEffectInstance::SkipBlending() {
        // MC BlendState.setImmediate.
        const bool hasEffect = !EndsWithin(GetEffectInfo(effect).blendOutAdvanceTicks);
        blendFactor = hasEffect ? 1.0f : 0.0f;
        blendFactorPrevFrame = blendFactor;
    }

    int MobEffectInstance::CompareTo(const MobEffectInstance& o) const {
        // MC compareTo: two Guava ComparisonChains. Booleans order false first.
        auto cmpBool = [](bool a, bool b) { return a == b ? 0 : (a ? 1 : -1); };
        auto cmpInt  = [](int64_t a, int64_t b) { return a < b ? -1 : (a > b ? 1 : 0); };
        const int64_t colorA = GetEffectColor(effect), colorB = GetEffectColor(o.effect);
        constexpr int kUpdateCutOff = 32147;
        if ((duration <= kUpdateCutOff || o.duration <= kUpdateCutOff) && (!ambient || !o.ambient)) {
            if (int c = cmpBool(ambient, o.ambient)) return c;
            if (int c = cmpBool(IsInfiniteDuration(), o.IsInfiniteDuration())) return c;
            if (int c = cmpInt(duration, o.duration)) return c;
            return static_cast<int>(cmpInt(colorA, colorB));
        }
        if (int c = cmpBool(ambient, o.ambient)) return c;
        return static_cast<int>(cmpInt(colorA, colorB));
    }

    uint32_t MobEffectInstance::ParticleArgb() const {
        // MobEffect's default particleFactory: ARGB.color(alpha, color).
        const uint32_t alpha = ambient ? kAmbientAlpha : 255u;
        return (alpha << 24) | (GetEffectColor(effect) & 0x00FFFFFFu);
    }

    // ── MobEffectUtil ──────────────────────────────────────────────────────

    const MobEffectInstance* FindEffectIn(const std::vector<MobEffectInstance>& effects,
                                          MobEffectId id) {
        for (const MobEffectInstance& e : effects) {
            if (e.effect == id) return &e;
        }
        return nullptr;
    }

    bool HasDigSpeed(const std::vector<MobEffectInstance>& effects) {
        return HasEffectIn(effects, MobEffectId::Haste) || HasEffectIn(effects, MobEffectId::ConduitPower);
    }

    int GetDigSpeedAmplification(const std::vector<MobEffectInstance>& effects) {
        int a = 0, b = 0;
        if (const auto* haste = FindEffectIn(effects, MobEffectId::Haste)) a = haste->amplifier;
        if (const auto* conduit = FindEffectIn(effects, MobEffectId::ConduitPower)) b = conduit->amplifier;
        return std::max(a, b);
    }

    bool HasWaterBreathing(const std::vector<MobEffectInstance>& effects) {
        return HasEffectIn(effects, MobEffectId::WaterBreathing) ||
               HasEffectIn(effects, MobEffectId::ConduitPower) ||
               HasEffectIn(effects, MobEffectId::BreathOfTheNautilus);
    }

    bool ShouldEffectsRefillAirSupply(const std::vector<MobEffectInstance>& effects) {
        return !HasEffectIn(effects, MobEffectId::BreathOfTheNautilus) ||
               HasEffectIn(effects, MobEffectId::WaterBreathing) ||
               HasEffectIn(effects, MobEffectId::ConduitPower);
    }

    float GetEffectDigSpeedMultiplier(const std::vector<MobEffectInstance>& effects) {
        // Player.getDestroySpeed, the two effect steps in their order.
        float speed = 1.0f;
        if (HasDigSpeed(effects)) {
            speed *= 1.0f + static_cast<float>(GetDigSpeedAmplification(effects) + 1) * 0.2f;
        }
        if (const auto* fatigue = FindEffectIn(effects, MobEffectId::MiningFatigue)) {
            speed *= static_cast<float>(std::pow(0.3, static_cast<double>(fatigue->amplifier + 1)));
        }
        return speed;
    }

    std::string FormatEffectDuration(const MobEffectInstance& instance, float tickrate) {
        if (instance.IsInfiniteDuration()) return "\xE2\x88\x9E";   // effect.duration.infinite "∞"
        // StringUtil.formatTickDuration(Mth.floor(duration * 1.0F), tickrate).
        int seconds = static_cast<int>(std::floor(static_cast<float>(instance.duration) / tickrate));
        int minutes = seconds / 60;
        seconds %= 60;
        const int hours = minutes / 60;
        minutes %= 60;
        char buf[32];
        if (hours > 0) std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hours, minutes, seconds);
        else           std::snprintf(buf, sizeof(buf), "%02d:%02d", minutes, seconds);
        return buf;
    }

    std::string GetEffectInstanceDisplayName(const MobEffectInstance& instance) {
        std::string name = GetEffectDisplayName(instance.effect);
        if (instance.amplifier >= 1 && instance.amplifier <= 9) {
            // enchantment.level.2 .. enchantment.level.10.
            static const char* kLevels[] = {"II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"};
            name += ' ';
            name += kLevels[instance.amplifier - 1];
        }
        return name;
    }

    float NightVisionScale(const MobEffectInstance& nightVision, float partialTick) {
        // GameRenderer.nightVisionScale: steady until the last 10 seconds,
        // then a 0.7 ± 0.3 flicker.
        if (!nightVision.EndsWithin(200)) return 1.0f;
        return 0.7f + std::sin((static_cast<float>(nightVision.duration) - partialTick) *
                               3.1415927f * 0.2f) * 0.3f;
    }

    bool AreAllEffectsAmbient(const std::vector<MobEffectInstance>& effects) {
        for (const MobEffectInstance& e : effects) {
            if (e.visible && !e.ambient) return false;
        }
        return true;
    }

    // ── Synched visuals ────────────────────────────────────────────────────

    EffectVisuals EffectVisuals::FromEffects(const std::vector<MobEffectInstance>& effects,
                                             bool glowing) {
        EffectVisuals v;
        // MC updateInvisibilityStatus: an empty map clears both; otherwise
        // invisible == hasEffect(INVISIBILITY), and the particle list is the
        // VISIBLE effects' particle options.
        for (const MobEffectInstance& e : effects) {
            if (e.effect == MobEffectId::Invisibility) v.flags |= kFlagInvisible;
            if (!e.visible) continue;
            v.particles.push_back(static_cast<uint8_t>(static_cast<uint8_t>(e.effect) |
                                                       (e.ambient ? kAmbientBit : 0)));
        }
        if (glowing) v.flags |= kFlagGlowing;
        return v;
    }

    bool EffectVisuals::AllAmbient() const {
        for (uint8_t p : particles) {
            if ((p & kAmbientBit) == 0) return false;
        }
        return true;
    }

    void SpawnEffectParticles(EntityLevel& level, const EffectVisuals& visuals,
                              const glm::dvec3& pos, float bbWidth, float bbHeight) {
        // MC LivingEntity.tickEffects, client branch.
        if (visuals.particles.empty()) return;
        JavaRandom& random = level.Random();
        const int bound = visuals.Invisible() ? 15 : 4;
        const int ambientFactor = visuals.AllAmbient() ? 5 : 1;
        if (random.NextInt(bound * ambientFactor) != 0) return;
        // Util.getRandom(particles, random).
        const uint8_t pick = visuals.particles[static_cast<size_t>(
            random.NextInt(static_cast<int32_t>(visuals.particles.size())))];
        const int raw = pick & 0x7F;
        if (!IsValidEffectId(raw)) return;
        const MobEffectInstance probe(static_cast<MobEffectId>(raw), 1, 0,
                                      (pick & EffectVisuals::kAmbientBit) != 0, true);
        // Entity.getRandomX(0.5) / getRandomY() / getRandomZ(0.5).
        const double w = static_cast<double>(bbWidth);
        const double x = pos.x + w * (2.0 * random.NextDouble() - 1.0) * 0.5;
        const double y = pos.y + static_cast<double>(bbHeight) * random.NextDouble();
        const double z = pos.z + w * (2.0 * random.NextDouble() - 1.0) * 0.5;
        // Every kind draws as the coloured ENTITY_EFFECT spell: the port's
        // particle engine has no TRIAL_OMEN / RAID_OMEN / SMALL_GUST /
        // ITEM_COBWEB / ITEM_SLIME / INFESTED sprites, so those six effects
        // show their registry colour instead of their own particle.
        const uint32_t argb = probe.ParticleArgb();
        const float a = static_cast<float>((argb >> 24) & 0xFF) / 255.0f;
        const float r = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
        const float g = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
        const float b = static_cast<float>(argb & 0xFF) / 255.0f;
        // MC passes (1.0, 1.0, 1.0) as the speed arguments; SpellParticle's
        // MobEffectProvider ignores them in favour of its own drift.
        level.AddColorParticle(ParticleKind::EntityEffect, x, y, z, 1.0, 1.0, 1.0, r, g, b, a);
    }

} // namespace Game
