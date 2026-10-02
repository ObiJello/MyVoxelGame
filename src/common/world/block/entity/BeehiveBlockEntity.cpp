// File: src/common/world/block/entity/BeehiveBlockEntity.cpp
#include "BeehiveBlockEntity.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/physics/Physics.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldMobSpawn.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>

namespace Game {

    namespace {

        // BeehiveBlockEntity.IGNORED_BEE_TAGS.
        constexpr const char* kIgnoredBeeTags[] = {
            "Air", "drop_chances", "equipment", "Brain", "CanPickUpLoot", "DeathTime", "fall_distance",
            "FallFlying", "Fire", "HurtTime", "LeftHanded", "Motion", "NoGravity", "OnGround",
            "PortalCooldown", "Pos", "Rotation", "sleeping_pos", "CannotEnterHiveTicks",
            "TicksSincePollination", "CropsGrownSincePollination", "hive_pos", "Passengers", "leash", "UUID",
        };

        bool HasNectar(const BeehiveOccupant& o) {
            const auto& tag = o.entityData.Tag();
            auto it = tag.value.find("HasNectar");
            if (it == tag.value.end() || !it->second) return false;
            if (it->second->type == ::World::NBTTagType::TAG_Byte) {
                return static_cast<const ::World::NBTTagByte&>(*it->second).value != 0;
            }
            return false;
        }

        // `instanceof FireBlock` — soul fire is a BaseFireBlock, not a FireBlock.
        bool IsFire(BlockID id) { return id == BlockID::Fire; }

        bool IsLitCampfire(BlockState state) {
            const BlockID id = state.Block();
            if (id != BlockID::Campfire && id != BlockID::SoulCampfire) return false;
            return state.GetValueByName("lit") == "true";
        }

        int FacingIndex(BlockState state) {
            const std::string_view f = state.GetValueByName("facing");
            if (f == "south") return 3;
            if (f == "west")  return 4;
            if (f == "east")  return 5;
            return 2;   // north
        }

        constexpr glm::ivec3 kSteps[6] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};

    } // namespace

    bool IsSmokeyPos(const ILevelWrite& level, const glm::ivec3& pos) {
        for (int i = 1; i <= 5; ++i) {
            const glm::ivec3 p = pos - glm::ivec3(0, i, 0);
            const BlockState state = level.GetBlockState(p.x, p.y, p.z);
            if (IsLitCampfire(state)) return true;
            // SHAPE_VIRTUAL_POST: the 4-wide centre column, full height.
            bool blocked = false;
            for (const auto& box : BlockRegistry::GetBlockCollisionShapeSet(state)) {
                if (box.max.x > 6.0f / 16.0f && box.min.x < 10.0f / 16.0f &&
                    box.max.z > 6.0f / 16.0f && box.min.z < 10.0f / 16.0f &&
                    box.max.y > 0.0f && box.min.y < 1.0f) {
                    blocked = true;
                    break;
                }
            }
            if (blocked) {
                const BlockState below = level.GetBlockState(p.x, p.y - 1, p.z);
                return IsLitCampfire(below);
            }
        }
        return false;
    }

    bool BeesStayInHive(ILevelWrite& level, const glm::ivec3& pos) {
        if (level.GetDimension() != DimensionId::Overworld) return false;
        EntityLevel* entities = level.Entities();
        if (!entities) return false;
        return !entities->IsDay() || entities->IsRainingAt(pos);
    }

    void AngerNearbyBees(ILevelWrite& level, const glm::ivec3& pos) {
        EntityLevel* entities = level.Entities();
        if (!entities) return;
        const AABB area = AABB::FromMinMax(glm::vec3(pos) - glm::vec3(8.0f, 6.0f, 8.0f),
                                           glm::vec3(pos) + glm::vec3(9.0f, 7.0f, 9.0f));
        std::vector<Entity*> nearby;
        entities->GetEntitiesInBox(area, nullptr, nearby);
        std::vector<Bee*> bees;
        for (Entity* e : nearby) {
            if (auto* bee = dynamic_cast<Bee*>(e); bee && bee->IsAlive()) bees.push_back(bee);
        }
        if (bees.empty()) return;
        std::vector<LivingEntity*> all;
        entities->GetPlayers(all);
        std::vector<LivingEntity*> players;
        for (LivingEntity* p : all) {
            if (p && p->IsAlive() && !p->IsSpectator() && p->GetAABB().Intersects(area)) players.push_back(p);
        }
        if (players.empty()) return;
        JavaRandom* random = level.Random();
        for (Bee* bee : bees) {
            if (bee->GetTarget() != nullptr) continue;
            // Util.getRandom(list, random): list.get(random.nextInt(size)).
            const int i = random ? random->NextInt(static_cast<int>(players.size())) : 0;
            bee->SetTarget(players[static_cast<size_t>(i)]);
        }
    }

    void BeehiveExplodedBy(ILevelWrite& level, const glm::ivec3& pos, BlockState state, const Entity* source) {
        if (!source) return;
        const EntityTypeId type = source->GetType();
        if (type != EntityTypeId::Tnt && type != EntityTypeId::Creeper && type != EntityTypeId::WitherSkull &&
            type != EntityTypeId::Wither && type != EntityTypeId::TntMinecart) {
            return;
        }
        if (auto* hive = dynamic_cast<BeehiveBlockEntity*>(level.GetBlockEntity(pos))) {
            hive->EmptyAllLivingFromHive(level, nullptr, state, BeehiveBlockEntity::ReleaseStatus::Emergency);
        }
    }

    bool BeehiveBlockEntity::IsFireNearby(ILevelWrite& level) const {
        const glm::ivec3 c = GetWorldPos();
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    if (IsFire(level.GetBlock(c.x + dx, c.y + dy, c.z + dz))) return true;
                }
            }
        }
        return false;
    }

    bool BeehiveBlockEntity::IsSedated(ILevelWrite& level) const { return IsSmokeyPos(level, GetWorldPos()); }

    void BeehiveBlockEntity::CheckFire(ILevelWrite& level) {
        if (IsFireNearby(level)) {
            const glm::ivec3 p = GetWorldPos();
            EmptyAllLivingFromHive(level, nullptr, level.GetBlockState(p.x, p.y, p.z), ReleaseStatus::Emergency);
        }
    }

    void BeehiveBlockEntity::AddOccupant(Bee& bee, ILevelWrite& level) {
        if (IsFull()) return;
        // stopRiding / ejectPassengers / dropLeash, then Occupant.of(bee):
        // its save minus the ignored tags, 2400 / 600 ticks to stay.
        bee.StopRiding();
        bee.EjectPassengers();
        bee.DropLeash();
        auto tag = SaveMobData(bee);
        if (!tag) return;
        for (const char* key : kIgnoredBeeTags) tag->value.erase(key);
        tag->value.erase("id");
        BeehiveOccupant occupant;
        occupant.entityType = "minecraft:bee";
        occupant.entityData = NbtCompoundValue(std::move(tag));
        occupant.ticksInHive = 0;
        occupant.minTicksInHive = HasNectar(occupant) ? kMinOccupationTicksNectar : kMinOccupationTicksNectarless;
        m_stored.push_back(std::move(occupant));

        if (bee.HasSavedFlowerPos() && (!m_savedFlowerPos || (level.Random() && level.Random()->NextBool()))) {
            m_savedFlowerPos = bee.GetSavedFlowerPos();
        }
        const glm::ivec3 p = GetWorldPos();
        level.PlaySound(nullptr, glm::dvec3(p), SoundEvents::BEEHIVE_ENTER, SoundSource::Blocks, 1.0f, 1.0f);
        level.GameEvent(GameEventId::BlockChange, p, GameEventContext::Of(&bee, level.GetBlockState(p.x, p.y, p.z)));
        bee.Discard();
        MarkDirty();
    }

    bool BeehiveBlockEntity::ReleaseOccupant(ILevelWrite& level, BlockState state, const BeehiveOccupant& occupant,
                                             std::vector<Entity*>* spawned, ReleaseStatus status) {
        const glm::ivec3 pos = GetWorldPos();
        if (BeesStayInHive(level, pos) && status != ReleaseStatus::Emergency) return false;
        const int facing = FacingIndex(state);
        const glm::ivec3 front = pos + kSteps[facing];
        bool frontBlocked = false;
        {
            const BlockState frontState = level.GetBlockState(front.x, front.y, front.z);
            frontBlocked = BlockRegistry::GetBlockCollisionShapeSet(frontState).count > 0;
        }
        if (frontBlocked && status != ReleaseStatus::Emergency) return false;

        // Occupant.createEntity: the saved data minus the ignored tags, a
        // #beehive_inhabitors type only, no gravity; a bee learns its hive
        // and catches up on age and love time.
        auto data = occupant.entityData.Copy();
        for (const char* key : kIgnoredBeeTags) data->value.erase(key);
        const std::string type = occupant.entityType.rfind("minecraft:", 0) == 0 ? occupant.entityType.substr(10)
                                                                                : occupant.entityType;
        if (!DataTags::HasTag(DataTags::Registry::EntityType, type, "minecraft:beehive_inhabitors") && type != "bee") {
            return false;
        }
        JavaRandom* random = level.Random();
        const std::optional<glm::ivec3> flower = m_savedFlowerPos;
        const int ticksInHive = occupant.ticksInHive;
        bool honeyRaised = false;
        Mob* mob = SpawnMobFromSavedData(level.GetDimension(), type, *data, [&](Mob& m) {
            m.SetNoGravity(true);
            if (auto* bee = dynamic_cast<Bee*>(&m)) {
                bee->SetHivePos(pos);
                // setBeeReleaseData: age catches up by the ticks inside
                // (unless locked), and the love time runs down.
                if (!bee->IsAgeLocked()) {
                    const int age = bee->GetAge();
                    if (age < 0) bee->SetAge(std::min(0, age + ticksInHive));
                    else if (age > 0) bee->SetAge(std::max(0, age - ticksInHive));
                }
                bee->SetInLoveTicks(std::max(0, bee->GetInLoveTicks() - ticksInHive));
                if (flower && !bee->HasSavedFlowerPos() && random && random->NextFloat() < 0.9f) {
                    bee->SetSavedFlowerPos(*flower);
                }
                if (status == ReleaseStatus::HoneyDelivered) {
                    bee->DropOffNectar();
                    honeyRaised = true;
                }
            }
            const float width = m.GetBbWidth();
            const double delta = frontBlocked ? 0.0 : 0.55 + static_cast<double>(width / 2.0f);
            m.position = glm::dvec3(pos.x + 0.5 + delta * kSteps[facing].x,
                                    pos.y + 0.5 - static_cast<double>(m.GetBbHeight() / 2.0f),
                                    pos.z + 0.5 + delta * kSteps[facing].z);
            m.oldPosition = m.position;
        });
        if (!mob) return false;
        // A nectar bee delivers honey: +1 (1 in 100 +2), capped at 5.
        if (honeyRaised && state.HasProperty(PropertyId::HONEY_LEVEL)) {
            const int honey = state.GetIndex(PropertyId::HONEY_LEVEL);
            if (honey < 5) {
                int increase = random && random->NextInt(100) == 0 ? 2 : 1;
                if (honey + increase > 5) --increase;
                level.SetBlock(pos.x, pos.y, pos.z, state.SetIndex(PropertyId::HONEY_LEVEL, honey + increase),
                               World::UpdateFlags::All);
            }
        }
        if (spawned) spawned->push_back(mob);
        level.PlaySound(nullptr, pos, SoundEvents::BEEHIVE_EXIT, SoundSource::Blocks, 1.0f, 1.0f);
        level.GameEvent(GameEventId::BlockChange, pos, GameEventContext::Of(mob, level.GetBlockState(pos.x, pos.y, pos.z)));
        return true;
    }

    void BeehiveBlockEntity::EmptyAllLivingFromHive(ILevelWrite& level, Entity* player, BlockState state,
                                                    ReleaseStatus status) {
        std::vector<Entity*> released;
        const size_t before = m_stored.size();
        m_stored.erase(std::remove_if(m_stored.begin(), m_stored.end(), [&](const BeehiveOccupant& o) {
            return ReleaseOccupant(level, state, o, &released, status);
        }), m_stored.end());
        if (m_stored.size() != before) MarkDirty();
        if (!player) return;
        const bool sedated = IsSedated(level);
        for (Entity* e : released) {
            auto* bee = dynamic_cast<Bee*>(e);
            if (!bee) continue;
            const glm::dvec3 d = player->position - bee->position;
            if (glm::dot(d, d) > 16.0) continue;
            if (!sedated) {
                if (auto* target = dynamic_cast<LivingEntity*>(player)) bee->SetTarget(target);
            } else {
                bee->SetStayOutOfHiveCountdown(400);
            }
        }
    }

    void BeehiveBlockEntity::Tick(World* world, float /*deltaTime*/) {
        if (!world || world->IsClientSide()) return;
        const glm::ivec3 pos = GetWorldPos();
        const BlockState state = world->GetBlockState(pos.x, pos.y, pos.z);
        // tickOccupants: each counts up; past its minimum it leaves.
        bool changed = false;
        for (size_t i = 0; i < m_stored.size();) {
            BeehiveOccupant& o = m_stored[i];
            if (o.ticksInHive++ > o.minTicksInHive) {
                const ReleaseStatus status = HasNectar(o) ? ReleaseStatus::HoneyDelivered : ReleaseStatus::BeeReleased;
                const BeehiveOccupant copy = o;
                if (ReleaseOccupant(*world, state, copy, nullptr, status)) {
                    m_stored.erase(m_stored.begin() + static_cast<std::ptrdiff_t>(i));
                    changed = true;
                    continue;
                }
            }
            ++i;
        }
        if (changed) MarkDirty();
        // The work hum.
        if (!m_stored.empty()) {
            if (JavaRandom* random = world->Random(); random && random->NextDouble() < 0.005) {
                world->PlaySound(nullptr, glm::dvec3(pos.x + 0.5, pos.y, pos.z + 0.5), SoundEvents::BEEHIVE_WORK,
                                 SoundSource::Blocks, 1.0f, 1.0f);
            }
        }
    }

    void BeehiveBlockEntity::ApplyItemComponents(const DataComponentMap& components) {
        m_stored.clear();
        if (const auto bees = components.get(DataComponents::BEES)) m_stored = bees->bees;
    }

    void BeehiveBlockEntity::CollectComponents(DataComponentMap& out) const {
        out.set(DataComponents::BEES, Bees{m_stored});
    }

} // namespace Game
