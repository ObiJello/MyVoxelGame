// File: src/common/sound/EntitySounds.cpp
#include "common/sound/EntitySounds.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/sound/LevelSound.hpp"
#include "common/sound/SoundEvents.hpp"

#include <string_view>
#include <unordered_map>
#include <vector>

namespace Game {

    namespace {

        struct SlugRow {
            const char*    slug;
            EntitySoundRow row;
        };

        using P = EntitySoundPred;
        using S = EntityStepMode;
        using Src = SoundSource;

        // The generated rows. `None` is spelled per enum: the macro arguments
        // name a predicate, a step mode and a source by bare identifier.
        const SlugRow kRows[] = {
#define ENTITY_SOUNDS(SLUG, SOURCE, VOLUME, INTERVAL,                                   \
                      APRED, AT, AF, HPRED, HT, HF, DPRED, DT, DF,                      \
                      SMODE, SPRED, ST, SF, SVOL, SPITCH, SWIM, SPLASH, SPLASHHI)       \
            {SLUG, EntitySoundRow{Src::SOURCE, VOLUME, INTERVAL,                        \
                                  {P::APRED, AT, AF}, {P::HPRED, HT, HF}, {P::DPRED, DT, DF}, \
                                  S::SMODE, {P::SPRED, ST, SF}, SVOL, SPITCH,            \
                                  SWIM, SPLASH, SPLASHHI}},
#include "common/sound/GeneratedEntitySounds.inc"
#undef ENTITY_SOUNDS
        };

        // An entity type with no row (none today — the generator covers every
        // engine slug): MC's base-class answers.
        const EntitySoundRow kDefaultRow{
            Src::Neutral, 1.0f, 80,
            {P::None, "", ""},
            {P::None, "entity.generic.hurt", "entity.generic.hurt"},
            {P::None, "entity.generic.death", "entity.generic.death"},
            S::Block, {P::None, "", ""}, 0.15f, 1.0f,
            "entity.generic.swim", "entity.generic.splash", "entity.generic.splash"};

        const std::vector<const EntitySoundRow*>& RowsByType() {
            static const std::vector<const EntitySoundRow*> table = [] {
                std::unordered_map<std::string_view, const EntitySoundRow*> bySlug;
                for (const SlugRow& r : kRows) bySlug.emplace(r.slug, &r.row);
                std::vector<const EntitySoundRow*> t(static_cast<size_t>(kEntityTypeCount), &kDefaultRow);
                for (size_t i = 0; i < t.size(); ++i) {
                    const auto it = bySlug.find(kEntityTypeTable[i].slug);
                    if (it != bySlug.end()) t[i] = it->second;
                }
                return t;
            }();
            return table;
        }

    } // namespace

    const EntitySoundRow& EntitySoundsOf(EntityTypeId type) {
        const auto& table = RowsByType();
        const size_t i = static_cast<size_t>(type);
        return i < table.size() ? *table[i] : kDefaultRow;
    }

    const char* PickSound(const EntitySoundPick& pick, const Entity& entity) {
        bool which = false;
        switch (pick.pred) {
            case P::None:       return pick.whenFalse;
            case P::Baby:       which = entity.IsBaby(); break;
            case P::InWater:    which = entity.IsInWater(); break;
            case P::UnderWater: which = entity.IsUnderWater(); break;
            case P::OnGround:   which = entity.onGround; break;
        }
        return which ? pick.whenTrue : pick.whenFalse;
    }

    bool PlayEntityLevelEventSound(EntityLevel& level, int type, const glm::ivec3& pos) {
        namespace E = EntityLevelEvent;
        const char* event = nullptr;
        SoundSource source = SoundSource::Hostile;
        float volume = 2.0f;
        switch (type) {
            case E::GHAST_WARNING:             event = SoundEvents::GHAST_WARN; volume = 10.0f; break;
            case E::GHAST_SHOOT:               event = SoundEvents::GHAST_SHOOT; volume = 10.0f; break;
            case E::DRAGON_SHOOT:              event = SoundEvents::ENDER_DRAGON_SHOOT; volume = 10.0f; break;
            case E::BLAZE_SHOOT:               event = SoundEvents::BLAZE_SHOOT; break;
            case E::ZOMBIE_ATTACK_WOODEN_DOOR: event = SoundEvents::ZOMBIE_ATTACK_WOODEN_DOOR; break;
            case E::ZOMBIE_ATTACK_IRON_DOOR:   event = SoundEvents::ZOMBIE_ATTACK_IRON_DOOR; break;
            case E::ZOMBIE_BREAK_WOODEN_DOOR:  event = SoundEvents::ZOMBIE_BREAK_WOODEN_DOOR; break;
            case E::WITHER_BREAK_BLOCK:        event = SoundEvents::WITHER_BREAK_BLOCK; break;
            case E::WITHER_SHOOT:              event = SoundEvents::WITHER_SHOOT; break;
            case E::BAT_TAKEOFF:               event = SoundEvents::BAT_TAKEOFF; source = SoundSource::Neutral; volume = 0.05f; break;
            case E::ZOMBIE_INFECT:             event = SoundEvents::ZOMBIE_INFECT; break;
            case E::ZOMBIE_VILLAGER_CONVERTED: event = SoundEvents::ZOMBIE_VILLAGER_CONVERTED; break;
            case E::ZOMBIE_TO_DROWNED:         event = SoundEvents::ZOMBIE_CONVERTED_TO_DROWNED; break;
            case E::HUSK_TO_ZOMBIE:            event = SoundEvents::HUSK_CONVERTED_TO_ZOMBIE; break;
            case E::SKELETON_TO_STRAY:         event = SoundEvents::SKELETON_CONVERTED_TO_STRAY; break;
            case E::PHANTOM_BITE: {
                // The one id with its own pitch curve: 0.9 + rand × 0.1, at 0.3.
                const float pitch = level.Random().NextFloat() * 0.1f + 0.9f;
                level.PlaySound(nullptr, Sound::BlockCenter(pos), SoundEvents::PHANTOM_BITE,
                                SoundSource::Hostile, 0.3f, pitch);
                return true;
            }
            default: return false;
        }
        JavaRandom& rng = level.Random();
        const float pitch = (rng.NextFloat() - rng.NextFloat()) * 0.2f + 1.0f;
        level.PlaySound(nullptr, Sound::BlockCenter(pos), event, source, volume, pitch);
        return true;
    }

} // namespace Game
