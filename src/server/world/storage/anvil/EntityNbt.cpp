// File: src/server/world/storage/anvil/EntityNbt.cpp
#include "server/world/storage/anvil/EntityNbt.hpp"
#include "common/entity/FallingBlockEntity.hpp"
#include "common/entity/PrimedTnt.hpp"
#include "common/entity/EndCrystal.hpp"
#include "common/world/block/FallingBlock.hpp"

#include "server/world/storage/anvil/ItemStackNbt.hpp"

#include "common/core/Log.hpp"
#include "common/entity/Animal.hpp"
#include "common/entity/Attributes.hpp"
#include "common/entity/EntityType.hpp"
#include "common/entity/ExperienceOrb.hpp"
#include "common/entity/ItemEntity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/NeutralMob.hpp"
#include "common/entity/TamableAnimal.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/mobs/Fish.hpp"
#include "common/entity/mobs/GenericMobs.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/mobs/Slime.hpp"
#include "common/entity/mobs/SulfurCube.hpp"

#include <algorithm>
#include "common/entity/projectile/AreaEffectCloud.hpp"
#include "common/entity/projectile/Arrow.hpp"
#include "common/entity/projectile/EvokerFangs.hpp"
#include "common/entity/projectile/EyeOfEnder.hpp"
#include "common/entity/projectile/HurtingProjectile.hpp"
#include "common/entity/projectile/Projectile.hpp"
#include "common/entity/projectile/ShulkerBullet.hpp"
#include "common/entity/projectile/ThrownTrident.hpp"

#include <string>
#include <unordered_map>

namespace Game::Anvil {

    namespace {

        constexpr std::string_view kNamespace = "minecraft:";

        std::string_view StripNamespace(std::string_view name) {
            if (name.rfind(kNamespace, 0) == 0) return name.substr(kNamespace.size());
            return name;
        }

        using CT = ::World::NBTTagCompound;
        using LT = ::World::NBTTagList;

        // ── small readers ───────────────────────────────────────────────────

        template <typename Tag>
        std::shared_ptr<Tag> As(const ::World::NBTTagPtr& t) {
            return std::dynamic_pointer_cast<Tag>(t);
        }

        bool ReadDoubleList(const CT& tag, const char* key, glm::dvec3& out) {
            auto list = As<LT>(tag.GetTag(key));
            if (!list || list->value.size() != 3) return false;
            for (int i = 0; i < 3; ++i) {
                auto d = As<::World::NBTTagDouble>(list->value[i]);
                out[i] = d ? d->value : 0.0;
            }
            return true;
        }

        bool ReadUuid(const CT& tag, const char* key, Uuid& out) {
            auto arr = As<::World::NBTTagIntArray>(tag.GetTag(key));
            if (!arr || arr->value.size() != 4) return false;
            int32_t words[4];
            for (int i = 0; i < 4; ++i) words[i] = arr->value[i];
            out = UuidFromIntArray(words);
            return true;
        }

        // ── small writers ───────────────────────────────────────────────────

        void WriteDoubleList(Nbt::Writer& w, const char* key, const glm::dvec3& v) {
            auto list = w.BeginList(key, Nbt::TagType::Double);
            w.ListDouble(list, v.x);
            w.ListDouble(list, v.y);
            w.ListDouble(list, v.z);
            w.EndList(list);
        }

        void WriteUuid(Nbt::Writer& w, const char* key, const Uuid& uuid) {
            int32_t words[4];
            UuidToIntArray(uuid, words);
            w.IntArray(key, words, 4);
        }

        // ── BlockState <-> vanilla's {Name, Properties} compound ────────────
        //
        // MC BlockState.CODEC. Used by the two block-shaped entities, which
        // must keep their FULL state (an anvil's facing, a waterlogged slab)
        // rather than just a block id the way the enderman's carried block does.
        void WriteBlockStateCompound(Nbt::Writer& w, const char* key, BlockState state) {
            const BlockID id = state.Block();
            w.BeginCompound(key);
            w.String("Name", std::string(kNamespace) +
                                 (id == BlockID::Air
                                      ? "air"
                                      : BlockRegistry::Get(id).registrySlug));
            const uint16_t propCount = BlockStates::PropertyCount(id);
            if (propCount > 0) {
                // Properties is OMITTED for a stateless block, and every value
                // is a TAG_String — never a byte or an int, even for booleans.
                w.BeginCompound("Properties");
                for (uint16_t slot = 0; slot < propCount; ++slot) {
                    const PropertyId prop = BlockStates::PropertyAt(id, slot);
                    w.String(BlockStates::PropertyName(prop), state.GetName(prop));
                }
                w.EndCompound();
            }
            w.EndCompound();
        }

        bool ReadBlockStateCompound(const CT& tag, const char* key, BlockState& out) {
            auto compound = As<CT>(tag.GetTag(key));
            if (!compound) return false;
            const std::string name = compound->GetValue<std::string>("Name", "");
            if (name.empty()) return false;

            const BlockID id = BlockStates::FromSlug(StripNamespace(name)).Block();
            const auto& def = BlockRegistry::GetStateDefinition(id);
            BlockRegistry::BlockStateDefinition::PropertyMap props;
            if (auto p = As<CT>(compound->GetTag("Properties"))) {
                for (const auto& [k, v] : p->value) {
                    if (auto str = std::dynamic_pointer_cast<::World::NBTTagString>(v)) {
                        props[k] = str->value;
                    }
                }
            }
            // Unknown property names and values are SKIPPED rather than
            // rejected (MC NbtUtils.readBlockState), so a state written by a
            // different version still loads as something sensible.
            out = BlockStates::FromIndex(id, def.IndexOf(props));
            return true;
        }

        // A reference is written whenever it HAS an identity, resolved or not.
        // That is the whole point: a cow keeps a grudge against a player who
        // has been offline for a month, exactly as vanilla does.
        void WriteRef(Nbt::Writer& w, const char* key, const EntityRef& ref) {
            if (ref.Empty()) return;
            WriteUuid(w, key, ref.GetUuid());
        }

        // ── base Entity ─────────────────────────────────────────────────────

        void WriteEntityBase(Nbt::Writer& w, const Entity& e) {
            WriteDoubleList(w, "Pos",    e.position);
            WriteDoubleList(w, "Motion", e.velocity);
            {
                auto rot = w.BeginList("Rotation", Nbt::TagType::Float);
                w.ListFloat(rot, e.yRot);
                w.ListFloat(rot, e.xRot);
                w.EndList(rot);
            }
            // TAG_Double and snake_case. Measured against real 4764 files:
            // FallDistance/Float does not exist there.
            w.Double("fall_distance", static_cast<double>(e.fallDistance));
            w.Short ("Fire", static_cast<int16_t>(e.GetRemainingFireTicks()));
            w.Short ("Air",  static_cast<int16_t>(e.GetAirSupply()));
            w.Bool  ("OnGround", e.onGround);
            w.Bool  ("Invulnerable", e.IsInvulnerable());
            w.Int   ("PortalCooldown", e.portal.GetCooldown());
            WriteUuid(w, "UUID", e.GetUuid());
            if (e.IsNoGravity()) w.Bool("NoGravity", true);
        }

        void ReadEntityBase(const CT& tag, Entity& e) {
            glm::dvec3 v{};
            if (ReadDoubleList(tag, "Pos", v))    e.position = v;
            if (ReadDoubleList(tag, "Motion", v)) e.velocity = v;
            if (auto rot = As<LT>(tag.GetTag("Rotation")); rot && rot->value.size() == 2) {
                auto f = [&](int i) {
                    auto t = As<::World::NBTTagFloat>(rot->value[i]);
                    return t ? t->value : 0.0f;
                };
                e.yRot = f(0);
                e.xRot = f(1);
                e.yRotO = e.yRot;
                e.xRotO = e.xRot;
            }
            e.fallDistance = static_cast<float>(tag.GetValue<double>("fall_distance", 0.0));
            e.SetRemainingFireTicks(tag.GetValue<int16_t>("Fire", -20));
            e.SetAirSupply(tag.GetValue<int16_t>("Air", 300));
            e.onGround = tag.GetValue<int8_t>("OnGround", 0) != 0;
            e.SetInvulnerable(tag.GetValue<int8_t>("Invulnerable", 0) != 0);
            e.SetNoGravity(tag.GetValue<int8_t>("NoGravity", 0) != 0);
            // Without this an entity saved mid-portal reloads with a zero
            // cooldown and teleports straight back on its first tick.
            e.portal.SetCooldown(tag.GetValue<int32_t>("PortalCooldown", 0));

            Uuid uuid{};
            if (ReadUuid(tag, "UUID", uuid)) e.SetUuid(uuid);

            // oldPosition must follow, or the first movement step interpolates
            // from wherever the freshly constructed entity happened to be.
            e.oldPosition = e.position;
        }

        // ── LivingEntity ────────────────────────────────────────────────────

        // ── status effects ──────────────────────────────────────────────────
        //
        // MobEffectInstance.CODEC: an `id` plus the Details map. Vanilla's
        // optionalFieldOf defaults let it omit amplifier/duration/ambient/
        // show_particles at their defaults; everything is written explicitly
        // here because an explicit field is still exactly what the codec
        // reads, and omission would make the output depend on values.
        //
        // The hidden chain is what a shorter, weaker effect suspended under a
        // stronger one looks like, and it nests, so this recurses the same way
        // the codec does.
        void WriteEffectBody(Nbt::Writer& w, const MobEffectInstance& e) {
            w.String("id", std::string(kNamespace) + GetEffectName(e.effect));
            w.Byte  ("amplifier", static_cast<int8_t>(e.amplifier));
            w.Int   ("duration",  e.duration);
            w.Bool  ("ambient",   e.ambient);
            w.Bool  ("show_particles", e.visible);
            // MC's showIcon is a separate field that DEFAULTS to showParticles
            // (Details::create). The engine models only the one flag, so
            // writing it for both is the faithful reduction rather than a
            // guess.
            w.Bool  ("show_icon", e.visible);
            if (e.hiddenEffect) {
                w.BeginCompound("hidden_effect");
                WriteEffectBody(w, *e.hiddenEffect);
                w.EndCompound();
            }
        }

        MobEffectId EffectFromName(std::string_view name) {
            const std::string_view bare = StripNamespace(name);
            for (int i = 0; i < static_cast<int>(MobEffectId::Count); ++i) {
                const auto id = static_cast<MobEffectId>(i);
                if (bare == GetEffectName(id)) return id;
            }
            return MobEffectId::Count;   // sentinel: unknown, drop it
        }

        // Returns false for an effect this build does not know, so the caller
        // can drop it rather than install a Speed instance under its name.
        bool ReadEffectBody(const CT& tag, MobEffectInstance& out) {
            const std::string id = tag.GetValue<std::string>("id", "");
            const MobEffectId effect = EffectFromName(id);
            if (effect == MobEffectId::Count) return false;

            out.effect    = effect;
            out.amplifier = tag.GetValue<int8_t>("amplifier", 0) & 0xFF;
            out.duration  = tag.GetValue<int32_t>("duration", 0);
            out.ambient   = tag.GetValue<int8_t>("ambient", 0) != 0;
            out.visible   = tag.GetValue<int8_t>("show_particles", 1) != 0;

            if (auto hidden = As<CT>(tag.GetTag("hidden_effect"))) {
                auto nested = std::make_unique<MobEffectInstance>();
                if (ReadEffectBody(*hidden, *nested)) out.hiddenEffect = std::move(nested);
            }
            return true;
        }

        // ── attributes ──────────────────────────────────────────────────────
        //
        // AttributeInstance.Packed: {id, base, modifiers?}. Only BASE values
        // are written. Modifiers are deliberately omitted, and that is not a
        // shortcut: every modifier this engine creates is transient and rebuilt
        // by the thing that owns it — SetBaby re-derives the baby speed bonus,
        // RestoreEffects re-applies each potion's, taming re-applies the wolf's
        // health bump. Writing them would double them on the next load.
        //
        // EVERY registered attribute is written, including one sitting at the
        // registry default. That is vanilla's AttributeMap.pack(), and it is
        // not an oversight to copy: apply() only touches attributes the file
        // names, so skipping a default-valued row would restore whatever the
        // CONSTRUCTOR chose instead of what was saved. A zombie whose speed
        // was raised to the registry default would come back at 0.23.
        void WriteAttributes(Nbt::Writer& w, const LivingEntity& l) {
            const auto& all = l.Attributes().All();
            if (all.empty()) return;

            auto list = w.BeginList("attributes", Nbt::TagType::Compound);
            for (const auto& inst : all) {
                const auto& def = kAttributeTable[static_cast<size_t>(inst.GetAttribute())];
                w.ListCompoundBegin(list);
                w.String("id",   std::string(kNamespace) + std::string(def.name));
                w.Double("base", inst.GetBaseValue());
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }

        bool AttributeFromName(std::string_view name, Attribute& out) {
            const std::string_view bare = StripNamespace(name);
            for (size_t i = 0; i < static_cast<size_t>(Attribute::Count); ++i) {
                if (bare == kAttributeTable[i].name) {
                    out = static_cast<Attribute>(i);
                    return true;
                }
            }
            return false;
        }

        void ReadAttributes(const CT& tag, LivingEntity& l) {
            auto list = As<LT>(tag.GetTag("attributes"));
            if (!list) return;
            for (const auto& elem : list->value) {
                auto c = As<CT>(elem);
                if (!c) continue;
                Attribute attr{};
                if (!AttributeFromName(c->GetValue<std::string>("id", ""), attr)) continue;
                // Only ever onto an attribute the mob actually registered:
                // SetBaseValue on an absent one would give a zombie a
                // jump_strength row it has no business owning, and MC's own
                // apply() ignores unknown ids the same way.
                if (!l.Attributes().Has(attr)) continue;
                l.Attributes().SetBaseValue(attr, c->GetValue<double>("base", 0.0));
            }
        }

        void WriteLiving(Nbt::Writer& w, const LivingEntity& l) {
            w.Float("Health",    l.GetHealth());
            w.Short("HurtTime",  static_cast<int16_t>(l.hurtTime));
            w.Short("DeathTime", static_cast<int16_t>(l.deathTime));
            w.Int  ("HurtByTimestamp", static_cast<int32_t>(l.GetLastHurtByMobTimestamp()));
            w.Float("AbsorptionAmount", l.GetAbsorptionAmount());
            WriteAttributes(w, l);
            w.Bool ("FallFlying", false);
            if (!l.LastHurtByMobRef().Empty()) {
                WriteRef(w, "last_hurt_by_mob", l.LastHurtByMobRef());
                // The RELATIVE form is the one vanilla reads back; the absolute
                // HurtByTimestamp above is measured against a tick counter that
                // restarts at zero on load, so on its own it would leave the
                // retaliation window either already expired or stuck open.
                w.Int("ticks_since_last_hurt_by_mob",
                      static_cast<int32_t>(l.tickCount - l.GetLastHurtByMobTimestamp()));
            }
            // A real 1.21 pig carries an empty Brain compound; matching it
            // keeps third-party tools from flagging the entity as malformed.
            w.BeginCompound("Brain");
            w.BeginCompound("memories");
            w.EndCompound();
            w.EndCompound();

            // MC omits the key entirely when there are none, and an empty
            // list would have to name an element type it has no basis to pick.
            if (!l.ActiveEffects().empty()) {
                auto effects = w.BeginList("active_effects", Nbt::TagType::Compound);
                for (const auto& e : l.ActiveEffects()) {
                    w.ListCompoundBegin(effects);
                    WriteEffectBody(w, e);
                    w.ListCompoundEnd(effects);
                }
                w.EndList(effects);
            }
        }

        void ReadLiving(const CT& tag, LivingEntity& l) {
            // ORDER IS VANILLA'S, and it is load-bearing: attributes and
            // effects both move MAX health, so Health must be applied AFTER
            // them or SetHealth clamps against a maximum that is about to
            // change (LivingEntity.readAdditionalSaveData does the same).
            l.SetAbsorptionAmount(tag.GetValue<float>("AbsorptionAmount", 0.0f));
            ReadAttributes(tag, l);

            // Installed DIRECTLY, never through AddEffect: that one applies
            // MC's upgrade/merge rules and fires OnEffectAdded, so loading
            // through it would re-run every effect's arrival side effects.
            // RestoreEffects still re-applies the attribute MODIFIERS, which a
            // bare assignment would drop.
            if (auto list = As<LT>(tag.GetTag("active_effects"))) {
                std::vector<MobEffectInstance> restored;
                restored.reserve(list->value.size());
                for (const auto& elem : list->value) {
                    auto c = As<CT>(elem);
                    if (!c) continue;
                    MobEffectInstance inst{};
                    if (ReadEffectBody(*c, inst)) restored.push_back(std::move(inst));
                }
                l.RestoreEffects(std::move(restored));
            }

            l.SetHealth(tag.GetValue<float>("Health", l.GetHealth()));
            l.hurtTime  = tag.GetValue<int16_t>("HurtTime", 0);
            l.deathTime = tag.GetValue<int16_t>("DeathTime", 0);

            Uuid uuid{};
            if (ReadUuid(tag, "last_hurt_by_mob", uuid)) l.SetLastHurtByMobUuid(uuid);

            // Vanilla's own precedence: the relative key wins and is rebased
            // onto this session's tick counter; HurtByTimestamp is the legacy
            // fallback for a file that predates it.
            if (tag.HasTag("ticks_since_last_hurt_by_mob")) {
                l.SetLastHurtByMobTimestamp(
                    l.tickCount - tag.GetValue<int32_t>("ticks_since_last_hurt_by_mob", 0));
            } else {
                l.SetLastHurtByMobTimestamp(tag.GetValue<int32_t>("HurtByTimestamp", 0));
            }
        }

        // ── Mob ─────────────────────────────────────────────────────────────

        void WriteMobLayer(Nbt::Writer& w, const Mob& m) {
            w.Bool("CanPickUpLoot",       m.CanPickUpLoot());
            w.Bool("PersistenceRequired", m.IsPersistenceRequired());
            w.Bool("LeftHanded",          m.IsLeftHanded());
            if (m.IsNoAi()) w.Bool("NoAI", true);
            if (m.HasHome()) {
                w.Int("home_radius", m.GetHomeRadius());
                const glm::ivec3 home = m.GetHomePosition();
                const int32_t pos[3] = {home.x, home.y, home.z};
                w.IntArray("home_pos", pos, 3);
            }
        }

        void ReadMobLayer(const CT& tag, Mob& m) {
            m.SetCanPickUpLoot(tag.GetValue<int8_t>("CanPickUpLoot", 0) != 0);
            m.SetLeftHanded   (tag.GetValue<int8_t>("LeftHanded", 0) != 0);
            m.SetNoAi         (tag.GetValue<int8_t>("NoAI", 0) != 0);

            // Load-bearing: without it a mob that was saved because a player
            // built a farm around it distance-despawns on the tick it loads.
            m.SetPersistenceRequired(tag.GetValue<int8_t>("PersistenceRequired", 0) != 0);

            // Radius BEFORE position — MC only reads home_pos when the radius
            // is non-negative, so the other order discards the position.
            if (tag.HasTag("home_radius")) {
                m.SetHomeRadius(tag.GetValue<int32_t>("home_radius", -1));
                if (auto arr = As<::World::NBTTagIntArray>(tag.GetTag("home_pos"));
                    arr && arr->value.size() == 3) {
                    m.SetHomeTo(glm::ivec3(arr->value[0], arr->value[1], arr->value[2]),
                                m.GetHomeRadius());
                }
            }
        }

        // ── Animal / ageable ────────────────────────────────────────────────

        void WriteAnimalLayer(Nbt::Writer& w, const Animal& a) {
            w.Int("Age",       a.GetAge());
            w.Int("ForcedAge", a.GetForcedAge());
            w.Int("InLove",    a.GetInLoveTicks());
            WriteRef(w, "LoveCause", a.LoveCauseRef());
        }

        void ReadAnimalLayer(const CT& tag, Animal& a) {
            a.SetAge      (tag.GetValue<int32_t>("Age", 0));
            a.SetForcedAge(tag.GetValue<int32_t>("ForcedAge", 0));
            // SetInLoveTicks, never SetInLove: the latter hardcodes 600 ticks
            // and broadcasts the heart-particle event, so loading through it
            // would reset every timer and spray hearts across the world.
            a.SetInLoveTicks(tag.GetValue<int32_t>("InLove", 0));

            Uuid uuid{};
            if (ReadUuid(tag, "LoveCause", uuid)) a.SetLoveCauseUuid(uuid);
        }

        // ── Tamable / neutral mixins ────────────────────────────────────────

        void WriteTamable(Nbt::Writer& w, const TamableAnimal& t) {
            WriteRef(w, "Owner", t.OwnerRef());
            w.Bool("Sitting", t.IsOrderedToSit());
        }

        void ReadTamable(const CT& tag, TamableAnimal& t) {
            Uuid uuid{};
            if (ReadUuid(tag, "Owner", uuid)) {
                t.SetOwnerUuid(uuid);
                // Side effects OFF: the tamed max-health comes from the saved
                // attributes, not from re-running the taming ceremony.
                t.SetTame(true, false);
            } else {
                t.SetTame(false, true);
            }
            t.SetOrderedToSit(tag.GetValue<int8_t>("Sitting", 0) != 0);
        }

        // NeutralMob.addPersistentAngerSaveData. Vanilla writes the ABSOLUTE
        // end time under "anger_end_time" and the target under "angry_at";
        // "AngerTime" is a legacy remaining-tick int it still READS but no
        // longer writes, and "AngryAt" it does not read at all. The engine
        // already keeps the absolute form, so the modern keys are a straight
        // copy and the lossy conversion this used to do is gone.
        //
        // The legacy pair is still written alongside: it costs a few bytes and
        // it is what an older Minecraft (or a third-party tool) reads.
        void WriteNeutral(Nbt::Writer& w, const NeutralMob& n, int64_t gameTime) {
            const int64_t endTime = n.GetPersistentAngerEndTime();
            w.Long("anger_end_time", endTime);
            WriteRef(w, "angry_at", n.AngryAtRef());

            const int64_t remaining =
                (endTime == NeutralMob::kNoAngerEndTime) ? 0 : (endTime - gameTime);
            w.Int("AngerTime", static_cast<int32_t>(remaining > 0 ? remaining : 0));
        }

        void ReadNeutral(const CT& tag, NeutralMob& n) {
            // Vanilla's own precedence: the absolute key wins outright, and the
            // legacy remaining-tick int is consulted only in its absence.
            if (tag.HasTag("anger_end_time")) {
                n.SetPersistentAngerEndTime(tag.GetValue<int64_t>("anger_end_time",
                                                                 NeutralMob::kNoAngerEndTime));
            } else {
                const int32_t remaining = tag.GetValue<int32_t>("AngerTime", 0);
                // SetTimeToRemainAngry is endTime = gameTime + remaining.
                if (remaining > 0) n.SetTimeToRemainAngry(remaining);
                else               n.SetPersistentAngerEndTime(NeutralMob::kNoAngerEndTime);
            }

            Uuid uuid{};
            // "AngryAt" is read only so that worlds this engine wrote before
            // the spelling was corrected still load their grudges.
            if (ReadUuid(tag, "angry_at", uuid) || ReadUuid(tag, "AngryAt", uuid)) {
                n.SetPersistentAngerTargetUuid(uuid);
            }
        }

        // ── string enums ────────────────────────────────────────────────────
        //
        // Several vanilla per-type keys are StringRepresentable enums, NOT
        // ids: writing an int where vanilla expects a name makes the codec
        // fail and MC falls back to the enum's default, silently turning
        // every snow fox red and every panda normal. The tables below are
        // indexed by the engine enum's own value, which was verified to match
        // vanilla's declaration order in each case.

        constexpr const char* kFoxVariantNames[] = { "red", "snow" };
        // MC TemperatureVariants, in TemperatureVariant order. Written the
        // way VariantUtils.writeVariant does: "variant" = "minecraft:<name>".
        constexpr const char* kTemperatureVariantNames[] = { "temperate", "warm", "cold" };
        constexpr const char* kPandaGeneNames[] = {
            "normal", "lazy", "worried", "playful", "brown", "weak", "aggressive"
        };
        constexpr const char* kArmadilloStateNames[] = {
            "idle", "rolling", "scared", "unrolling"
        };
        // MC CatVariants bootstrap order — the same order the renderer's
        // texture table uses, so the engine's raw index IS this index.
        constexpr const char* kCatVariantNames[] = {
            "tabby", "black", "red", "siamese", "british_shorthair", "calico",
            "persian", "ragdoll", "white", "jellie", "all_black"
        };

        template <size_t N>
        std::string EnumName(const char* const (&table)[N], size_t index) {
            return std::string(kNamespace) + (index < N ? table[index] : table[0]);
        }

        // Returns the table default (index 0) for anything unrecognised,
        // which is what every one of vanilla's own codecs does here.
        template <size_t N>
        size_t EnumIndex(const char* const (&table)[N], std::string_view name) {
            const std::string_view bare = StripNamespace(name);
            for (size_t i = 0; i < N; ++i) if (bare == table[i]) return i;
            return 0;
        }

        // "variant" = "minecraft:<name>" (VariantUtils.writeVariant); EnumName
        // adds the namespace and EnumIndex strips it, so a bare "cold" reads
        // too and anything unknown falls back to temperate.
        std::string TemperatureVariantId(uint8_t v) {
            return EnumName(kTemperatureVariantNames, static_cast<size_t>(v));
        }
        uint8_t TemperatureVariantFromId(const std::string& id) {
            return static_cast<uint8_t>(EnumIndex(kTemperatureVariantNames, id));
        }

        // ── Projectile ──────────────────────────────────────────────────────
        //
        // Vanilla projectiles are NOT LivingEntities, so the Health / Brain /
        // Mob keys the layers above emit are foreign to them. They are still
        // written: vanilla's ValueInput reads only the keys it asks for and
        // ignores the rest, so the file loads in real Minecraft either way,
        // and dropping them would cost this engine — where a Projectile IS a
        // Mob — its own round-trip. The projectile keys below are the ones
        // Minecraft actually reads back.

        void WriteProjectileLayer(Nbt::Writer& w, const Projectile& p) {
            WriteRef(w, "Owner", p.OwnerRef());
            // MC writes HasBeenShot unconditionally and LeftOwner only when
            // set (Projectile.addAdditionalSaveData). HasBeenShot is not
            // modelled — a projectile in this engine is always already shot —
            // so it is emitted as the constant vanilla would have written for
            // an in-flight projectile.
            w.Bool("HasBeenShot", true);
            if (p.HasLeftOwner()) w.Bool("LeftOwner", true);
        }

        void ReadProjectileLayer(const CT& tag, Projectile& p) {
            Uuid uuid{};
            if (ReadUuid(tag, "Owner", uuid)) p.SetOwnerUuid(uuid);
            p.SetLeftOwner(tag.GetValue<int8_t>("LeftOwner", 0) != 0);
        }

        // AbstractArrow. `pickup`, `crit`, `PierceLevel`, `SoundEvent`,
        // `item` and `weapon` are vanilla keys this engine models no state
        // for, so they are not invented — a reader that wants them gets
        // vanilla's own defaults (DISALLOWED / false / 0 / the type's default
        // pickup item), which is exactly what an arrow fired by a mob has.
        void WriteArrowLayer(Nbt::Writer& w, const Arrow& a) {
            w.Short ("life",   static_cast<int16_t>(a.GetLife()));
            w.Byte  ("shake",  static_cast<int8_t>(a.GetShakeTime()));
            w.Bool  ("inGround", a.IsInGroundArrow());
            w.Double("damage", a.GetBaseDamage());
            // Engine-only: vanilla re-derives its equivalent from inGround.
            // Without it a trident that has already dealt its damage becomes
            // able to hit again on the tick it loads.
            w.Int   ("obey_in_ground_time", a.GetInGroundTime());
        }

        void ReadArrowLayer(const CT& tag, Arrow& a) {
            a.SetLife        (tag.GetValue<int16_t>("life", 0));
            // Vanilla masks the byte back to unsigned; shake is 0..7.
            a.SetShakeTime   (tag.GetValue<int8_t>("shake", 0) & 0xFF);
            a.SetInGround    (tag.GetValue<int8_t>("inGround", 0) != 0);
            a.SetBaseDamage  (tag.GetValue<double>("damage", Arrow::kArrowBaseDamage));
            a.SetInGroundTime(tag.GetValue<int32_t>("obey_in_ground_time", 0));
        }

    } // namespace

    // ── type names ──────────────────────────────────────────────────────────

    std::string EntityName(EntityTypeId type) {
        const std::string_view slug = GetEntityTypeInfo(type).slug;
        if (slug.empty()) return {};
        return std::string(kNamespace) + std::string(slug);
    }

    bool EntityTypeFromName(std::string_view name, EntityTypeId& out) {
        static const std::unordered_map<std::string, EntityTypeId> index = [] {
            std::unordered_map<std::string, EntityTypeId> map;
            for (uint16_t i = 0; i < static_cast<uint16_t>(EntityTypeId::Count); ++i) {
                const auto type = static_cast<EntityTypeId>(i);
                const std::string_view slug = GetEntityTypeInfo(type).slug;
                if (!slug.empty()) map.emplace(std::string(slug), type);
            }
            return map;
        }();

        const auto it = index.find(std::string(StripNamespace(name)));
        if (it == index.end()) return false;
        out = it->second;
        return true;
    }

    EntityKind ClassifyEntity(const ::World::NBTTagCompound& tag, EntityTypeId& outType) {
        const std::string id = tag.GetValue<std::string>("id");
        if (id.empty()) return EntityKind::Unknown;

        const std::string_view bare = StripNamespace(id);
        if (bare == "item")           return EntityKind::Item;
        if (bare == "experience_orb") return EntityKind::Orb;
        if (!EntityTypeFromName(id, outType)) return EntityKind::Unknown;
        return EntityKind::Mob;
    }

    // ── mobs ────────────────────────────────────────────────────────────────

    bool WriteMob(Nbt::Writer& w, Nbt::Writer::ListScope& list, const Mob& mob) {
        // Three reasons never to write: the type has no vanilla name, the
        // entity opted out (projectiles until their owners can round-trip),
        // or it is already dead and merely waiting for the sweep.
        if (!mob.CanSerialize() || mob.IsRemoved()) return false;
        const std::string name = EntityName(mob.GetType());
        if (name.empty()) return false;

        w.ListCompoundBegin(list);
        w.String("id", name);
        WriteEntityBase(w, mob);
        WriteLiving(w, mob);
        WriteMobLayer(w, mob);

        if (const auto* animal = dynamic_cast<const Animal*>(&mob)) WriteAnimalLayer(w, *animal);
        if (const auto* tamable = dynamic_cast<const TamableAnimal*>(&mob)) WriteTamable(w, *tamable);
        if (const auto* neutral = dynamic_cast<const NeutralMob*>(&mob)) {
            const EntityLevel* level = mob.Level();
            WriteNeutral(w, *neutral, level ? level->GetGameTime() : 0);
        }

        // Projectiles: super-first, exactly as the vanilla chain emits them.
        if (const auto* proj = dynamic_cast<const Projectile*>(&mob)) {
            WriteProjectileLayer(w, *proj);
            if (const auto* hurting = dynamic_cast<const HurtingProjectile*>(proj)) {
                w.Double("acceleration_power", hurting->GetAccelerationPower());
            }
            if (const auto* arrow = dynamic_cast<const Arrow*>(proj)) {
                WriteArrowLayer(w, *arrow);
            }
        }

        // Per-type extras. Only types whose state has a real setter appear
        // here; anything else round-trips on the layers above, which is what
        // every Generic* mob relies on.
        switch (mob.GetType()) {
            case EntityTypeId::Sheep:
                if (const auto* s = dynamic_cast<const Sheep*>(&mob)) {
                    w.Byte("Color", static_cast<int8_t>(s->GetColor()));
                    w.Bool("Sheared", s->IsSheared());
                }
                break;
            // MC SnowGolem.addAdditionalSaveData: "Pumpkin".
            case EntityTypeId::SnowGolem:
                if (const auto* g = dynamic_cast<const SnowGolem*>(&mob)) {
                    w.Bool("Pumpkin", g->HasPumpkin());
                }
                break;
            case EntityTypeId::Chicken:
                if (const auto* c = dynamic_cast<const Chicken*>(&mob)) {
                    w.Bool("IsChickenJockey", c->IsChickenJockey());
                    w.String("variant", TemperatureVariantId(c->GetVariantByte()));
                }
                break;
            case EntityTypeId::Cow:
                if (const auto* c = dynamic_cast<const Cow*>(&mob)) {
                    w.String("variant", TemperatureVariantId(c->GetVariantByte()));
                }
                break;
            case EntityTypeId::Pig:
                if (const auto* p = dynamic_cast<const Pig*>(&mob)) {
                    w.String("variant", TemperatureVariantId(p->GetVariantByte()));
                }
                break;
            case EntityTypeId::Slime:
            case EntityTypeId::MagmaCube:
                if (const auto* s = dynamic_cast<const Slime*>(&mob)) {
                    // Vanilla stores size - 1.
                    w.Int("Size", s->GetSize() - 1);
                }
                break;
            // MC 26.3 SulfurCube.addAdditionalSaveData over AbstractCubeMob's
            // Size and AgeableMob's Age/ForcedAge/AgeLocked. The swallowed
            // block is MC's BODY equipment slot; here it is the item's slug.
            case EntityTypeId::SulfurCube:
                if (const auto* c = dynamic_cast<const SulfurCube*>(&mob)) {
                    w.Int("Size", c->GetSize() - 1);
                    w.Int("Age", c->GetAge());
                    w.Int("ForcedAge", c->GetForcedAge());
                    w.Bool("AgeLocked", c->IsAgeLocked());
                    w.Int("pickup_timer", c->GetPickupTimer());
                    w.Bool("from_bucket", c->FromBucket());
                    w.Int("fuse", c->GetFuse());
                    if (c->HasBodyItem()) {
                        w.String("BodyItem", ItemName(c->GetBodyItem()));
                    }
                }
                break;
            case EntityTypeId::Zombie:
            case EntityTypeId::Husk:
            case EntityTypeId::Drowned:
            case EntityTypeId::ZombieVillager:
                if (const auto* z = dynamic_cast<const Zombie*>(&mob)) {
                    w.Bool("IsBaby", z->IsBaby());
                    w.Bool("CanBreakDoors", z->CanBreakDoors());
                    w.Int ("InWaterTime", z->GetInWaterTime());
                    w.Int ("DrownedConversionTime", z->GetDrownedConversionTime());
                }
                break;
            case EntityTypeId::Fox:
                if (const auto* f = dynamic_cast<const Fox*>(&mob)) {
                    w.Bool  ("Sleeping",  f->IsSleeping());
                    w.Bool  ("Sitting",   f->IsSitting());
                    w.Bool  ("Crouching", f->IsFoxCrouching());
                    w.String("Type", EnumName(kFoxVariantNames,
                                              static_cast<size_t>(f->GetVariant())));
                    // "Trusted" is omitted: this engine models no trust list
                    // (DefendTrustedTargetGoal documents it as permanently
                    // empty), so an empty list would assert a fact we cannot
                    // know and would erase a real one on an imported world.
                }
                break;
            case EntityTypeId::Panda:
                if (const auto* p = dynamic_cast<const Panda*>(&mob)) {
                    w.String("MainGene",   EnumName(kPandaGeneNames,
                                                    static_cast<size_t>(p->GetMainGene())));
                    w.String("HiddenGene", EnumName(kPandaGeneNames,
                                                    static_cast<size_t>(p->GetHiddenGene())));
                }
                break;
            case EntityTypeId::Rabbit:
                if (const auto* r = dynamic_cast<const Rabbit*>(&mob)) {
                    w.Int("MoreCarrotTicks", r->GetMoreCarrotTicks());
                    // "RabbitType" is omitted — the engine models no rabbit
                    // variant at all, and writing 0 would repaint every
                    // imported black/gold/killer rabbit brown.
                }
                break;
            case EntityTypeId::Ocelot:
                if (const auto* o = dynamic_cast<const Ocelot*>(&mob)) {
                    w.Bool("Trusting", o->IsTrusting());
                }
                break;
            case EntityTypeId::Cat:
                if (const auto* c = dynamic_cast<const Cat*>(&mob)) {
                    w.String("variant", EnumName(kCatVariantNames, c->GetVariantByte()));
                }
                break;
            case EntityTypeId::Turtle:
                if (const auto* t = dynamic_cast<const Turtle*>(&mob)) {
                    const glm::ivec3 home = t->HomePos();
                    const int32_t hp[3] = {home.x, home.y, home.z};
                    w.IntArray("home_pos", hp, 3);
                    w.Bool("has_egg", t->HasEgg());
                }
                break;
            case EntityTypeId::Horse:
            case EntityTypeId::Donkey:
            case EntityTypeId::Mule:
            case EntityTypeId::SkeletonHorse:
            case EntityTypeId::ZombieHorse:
                if (const auto* h = dynamic_cast<const AbstractHorse*>(&mob)) {
                    w.Bool("EatingHaystack", h->IsEating());
                    w.Int ("Temper",         h->GetTemper());
                    // NOT the TamableAnimal "Tame" — AbstractHorse owns its
                    // own tamed flag and does not use that mixin.
                    w.Bool("Tame",           h->IsTamedHorse());
                }
                break;
            case EntityTypeId::Bat:
                if (const auto* b = dynamic_cast<const Bat*>(&mob)) {
                    // Vanilla writes the whole flags byte; bit 0 is the only
                    // one defined.
                    w.Byte("BatFlags", static_cast<int8_t>(b->IsResting() ? 1 : 0));
                }
                break;
            case EntityTypeId::Bee:
                if (const auto* b = dynamic_cast<const Bee*>(&mob)) {
                    w.Bool("HasStung",  b->HasStung());
                    w.Bool("HasNectar", b->HasNectar());
                    w.Int ("TicksSincePollination",      b->GetTicksWithoutNectar());
                    w.Int ("CropsGrownSincePollination", b->GetCropsGrownSincePollination());
                    if (b->HasSavedFlowerPos()) {
                        const glm::ivec3 fp = b->GetSavedFlowerPos();
                        const int32_t a[3] = {fp.x, fp.y, fp.z};
                        w.IntArray("flower_pos", a, 3);
                    }
                }
                break;
            case EntityTypeId::Dolphin:
                if (const auto* d = dynamic_cast<const Dolphin*>(&mob)) {
                    w.Int("Moistness", d->GetMoistness());
                }
                break;
            case EntityTypeId::Pufferfish:
                if (const auto* p = dynamic_cast<const Pufferfish*>(&mob)) {
                    w.Int("PuffState", p->GetPuffState());
                }
                break;
            case EntityTypeId::Camel:
                if (const auto* c = dynamic_cast<const Camel*>(&mob)) {
                    w.Long("LastPoseTick", c->GetLastPoseChangeTick());
                }
                break;
            case EntityTypeId::Axolotl:
                if (const auto* a = dynamic_cast<const Axolotl*>(&mob)) {
                    // LEGACY_CODEC — an int id, not a name.
                    w.Int("Variant", static_cast<int32_t>(a->GetVariant()));
                }
                break;
            case EntityTypeId::Armadillo:
                if (const auto* a = dynamic_cast<const Armadillo*>(&mob)) {
                    w.String("state", EnumName(kArmadilloStateNames,
                                               static_cast<size_t>(a->GetState())));
                }
                break;
            case EntityTypeId::Creeper:
                if (const auto* c = dynamic_cast<const Creeper*>(&mob)) {
                    w.Bool("ignited", c->IsIgnited());
                }
                break;
            case EntityTypeId::Enderman:
                if (const auto* e = dynamic_cast<const Enderman*>(&mob)) {
                    // storeNullable: the key is ABSENT for an empty-handed
                    // enderman, never air. Properties are omitted because the
                    // engine carries a BlockID, not a full state — vanilla
                    // fills the rest from the block's defaults.
                    const BlockID held = e->GetCarriedBlock();
                    if (held != BlockID::Air) {
                        w.BeginCompound("carriedBlockState");
                        w.String("Name", std::string(kNamespace) +
                                             BlockRegistry::Get(held).registrySlug);
                        w.EndCompound();
                    }
                }
                break;
            case EntityTypeId::Shulker:
                if (const auto* sh = dynamic_cast<const Shulker*>(&mob)) {
                    w.Byte("AttachFace", static_cast<int8_t>(sh->GetAttachFace()));
                    w.Byte("Peek",       static_cast<int8_t>(sh->GetRawPeekAmount()));
                }
                break;
            case EntityTypeId::Phantom:
                if (const auto* p = dynamic_cast<const Phantom*>(&mob)) {
                    w.Int("size", p->GetPhantomSize());
                    if (p->HasAnchorPoint()) {
                        const glm::ivec3 ap = p->GetAnchorPoint();
                        const int32_t a[3] = {ap.x, ap.y, ap.z};
                        w.IntArray("anchor_pos", a, 3);
                    }
                }
                break;
            case EntityTypeId::Endermite:
                // MC Endermite.addAdditionalSaveData — the two-minute clock.
                if (const auto* em = dynamic_cast<const Endermite*>(&mob)) {
                    w.Int("Lifetime", em->GetLife());
                }
                break;
            case EntityTypeId::Vex:
                if (const auto* v = dynamic_cast<const Vex*>(&mob)) {
                    if (v->HasBoundOrigin()) {
                        const glm::ivec3 bo = v->GetBoundOrigin();
                        const int32_t a[3] = {bo.x, bo.y, bo.z};
                        w.IntArray("bound_pos", a, 3);
                    }
                    if (v->HasLimitedLife()) w.Int("life_ticks", v->GetLimitedLifeTicks());
                    // The summoner's identity is written when it is live so a
                    // vanilla reader gets it; the engine cannot read it back
                    // (Vex holds a raw Mob*, not an EntityRef), which is why
                    // there is no matching case on the load side.
                    if (const Mob* owner = v->GetVexOwner()) {
                        WriteUuid(w, "owner", owner->GetUuid());
                    }
                }
                break;
            case EntityTypeId::Ravager:
                if (const auto* r = dynamic_cast<const Ravager*>(&mob)) {
                    w.Int("AttackTick", r->GetAttackTick());
                    w.Int("StunTick",   r->GetStunnedTick());
                    w.Int("RoarTick",   r->GetRoarTick());
                }
                break;
            case EntityTypeId::Ghast:
                if (const auto* g = dynamic_cast<const Ghast*>(&mob)) {
                    w.Byte("ExplosionPower", static_cast<int8_t>(g->GetExplosionPower()));
                }
                break;
            case EntityTypeId::Evoker:
            case EntityTypeId::Illusioner:
                if (const auto* sc = dynamic_cast<const SpellcasterIllager*>(&mob)) {
                    w.Int("SpellTicks", sc->GetSpellCastingTime());
                }
                break;
            case EntityTypeId::Wither:
                if (const auto* wi = dynamic_cast<const Wither*>(&mob)) {
                    w.Int("Invul", wi->GetInvulnerableTicks());
                }
                break;
            case EntityTypeId::EnderDragon:
                if (const auto* d = dynamic_cast<const EnderDragon*>(&mob)) {
                    w.Int("DragonPhase", static_cast<int32_t>(d->GetPhase()));
                    // MC DRAGON_DEATH_TIME_KEY — a save mid-cinematic resumes
                    // the float-up rather than restarting a live dragon.
                    w.Int("DragonDeathTime", d->deathTime);
                }
                break;
            case EntityTypeId::EndCrystal:
                if (const auto* c = dynamic_cast<const EndCrystal*>(&mob)) {
                    // MC stores beam_target with BlockPos.CODEC — an int
                    // array — and ShowBottom as a byte. Invulnerable (the
                    // four ritual crystals) rides WriteEntityBase.
                    if (c->HasBeamTarget()) {
                        const int beam[3] = { c->BeamTarget().x,
                                              c->BeamTarget().y,
                                              c->BeamTarget().z };
                        w.IntArray("beam_target", beam, 3);
                    }
                    w.Bool("ShowBottom", c->ShowsBottom());
                }
                break;
            case EntityTypeId::Piglin:
                if (const auto* p = dynamic_cast<const Piglin*>(&mob)) {
                    w.Bool("IsBaby", p->IsBaby());
                    // POLARITY: the key is the negation of the getter.
                    w.Bool("CannotHunt", !p->CanHunt());
                    w.Bool("IsImmuneToZombification", p->IsImmuneToZombification());
                    w.Int ("TimeInOverworld", p->GetTimeInOverworld());
                }
                break;
            case EntityTypeId::PiglinBrute:
                if (const auto* p = dynamic_cast<const PiglinBrute*>(&mob)) {
                    w.Bool("IsImmuneToZombification", p->IsImmuneToZombification());
                    w.Int ("TimeInOverworld", p->GetTimeInOverworld());
                }
                break;
            case EntityTypeId::Zoglin:
                if (const auto* z = dynamic_cast<const Zoglin*>(&mob)) {
                    w.Bool("IsBaby", z->IsBaby());
                }
                break;
            case EntityTypeId::ZombifiedPiglin:
                if (const auto* z = dynamic_cast<const Zombie*>(&mob)) {
                    w.Bool("IsBaby", z->IsBaby());
                    w.Bool("CanBreakDoors", z->CanBreakDoors());
                }
                break;
            case EntityTypeId::Trident:
                if (const auto* t = dynamic_cast<const ThrownTrident*>(&mob)) {
                    w.Bool("DealtDamage", t->IsDealtDamage());
                }
                break;
            case EntityTypeId::Fireball:
                if (const auto* f = dynamic_cast<const LargeFireball*>(&mob)) {
                    w.Byte("ExplosionPower", static_cast<int8_t>(f->GetExplosionPower()));
                }
                break;
            case EntityTypeId::WitherSkull:
                if (const auto* k = dynamic_cast<const WitherSkull*>(&mob)) {
                    w.Bool("dangerous", k->IsDangerous());
                }
                break;
            case EntityTypeId::ShulkerBullet:
                if (const auto* b = dynamic_cast<const ShulkerBullet*>(&mob)) {
                    // Vanilla stores Dir as a nullable legacy direction id and
                    // omits the key when there is none; -1 is this engine's
                    // "none", so the key is simply not written for it.
                    if (b->GetMoveDirection() >= 0) w.Byte("Dir", static_cast<int8_t>(b->GetMoveDirection()));
                    w.Int   ("Steps", b->GetFlightSteps());
                    const glm::dvec3 d = b->GetTargetDelta();
                    w.Double("TXD", d.x);
                    w.Double("TYD", d.y);
                    w.Double("TZD", d.z);
                }
                break;
            case EntityTypeId::EyeOfEnder:
                if (const auto* e = dynamic_cast<const EyeOfEnder*>(&mob)) {
                    w.BeginCompound("Item");
                    WriteItemStackBody(w, e->GetItem());
                    w.EndCompound();
                    // Engine-only: without them a saved eye restarts its
                    // 80-tick flight and re-rolls whether it survives.
                    w.Int ("obey_life", e->GetLife());
                    w.Bool("obey_survives", e->SurvivesAfterDeath());
                }
                break;
            case EntityTypeId::FallingBlock:
                if (const auto* fb = dynamic_cast<const FallingBlockEntity*>(&mob)) {
                    // MC FallingBlockEntity.addAdditionalSaveData, key for key.
                    // BlockState is vanilla's {Name, Properties} compound so
                    // the save stays readable by real Minecraft.
                    WriteBlockStateCompound(w, "BlockState", fb->CarriedState());
                    w.Int  ("Time",           fb->Time());
                    w.Bool ("DropItem",       fb->DropsItem());
                    w.Bool ("HurtEntities",   fb->HurtsEntities());
                    w.Float("FallHurtAmount", fb->FallDamagePerDistance());
                    w.Int  ("FallHurtMax",    fb->FallDamageMax());
                    w.Bool ("CancelDrop",     fb->CancelDrop());
                    // TileEntityData is deliberately skipped: the engine does
                    // not carry block-entity NBT on a carried state, so a
                    // falling chest would land empty either way. Named here so
                    // the omission is visible rather than mysterious.
                }
                break;
            case EntityTypeId::Tnt:
                if (const auto* tnt = dynamic_cast<const PrimedTnt*>(&mob)) {
                    // MC PrimedTnt.addAdditionalSaveData.
                    w.Short("fuse", static_cast<int16_t>(tnt->GetFuse()));
                    WriteBlockStateCompound(w, "block_state", tnt->CarriedState());
                    // MC writes explosion_power ONLY when it differs from the
                    // default, so an ordinary TNT's compound stays small.
                    if (tnt->ExplosionPower() != PrimedTnt::kDefaultPower) {
                        w.Float("explosion_power", tnt->ExplosionPower());
                    }
                    WriteRef(w, "owner", tnt->OwnerRef());
                }
                break;
            case EntityTypeId::EvokerFangs:
                if (const auto* f = dynamic_cast<const EvokerFangs*>(&mob)) {
                    w.Int("Warmup", f->GetWarmupDelay());
                }
                break;
            case EntityTypeId::AreaEffectCloud:
                if (const auto* c = dynamic_cast<const AreaEffectCloud*>(&mob)) {
                    w.Float("Radius",              c->GetRadius());
                    w.Int  ("Duration",            c->GetDuration());
                    w.Int  ("WaitTime",            c->GetWaitTime());
                    w.Int  ("ReapplicationDelay",  c->GetReapplicationDelay());
                    w.Int  ("DurationOnUse",       c->GetDurationOnUse());
                    w.Float("RadiusOnUse",         c->GetRadiusOnUse());
                    w.Float("RadiusPerTick",       c->GetRadiusPerTick());
                    w.Float("potion_duration_scale", c->GetPotionDurationScale());
                }
                break;
            default:
                break;
        }

        // Riders are NESTED, never stored as siblings — vanilla's
        // Entity.saveWithoutId writes childrenList("Passengers") and its
        // EntityStorage only ever iterates ROOT entities. Recursion is bounded
        // by the rider graph itself, which Entity refuses to make cyclic
        // (StartRiding rejects a cycle).
        //
        // Only Mob passengers travel: a riding PLAYER lives in playerdata, and
        // vanilla dismounts players on save rather than nesting them here.
        {
            bool opened = false;
            Nbt::Writer::ListScope riders{};
            for (const Entity* p : mob.GetPassengers()) {
                const auto* rider = dynamic_cast<const Mob*>(p);
                if (!rider) continue;
                if (!opened) {
                    riders = w.BeginList("Passengers", Nbt::TagType::Compound);
                    opened = true;
                }
                WriteMob(w, riders, *rider);
            }
            if (opened) w.EndList(riders);
        }

        w.ListCompoundEnd(list);
        return true;
    }

    void ApplyMobNbt(const ::World::NBTTagCompound& tag, Mob& mob) {
        // ORDER MATTERS and is MC's read order, not the reverse of the write.
        //
        // Slime is the one class that reads its own field BEFORE super, because
        // SetSize rewrites MaxHealth and would otherwise heal a damaged slime
        // back to full on every load.
        if (mob.GetType() == EntityTypeId::Slime || mob.GetType() == EntityTypeId::MagmaCube ||
            mob.GetType() == EntityTypeId::SulfurCube) {
            if (auto* s = dynamic_cast<Slime*>(&mob)) {
                if (tag.HasTag("Size")) {
                    const int size = tag.GetValue<int32_t>("Size", 0) + 1;
                    s->SetSize(size < 1 ? 1 : size, /*resetHealth=*/false);
                }
            }
        }

        // Phantom, for the same reason as Slime: SetPhantomSize re-bases
        // MAX_HEALTH and ATTACK_DAMAGE, so applying it after ReadLiving would
        // heal a wounded phantom back to the new maximum.
        if (mob.GetType() == EntityTypeId::Phantom) {
            if (auto* p = dynamic_cast<Phantom*>(&mob)) {
                if (tag.HasTag("size")) p->SetPhantomSize(tag.GetValue<int32_t>("size", 0));
            }
        }

        ReadEntityBase(tag, mob);
        ReadLiving(tag, mob);
        ReadMobLayer(tag, mob);

        if (auto* animal = dynamic_cast<Animal*>(&mob)) ReadAnimalLayer(tag, *animal);
        if (auto* tamable = dynamic_cast<TamableAnimal*>(&mob)) ReadTamable(tag, *tamable);
        if (auto* neutral = dynamic_cast<NeutralMob*>(&mob)) ReadNeutral(tag, *neutral);

        if (auto* proj = dynamic_cast<Projectile*>(&mob)) {
            ReadProjectileLayer(tag, *proj);
            if (auto* hurting = dynamic_cast<HurtingProjectile*>(proj)) {
                hurting->SetAccelerationPower(
                    tag.GetValue<double>("acceleration_power", 0.1));
            }
            if (auto* arrow = dynamic_cast<Arrow*>(proj)) ReadArrowLayer(tag, *arrow);
        }

        switch (mob.GetType()) {
            case EntityTypeId::SulfurCube:
                if (auto* c = dynamic_cast<SulfurCube*>(&mob)) {
                    c->SetAge(tag.GetValue<int32_t>("Age", 0));
                    c->SetForcedAge(tag.GetValue<int32_t>("ForcedAge", 0));
                    c->SetAgeLocked(tag.GetValue<int8_t>("AgeLocked", 0) != 0);
                    c->SetPickupTimer(tag.GetValue<int32_t>("pickup_timer", 0));
                    c->SetFromBucket(tag.GetValue<int8_t>("from_bucket", 0) != 0);
                    if (tag.HasTag("BodyItem")) {
                        const ItemID item = ItemFromName(
                            tag.GetValue<std::string>("BodyItem", ""));
                        if (item != Items::Air) c->SetBodyItem(item);
                    }
                    // The fuse rides MAX_FUSE's byte on the wire; a saved lit
                    // cube resumes its countdown (MC: fuse then MAX_FUSE = fuse).
                    const int fuse = tag.GetValue<int32_t>("fuse", -1);
                    if (fuse >= 0) c->SetAnimStateByte(static_cast<uint8_t>(std::min(254, fuse) + 1));
                }
                break;
            case EntityTypeId::Sheep:
                if (auto* s = dynamic_cast<Sheep*>(&mob)) {
                    s->SetColor(static_cast<uint8_t>(tag.GetValue<int8_t>("Color", 0)));
                    s->SetSheared(tag.GetValue<int8_t>("Sheared", 0) != 0);
                }
                break;
            // MC SnowGolem.readAdditionalSaveData: getBooleanOr("Pumpkin", true).
            case EntityTypeId::SnowGolem:
                if (auto* g = dynamic_cast<SnowGolem*>(&mob)) {
                    g->SetPumpkin(tag.GetValue<int8_t>("Pumpkin", 1) != 0);
                }
                break;
            case EntityTypeId::Chicken:
                if (auto* c = dynamic_cast<Chicken*>(&mob)) {
                    c->SetChickenJockey(tag.GetValue<int8_t>("IsChickenJockey", 0) != 0);
                    if (tag.HasTag("variant")) c->SetVariantByte(TemperatureVariantFromId(tag.GetValue<std::string>("variant", "")));
                }
                break;
            case EntityTypeId::Cow:
                if (auto* c = dynamic_cast<Cow*>(&mob)) {
                    if (tag.HasTag("variant")) c->SetVariantByte(TemperatureVariantFromId(tag.GetValue<std::string>("variant", "")));
                }
                break;
            case EntityTypeId::Pig:
                if (auto* p = dynamic_cast<Pig*>(&mob)) {
                    if (tag.HasTag("variant")) p->SetVariantByte(TemperatureVariantFromId(tag.GetValue<std::string>("variant", "")));
                }
                break;
            case EntityTypeId::Zombie:
            case EntityTypeId::Husk:
            case EntityTypeId::Drowned:
            case EntityTypeId::ZombieVillager:
                if (auto* z = dynamic_cast<Zombie*>(&mob)) {
                    // SetBaby re-derives the baby speed modifier, which is why
                    // attribute MODIFIERS are not persisted: they are rebuilt.
                    z->SetBaby(tag.GetValue<int8_t>("IsBaby", 0) != 0);
                    z->SetCanBreakDoors(tag.GetValue<int8_t>("CanBreakDoors", 0) != 0);
                    z->SetInWaterTime(tag.GetValue<int32_t>("InWaterTime", 0));
                    z->SetDrownedConversionTime(
                        tag.GetValue<int32_t>("DrownedConversionTime", -1));
                }
                break;
            case EntityTypeId::Fox:
                if (auto* f = dynamic_cast<Fox*>(&mob)) {
                    f->SetSleeping   (tag.GetValue<int8_t>("Sleeping", 0) != 0);
                    f->SetSitting    (tag.GetValue<int8_t>("Sitting", 0) != 0);
                    f->SetIsCrouching(tag.GetValue<int8_t>("Crouching", 0) != 0);
                    f->SetVariant(static_cast<Fox::Variant>(
                        EnumIndex(kFoxVariantNames, tag.GetValue<std::string>("Type", ""))));
                }
                break;
            case EntityTypeId::Panda:
                if (auto* p = dynamic_cast<Panda*>(&mob)) {
                    p->SetMainGene(static_cast<Panda::Gene>(
                        EnumIndex(kPandaGeneNames, tag.GetValue<std::string>("MainGene", ""))));
                    p->SetHiddenGene(static_cast<Panda::Gene>(
                        EnumIndex(kPandaGeneNames, tag.GetValue<std::string>("HiddenGene", ""))));
                }
                break;
            case EntityTypeId::Rabbit:
                if (auto* r = dynamic_cast<Rabbit*>(&mob)) {
                    r->SetMoreCarrotTicks(tag.GetValue<int32_t>("MoreCarrotTicks", 0));
                }
                break;
            case EntityTypeId::Ocelot:
                if (auto* o = dynamic_cast<Ocelot*>(&mob)) {
                    // Through the setter, not the field: it reassesses the
                    // avoid-players goal, which is the whole point of trusting.
                    o->SetTrusting(tag.GetValue<int8_t>("Trusting", 0) != 0);
                }
                break;
            case EntityTypeId::Cat:
                if (auto* c = dynamic_cast<Cat*>(&mob)) {
                    if (tag.HasTag("variant")) {
                        c->SetVariantByte(static_cast<uint8_t>(
                            EnumIndex(kCatVariantNames,
                                      tag.GetValue<std::string>("variant", ""))));
                    }
                }
                break;
            case EntityTypeId::Turtle:
                if (auto* t = dynamic_cast<Turtle*>(&mob)) {
                    if (auto arr = As<::World::NBTTagIntArray>(tag.GetTag("home_pos"));
                        arr && arr->value.size() == 3) {
                        t->SetHomePos(glm::ivec3(arr->value[0], arr->value[1], arr->value[2]));
                    }
                    t->SetHasEgg(tag.GetValue<int8_t>("has_egg", 0) != 0);
                }
                break;
            case EntityTypeId::Horse:
            case EntityTypeId::Donkey:
            case EntityTypeId::Mule:
            case EntityTypeId::SkeletonHorse:
            case EntityTypeId::ZombieHorse:
                if (auto* h = dynamic_cast<AbstractHorse*>(&mob)) {
                    h->SetEating    (tag.GetValue<int8_t>("EatingHaystack", 0) != 0);
                    h->SetTemper    (tag.GetValue<int32_t>("Temper", 0));
                    h->SetTamedHorse(tag.GetValue<int8_t>("Tame", 0) != 0);
                }
                break;
            case EntityTypeId::Bat:
                if (auto* b = dynamic_cast<Bat*>(&mob)) {
                    b->SetResting((tag.GetValue<int8_t>("BatFlags", 0) & 1) != 0);
                }
                break;
            case EntityTypeId::Bee:
                if (auto* b = dynamic_cast<Bee*>(&mob)) {
                    b->SetHasStung (tag.GetValue<int8_t>("HasStung", 0) != 0);
                    b->SetHasNectar(tag.GetValue<int8_t>("HasNectar", 0) != 0);
                    b->SetTicksWithoutNectar(
                        tag.GetValue<int32_t>("TicksSincePollination", 0));
                    b->SetCropsGrownSincePollination(
                        tag.GetValue<int32_t>("CropsGrownSincePollination", 0));
                    if (auto arr = As<::World::NBTTagIntArray>(tag.GetTag("flower_pos"));
                        arr && arr->value.size() == 3) {
                        b->SetSavedFlowerPos(
                            glm::ivec3(arr->value[0], arr->value[1], arr->value[2]));
                    }
                }
                break;
            case EntityTypeId::Dolphin:
                if (auto* d = dynamic_cast<Dolphin*>(&mob)) {
                    d->SetMoistness(tag.GetValue<int32_t>("Moistness", 2400));
                }
                break;
            case EntityTypeId::Pufferfish:
                if (auto* p = dynamic_cast<Pufferfish*>(&mob)) {
                    p->SetPuffState(tag.GetValue<int32_t>("PuffState", 0));
                }
                break;
            case EntityTypeId::Camel:
                if (auto* c = dynamic_cast<Camel*>(&mob)) {
                    c->SetLastPoseChangeTick(tag.GetValue<int64_t>("LastPoseTick", 0));
                }
                break;
            case EntityTypeId::Axolotl:
                if (auto* a = dynamic_cast<Axolotl*>(&mob)) {
                    const int32_t v = tag.GetValue<int32_t>("Variant", 0);
                    a->SetVariant(static_cast<Axolotl::Variant>(
                        (v >= 0 && v < 5) ? v : 0));
                }
                break;
            case EntityTypeId::Armadillo:
                if (auto* a = dynamic_cast<Armadillo*>(&mob)) {
                    a->SwitchToState(static_cast<Armadillo::State>(
                        EnumIndex(kArmadilloStateNames,
                                  tag.GetValue<std::string>("state", ""))));
                }
                break;
            case EntityTypeId::Creeper:
                // Vanilla's read is one-way — it calls ignite() only for a
                // true value and has no way to un-ignite. Matching it means
                // never touching the flag for a saved false.
                if (auto* c = dynamic_cast<Creeper*>(&mob)) {
                    if (tag.GetValue<int8_t>("ignited", 0) != 0) c->Ignite();
                }
                break;
            case EntityTypeId::Enderman:
                if (auto* e = dynamic_cast<Enderman*>(&mob)) {
                    if (auto st = As<CT>(tag.GetTag("carriedBlockState"))) {
                        const std::string name = st->GetValue<std::string>("Name", "");
                        // FromSlug takes a BARE slug and returns a state; the
                        // enderman only carries the block identity.
                        e->SetCarriedBlock(
                            BlockStates::FromSlug(StripNamespace(name)).Block());
                    }
                }
                break;
            case EntityTypeId::Shulker:
                if (auto* sh = dynamic_cast<Shulker*>(&mob)) {
                    sh->SetAttachFace(tag.GetValue<int8_t>("AttachFace", 0));
                    // Through SetRawPeekAmount, which also refreshes the
                    // closed-lid armour modifier a raw assignment would skip.
                    sh->SetRawPeekAmount(tag.GetValue<int8_t>("Peek", 0));
                }
                break;
            case EntityTypeId::Endermite:
                if (auto* em = dynamic_cast<Endermite*>(&mob)) {
                    em->SetLife(tag.GetValue<int32_t>("Lifetime", 0));
                }
                break;
            case EntityTypeId::Vex:
                if (auto* v = dynamic_cast<Vex*>(&mob)) {
                    if (auto arr = As<::World::NBTTagIntArray>(tag.GetTag("bound_pos"));
                        arr && arr->value.size() == 3) {
                        v->SetBoundOrigin(
                            glm::ivec3(arr->value[0], arr->value[1], arr->value[2]));
                    }
                    if (tag.HasTag("life_ticks")) {
                        v->SetLimitedLife(tag.GetValue<int32_t>("life_ticks", 0));
                    }
                }
                break;
            case EntityTypeId::Ravager:
                if (auto* r = dynamic_cast<Ravager*>(&mob)) {
                    r->SetAttackTick (tag.GetValue<int32_t>("AttackTick", 0));
                    r->SetStunnedTick(tag.GetValue<int32_t>("StunTick", 0));
                    r->SetRoarTick   (tag.GetValue<int32_t>("RoarTick", 0));
                }
                break;
            case EntityTypeId::Ghast:
                if (auto* g = dynamic_cast<Ghast*>(&mob)) {
                    g->SetExplosionPower(tag.GetValue<int8_t>("ExplosionPower", 1));
                }
                break;
            case EntityTypeId::Evoker:
            case EntityTypeId::Illusioner:
                if (auto* sc = dynamic_cast<SpellcasterIllager*>(&mob)) {
                    sc->SetSpellCastingTime(tag.GetValue<int32_t>("SpellTicks", 0));
                }
                break;
            case EntityTypeId::Wither:
                if (auto* wi = dynamic_cast<Wither*>(&mob)) {
                    wi->SetInvulnerableTicks(tag.GetValue<int32_t>("Invul", 0));
                }
                break;
            case EntityTypeId::EnderDragon:
                // Vanilla's read is optional with NO default — an absent key
                // leaves the phase the constructor chose.
                if (auto* d = dynamic_cast<EnderDragon*>(&mob)) {
                    if (tag.HasTag("DragonPhase")) {
                        d->SetPhase(static_cast<DragonPhase>(
                            tag.GetValue<int32_t>("DragonPhase", 0)));
                    }
                    d->deathTime = tag.GetValue<int32_t>("DragonDeathTime", 0);
                }
                break;
            case EntityTypeId::EndCrystal:
                if (auto* c = dynamic_cast<EndCrystal*>(&mob)) {
                    if (auto arr = As<::World::NBTTagIntArray>(tag.GetTag("beam_target"));
                        arr && arr->value.size() == 3) {
                        c->SetBeamTarget(glm::ivec3(arr->value[0], arr->value[1],
                                                    arr->value[2]));
                    }
                    c->SetShowBottom(tag.GetValue<int8_t>("ShowBottom", 1) != 0);
                }
                break;
            case EntityTypeId::Piglin:
                if (auto* p = dynamic_cast<Piglin*>(&mob)) {
                    p->SetBaby      (tag.GetValue<int8_t>("IsBaby", 0) != 0);
                    p->SetCannotHunt(tag.GetValue<int8_t>("CannotHunt", 0) != 0);
                    p->SetImmuneToZombification(
                        tag.GetValue<int8_t>("IsImmuneToZombification", 0) != 0);
                    p->SetTimeInOverworld(tag.GetValue<int32_t>("TimeInOverworld", 0));
                    // AbstractPiglin overrides Mob's default to TRUE, so the
                    // Mob layer's read (which defaulted to false) is redone.
                    p->SetCanPickUpLoot(tag.GetValue<int8_t>("CanPickUpLoot", 1) != 0);
                }
                break;
            case EntityTypeId::PiglinBrute:
                if (auto* p = dynamic_cast<PiglinBrute*>(&mob)) {
                    p->SetImmuneToZombification(
                        tag.GetValue<int8_t>("IsImmuneToZombification", 0) != 0);
                    p->SetTimeInOverworld(tag.GetValue<int32_t>("TimeInOverworld", 0));
                    p->SetCanPickUpLoot(tag.GetValue<int8_t>("CanPickUpLoot", 1) != 0);
                }
                break;
            case EntityTypeId::Zoglin:
                if (auto* z = dynamic_cast<Zoglin*>(&mob)) {
                    z->SetBaby(tag.GetValue<int8_t>("IsBaby", 0) != 0);
                }
                break;
            case EntityTypeId::ZombifiedPiglin:
                if (auto* z = dynamic_cast<Zombie*>(&mob)) {
                    z->SetBaby(tag.GetValue<int8_t>("IsBaby", 0) != 0);
                    z->SetCanBreakDoors(tag.GetValue<int8_t>("CanBreakDoors", 0) != 0);
                    z->SetInWaterTime(tag.GetValue<int32_t>("InWaterTime", 0));
                    z->SetDrownedConversionTime(
                        tag.GetValue<int32_t>("DrownedConversionTime", -1));
                }
                break;
            case EntityTypeId::Trident:
                if (auto* t = dynamic_cast<ThrownTrident*>(&mob)) {
                    t->SetDealtDamage(tag.GetValue<int8_t>("DealtDamage", 0) != 0);
                }
                break;
            case EntityTypeId::Fireball:
                if (auto* f = dynamic_cast<LargeFireball*>(&mob)) {
                    f->SetExplosionPower(tag.GetValue<int8_t>("ExplosionPower", 1));
                }
                break;
            case EntityTypeId::WitherSkull:
                if (auto* k = dynamic_cast<WitherSkull*>(&mob)) {
                    k->SetDangerous(tag.GetValue<int8_t>("dangerous", 0) != 0);
                }
                break;
            case EntityTypeId::ShulkerBullet:
                if (auto* b = dynamic_cast<ShulkerBullet*>(&mob)) {
                    b->SetMoveDirection(tag.HasTag("Dir")
                                            ? tag.GetValue<int8_t>("Dir", -1)
                                            : -1);
                    b->SetFlightSteps(tag.GetValue<int32_t>("Steps", 0));
                    b->SetTargetDelta(glm::dvec3(tag.GetValue<double>("TXD", 0.0),
                                                 tag.GetValue<double>("TYD", 0.0),
                                                 tag.GetValue<double>("TZD", 0.0)));
                }
                break;
            case EntityTypeId::EyeOfEnder:
                if (auto* e = dynamic_cast<EyeOfEnder*>(&mob)) {
                    if (auto item = As<CT>(tag.GetTag("Item"))) {
                        e->SetItem(ReadItemStack(*item));
                    }
                    e->SetLife(tag.GetValue<int32_t>("obey_life", 0));
                    e->SetSurvivesAfterDeath(tag.GetValue<int8_t>("obey_survives", 0) != 0);
                }
                break;
            case EntityTypeId::FallingBlock:
                if (auto* fb = dynamic_cast<FallingBlockEntity*>(&mob)) {
                    BlockState carried{};
                    if (ReadBlockStateCompound(tag, "BlockState", carried)) {
                        fb->SetCarriedState(carried);
                    }
                    fb->SetTime(tag.GetValue<int32_t>("Time", 0));
                    fb->SetDropsItem(tag.GetValue<int8_t>("DropItem", 1) != 0);
                    fb->SetCancelDrop(tag.GetValue<int8_t>("CancelDrop", 0) != 0);
                    // MC's default for HurtEntities is `blockState.is(#anvil)`,
                    // NOT false — an anvil saved before this field existed must
                    // still bite when it lands.
                    const bool defaultHurts = IsAnvil(fb->CarriedState().Block());
                    fb->SetHurtsEntitiesRaw(
                        tag.GetValue<int8_t>("HurtEntities", defaultHurts ? 1 : 0) != 0,
                        tag.GetValue<float>("FallHurtAmount", 0.0f),
                        tag.GetValue<int32_t>("FallHurtMax",
                                              FallingBlockEntity::kDefaultFallHurtMax));
                }
                break;
            case EntityTypeId::Tnt:
                if (auto* tnt = dynamic_cast<PrimedTnt*>(&mob)) {
                    tnt->SetFuse(tag.GetValue<int16_t>(
                        "fuse", static_cast<int16_t>(PrimedTnt::kDefaultFuse)));
                    BlockState carried{};
                    if (ReadBlockStateCompound(tag, "block_state", carried)) {
                        tnt->SetCarriedState(carried);
                    }
                    // MC clamps on load — a hand-edited save asking for a
                    // radius whose 16^3 ray march would eat the tick is capped.
                    tnt->SetExplosionPower(
                        tag.GetValue<float>("explosion_power", PrimedTnt::kDefaultPower));
                    Uuid owner{};
                    if (ReadUuid(tag, "owner", owner)) tnt->SetOwnerUuid(owner);
                }
                break;
            case EntityTypeId::EvokerFangs:
                if (auto* f = dynamic_cast<EvokerFangs*>(&mob)) {
                    f->SetWarmupDelay(tag.GetValue<int32_t>("Warmup", 0));
                }
                break;
            case EntityTypeId::AreaEffectCloud:
                if (auto* c = dynamic_cast<AreaEffectCloud*>(&mob)) {
                    // SetRadiusRaw, not SetRadius: the latter resizes the box
                    // and feeds the shrink-to-nothing discard, which would
                    // delete a cloud the moment its save is applied.
                    c->SetRadiusRaw(tag.GetValue<float>("Radius", 3.0f));
                    c->SetDuration(tag.GetValue<int32_t>("Duration", -1));
                    c->SetWaitTime(tag.GetValue<int32_t>("WaitTime", 20));
                    c->SetReapplicationDelay(tag.GetValue<int32_t>("ReapplicationDelay", 20));
                    c->SetDurationOnUse(tag.GetValue<int32_t>("DurationOnUse", 0));
                    c->SetRadiusOnUse(tag.GetValue<float>("RadiusOnUse", 0.0f));
                    c->SetRadiusPerTick(tag.GetValue<float>("RadiusPerTick", 0.0f));
                    c->SetPotionDurationScale(tag.GetValue<float>("potion_duration_scale", 1.0f));
                }
                break;
            default:
                break;
        }
    }

    // ── item entities and orbs ──────────────────────────────────────────────

    bool WriteItem(Nbt::Writer& w, Nbt::Writer::ListScope& list, const ItemEntity& item) {
        if (item.stack.IsEmpty()) return false;   // vanilla discards these on load

        w.ListCompoundBegin(list);
        w.String("id", "minecraft:item");
        WriteDoubleList(w, "Pos",    item.pos);
        WriteDoubleList(w, "Motion", item.vel);
        {
            auto rot = w.BeginList("Rotation", Nbt::TagType::Float);
            w.ListFloat(rot, 0.0f);
            w.ListFloat(rot, 0.0f);
            w.EndList(rot);
        }
        w.Double("fall_distance", 0.0);
        w.Short ("Fire", -20);
        w.Short ("Air",  300);
        w.Bool  ("OnGround", item.onGround);
        w.Bool  ("Invulnerable", false);
        w.Int   ("PortalCooldown", 0);
        WriteUuid(w, "UUID", item.uuid);

        w.Short("Age",         static_cast<int16_t>(item.age));
        w.Short("PickupDelay", static_cast<int16_t>(item.pickupDelay));
        w.Short("Health",      5);
        w.BeginCompound("Item");
        WriteItemStackBody(w, item.stack);
        w.EndCompound();

        w.ListCompoundEnd(list);
        return true;
    }

    bool ReadItem(const ::World::NBTTagCompound& tag, ItemEntity& out) {
        auto stackTag = As<CT>(tag.GetTag("Item"));
        if (!stackTag) return false;
        out.stack = ReadItemStack(*stackTag);
        if (out.stack.IsEmpty()) return false;    // matches vanilla's discard

        glm::dvec3 v{};
        if (ReadDoubleList(tag, "Pos", v))    out.pos = v;
        if (ReadDoubleList(tag, "Motion", v)) out.vel = v;
        out.onGround    = tag.GetValue<int8_t>("OnGround", 0) != 0;
        out.age         = tag.GetValue<int16_t>("Age", 0);
        out.pickupDelay = tag.GetValue<int16_t>("PickupDelay", 0);
        ReadUuid(tag, "UUID", out.uuid);
        return true;
    }

    bool WriteOrb(Nbt::Writer& w, Nbt::Writer::ListScope& list, const ExperienceOrb& orb) {
        if (orb.value <= 0 || orb.count <= 0) return false;

        w.ListCompoundBegin(list);
        w.String("id", "minecraft:experience_orb");
        WriteDoubleList(w, "Pos",    orb.pos);
        WriteDoubleList(w, "Motion", orb.vel);
        {
            auto rot = w.BeginList("Rotation", Nbt::TagType::Float);
            w.ListFloat(rot, 0.0f);
            w.ListFloat(rot, 0.0f);
            w.EndList(rot);
        }
        w.Double("fall_distance", 0.0);
        w.Short ("Fire", -20);
        w.Short ("Air",  300);
        w.Bool  ("OnGround", orb.onGround);
        w.Bool  ("Invulnerable", false);
        w.Int   ("PortalCooldown", 0);
        WriteUuid(w, "UUID", orb.uuid);

        w.Short("Value",  static_cast<int16_t>(orb.value));
        w.Int  ("Count",  orb.count);
        w.Short("Age",    static_cast<int16_t>(orb.age));
        w.Short("Health", 5);

        w.ListCompoundEnd(list);
        return true;
    }

    bool ReadOrb(const ::World::NBTTagCompound& tag, ExperienceOrb& out) {
        out.value = tag.GetValue<int16_t>("Value", 0);
        if (out.value <= 0) return false;
        out.count = tag.GetValue<int32_t>("Count", 1);
        if (out.count <= 0) out.count = 1;

        glm::dvec3 v{};
        if (ReadDoubleList(tag, "Pos", v))    out.pos = v;
        if (ReadDoubleList(tag, "Motion", v)) out.vel = v;
        out.onGround = tag.GetValue<int8_t>("OnGround", 0) != 0;
        out.age      = tag.GetValue<int16_t>("Age", 0);
        ReadUuid(tag, "UUID", out.uuid);
        return true;
    }

} // namespace Game::Anvil
