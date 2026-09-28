// File: src/common/world/damagesource/CombatTracker.cpp
#include "CombatTracker.hpp"

#include "common/core/Log.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/text/Language.hpp"
#include "common/text/TextComponent.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/tags/DataTags.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace Game {

    namespace {

        // ── Damage types (MC DamageType: message_id + death_message_type) ──

        enum class DeathMessageType : uint8_t { Default, FallVariants, IntentionalGameDesign };

        struct DamageTypeMessage {
            std::string msgId;
            DeathMessageType type = DeathMessageType::Default;
        };

        // Same rule as DataTags and the terrain library: MC_DATA_ROOT, else ./data.
        std::filesystem::path DataRoot() {
            if (const char* env = std::getenv("MC_DATA_ROOT")) return env;
            return "data";
        }

        // The damage_type registry's message fields, read once from
        // data/<ns>/damage_type/*.json. The built-in rows are the 26.3
        // values for every type the engine's damage paths produce
        // (DamageSourceInfo::TypeIdFor), so a missing data folder still
        // words every death correctly.
        const std::unordered_map<std::string, DamageTypeMessage>& DamageTypeMessages() {
            static const std::unordered_map<std::string, DamageTypeMessage> table = [] {
                using T = DeathMessageType;
                std::unordered_map<std::string, DamageTypeMessage> out = {
                    {"minecraft:generic",               {"generic", T::Default}},
                    {"minecraft:generic_kill",          {"genericKill", T::Default}},
                    {"minecraft:mob_attack",            {"mob", T::Default}},
                    {"minecraft:mob_attack_no_aggro",   {"mob", T::Default}},
                    {"minecraft:player_attack",         {"player", T::Default}},
                    {"minecraft:fall",                  {"fall", T::FallVariants}},
                    {"minecraft:ender_pearl",           {"fall", T::FallVariants}},
                    {"minecraft:on_fire",               {"onFire", T::Default}},
                    {"minecraft:in_fire",               {"inFire", T::Default}},
                    {"minecraft:lava",                  {"lava", T::Default}},
                    {"minecraft:drown",                 {"drown", T::Default}},
                    {"minecraft:explosion",             {"explosion", T::Default}},
                    {"minecraft:player_explosion",      {"explosion.player", T::Default}},
                    {"minecraft:out_of_world",          {"outOfWorld", T::Default}},
                    {"minecraft:magic",                 {"magic", T::Default}},
                    {"minecraft:indirect_magic",        {"indirectMagic", T::Default}},
                    {"minecraft:wither",                {"wither", T::Default}},
                    {"minecraft:cramming",              {"cramming", T::Default}},
                    {"minecraft:falling_block",         {"fallingBlock", T::Default}},
                    {"minecraft:falling_anvil",         {"anvil", T::Default}},
                    {"minecraft:falling_stalactite",    {"fallingStalactite", T::Default}},
                    {"minecraft:stalagmite",            {"stalagmite", T::Default}},
                    {"minecraft:thorns",                {"thorns", T::Default}},
                    {"minecraft:arrow",                 {"arrow", T::Default}},
                    {"minecraft:trident",               {"trident", T::Default}},
                    {"minecraft:fireball",              {"fireball", T::Default}},
                    {"minecraft:unattributed_fireball", {"onFire", T::Default}},
                    {"minecraft:wither_skull",          {"witherSkull", T::Default}},
                    {"minecraft:wind_charge",           {"mob", T::Default}},
                    {"minecraft:thrown",                {"thrown", T::Default}},
                    {"minecraft:mob_projectile",        {"mob", T::Default}},
                    {"minecraft:bad_respawn_point",     {"badRespawnPoint", T::IntentionalGameDesign}},
                };
                // The data pack is authoritative where it is present.
                std::error_code ec;
                const std::filesystem::path root = DataRoot();
                if (!std::filesystem::is_directory(root, ec)) return out;
                for (const auto& nsEntry : std::filesystem::directory_iterator(root, ec)) {
                    if (!nsEntry.is_directory()) continue;
                    const std::string ns = nsEntry.path().filename().string();
                    const std::filesystem::path dir = nsEntry.path() / "damage_type";
                    if (!std::filesystem::is_directory(dir, ec)) continue;
                    for (const auto& f : std::filesystem::directory_iterator(dir, ec)) {
                        if (!f.is_regular_file() || f.path().extension() != ".json") continue;
                        std::ifstream in(f.path());
                        if (!in) continue;
                        const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
                        if (!j.is_object() || !j.contains("message_id") || !j["message_id"].is_string()) continue;
                        DamageTypeMessage msg;
                        msg.msgId = j["message_id"].get<std::string>();
                        const std::string kind = j.value("death_message_type", std::string("default"));
                        if (kind == "fall_variants")                msg.type = T::FallVariants;
                        else if (kind == "intentional_game_design") msg.type = T::IntentionalGameDesign;
                        out[ns + ":" + f.path().stem().string()] = std::move(msg);
                    }
                }
                return out;
            }();
            return table;
        }

        DamageTypeMessage MessageFor(const std::string& typeId) {
            const auto& table = DamageTypeMessages();
            const auto it = table.find(typeId);
            if (it != table.end()) return it->second;
            return {"generic", DeathMessageType::Default};
        }

        // ── Climbable blocks (MC BlockTags.CLIMBABLE / TRAPDOORS) ─────────

        enum class ClimbKind : uint8_t { None, Climbable, Trapdoor };

        struct ClimbTable {
            std::vector<ClimbKind>    kind;
            std::vector<FallLocation> location;   // MC FallLocation.blockToFallLocation
            BlockID ladder = BlockID::Air;
        };

        const ClimbTable& GetClimbTable() {
            static const ClimbTable table = [] {
                ClimbTable t;
                t.kind.assign(BlockRegistry::Size, ClimbKind::None);
                t.location.assign(BlockRegistry::Size, FallLocation::OtherClimbable);
                for (size_t i = 0; i < BlockRegistry::Size; ++i) {
                    const std::string& slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
                    if (slug.empty()) continue;
                    const bool trapdoor = DataTags::HasTag(DataTags::Registry::Block, slug, "minecraft:trapdoors");
                    if (DataTags::HasTag(DataTags::Registry::Block, slug, "minecraft:climbable")) {
                        t.kind[i] = ClimbKind::Climbable;
                    } else if (trapdoor) {
                        t.kind[i] = ClimbKind::Trapdoor;
                    }
                    if (slug == "ladder") t.ladder = static_cast<BlockID>(i);
                    if (slug == "ladder" || trapdoor)                                  t.location[i] = FallLocation::Ladder;
                    else if (slug == "vine")                                           t.location[i] = FallLocation::Vines;
                    else if (slug == "weeping_vines" || slug == "weeping_vines_plant") t.location[i] = FallLocation::WeepingVines;
                    else if (slug == "twisting_vines" || slug == "twisting_vines_plant") t.location[i] = FallLocation::TwistingVines;
                    else if (slug == "scaffolding")                                    t.location[i] = FallLocation::Scaffolding;
                }
                return t;
            }();
            return table;
        }

        FallLocation BlockToFallLocation(const BlockState& state) {
            const auto& t = GetClimbTable();
            const size_t i = static_cast<size_t>(state.Block());
            return i < t.location.size() ? t.location[i] : FallLocation::OtherClimbable;
        }

        // MC ItemStack.getDisplayName of a stack that has a CUSTOM_NAME:
        // the hover name in square brackets.
        std::optional<std::string> CustomNamedMainHand(Entity& entity) {
            auto* living = dynamic_cast<LivingEntity*>(&entity);
            if (!living) return std::nullopt;
            const ItemStack* held = living->EquipmentInSlot(EquipmentSlot::MAINHAND);
            if (!held || held->IsEmpty() || !held->get(DataComponents::CUSTOM_NAME)) return std::nullopt;
            return "[" + GetItemStackHoverName(*held) + "]";
        }

        // MC Component.translatable(key, args...).getString(), against the
        // en_us table.
        std::string Translate(const std::string& key, std::initializer_list<std::string> args) {
            std::vector<Text::Component> with;
            with.reserve(args.size());
            for (const std::string& a : args) with.push_back(Text::Component::Literal(a));
            return Text::GetString(Text::Component::Translatable(key, std::move(with)));
        }

        bool IsFallType(const std::string& typeId) {
            return DataTags::HasTag(DataTags::Registry::DamageType, typeId, "minecraft:is_fall");
        }
        bool IsAlwaysMostSignificantFall(const std::string& typeId) {
            return DataTags::HasTag(DataTags::Registry::DamageType, typeId,
                                    "minecraft:always_most_significant_fall");
        }

    } // namespace

    std::string_view FallLocationLanguageKey(FallLocation location) {
        switch (location) {
            case FallLocation::Generic:        return "death.fell.accident.generic";
            case FallLocation::Ladder:         return "death.fell.accident.ladder";
            case FallLocation::Vines:          return "death.fell.accident.vines";
            case FallLocation::WeepingVines:   return "death.fell.accident.weeping_vines";
            case FallLocation::TwistingVines:  return "death.fell.accident.twisting_vines";
            case FallLocation::Scaffolding:    return "death.fell.accident.scaffolding";
            case FallLocation::OtherClimbable: return "death.fell.accident.other_climbable";
            case FallLocation::Water:          return "death.fell.accident.water";
        }
        return "death.fell.accident.generic";
    }

    std::string EntityTypeDisplayName(std::string_view slug) {
        std::string key;
        const size_t colon = slug.find(':');
        if (colon == std::string_view::npos) {
            key = "entity.minecraft." + std::string(slug);
        } else {
            key = "entity." + std::string(slug.substr(0, colon)) + "." + std::string(slug.substr(colon + 1));
        }
        if (Language::Has(key)) return Language::Get(key);
        // No translation (the mod ports' types): the path, title-cased.
        std::string name(colon == std::string_view::npos ? slug : slug.substr(colon + 1));
        bool upper = true;
        for (char& c : name) {
            if (c == '_') { c = ' '; upper = true; continue; }
            if (upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            upper = false;
        }
        return name;
    }

    std::string EntityDisplayName(const Entity& entity, const EntityLevel* level) {
        if (entity.IsPlayer() && level) {
            if (const auto* living = dynamic_cast<const LivingEntity*>(&entity)) {
                std::string name = level->GetPlayerName(*living);
                if (!name.empty()) return name;
            }
        }
        if (const auto& custom = entity.GetCustomName()) return *custom;
        return EntityTypeDisplayName(entity.TypeInfo().slug);
    }

    // ── CombatTracker ───────────────────────────────────────────────────

    void CombatTracker::RecordDamage(LivingEntity& mob, std::string_view typeId, float damage,
                                     Entity* causing, Entity* direct) {
        RecheckStatus(mob);

        Entry entry;
        entry.typeId = std::string(typeId);
        entry.damage = damage;
        // MC FallLocation.getCurrentFallLocation: the block at the last
        // climbable cell (whatever it is now), else water, else none.
        if (m_lastClimbablePos) {
            const EntityLevel* level = mob.Level();
            const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
            const BlockState state = blocks
                ? blocks->GetBlockState(m_lastClimbablePos->x, m_lastClimbablePos->y, m_lastClimbablePos->z)
                : BlockState{};
            entry.fallLocation = BlockToFallLocation(state);
        } else if (mob.IsInWater()) {
            entry.fallLocation = FallLocation::Water;
        }
        entry.fallDistance = mob.fallDistance;

        const EntityLevel* level = mob.Level();
        if (causing) {
            entry.causingName = EntityDisplayName(*causing, level);
            entry.causingItemName = CustomNamedMainHand(*causing);
        }
        if (direct) entry.directName = EntityDisplayName(*direct, level);
        m_entries.push_back(std::move(entry));

        m_lastDamageTime = mob.tickCount;
        m_takingDamage = true;
        // MC shouldEnterCombat: the source's entity is a LivingEntity.
        if (!m_inCombat && mob.IsAlive() && causing && dynamic_cast<LivingEntity*>(causing)) {
            m_inCombat = true;
            m_combatStartTime = mob.tickCount;
            m_combatEndTime = m_combatStartTime;
        }
    }

    const CombatTracker::Entry* CombatTracker::MostSignificantFall() const {
        const Entry* result = nullptr;
        const Entry* alternative = nullptr;
        float altDamage = 0.0f;
        float bestFall = 0.0f;
        for (size_t i = 0; i < m_entries.size(); ++i) {
            const Entry& entry = m_entries[i];
            const Entry* previous = i > 0 ? &m_entries[i - 1] : nullptr;
            const bool isFakeFall = IsAlwaysMostSignificantFall(entry.typeId);
            const float fallDistance = isFakeFall ? 3.4028235e38f : entry.fallDistance;
            if ((IsFallType(entry.typeId) || isFakeFall) && fallDistance > 0.0f &&
                (result == nullptr || fallDistance > bestFall)) {
                result = i > 0 ? previous : &entry;
                bestFall = fallDistance;
            }
            if (entry.fallLocation && (alternative == nullptr || entry.damage > altDamage)) {
                alternative = &entry;
                altDamage = entry.damage;
            }
        }
        if (bestFall > 5.0f && result) return result;
        if (altDamage > 5.0f && alternative) return alternative;
        return nullptr;
    }

    std::string CombatTracker::FallMessage(const LivingEntity& mob, const Entry& knockOff,
                                           const Entry& killingBlow) const {
        const std::string victim = EntityDisplayName(mob, mob.Level());
        if (!IsFallType(knockOff.typeId) && !IsAlwaysMostSignificantFall(knockOff.typeId)) {
            // MC getMessageForAssistedFall: the ".item" form when that
            // entity's main hand holds a custom-named stack.
            const auto assisted = [&](const std::string& name, const std::optional<std::string>& item,
                                      const char* withItem, const char* withoutItem) {
                if (item) return Translate(withItem, {victim, name, *item});
                return Translate(withoutItem, {victim, name});
            };
            const std::optional<std::string>& killerName = killingBlow.causingName;
            const std::optional<std::string>& attackerName = knockOff.causingName;
            if (attackerName && attackerName != killerName) {
                return assisted(*attackerName, knockOff.causingItemName,
                                "death.fell.assist.item", "death.fell.assist");
            }
            if (killerName) {
                return assisted(*killerName, killingBlow.causingItemName,
                                "death.fell.finish.item", "death.fell.finish");
            }
            return Translate("death.fell.killer", {victim});
        }
        const FallLocation location = knockOff.fallLocation.value_or(FallLocation::Generic);
        std::string key(FallLocationLanguageKey(location));
        // 26.3's en_us has no line for WATER; MC would print the bare key.
        if (!Language::Has(key)) key = FallLocationLanguageKey(FallLocation::Generic);
        return Translate(key, {victim});
    }

    std::string CombatTracker::LocalizedDeathMessage(LivingEntity& mob, const Entry& killingBlow) const {
        // MC DamageSource.getLocalizedDeathMessage.
        const std::string victim = EntityDisplayName(mob, mob.Level());
        const std::string deathMsg = "death.attack." + MessageFor(killingBlow.typeId).msgId;
        if (!killingBlow.causingName && !killingBlow.directName) {
            // MC getKillCredit: whoever hurt it last, while the memory lasts
            // (LivingEntity's 100-tick lastHurtByMob).
            Entity* credit = mob.GetLastHurtByMob();
            if (credit && dynamic_cast<LivingEntity*>(credit)) {
                const std::string playerMsg = deathMsg + ".player";
                if (Language::Has(playerMsg)) {
                    return Translate(playerMsg, {victim, EntityDisplayName(*credit, mob.Level())});
                }
            }
            return Translate(deathMsg, {victim});
        }
        const std::string& name = killingBlow.causingName ? *killingBlow.causingName : *killingBlow.directName;
        if (killingBlow.causingItemName && Language::Has(deathMsg + ".item")) {
            return Translate(deathMsg + ".item", {victim, name, *killingBlow.causingItemName});
        }
        return Translate(deathMsg, {victim, name});
    }

    std::string CombatTracker::GetDeathMessage(LivingEntity& mob) const {
        if (m_entries.empty()) {
            return Translate("death.attack.generic", {EntityDisplayName(mob, mob.Level())});
        }
        const Entry& killingBlow = m_entries.back();
        const Entry* knockOff = MostSignificantFall();
        const DamageTypeMessage type = MessageFor(killingBlow.typeId);
        if (type.type == DeathMessageType::FallVariants && knockOff) {
            return FallMessage(mob, *knockOff, killingBlow);
        }
        if (type.type == DeathMessageType::IntentionalGameDesign) {
            // MC: "death.attack.<id>.message" with the bracketed ".link" text
            // (a click-through link in MC; plain text in a system line here).
            const std::string deathMsg = "death.attack." + type.msgId;
            const std::string link = "[" + Language::Get(deathMsg + ".link") + "]";
            return Translate(deathMsg + ".message", {EntityDisplayName(mob, mob.Level()), link});
        }
        return LocalizedDeathMessage(mob, killingBlow);
    }

    int CombatTracker::GetCombatDuration(const LivingEntity& mob) const {
        return m_inCombat ? mob.tickCount - m_combatStartTime : m_combatEndTime - m_combatStartTime;
    }

    void CombatTracker::RecheckStatus(const LivingEntity& mob) {
        const int reset = m_inCombat ? kResetCombatStatusTime : kResetDamageStatusTime;
        if (m_takingDamage && (!mob.IsAlive() || mob.tickCount - m_lastDamageTime > reset)) {
            m_takingDamage = false;
            m_inCombat = false;
            m_combatEndTime = mob.tickCount;
            m_entries.clear();
        }
    }

    void CombatTracker::TrackClimbable(const LivingEntity& mob) {
        // MC checkFallDamage: landing forgets the climb.
        if (mob.onGround) {
            m_lastClimbablePos.reset();
            return;
        }
        const EntityLevel* level = mob.Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (!blocks) return;
        const ClimbTable& table = GetClimbTable();
        const glm::ivec3 p = mob.BlockPosition();
        const BlockState state = blocks->GetBlockState(p.x, p.y, p.z);
        const size_t i = static_cast<size_t>(state.Block());
        const ClimbKind kind = i < table.kind.size() ? table.kind[i] : ClimbKind::None;
        if (kind == ClimbKind::Climbable) {
            m_lastClimbablePos = p;
        } else if (kind == ClimbKind::Trapdoor) {
            // MC trapdoorUsableAsLadder: open, over a ladder facing the same way.
            if (state.GetValueByName("open") != "true") return;
            const BlockState below = blocks->GetBlockState(p.x, p.y - 1, p.z);
            if (table.ladder == BlockID::Air || below.Block() != table.ladder) return;
            if (below.GetValueByName("facing") == state.GetValueByName("facing")) m_lastClimbablePos = p;
        }
    }

} // namespace Game
