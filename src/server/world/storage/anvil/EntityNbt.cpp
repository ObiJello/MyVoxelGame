// File: src/server/world/storage/anvil/EntityNbt.cpp
#include "server/world/storage/anvil/EntityNbt.hpp"
#include "common/entity/mobs/FarmSoundVariants.hpp"
#include "common/entity/MountInventory.hpp"
#include "common/entity/FallingBlockEntity.hpp"
#include "common/entity/PrimedTnt.hpp"
#include "common/entity/EndCrystal.hpp"
#include "common/entity/ArmorStand.hpp"
#include "common/entity/decoration/Painting.hpp"
#include "common/entity/decoration/ItemFrame.hpp"
#include "common/entity/OminousItemSpawner.hpp"
#include "common/entity/decoration/Cushion.hpp"
#include "common/entity/vehicle/Boat.hpp"
#include "common/entity/vehicle/Minecart.hpp"
#include "common/world/block/entity/SpawnerBlockEntity.hpp"
#include "server/world/storage/anvil/SpawnerNbt.hpp"
#include "common/entity/decoration/PaintingVariants.hpp"
#include "common/world/block/FallingBlock.hpp"

#include "server/world/storage/anvil/ItemStackNbt.hpp"
#include "server/world/storage/anvil/VillagerNbt.hpp"
#include "common/entity/npc/Villager.hpp"
#include "common/entity/npc/WanderingTrader.hpp"

#include "common/core/Log.hpp"
#include "common/entity/Animal.hpp"
#include "common/entity/Attributes.hpp"
#include "common/network/packets/game/UpdateAttributesS2CPacket.hpp"   // IsClientSyncableAttribute
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
#include "server/world/storage/anvil/VibrationNbt.hpp"
#include "common/entity/ai/brain/Brain.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/mobs/Fish.hpp"
#include "common/entity/mobs/GenericMobs.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/mobs/Pillager.hpp"
#include "common/entity/raid/Raider.hpp"
#include "common/entity/mobs/Slime.hpp"
#include "common/entity/mobs/SulfurCube.hpp"

#include <algorithm>
#include "common/entity/projectile/AreaEffectCloud.hpp"
#include "common/entity/projectile/Arrow.hpp"
#include "common/entity/projectile/EvokerFangs.hpp"
#include "common/entity/projectile/EyeOfEnder.hpp"
#include "common/entity/projectile/FireworkRocket.hpp"
#include "common/entity/projectile/HurtingProjectile.hpp"
#include "common/entity/projectile/Projectile.hpp"
#include "common/entity/projectile/ShulkerBullet.hpp"
#include "common/entity/projectile/ThrownTrident.hpp"
#include "common/entity/projectile/ThrowableProjectile.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/ModMobNbt.hpp"

#include <optional>
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

        // MC NumericTag.doubleValue: any numeric element reads (a hand-typed
        // /summon {Motion:[0,1,0]} is a list of ints); anything else is 0.
        double NumberOf(const ::World::NBTTagPtr& t) {
            if (!t) return 0.0;
            switch (t->type) {
                case ::World::NBTTagType::TAG_Byte:   return static_cast<::World::NBTTagByte&>(*t).value;
                case ::World::NBTTagType::TAG_Short:  return static_cast<::World::NBTTagShort&>(*t).value;
                case ::World::NBTTagType::TAG_Int:    return static_cast<::World::NBTTagInt&>(*t).value;
                case ::World::NBTTagType::TAG_Long:   return static_cast<double>(static_cast<::World::NBTTagLong&>(*t).value);
                case ::World::NBTTagType::TAG_Float:  return static_cast<::World::NBTTagFloat&>(*t).value;
                case ::World::NBTTagType::TAG_Double: return static_cast<::World::NBTTagDouble&>(*t).value;
                default:                              return 0.0;
            }
        }

        bool ReadDoubleList(const CT& tag, const char* key, glm::dvec3& out) {
            auto list = As<LT>(tag.GetTag(key));
            if (!list || list->value.size() != 3) return false;
            for (int i = 0; i < 3; ++i) out[i] = NumberOf(list->value[i]);
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
            // MC Entity.saveWithoutId: "Silent" only when set.
            if (e.IsSilent()) w.Bool("Silent", true);
            // MC Entity.saveWithoutId: "CustomName" (a text component — a
            // bare string for plain text) and "CustomNameVisible", only when
            // there is a name / the flag is set.
            if (const auto& name = e.GetCustomName()) {
                WriteTextComponent(w, "CustomName", Text::Component::Literal(*name));
            }
            if (e.IsCustomNameVisible()) w.Bool("CustomNameVisible", true);
            WriteEntityTags(w, e.Tags());
        }

        void ReadEntityBase(const CT& tag, Entity& e) {
            glm::dvec3 v{};
            if (ReadDoubleList(tag, "Pos", v))    e.position = v;
            if (ReadDoubleList(tag, "Motion", v)) e.velocity = v;
            if (auto rot = As<LT>(tag.GetTag("Rotation")); rot && rot->value.size() == 2) {
                auto f = [&](int i) { return static_cast<float>(NumberOf(rot->value[i])); };
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
            e.SetSilent(tag.GetValue<int8_t>("Silent", 0) != 0);
            // MC Entity.load: CustomName through ComponentSerialization (a
            // string or the compound form); the engine keeps its plain text.
            if (const auto nameTag = tag.GetTag("CustomName")) {
                if (auto component = ReadTextComponent(*nameTag)) {
                    std::string name = Text::GetString(*component);
                    if (!name.empty()) e.SetCustomName(std::move(name));
                }
            }
            e.SetCustomNameVisible(tag.GetValue<int8_t>("CustomNameVisible", 0) != 0);
            ReadEntityTags(tag, e.Tags());
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
            // MC Details.show_icon — its own field (defaults to
            // show_particles when absent, Details::create).
            w.Bool  ("show_icon", e.showIcon);
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
            // Details::create: show_icon.orElse(showParticles).
            out.showIcon  = tag.HasTag("show_icon")
                ? tag.GetValue<int8_t>("show_icon", 1) != 0 : out.visible;

            if (auto hidden = As<CT>(tag.GetTag("hidden_effect"))) {
                auto nested = std::make_unique<MobEffectInstance>();
                if (ReadEffectBody(*hidden, *nested)) out.hiddenEffect = std::move(nested);
            }
            return true;
        }

        // ── attributes ──────────────────────────────────────────────────────
        //
        // AttributeInstance.Packed: {id, base, modifiers?}. BASE values, plus
        // only the permanent modifiers nothing rebuilds (kPersistentModifiers
        // below: finalizeSpawn's random rolls, the leader zombie's bonus).
        // Every other modifier is transient and rebuilt by the thing that owns
        // it — SetBaby re-derives the baby speed bonus, RestoreEffects
        // re-applies each potion's, taming re-applies the wolf's health bump,
        // Mob::SetEquipment the worn gear's. Writing those would double them
        // on the next load.
        //
        // EVERY registered attribute is written, including one sitting at the
        // registry default. That is vanilla's AttributeMap.pack(), and it is
        // not an oversight to copy: apply() only touches attributes the file
        // names, so skipping a default-valued row would restore whatever the
        // CONSTRUCTOR chose instead of what was saved. A zombie whose speed
        // was raised to the registry default would come back at 0.23.
        // The PERMANENT modifiers — the ones nothing rebuilds on load, so
        // MC's AttributeInstance.pack writes them ("modifiers": [{id, amount,
        // operation}]): finalizeSpawn's random rolls and the zombie family's
        // leader / reinforcement charges. Everything else stays transient
        // (see above). MC shares one id across attributes (the leader bonus
        // sits on both health and reinforcements); the engine keys them
        // apart, so the table maps (attribute, engine id) <-> MC id.
        struct PersistentModifierName {
            ModifierId  id;
            Attribute   attribute;
            const char* mcId;
        };
        constexpr PersistentModifierName kPersistentModifiers[] = {
            { ModifierId::RandomSpawnBonus,        Attribute::FollowRange,         "minecraft:random_spawn_bonus" },
            { ModifierId::RandomSpawnBonus,        Attribute::KnockbackResistance, "minecraft:random_spawn_bonus" },
            { ModifierId::ZombieRandomKnockback,   Attribute::FollowRange,         "minecraft:zombie_random_spawn_bonus" },
            { ModifierId::ZombieLeaderHealth,      Attribute::MaxHealth,           "minecraft:leader_zombie_bonus" },
            { ModifierId::ZombieLeaderReinf,       Attribute::SpawnReinforcements, "minecraft:leader_zombie_bonus" },
            { ModifierId::ZombieSpawnReinf,        Attribute::SpawnReinforcements, "minecraft:reinforcement_caller_charge" },
            { ModifierId::ZombieReinfCalleeCharge, Attribute::SpawnReinforcements, "minecraft:reinforcement_callee_charge" },
        };

        const char* PersistentModifierMcId(Attribute attribute, uint32_t id) {
            for (const auto& row : kPersistentModifiers) {
                if (row.attribute == attribute && static_cast<uint32_t>(row.id) == id) return row.mcId;
            }
            return nullptr;
        }

        bool PersistentModifierFromMcId(Attribute attribute, std::string_view mcId, ModifierId& out) {
            const std::string_view bare = StripNamespace(mcId);
            for (const auto& row : kPersistentModifiers) {
                if (row.attribute == attribute && StripNamespace(row.mcId) == bare) {
                    out = row.id;
                    return true;
                }
            }
            return false;
        }

        // MC AttributeModifier.Operation serialized names.
        const char* OperationName(AttributeOperation op) {
            switch (op) {
                case AttributeOperation::AddValue:           return "add_value";
                case AttributeOperation::AddMultipliedBase:  return "add_multiplied_base";
                case AttributeOperation::AddMultipliedTotal: return "add_multiplied_total";
            }
            return "add_value";
        }

        AttributeOperation OperationFromName(std::string_view name) {
            if (name == "add_multiplied_base")  return AttributeOperation::AddMultipliedBase;
            if (name == "add_multiplied_total") return AttributeOperation::AddMultipliedTotal;
            return AttributeOperation::AddValue;
        }

        void WriteAttributes(Nbt::Writer& w, const LivingEntity& l) {
            WriteAttributeList(w, l.Attributes());
        }

        void ReadAttributes(const CT& tag, LivingEntity& l) {
            bool customized = false;
            ReadAttributeList(tag, l.Attributes(), /*onlyRegistered=*/true, &customized);
            if (customized) l.MarkAttributesCustomized();
            l.RefreshAttributeScale();
        }

        // ── Allay (MC Allay.addAdditionalSaveData / its Brain memories) ─────

        // MC's four allay memories with a codec (MemoryModuleType.register(
        // id, codec)): liked_player (UUID), liked_noteblock (GlobalPos),
        // liked_noteblock_cooldown_ticks and item_pickup_cooldown_ticks
        // (Codec.INT). None is set with an expiry, so no "ttl".
        void WriteAllayBrain(Nbt::Writer& w, const Allay& allay) {
            w.BeginCompound("Brain");
            w.BeginCompound("memories");
            if (const auto& liked = allay.GetLikedPlayerUuid()) {
                w.BeginCompound("minecraft:liked_player");
                WriteUuid(w, "value", *liked);
                w.EndCompound();
            }
            if (const Brain* brain = allay.GetBrain()) {
                if (const auto pos = brain->GetBlockPos(MemoryModule::LikedNoteblockPosition)) {
                    const EntityLevel* level = allay.Level();
                    const DimensionId dim = level ? level->Dimension() : DimensionId::Overworld;
                    w.BeginCompound("minecraft:liked_noteblock");
                    w.BeginCompound("value");
                    const int32_t p[3] = { pos->x, pos->y, pos->z };
                    w.IntArray("pos", p, 3);
                    w.String("dimension", DimensionRegistryName(dim));
                    w.EndCompound();
                    w.EndCompound();
                }
                const auto writeInt = [&](MemoryModule m, const char* id) {
                    if (const auto v = brain->GetInt(m)) {
                        w.BeginCompound(id);
                        w.Int("value", *v);
                        w.EndCompound();
                    }
                };
                writeInt(MemoryModule::LikedNoteblockCooldownTicks, "minecraft:liked_noteblock_cooldown_ticks");
                writeInt(MemoryModule::ItemPickupCooldownTicks,     "minecraft:item_pickup_cooldown_ticks");
            }
            w.EndCompound();
            w.EndCompound();
        }

        void ReadAllayBrain(const CT& tag, Allay& allay) {
            auto brainTag = As<CT>(tag.GetTag("Brain"));
            auto memories = brainTag ? As<CT>(brainTag->GetTag("memories")) : nullptr;
            if (!memories) return;
            if (auto liked = As<CT>(memories->GetTag("minecraft:liked_player"))) {
                Uuid uuid{};
                if (ReadUuid(*liked, "value", uuid)) allay.SetLikedPlayerUuid(uuid);
            }
            Brain* brain = allay.GetBrain();
            if (!brain) return;
            if (auto entry = As<CT>(memories->GetTag("minecraft:liked_noteblock"))) {
                auto value = As<CT>(entry->GetTag("value"));
                auto pos = value ? As<::World::NBTTagIntArray>(value->GetTag("pos")) : nullptr;
                const EntityLevel* level = allay.Level();
                const DimensionId here = level ? level->Dimension() : DimensionId::Overworld;
                const auto dim = value ? DimensionFromRegistryName(value->GetValue<std::string>("dimension", ""))
                                       : std::nullopt;
                // A noteblock in another dimension can never be deposited
                // at (GlobalPos.isCloseEnough fails): MC's getter drops it.
                if (pos && pos->value.size() == 3 && (!dim || *dim == here)) {
                    brain->SetMemory(MemoryModule::LikedNoteblockPosition,
                                     glm::ivec3(pos->value[0], pos->value[1], pos->value[2]));
                }
            }
            const auto readInt = [&](MemoryModule m, const char* id) {
                if (auto entry = As<CT>(memories->GetTag(id))) {
                    if (entry->HasTag("value")) {
                        brain->SetMemory(m, static_cast<int>(NumberOf(entry->GetTag("value"))));
                    }
                }
            };
            readInt(MemoryModule::LikedNoteblockCooldownTicks, "minecraft:liked_noteblock_cooldown_ticks");
            readInt(MemoryModule::ItemPickupCooldownTicks,     "minecraft:item_pickup_cooldown_ticks");
        }

        // MC Allay.addAdditionalSaveData: InventoryCarrier's "Inventory"
        // (SimpleContainer.storeAsItemList — empties skipped), the vibration
        // listener's Data ("listener": event, selector, event_delay), and
        // "DuplicationCooldown", and the held item as Mob's EntityEquipment
        // ("equipment": {mainhand}).
        // MC InventoryCarrier.writeInventoryToTag / readInventoryFromTag —
        // "Inventory", SimpleContainer.storeAsItemList (empties skipped) /
        // fromItemList (filled in order). The piglin's and pillager's
        // pockets.
        void WriteCarrierInventory(Nbt::Writer& w, const SimpleContainer& inv) {
            auto list = w.BeginList("Inventory", Nbt::TagType::Compound);
            for (int i = 0; i < inv.GetContainerSize(); ++i) {
                const ItemStack& stack = inv.GetItem(i);
                if (stack.IsEmpty()) continue;
                w.ListCompoundBegin(list);
                WriteItemStackBody(w, stack);
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }

        void ReadCarrierInventory(const CT& tag, SimpleContainer& inv) {
            for (int i = 0; i < inv.GetContainerSize(); ++i) inv.SetItem(i, ItemStack{});
            auto list = As<LT>(tag.GetTag("Inventory"));
            if (!list) return;
            int slot = 0;
            for (const auto& elem : list->value) {
                if (slot >= inv.GetContainerSize()) break;
                auto c = As<CT>(elem);
                if (!c) continue;
                const ItemStack stack = ReadItemStack(*c);
                if (stack.IsEmpty()) continue;
                inv.SetItem(slot++, stack);
            }
        }

        void WriteAllayData(Nbt::Writer& w, const Allay& allay) {
            auto list = w.BeginList("Inventory", Nbt::TagType::Compound);
            const SimpleContainer& inv = allay.GetInventory();
            for (int i = 0; i < inv.GetContainerSize(); ++i) {
                const ItemStack& stack = inv.GetItem(i);
                if (stack.IsEmpty()) continue;
                w.ListCompoundBegin(list);
                WriteItemStackBody(w, stack);
                w.ListCompoundEnd(list);
            }
            w.EndList(list);

            WriteVibrationData(w, "listener", allay.SavedVibrationData());

            w.Long("DuplicationCooldown", allay.GetDuplicationCooldown());

            if (!allay.GetMainHandItem().IsEmpty()) {
                w.BeginCompound("equipment");
                w.BeginCompound("mainhand");
                WriteItemStackBody(w, allay.GetMainHandItem());
                w.EndCompound();
                w.EndCompound();
            }
        }

        void ReadAllayData(const CT& tag, Allay& allay) {
            SimpleContainer& inv = allay.GetInventory();
            for (int i = 0; i < inv.GetContainerSize(); ++i) inv.SetItem(i, ItemStack{});
            if (auto list = As<LT>(tag.GetTag("Inventory"))) {
                int slot = 0;
                for (const auto& elem : list->value) {
                    if (slot >= inv.GetContainerSize()) break;
                    auto c = As<CT>(elem);
                    if (!c) continue;
                    const ItemStack stack = ReadItemStack(*c);
                    if (stack.IsEmpty()) continue;
                    inv.SetItem(slot++, stack);
                }
            }

            allay.SetVibrationData(ReadVibrationData(tag, "listener"));

            // MC reads getIntOr("DuplicationCooldown", 0) of a Long tag.
            allay.SetDuplicationCooldown(static_cast<int64_t>(NumberOf(tag.GetTag("DuplicationCooldown"))));

            if (auto equipment = As<CT>(tag.GetTag("equipment"))) {
                if (auto main = As<CT>(equipment->GetTag("mainhand"))) {
                    allay.SetMainHandItem(ReadItemStack(*main));
                }
            }
            ReadAllayBrain(tag, allay);
        }

        void WriteLiving(Nbt::Writer& w, const LivingEntity& l) {
            w.Float("Health",    l.GetHealth());
            w.Short("HurtTime",  static_cast<int16_t>(l.hurtTime));
            w.Short("DeathTime", static_cast<int16_t>(l.deathTime));
            w.Int  ("HurtByTimestamp", static_cast<int32_t>(l.GetLastHurtByMobTimestamp()));
            w.Float("AbsorptionAmount", l.GetAbsorptionAmount());
            // Engine extras (vanilla ignores them): the head and body yaw
            // vanilla drops on save — see ReadLiving.
            w.Float("obey_head_yaw", l.yHeadRot);
            w.Float("obey_body_yaw", l.yBodyRot);
            // The limb swing (WalkAnimationState) and the animation age, for
            // the same reason: a mob left mid-stride or mid-flap comes back
            // in that pose. Only when there is something to keep.
            if (l.walkAnimation.speed != 0.0f || l.walkAnimation.speedOld != 0.0f ||
                l.walkAnimation.position != 0.0f) {
                w.Float("obey_walk_pos",       l.walkAnimation.position);
                w.Float("obey_walk_speed",     l.walkAnimation.speed);
                w.Float("obey_walk_speed_old", l.walkAnimation.speedOld);
                w.Float("obey_walk_scale",     l.walkAnimation.positionScale);
            }
            if (l.viewAge >= 0) w.Int("obey_view_age", l.viewAge + l.tickCount);
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
            // The villager saves its claims (home, job site, bell) there.
            if (const auto* villager = dynamic_cast<const Villager*>(&l)) {
                WriteVillagerBrain(w, *villager);
            } else if (const auto* allay = dynamic_cast<const Allay*>(&l)) {
                WriteAllayBrain(w, *allay);
            } else {
                w.BeginCompound("Brain");
                w.BeginCompound("memories");
                w.EndCompound();
                w.EndCompound();
            }

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
            // Head and body yaw. MC Entity.load snaps both to the saved yRot
            // (setYHeadRot / setYBodyRot) — it saves only Rotation. Missing
            // here, a loaded mob came back with its head and body at 0
            // (facing south) whatever way it was facing, and the join
            // transition's hold showed that until the AI turned them back.
            // On top of vanilla's snap, the engine keeps the real angles
            // (obey_* extras, which vanilla ignores): a dog looking over its
            // shoulder when the world was left looks there again on rejoin,
            // as the last-world panorama shows it.
            l.yHeadRot = l.yHeadRotO = tag.GetValue<float>("obey_head_yaw", l.yRot);
            l.yBodyRot = l.yBodyRotO = tag.GetValue<float>("obey_body_yaw", l.yRot);
            if (tag.HasTag("obey_walk_pos")) {
                l.walkAnimation.position      = tag.GetValue<float>("obey_walk_pos", 0.0f);
                l.walkAnimation.speed         = tag.GetValue<float>("obey_walk_speed", 0.0f);
                l.walkAnimation.speedOld      = tag.GetValue<float>("obey_walk_speed_old", 0.0f);
                l.walkAnimation.positionScale = tag.GetValue<float>("obey_walk_scale", 1.0f);
            }
            if (tag.HasTag("obey_view_age")) {
                l.viewAge = std::max(0, tag.GetValue<int32_t>("obey_view_age", 0) - l.tickCount);
            }

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

        // The types that write their own "equipment" compound (their own
        // storage — the allay's held item, the wolf's armour, the stand's six
        // slots); the Mob tier's generic equipment stays out of their tags.
        bool HasOwnEquipmentTag(EntityTypeId type) {
            return type == EntityTypeId::Allay || type == EntityTypeId::Wolf || type == EntityTypeId::ArmorStand;
        }

        // MC EquipmentSlot.CODEC names, by ordinal — the humanoid six.
        constexpr const char* kHumanoidSlotNames[Mob::kEquipmentSlotCount] = {
            "mainhand", "offhand", "feet", "legs", "chest", "head", "body", "saddle"
        };

        // MC Mob.addAdditionalSaveData's equipment half: EntityEquipment.CODEC
        // ("equipment", one compound per non-empty slot) and DropChances.CODEC
        // ("drop_chances", only the slots off the 0.085 default).
        void WriteMobEquipment(Nbt::Writer& w, const Mob& m) {
            if (HasOwnEquipmentTag(m.GetType())) return;
            if (m.HasAnyEquipment()) {
                w.BeginCompound("equipment");
                for (int i = 0; i < Mob::kEquipmentSlotCount; ++i) {
                    const ItemStack& stack = m.GetEquipment(static_cast<EquipmentSlot>(i));
                    if (stack.IsEmpty()) continue;
                    w.BeginCompound(kHumanoidSlotNames[i]);
                    WriteItemStackBody(w, stack);
                    w.EndCompound();
                }
                w.EndCompound();
            }
            bool anyChance = false;
            for (int i = 0; i < Mob::kEquipmentSlotCount; ++i) {
                if (m.GetEquipmentDropChance(static_cast<EquipmentSlot>(i)) != Mob::kDefaultEquipmentDropChance) {
                    anyChance = true;
                }
            }
            if (anyChance) {
                w.BeginCompound("drop_chances");
                for (int i = 0; i < Mob::kEquipmentSlotCount; ++i) {
                    const float chance = m.GetEquipmentDropChance(static_cast<EquipmentSlot>(i));
                    if (chance != Mob::kDefaultEquipmentDropChance) w.Float(kHumanoidSlotNames[i], chance);
                }
                w.EndCompound();
            }
        }

        void ReadMobEquipment(const CT& tag, Mob& m) {
            if (HasOwnEquipmentTag(m.GetType())) return;
            if (auto equipment = As<CT>(tag.GetTag("equipment"))) {
                for (int i = 0; i < Mob::kEquipmentSlotCount; ++i) {
                    if (auto stack = As<CT>(equipment->GetTag(kHumanoidSlotNames[i]))) {
                        m.SetEquipment(static_cast<EquipmentSlot>(i), ReadItemStack(*stack));
                    }
                }
            } else {
                // Pre-1.21.5 saves: HandItems [main, off], ArmorItems
                // [feet, legs, chest, head].
                const auto readList = [&](const char* key, std::initializer_list<EquipmentSlot> slots) {
                    auto list = As<LT>(tag.GetTag(key));
                    if (!list) return;
                    size_t i = 0;
                    for (const EquipmentSlot slot : slots) {
                        if (i >= list->value.size()) break;
                        if (auto stack = As<CT>(list->value[i])) m.SetEquipment(slot, ReadItemStack(*stack));
                        ++i;
                    }
                };
                readList("HandItems", { EquipmentSlot::MAINHAND, EquipmentSlot::OFFHAND });
                readList("ArmorItems", { EquipmentSlot::FEET, EquipmentSlot::LEGS,
                                         EquipmentSlot::CHEST, EquipmentSlot::HEAD });
                // The older mount keys the 1.21.5 datafixer folds into
                // "equipment": body_armor_item (1.20.5+), a horse's
                // ArmorItem / SaddleItem, a llama's DecorItem, a pig's or
                // strider's Saddle flag.
                if (auto body = As<CT>(tag.GetTag("body_armor_item"))) {
                    m.SetEquipment(EquipmentSlot::BODY, ReadItemStack(*body));
                } else if (auto armor = As<CT>(tag.GetTag("ArmorItem"))) {
                    m.SetEquipment(EquipmentSlot::BODY, ReadItemStack(*armor));
                } else if (auto decor = As<CT>(tag.GetTag("DecorItem"))) {
                    m.SetEquipment(EquipmentSlot::BODY, ReadItemStack(*decor));
                }
                if (auto saddle = As<CT>(tag.GetTag("SaddleItem"))) {
                    m.SetEquipment(EquipmentSlot::SADDLE, ReadItemStack(*saddle));
                } else if (tag.HasTag("Saddle") && NumberOf(tag.GetTag("Saddle")) != 0.0) {
                    m.SetEquipment(EquipmentSlot::SADDLE, ItemStack(Items::Saddle, 1));
                }
            }
            if (auto chances = As<CT>(tag.GetTag("drop_chances"))) {
                for (int i = 0; i < Mob::kEquipmentSlotCount; ++i) {
                    if (chances->HasTag(kHumanoidSlotNames[i])) {
                        m.SetEquipmentDropChance(static_cast<EquipmentSlot>(i),
                                                 chances->GetValue<float>(kHumanoidSlotNames[i],
                                                                          Mob::kDefaultEquipmentDropChance));
                    }
                }
            } else {
                const auto readChances = [&](const char* key, std::initializer_list<EquipmentSlot> slots) {
                    auto list = As<LT>(tag.GetTag(key));
                    if (!list) return;
                    size_t i = 0;
                    for (const EquipmentSlot slot : slots) {
                        if (i >= list->value.size()) break;
                        if (auto f = As<::World::NBTTagFloat>(list->value[i])) m.SetEquipmentDropChance(slot, f->value);
                        ++i;
                    }
                };
                readChances("HandDropChances", { EquipmentSlot::MAINHAND, EquipmentSlot::OFFHAND });
                readChances("ArmorDropChances", { EquipmentSlot::FEET, EquipmentSlot::LEGS,
                                                  EquipmentSlot::CHEST, EquipmentSlot::HEAD });
            }
        }

        void WriteMobLayer(Nbt::Writer& w, const Mob& m) {
            w.Bool("CanPickUpLoot",       m.CanPickUpLoot());
            w.Bool("PersistenceRequired", m.IsPersistenceRequired());
            w.Bool("LeftHanded",          m.IsLeftHanded());
            WriteMobEquipment(w, m);
            if (m.IsNoAi()) w.Bool("NoAI", true);
            if (m.HasHome()) {
                w.Int("home_radius", m.GetHomeRadius());
                const glm::ivec3 home = m.GetHomePosition();
                const int32_t pos[3] = {home.x, home.y, home.z};
                w.IntArray("home_pos", pos, 3);
            }
            // MC Mob.addAdditionalSaveData → writeLeashData: "leash" is the
            // holder's {UUID} or, for a fence knot (never saved itself), the
            // fence's [x, y, z] — LeashData.CODEC's xor.
            if (m.IsLeashable()) {
                const Leash::HolderRef ref = m.GetLeashSaveRef();
                if (ref.knotPos) {
                    const int32_t knot[3] = {ref.knotPos->x, ref.knotPos->y, ref.knotPos->z};
                    w.IntArray("leash", knot, 3);
                } else if (ref.uuid) {
                    w.BeginCompound("leash");
                    WriteUuid(w, "UUID", *ref.uuid);
                    w.EndCompound();
                }
            }
        }

        void ReadMobLayer(const CT& tag, Mob& m) {
            m.SetCanPickUpLoot(tag.GetValue<int8_t>("CanPickUpLoot", 0) != 0);
            m.SetLeftHanded   (tag.GetValue<int8_t>("LeftHanded", 0) != 0);
            m.SetNoAi         (tag.GetValue<int8_t>("NoAI", 0) != 0);

            // Load-bearing: without it a mob that was saved because a player
            // built a farm around it distance-despawns on the tick it loads.
            m.SetPersistenceRequired(tag.GetValue<int8_t>("PersistenceRequired", 0) != 0);
            ReadMobEquipment(tag, m);

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

            // MC readLeashData: the reference waits for the next tickLeash,
            // which finds the holder (or remakes the knot) — or, 100 ticks
            // on, drops the lead.
            if (m.IsLeashable()) {
                Leash::HolderRef ref;
                const ::World::NBTTagPtr leash = tag.GetTag("leash");
                if (auto compound = As<CT>(leash)) {
                    Uuid uuid{};
                    if (ReadUuid(*compound, "UUID", uuid)) ref.uuid = uuid;
                } else if (auto arr = As<::World::NBTTagIntArray>(leash); arr && arr->value.size() == 3) {
                    ref.knotPos = glm::ivec3(arr->value[0], arr->value[1], arr->value[2]);
                }
                m.SetDelayedLeashRef(ref);
            }
        }

        // ── Ageable / animal ────────────────────────────────────────────────

        // MC AgeableMob.addAdditionalSaveData: Age, ForcedAge, AgeLocked —
        // for every AgeableMob (the dolphin and the villager included), not
        // only the animals, which is where the age used to be written.
        void WriteAgeableLayer(Nbt::Writer& w, const AgeableMob& a) {
            w.Int("Age",       a.GetAge());
            w.Int("ForcedAge", a.GetForcedAge());
            w.Bool("AgeLocked", a.IsAgeLocked());
        }

        void ReadAgeableLayer(const CT& tag, AgeableMob& a) {
            a.SetAge      (tag.GetValue<int32_t>("Age", 0));
            a.SetForcedAge(tag.GetValue<int32_t>("ForcedAge", 0));
            a.SetAgeLocked(tag.GetValue<int8_t>("AgeLocked", 0) != 0);
        }

        void WriteAnimalLayer(Nbt::Writer& w, const Animal& a) {
            w.Int("InLove",    a.GetInLoveTicks());
            WriteRef(w, "LoveCause", a.LoveCauseRef());
        }

        void ReadAnimalLayer(const CT& tag, Animal& a) {
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
            // MC TamableAnimal.readAdditionalSaveData: setInSittingPose(
            // orderedToSit) — a pet saved sitting loads sitting, instead of
            // standing up for the tick before SitWhenOrderedToGoal runs.
            t.SetInSittingPose(t.IsOrderedToSit());
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
                n.RestoreAngerTargetOnLoad(uuid);
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

        // AbstractArrow. `pickup`, `PierceLevel` and `SoundEvent` are
        // vanilla keys this engine models no state for, so they are not
        // invented — a reader that wants them gets vanilla's own defaults
        // (DISALLOWED / 0 / the type's sound). `crit` and `weapon` (the
        // launcher whose enchantments the hit reads) are written as vanilla
        // writes them.
        void WriteArrowLayer(Nbt::Writer& w, const Arrow& a) {
            w.Short ("life",   static_cast<int16_t>(a.GetLife()));
            w.Byte  ("shake",  static_cast<int8_t>(a.GetShakeTime()));
            w.Bool  ("inGround", a.IsInGroundArrow());
            w.Double("damage", a.GetBaseDamage());
            w.Bool  ("crit",   a.IsCritArrow());
            if (!a.GetFiredFromWeapon().IsEmpty()) {
                w.BeginCompound("weapon");
                WriteItemStackBody(w, a.GetFiredFromWeapon());
                w.EndCompound();
            }
            // MC AbstractArrow: PierceLevel (a Piercing crossbow's) and the
            // SoundEvent (a crossbow's CROSSBOW_HIT; the type's default
            // otherwise).
            w.Byte  ("PierceLevel", static_cast<int8_t>(a.GetPierceLevel()));
            {
                std::string sound = a.HitSoundEvent();
                if (sound.find(':') == std::string::npos) sound = "minecraft:" + sound;
                w.String("SoundEvent", sound);
            }
            // Engine-only: vanilla re-derives its equivalent from inGround.
            // Without it a trident that has already dealt its damage becomes
            // able to hit again on the tick it loads.
            w.Int   ("obey_in_ground_time", a.GetInGroundTime());
            // MC AbstractArrow: "pickup" (the Pickup ordinal).
            w.Byte  ("pickup", static_cast<int8_t>(a.GetPickup()));
            // MC Arrow's potion lives on its pickup stack ("item"). A
            // trident's item is the thrown trident itself (its enchantments,
            // its wear); an arrow's is written for a tipped shot — a plain
            // arrow's item is the default vanilla assumes when the key is
            // absent.
            if (a.GetType() == EntityTypeId::Trident) {
                const ItemStack pickup = a.GetPickupItem();
                w.BeginCompound("item");
                WriteItemStackBody(w, pickup);
                w.EndCompound();
            } else if (!a.GetPotionContents().IsEmpty() && a.GetType() == EntityTypeId::Arrow) {
                // A bow-fired tipped arrow carries a tipped_arrow (whose
                // default scale is the 0.125); a stray's or bogged's carries
                // a plain arrow with the effect added (scale 1.0).
                const float scale = a.GetPotionDurationScale();
                ItemStack pickup(scale == 0.125f ? Items::TippedArrow : Items::Arrow, 1);
                pickup.components.set(DataComponents::POTION_CONTENTS, a.GetPotionContents());
                if (scale != 0.125f && scale != 1.0f) {
                    pickup.components.set(DataComponents::POTION_DURATION_SCALE, scale);
                }
                w.BeginCompound("item");
                WriteItemStackBody(w, pickup);
                w.EndCompound();
            }
        }

        void ReadArrowLayer(const CT& tag, Arrow& a) {
            a.SetLife        (tag.GetValue<int16_t>("life", 0));
            // Vanilla masks the byte back to unsigned; shake is 0..7.
            a.SetShakeTime   (tag.GetValue<int8_t>("shake", 0) & 0xFF);
            a.SetInGround    (tag.GetValue<int8_t>("inGround", 0) != 0);
            a.SetBaseDamage  (tag.GetValue<double>("damage", Arrow::kArrowBaseDamage));
            a.SetInGroundTime(tag.GetValue<int32_t>("obey_in_ground_time", 0));
            a.SetCritArrow   (tag.GetValue<int8_t>("crit", 0) != 0);
            a.SetPierceLevel (tag.GetValue<int8_t>("PierceLevel", 0) & 0xFF);
            {
                std::string sound = tag.GetValue<std::string>("SoundEvent", "");
                if (sound.rfind("minecraft:", 0) == 0) sound.erase(0, 10);
                // The type's own default needs no override.
                if (!sound.empty() && sound != a.HitSoundEvent()) a.SetSoundEvent(sound);
            }
            if (auto weapon = As<CT>(tag.GetTag("weapon"))) {
                const ItemStack launcher = ReadItemStack(*weapon);
                if (!launcher.IsEmpty()) a.SetFiredFromWeapon(launcher);
            }
            {
                const int pickup = tag.GetValue<int8_t>("pickup", 0);
                a.SetPickup(pickup == 1 ? Arrow::Pickup::Allowed
                          : pickup == 2 ? Arrow::Pickup::CreativeOnly : Arrow::Pickup::Disallowed);
            }
            if (auto item = As<CT>(tag.GetTag("item"))) {
                const ItemStack pickup = ReadItemStack(*item);
                if (!pickup.IsEmpty()) {
                    if (auto* trident = dynamic_cast<ThrownTrident*>(&a)) {
                        // ThrownTrident.readAdditionalSaveData: ID_LOYALTY
                        // re-read off the item.
                        trident->SetTridentItem(pickup);
                    } else {
                        a.SetPickupItemStack(pickup);
                        a.SetPotionFromPickupStack(pickup);
                    }
                }
            }
        }

        // ── Boats and minecarts ─────────────────────────────────────────────
        //
        // MC ContainerEntity.addChestVehicleSaveData: an unrolled loot table
        // INSTEAD of the items (seed only when non-zero), else
        // ContainerHelper.saveAllItems ("Items", each with its "Slot").
        void WriteChestVehicle(Nbt::Writer& w, const VehicleContainer& c) {
            if (c.HasContainerLootTable()) {
                w.String("LootTable", c.GetContainerLootTable());
                if (c.GetContainerLootTableSeed() != 0) w.Long("LootTableSeed", c.GetContainerLootTableSeed());
                return;
            }
            auto list = w.BeginList("Items", Nbt::TagType::Compound);
            const std::vector<ItemStack>& items = c.Items();
            for (size_t i = 0; i < items.size(); ++i) {
                if (items[i].IsEmpty()) continue;
                w.ListCompoundBegin(list);
                WriteItemStackBody(w, items[i], static_cast<int>(i));
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }

        // MC ContainerEntity.readChestVehicleSaveData.
        void ReadChestVehicle(const CT& tag, VehicleContainer& c) {
            c.ClearItemStacks();
            const std::string lootTable = tag.GetValue<std::string>("LootTable", "");
            const int64_t seed = tag.HasTag("LootTableSeed")
                ? static_cast<int64_t>(NumberOf(tag.GetTag("LootTableSeed"))) : 0;
            c.SetContainerLootTable(lootTable, seed);
            if (!lootTable.empty()) return;
            auto list = As<LT>(tag.GetTag("Items"));
            if (!list) return;
            std::vector<ItemStack>& items = c.Items();
            for (const auto& elem : list->value) {
                auto item = As<CT>(elem);
                if (!item) continue;
                // ContainerHelper.loadAllItems: "Slot" & 255, in range only.
                const int slot = static_cast<int>(NumberOf(item->GetTag("Slot"))) & 255;
                if (slot < 0 || slot >= static_cast<int>(items.size())) continue;
                items[static_cast<size_t>(slot)] = ReadItemStack(*item);
            }
        }

        // MC AbstractChestedHorse.addAdditionalSaveData: "ChestedHorse", and
        // while chested the inventory as "Items" (ItemStackWithSlot — each
        // with its "Slot"). The donkey, the mule and the llamas.
        void WriteMountChest(Nbt::Writer& w, const Mob& mob) {
            const MountInventory* inventory = mob.GetMountInventory();
            if (!inventory || !inventory->CanCarryChest()) return;
            w.Bool("ChestedHorse", inventory->HasChest());
            if (!inventory->HasChest()) return;
            auto list = w.BeginList("Items", Nbt::TagType::Compound);
            const SimpleContainer& items = inventory->Container();
            for (int i = 0; i < items.GetContainerSize(); ++i) {
                const ItemStack& stack = items.GetItem(i);
                if (stack.IsEmpty()) continue;
                w.ListCompoundBegin(list);
                WriteItemStackBody(w, stack, i);
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }

        // MC AbstractChestedHorse.readAdditionalSaveData: setChest, then
        // createInventory (at the columns the mob now reports — a llama's
        // strength is read first), then the "Items" whose slot fits.
        void ReadMountChest(const CT& tag, Mob& mob) {
            MountInventory* inventory = mob.GetMountInventory();
            if (!inventory || !inventory->CanCarryChest()) return;
            inventory->SetChest(tag.GetValue<int8_t>("ChestedHorse", 0) != 0);
            mob.CreateMountInventory();
            if (!inventory->HasChest()) return;
            auto list = As<LT>(tag.GetTag("Items"));
            if (!list) return;
            SimpleContainer& items = inventory->Container();
            for (const auto& elem : list->value) {
                auto item = As<CT>(elem);
                if (!item) continue;
                // ItemStackWithSlot.isValidInContainer: 0 <= slot < size.
                const int slot = static_cast<int>(NumberOf(item->GetTag("Slot"))) & 255;
                if (slot < 0 || slot >= items.GetContainerSize()) continue;
                items.SetItem(slot, ReadItemStack(*item));
            }
        }

        // MC AbstractBoat / AbstractMinecart (+ subclasses)
        // addAdditionalSaveData — the layers above (Entity, and the Mob layer
        // that carries a boat's "leash") already wrote the rest.
        void WriteVehicleLayer(Nbt::Writer& w, const VehicleEntity& v) {
            if (const auto* boat = dynamic_cast<const Boat*>(&v)) {
                if (const VehicleContainer* c = boat->Container()) WriteChestVehicle(w, *c);
                return;
            }
            const auto* cart = dynamic_cast<const AbstractMinecart*>(&v);
            if (!cart) return;
            if (cart->HasCustomDisplayBlockState()) {
                WriteBlockStateCompound(w, "DisplayState", cart->CustomDisplayBlockState());
            }
            if (cart->GetDisplayOffset() != cart->GetDefaultDisplayOffset()) {
                w.Int("DisplayOffset", cart->GetDisplayOffset());
            }
            w.Bool("FlippedRotation", cart->IsFlipped());
            w.Bool("HasTicked", cart->FirstTickFlag());
            if (const auto* container = dynamic_cast<const MinecartContainerBase*>(cart)) {
                WriteChestVehicle(w, container->Container());
            }
            if (const auto* hopper = dynamic_cast<const MinecartHopper*>(cart)) {
                w.Bool("Enabled", hopper->IsEnabled());
            } else if (const auto* furnace = dynamic_cast<const MinecartFurnace*>(cart)) {
                w.Double("PushX", furnace->GetPush().x);
                w.Double("PushZ", furnace->GetPush().z);
                w.Short("Fuel", static_cast<int16_t>(furnace->GetFuel()));
            } else if (const auto* tnt = dynamic_cast<const MinecartTNT*>(cart)) {
                w.Int("fuse", tnt->GetFuse());
                if (tnt->ExplosionPowerBase() != MinecartTNT::kDefaultExplosionPowerBase) {
                    w.Float("explosion_power", tnt->ExplosionPowerBase());
                }
                if (tnt->ExplosionSpeedFactor() != MinecartTNT::kDefaultExplosionSpeedFactor) {
                    w.Float("explosion_speed_factor", tnt->ExplosionSpeedFactor());
                }
            } else if (const auto* spawner = dynamic_cast<const MinecartSpawner*>(cart)) {
                WriteSpawner(w, spawner->Spawner());
            } else if (const auto* command = dynamic_cast<const MinecartCommandBlock*>(cart)) {
                // MC BaseCommandBlock.save (the name is the entity's own
                // CustomName, written by the Entity layer).
                w.String("Command", command->GetCommand());
                w.Int("SuccessCount", command->GetSuccessCount());
                w.Bool("TrackOutput", command->TrackOutput());
                if (command->TrackOutput() && !command->GetLastOutput().empty()) {
                    // ComponentSerialization.CODEC: a plain-text component.
                    w.String("LastOutput", command->GetLastOutput());
                }
                w.Bool("UpdateLastExecution", command->UpdateLastExecution());
                if (command->UpdateLastExecution() && command->GetLastExecution() != -1) {
                    w.Long("LastExecution", command->GetLastExecution());
                }
            }
        }

        // The readAdditionalSaveData twins.
        void ReadVehicleLayer(const CT& tag, VehicleEntity& v) {
            if (auto* boat = dynamic_cast<Boat*>(&v)) {
                if (VehicleContainer* c = boat->Container()) ReadChestVehicle(tag, *c);
                return;
            }
            auto* cart = dynamic_cast<AbstractMinecart*>(&v);
            if (!cart) return;
            BlockState display{};
            if (ReadBlockStateCompound(tag, "DisplayState", display)) {
                cart->SetCustomDisplayBlockState(display);
            } else {
                cart->SetCustomDisplayBlockState(std::nullopt);
            }
            cart->SetDisplayOffset(tag.HasTag("DisplayOffset")
                ? static_cast<int>(NumberOf(tag.GetTag("DisplayOffset"))) : cart->GetDefaultDisplayOffset());
            cart->SetFlipped(tag.HasTag("FlippedRotation") && NumberOf(tag.GetTag("FlippedRotation")) != 0.0);
            cart->SetFirstTickFlag(tag.HasTag("HasTicked") && NumberOf(tag.GetTag("HasTicked")) != 0.0);
            if (auto* container = dynamic_cast<MinecartContainerBase*>(cart)) {
                ReadChestVehicle(tag, container->Container());
            }
            if (auto* hopper = dynamic_cast<MinecartHopper*>(cart)) {
                hopper->SetEnabled(!tag.HasTag("Enabled") || NumberOf(tag.GetTag("Enabled")) != 0.0);
            } else if (auto* furnace = dynamic_cast<MinecartFurnace*>(cart)) {
                const double pushX = tag.HasTag("PushX") ? NumberOf(tag.GetTag("PushX")) : 0.0;
                const double pushZ = tag.HasTag("PushZ") ? NumberOf(tag.GetTag("PushZ")) : 0.0;
                furnace->SetPush(glm::dvec3(pushX, 0.0, pushZ));
                furnace->SetFuel(tag.HasTag("Fuel") ? static_cast<int16_t>(NumberOf(tag.GetTag("Fuel"))) : 0);
            } else if (auto* tnt = dynamic_cast<MinecartTNT*>(cart)) {
                tnt->SetFuse(tag.HasTag("fuse") ? static_cast<int>(NumberOf(tag.GetTag("fuse"))) : -1);
                tnt->SetExplosionPowerBase(tag.HasTag("explosion_power")
                    ? static_cast<float>(NumberOf(tag.GetTag("explosion_power")))
                    : MinecartTNT::kDefaultExplosionPowerBase);
                tnt->SetExplosionSpeedFactor(tag.HasTag("explosion_speed_factor")
                    ? static_cast<float>(NumberOf(tag.GetTag("explosion_speed_factor")))
                    : MinecartTNT::kDefaultExplosionSpeedFactor);
            } else if (auto* spawner = dynamic_cast<MinecartSpawner*>(cart)) {
                spawner->Spawner().MoveCarriedTo(cart->BlockPosition());
                ReadSpawner(tag, spawner->Spawner());
            } else if (auto* command = dynamic_cast<MinecartCommandBlock*>(cart)) {
                command->SetCommand(tag.GetValue<std::string>("Command", ""));
                command->SetSuccessCount(tag.HasTag("SuccessCount")
                    ? static_cast<int>(NumberOf(tag.GetTag("SuccessCount"))) : 0);
                const bool track = !tag.HasTag("TrackOutput") || NumberOf(tag.GetTag("TrackOutput")) != 0.0;
                command->SetTrackOutput(track);
                command->SetLastOutput(track ? tag.GetValue<std::string>("LastOutput", "") : std::string());
                const bool update = !tag.HasTag("UpdateLastExecution") ||
                                    NumberOf(tag.GetTag("UpdateLastExecution")) != 0.0;
                command->SetUpdateLastExecution(update);
                command->SetLastExecution(update && tag.HasTag("LastExecution")
                    ? static_cast<int64_t>(NumberOf(tag.GetTag("LastExecution"))) : -1);
            }
        }

    } // namespace

    // ── mod mob fields ──────────────────────────────────────────────────────

    namespace {
        // Mob::SaveModNbt / LoadModNbt over this file's writer and parsed
        // compound (ModMobNbt.hpp).
        class ModNbtWriterAdapter final : public ModNbtOut {
        public:
            explicit ModNbtWriterAdapter(Nbt::Writer& w) : m_w(w) {}
            void Byte(std::string_view name, int8_t v) override { m_w.Byte(name, v); }
            void Int(std::string_view name, int32_t v) override { m_w.Int(name, v); }
            void Float(std::string_view name, float v) override { m_w.Float(name, v); }
            void String(std::string_view name, std::string_view v) override { m_w.String(name, v); }
        private:
            Nbt::Writer& m_w;
        };

        class ModNbtTagAdapter final : public ModNbtIn {
        public:
            explicit ModNbtTagAdapter(const ::World::NBTTagCompound& tag) : m_tag(tag) {}
            bool Has(const std::string& name) const override { return m_tag.HasTag(name); }
            int8_t Byte(const std::string& name, int8_t def) const override {
                return m_tag.GetValue<int8_t>(name, def);
            }
            int32_t Int(const std::string& name, int32_t def) const override {
                return m_tag.GetValue<int32_t>(name, def);
            }
            float Float(const std::string& name, float def) const override {
                return m_tag.GetValue<float>(name, def);
            }
            std::string String(const std::string& name, const std::string& def) const override {
                return m_tag.GetValue<std::string>(name, def);
            }
        private:
            const ::World::NBTTagCompound& m_tag;
        };
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

    namespace {
        // The fields of one saved mob, into the compound the caller has open
        // (a list element for a chunk or a Passengers list, a named compound
        // for a player's RootVehicle).
        void WriteMobBody(Nbt::Writer& w, const Mob& mob, const std::string& name);
    } // namespace

    bool WriteMob(Nbt::Writer& w, Nbt::Writer::ListScope& list, const Mob& mob) {
        // Three reasons never to write: the type has no vanilla name, the
        // entity opted out (projectiles until their owners can round-trip),
        // or it is already dead and merely waiting for the sweep.
        if (!mob.CanSerialize() || mob.IsRemoved()) return false;
        const std::string name = EntityName(mob.GetType());
        if (name.empty()) return false;

        w.ListCompoundBegin(list);
        WriteMobBody(w, mob, name);
        w.ListCompoundEnd(list);
        return true;
    }

    bool WriteMobCompound(Nbt::Writer& w, std::string_view key, const Mob& mob) {
        if (!mob.CanSerialize() || mob.IsRemoved()) return false;
        const std::string name = EntityName(mob.GetType());
        if (name.empty()) return false;
        w.BeginCompound(key);
        WriteMobBody(w, mob, name);
        w.EndCompound();
        return true;
    }

    namespace {
    void WriteMobBody(Nbt::Writer& w, const Mob& mob, const std::string& name) {
        w.String("id", name);
        WriteEntityBase(w, mob);
        WriteLiving(w, mob);
        WriteMobLayer(w, mob);
        // Engine extra (vanilla ignores it): Mob::GetRenderPhase — see
        // ApplyMobNbt's end.
        {
            float phase[Mob::kRenderPhaseMax] = {};
            const int n = std::min(mob.GetRenderPhase(phase), Mob::kRenderPhaseMax);
            if (n > 0) {
                auto list = w.BeginList("obey_render_phase", Nbt::TagType::Float);
                for (int i = 0; i < n; ++i) w.ListFloat(list, phase[i]);
                w.EndList(list);
            }
        }

        if (const auto* ageable = dynamic_cast<const AgeableMob*>(&mob)) WriteAgeableLayer(w, *ageable);
        if (const auto* animal = dynamic_cast<const Animal*>(&mob)) WriteAnimalLayer(w, *animal);
        if (const auto* tamable = dynamic_cast<const TamableAnimal*>(&mob)) WriteTamable(w, *tamable);
        if (const auto* neutral = dynamic_cast<const NeutralMob*>(&mob)) {
            const EntityLevel* level = mob.Level();
            WriteNeutral(w, *neutral, level ? level->GetGameTime() : 0);
        }
        // MC Warden.addAdditionalSaveData: the vibration it is hearing
        // ("listener", VibrationSystem.Data.CODEC).
        if (const auto* warden = dynamic_cast<const Warden*>(&mob)) {
            WriteVibrationData(w, "listener", warden->SavedVibrationData());
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

        // MC PatrollingMonster.addAdditionalSaveData: patrol_target
        // (BlockPos.CODEC, an int array, only when set), PatrolLeader,
        // Patrolling; Raider's Wave and CanJoinRaid on top (RaidId belongs to
        // the raid system, which is not ported).
        if (const auto* patrol = dynamic_cast<const PatrollingMonster*>(&mob)) {
            if (const auto& target = patrol->GetPatrolTarget()) {
                const int32_t a[3] = { target->x, target->y, target->z };
                w.IntArray("patrol_target", a, 3);
            }
            w.Bool("PatrolLeader", patrol->IsPatrolLeader());
            w.Bool("Patrolling", patrol->IsPatrolling());
            if (const auto* raider = dynamic_cast<const Raider*>(patrol)) {
                w.Int("Wave", raider->GetWave());
                w.Bool("CanJoinRaid", raider->CanJoinRaid());
            }
        }

        // MC Bucketable "FromBucket" (AbstractFish / Axolotl); the tadpole
        // (an AbstractFish, fromBucket always true) adds Age / AgeLocked.
        if (const auto* fish = dynamic_cast<const Fish*>(&mob)) {
            w.Bool("FromBucket", fish->FromBucket());
        } else if (const auto* axolotl = dynamic_cast<const Axolotl*>(&mob)) {
            w.Bool("FromBucket", axolotl->FromBucket());
        } else if (const auto* tadpole = dynamic_cast<const Tadpole*>(&mob)) {
            w.Bool("FromBucket", true);
            w.Int("Age", tadpole->GetAge());
            w.Bool("AgeLocked", tadpole->IsAgeLocked());
        }

        // Boats and minecarts (common/entity/vehicle).
        if (IsVehicleEntityType(mob.GetType())) {
            if (const auto* vehicle = dynamic_cast<const VehicleEntity*>(&mob)) WriteVehicleLayer(w, *vehicle);
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
            // MC IronGolem.addAdditionalSaveData: "PlayerCreated" (the anger
            // half is WriteNeutral's).
            case EntityTypeId::IronGolem:
                if (const auto* g = dynamic_cast<const IronGolem*>(&mob)) {
                    w.Bool("PlayerCreated", g->IsPlayerCreated());
                }
                break;
            // MC SnowGolem.addAdditionalSaveData: "Pumpkin".
            case EntityTypeId::SnowGolem:
                if (const auto* g = dynamic_cast<const SnowGolem*>(&mob)) {
                    w.Bool("Pumpkin", g->HasPumpkin());
                }
                break;
            // MC CopperGolem.addAdditionalSaveData: "next_weather_age" and
            // "weather_state" (WeatheringCopper.WeatherState.CODEC — the
            // lower-case name). The carried stack and the antenna's flower
            // are its equipment (WriteMobEquipment).
            case EntityTypeId::CopperGolem:
                if (const auto* g = dynamic_cast<const CopperGolem*>(&mob)) {
                    w.Long("next_weather_age", g->GetNextWeatheringTick());
                    w.String("weather_state", CopperGolem::WeatherStateName(g->GetWeatherState()));
                }
                break;
            case EntityTypeId::Chicken:
                if (const auto* c = dynamic_cast<const Chicken*>(&mob)) {
                    w.Bool("IsChickenJockey", c->IsChickenJockey());
                    w.String("variant", TemperatureVariantId(c->GetVariantByte()));
                    w.String("sound_variant", std::string("minecraft:") + FarmSoundVariants::Name(FarmSoundVariants::Mob::Chicken, c->GetSoundVariant()));
                }
                break;
            case EntityTypeId::Cow:
                if (const auto* c = dynamic_cast<const Cow*>(&mob)) {
                    w.String("variant", TemperatureVariantId(c->GetVariantByte()));
                    w.String("sound_variant", std::string("minecraft:") + FarmSoundVariants::Name(FarmSoundVariants::Mob::Cow, c->GetSoundVariant()));
                }
                break;
            case EntityTypeId::Pig:
                if (const auto* p = dynamic_cast<const Pig*>(&mob)) {
                    w.String("variant", TemperatureVariantId(p->GetVariantByte()));
                    w.String("sound_variant", std::string("minecraft:") + FarmSoundVariants::Name(FarmSoundVariants::Mob::Pig, p->GetSoundVariant()));
                }
                break;
            // MC MushroomCow.addAdditionalSaveData: "Type" (Variant.CODEC —
            // "red" / "brown").
            case EntityTypeId::Mooshroom:
                if (const auto* m = dynamic_cast<const Mooshroom*>(&mob)) {
                    w.String("Type", m->GetVariant() == Mooshroom::Variant::Brown ? "brown" : "red");
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
                if (const auto* zv = dynamic_cast<const ZombieVillager*>(&mob)) {
                    WriteZombieVillagerNbt(w, *zv);
                }
                break;
            // MC Villager / AbstractVillager (VillagerNbt.hpp).
            case EntityTypeId::Villager:
                if (const auto* v = dynamic_cast<const Villager*>(&mob)) WriteVillagerNbt(w, *v);
                break;
            // MC WanderingTrader (VillagerNbt.hpp).
            case EntityTypeId::WanderingTrader:
                if (const auto* t = dynamic_cast<const WanderingTrader*>(&mob)) WriteWanderingTraderNbt(w, *t);
                break;
            case EntityTypeId::Fox:
                if (const auto* f = dynamic_cast<const Fox*>(&mob)) {
                    w.Bool  ("Sleeping",  f->IsSleeping());
                    w.Bool  ("Sitting",   f->IsSitting());
                    w.Bool  ("Crouching", f->IsFoxCrouching());
                    w.String("Type", EnumName(kFoxVariantNames,
                                              static_cast<size_t>(f->GetVariant())));
                    // MC Fox.addAdditionalSaveData: "Trusted", a list of
                    // int-array UUIDs (EntityReference codec), always written.
                    {
                        const std::vector<Uuid> trusted = f->GetTrustedUuids();
                        auto list = w.BeginList("Trusted", Nbt::TagType::IntArray);
                        for (const Uuid& uuid : trusted) {
                            int32_t words[4];
                            UuidToIntArray(uuid, words);
                            w.ListIntArray(list, words, 4);
                        }
                        w.EndList(list);
                    }
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
                    // MC Rabbit.addAdditionalSaveData: "RabbitType" — the
                    // variant's id (Variant.LEGACY_CODEC).
                    w.Int("RabbitType", static_cast<int32_t>(r->GetVariant()));
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
                    w.String("sound_variant", std::string("minecraft:") + FarmSoundVariants::Name(FarmSoundVariants::Mob::Cat, c->GetSoundVariant()));
                    // MC Cat: DyeColor.LEGACY_ID_CODEC — the ordinal as a byte.
                    w.Byte("CollarColor", static_cast<int8_t>(c->GetCollarColor()));
                }
                break;
            // MC Frog.addAdditionalSaveData: VariantUtils.writeVariant.
            case EntityTypeId::Frog:
                if (const auto* frog = dynamic_cast<const Frog*>(&mob)) {
                    w.String("variant", std::string("minecraft:") + Frog::VariantName(frog->GetVariant()));
                }
                break;
            // MC Wolf.addAdditionalSaveData: CollarColor (legacy byte id),
            // the coat as "variant", the anger pair (WriteNeutral above) and
            // "sound_variant"; the BODY slot rides EntityEquipment's
            // "equipment" compound with its guaranteed drop chance.
            case EntityTypeId::Wolf:
                if (const auto* wolf = dynamic_cast<const Wolf*>(&mob)) {
                    w.Byte("CollarColor", static_cast<int8_t>(wolf->GetCollarColor()));
                    w.String("variant", std::string("minecraft:") +
                                            WolfVariants::Name(wolf->GetVariant()));
                    w.String("sound_variant", std::string("minecraft:") +
                                                  WolfSoundVariants::Name(wolf->GetSoundVariant()));
                    if (wolf->IsWearingBodyArmor()) {
                        w.BeginCompound("equipment");
                        w.BeginCompound("body");
                        WriteItemStackBody(w, wolf->GetBodyArmorItem());
                        w.EndCompound();
                        w.EndCompound();
                        // MC DropChances: setItemSlotAndDropWhenKilled's
                        // guaranteed drop is written as 2.0.
                        w.BeginCompound("drop_chances");
                        w.Float("body", 2.0f);
                        w.EndCompound();
                    }
                }
                break;
            // MC Llama.addAdditionalSaveData: "Strength".
            case EntityTypeId::Llama:
            case EntityTypeId::TraderLlama:
                if (const auto* llama = dynamic_cast<const Llama*>(&mob)) {
                    // MC AbstractHorse.addAdditionalSaveData (Llama extends it).
                    w.Int ("Temper", llama->GetTemper());
                    w.Bool("Tame",   llama->IsTamed());
                    WriteRef(w, "Owner", llama->OwnerRef());
                    w.Int("Strength", llama->GetStrength());
                    // MC Llama.Variant.LEGACY_CODEC — the id as an int.
                    w.Int("Variant", static_cast<int32_t>(llama->GetVariant()));
                    WriteMountChest(w, *llama);
                }
                // MC TraderLlama.addAdditionalSaveData.
                if (const auto* trader = dynamic_cast<const TraderLlama*>(&mob)) {
                    w.Int("DespawnDelay", trader->GetDespawnDelay());
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
                    w.Bool("Bred",           h->IsBred());
                    w.Int ("Temper",         h->GetTemper());
                    // NOT the TamableAnimal "Tame" — AbstractHorse owns its
                    // own tamed flag and does not use that mixin.
                    w.Bool("Tame",           h->IsTamedHorse());
                    // MC EntityReference.store(owner, output, "Owner").
                    WriteRef(w, "Owner", h->OwnerRef());
                    // AbstractChestedHorse (the donkey, the mule).
                    WriteMountChest(w, *h);
                }
                // MC Horse.addAdditionalSaveData: "Variant" = variant | markings << 8.
                if (const auto* horse = dynamic_cast<const Horse*>(&mob)) {
                    w.Int("Variant", horse->GetTypeVariant());
                }
                // MC SkeletonHorse.addAdditionalSaveData.
                if (const auto* s = dynamic_cast<const SkeletonHorse*>(&mob)) {
                    w.Bool("SkeletonTrap",    s->IsTrap());
                    w.Int ("SkeletonTrapTime", s->GetTrapTime());
                }
                break;
            case EntityTypeId::HappyGhast:
                // MC HappyGhast.addAdditionalSaveData.
                if (const auto* g = dynamic_cast<const HappyGhast*>(&mob)) {
                    w.Int("still_timeout", g->GetServerStillTimeout());
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
                    // MC Bee.addAdditionalSaveData: hive_pos (nullable),
                    // CannotEnterHiveTicks.
                    if (const auto& hive = b->GetHivePos()) {
                        const int32_t h[3] = {hive->x, hive->y, hive->z};
                        w.IntArray("hive_pos", h, 3);
                    }
                    w.Int("CannotEnterHiveTicks", b->GetStayOutOfHiveCountdown());
                }
                break;
            case EntityTypeId::Dolphin:
                if (const auto* d = dynamic_cast<const Dolphin*>(&mob)) {
                    w.Int("Moistness", d->GetMoistness());
                    w.Bool("GotFish", d->GotFish());
                }
                break;
            case EntityTypeId::Pufferfish:
                if (const auto* p = dynamic_cast<const Pufferfish*>(&mob)) {
                    w.Int("PuffState", p->GetPuffState());
                }
                break;
            case EntityTypeId::TropicalFish:
                // MC TropicalFish.addAdditionalSaveData: "Variant", the
                // packed int (Variant.CODEC = Codec.INT).
                if (const auto* t = dynamic_cast<const TropicalFish*>(&mob)) {
                    w.Int("Variant", t->GetPackedVariant());
                }
                break;
            case EntityTypeId::Salmon:
                // MC Salmon.addAdditionalSaveData: "type", the size's name.
                if (const auto* sal = dynamic_cast<const Salmon*>(&mob)) {
                    w.String("type", Salmon::SizeName(sal->GetSize()));
                }
                break;
            case EntityTypeId::Allay:
                if (const auto* a = dynamic_cast<const Allay*>(&mob)) WriteAllayData(w, *a);
                break;
            case EntityTypeId::Camel:
            case EntityTypeId::CamelHusk:   // MC CamelHusk extends Camel
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
            // MC Parrot.addAdditionalSaveData: "Variant" through
            // Parrot.Variant.LEGACY_CODEC — the int id.
            case EntityTypeId::Parrot:
                if (const auto* p = dynamic_cast<const Parrot*>(&mob)) {
                    w.Int("Variant", static_cast<int32_t>(p->GetVariant()));
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
                    // MC Creeper.addAdditionalSaveData: "powered" (the
                    // charged flag), "Fuse", "ExplosionRadius", "ignited".
                    w.Bool("powered", c->IsPowered());
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
            // MC ZombieNautilus.addAdditionalSaveData: VariantUtils.writeVariant.
            case EntityTypeId::ZombieNautilus:
                if (const auto* zn = dynamic_cast<const ZombieNautilus*>(&mob)) {
                    w.String("variant", std::string("minecraft:") + ZombieNautilus::VariantName(zn->GetVariantByte()));
                }
                break;
            case EntityTypeId::Shulker:
                if (const auto* sh = dynamic_cast<const Shulker*>(&mob)) {
                    w.Byte("AttachFace", static_cast<int8_t>(sh->GetAttachFace()));
                    w.Byte("Peek",       static_cast<int8_t>(sh->GetRawPeekAmount()));
                    // MC Shulker.addAdditionalSaveData: "Color" (16 = none).
                    w.Byte("Color",      static_cast<int8_t>(sh->GetColor()));
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
            case EntityTypeId::Painting:
                if (const auto* p = dynamic_cast<const Painting*>(&mob)) {
                    // MC Painting.addAdditionalSaveData: "facing" as the 2D
                    // data value (Direction.LEGACY_ID_CODEC_2D), then
                    // BlockAttachedEntity's "block_pos" (BlockPos.CODEC, an
                    // int array), then the variant's id (VariantUtils).
                    w.Byte("facing", static_cast<int8_t>(ToYRot(p->GetDirection()) / 90.0f));
                    const int32_t cell[3] = { p->HangingPos().x, p->HangingPos().y, p->HangingPos().z };
                    w.IntArray("block_pos", cell, 3);
                    if (const PaintingVariant* v = p->Variant()) w.String("variant", v->id);
                }
                break;
            case EntityTypeId::Cushion:
                if (const auto* c = dynamic_cast<const Cushion*>(&mob)) {
                    // MC Cushion.addAdditionalSaveData: BlockAttachedEntity's
                    // "block_pos" (BlockPos.CODEC), then "color"
                    // (DyeColor.CODEC — the name).
                    const int32_t cell[3] = { c->GetBlockPos().x, c->GetBlockPos().y, c->GetBlockPos().z };
                    w.IntArray("block_pos", cell, 3);
                    w.String("color", Cushion::ColorName(c->GetColor()));
                }
                break;
            case EntityTypeId::OminousItemSpawner:
                if (const auto* o = dynamic_cast<const OminousItemSpawner*>(&mob)) {
                    // MC OminousItemSpawner.addAdditionalSaveData: "item"
                    // when it still holds one, then "spawn_item_after_ticks".
                    if (!o->GetItem().IsEmpty()) {
                        w.BeginCompound("item");
                        WriteItemStackBody(w, o->GetItem());
                        w.EndCompound();
                    }
                    w.Long("spawn_item_after_ticks", o->GetSpawnItemAfterTicks());
                }
                break;
            case EntityTypeId::ItemFrame:
            case EntityTypeId::GlowItemFrame:
                if (const auto* f = dynamic_cast<const ItemFrame*>(&mob)) {
                    // MC ItemFrame.addAdditionalSaveData, after
                    // BlockAttachedEntity's "block_pos". "Facing" is the 3D
                    // data value (Direction.LEGACY_ID_CODEC) — the engine's
                    // Direction order is the same.
                    const int32_t cell[3] = { f->HangingPos().x, f->HangingPos().y, f->HangingPos().z };
                    w.IntArray("block_pos", cell, 3);
                    if (!f->GetItem().IsEmpty()) {
                        w.BeginCompound("Item");
                        WriteItemStackBody(w, f->GetItem());
                        w.EndCompound();
                    }
                    w.Byte ("ItemRotation", static_cast<int8_t>(f->GetRotation()));
                    w.Float("ItemDropChance", f->GetDropChance());
                    w.Byte ("Facing", static_cast<int8_t>(f->GetDirection()));
                    w.Bool ("Invisible", f->IsInvisible());
                    w.Bool ("Fixed", f->IsFixed());
                }
                break;
            case EntityTypeId::ArmorStand:
                if (const auto* a = dynamic_cast<const ArmorStand*>(&mob)) {
                    // MC ArmorStand.addAdditionalSaveData, plus the engine's
                    // Invisible (MC's is Entity's shared flag byte).
                    w.Bool("Invisible", a->IsInvisible());
                    w.Bool("Small", a->IsSmall());
                    w.Bool("ShowArms", a->ShowArms());
                    w.Int ("DisabledSlots", a->GetDisabledSlots());
                    w.Bool("NoBasePlate", !a->ShowBasePlate());
                    if (a->IsMarker()) w.Bool("Marker", true);
                    // ArmorStandPose.CODEC: six float triples, degrees.
                    w.BeginCompound("Pose");
                    const auto rot = [&](const char* name, const glm::vec3& r) {
                        auto list = w.BeginList(name, Nbt::TagType::Float);
                        w.ListFloat(list, r.x);
                        w.ListFloat(list, r.y);
                        w.ListFloat(list, r.z);
                        w.EndList(list);
                    };
                    const ArmorStand::Pose& pose = a->GetPose();
                    rot("Head",     pose.head);
                    rot("Body",     pose.body);
                    rot("LeftArm",  pose.leftArm);
                    rot("RightArm", pose.rightArm);
                    rot("LeftLeg",  pose.leftLeg);
                    rot("RightLeg", pose.rightLeg);
                    w.EndCompound();
                    // MC EntityEquipment's codec (1.21.5+): "equipment", one
                    // compound per non-empty slot, keyed by the slot's name.
                    w.BeginCompound("equipment");
                    static constexpr std::pair<const char*, EquipmentSlot> kSlots[6] = {
                        {"mainhand", EquipmentSlot::MAINHAND}, {"offhand", EquipmentSlot::OFFHAND},
                        {"feet", EquipmentSlot::FEET},         {"legs", EquipmentSlot::LEGS},
                        {"chest", EquipmentSlot::CHEST},       {"head", EquipmentSlot::HEAD},
                    };
                    for (const auto& [name, slot] : kSlots) {
                        const ItemStack& stack = a->GetItemBySlot(slot);
                        if (stack.IsEmpty()) continue;
                        w.BeginCompound(name);
                        WriteItemStackBody(w, stack);
                        w.EndCompound();
                    }
                    w.EndCompound();
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
                    WriteCarrierInventory(w, p->GetInventory());
                }
                break;
            case EntityTypeId::Pillager:
                if (const auto* p = dynamic_cast<const Pillager*>(&mob)) {
                    WriteCarrierInventory(w, p->GetInventory());
                }
                break;
            case EntityTypeId::PiglinBrute:
                if (const auto* p = dynamic_cast<const PiglinBrute*>(&mob)) {
                    w.Bool("IsImmuneToZombification", p->IsImmuneToZombification());
                    w.Int ("TimeInOverworld", p->GetTimeInOverworld());
                }
                break;
            // MC Hoglin.addAdditionalSaveData.
            case EntityTypeId::Hoglin:
                if (const auto* h = dynamic_cast<const Hoglin*>(&mob)) {
                    w.Bool("IsImmuneToZombification", h->IsImmuneToZombification());
                    w.Int ("TimeInOverworld", h->GetTimeInOverworld());
                    w.Bool("CannotBeHunted", h->CannotBeHunted());
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
            case EntityTypeId::SplashPotion:
                // MC ThrowableItemProjectile.addAdditionalSaveData: "Item".
                // The item is what makes a thrown potion lingering here (see
                // ThrownSplashPotion), so it has to survive the save.
                if (const auto* p = dynamic_cast<const ThrownSplashPotion*>(&mob)) {
                    w.BeginCompound("Item");
                    WriteItemStackBody(w, p->GetItem());
                    w.EndCompound();
                }
                break;
            case EntityTypeId::FireworkRocket:
                // MC FireworkRocketEntity.addAdditionalSaveData: Life,
                // LifeTime, FireworksItem, ShotAtAngle. (The attachment is
                // synched data only — a reloaded rocket rides nobody.)
                if (const auto* r = dynamic_cast<const FireworkRocket*>(&mob)) {
                    w.Int("Life", r->GetLife());
                    w.Int("LifeTime", r->GetLifetime());
                    w.BeginCompound("FireworksItem");
                    WriteItemStackBody(w, r->GetItem());
                    w.EndCompound();
                    w.Bool("ShotAtAngle", r->IsShotAtAngle());
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
                    // MC AreaEffectCloud: potion_contents, omitted when EMPTY.
                    if (!c->GetPotionContents().IsEmpty()) {
                        w.BeginCompound("potion_contents");
                        WritePotionContentsBody(w, c->GetPotionContents());
                        w.EndCompound();
                    }
                }
                break;
            default:
                break;
        }

        // The mod mobs' own fields (Mob::SaveModNbt, ModMobNbt.hpp).
        {
            ModNbtWriterAdapter modOut(w);
            mob.SaveModNbt(modOut);
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
    }
    } // namespace

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

        if (auto* ageable = dynamic_cast<AgeableMob*>(&mob)) ReadAgeableLayer(tag, *ageable);
        if (auto* animal = dynamic_cast<Animal*>(&mob)) ReadAnimalLayer(tag, *animal);
        if (auto* tamable = dynamic_cast<TamableAnimal*>(&mob)) ReadTamable(tag, *tamable);
        if (auto* neutral = dynamic_cast<NeutralMob*>(&mob)) ReadNeutral(tag, *neutral);
        // MC PatrollingMonster / Raider.readAdditionalSaveData (defaults: no
        // target, false, false, wave 0, false).
        if (auto* patrol = dynamic_cast<PatrollingMonster*>(&mob)) {
            std::optional<glm::ivec3> target;
            if (auto arr = As<::World::NBTTagIntArray>(tag.GetTag("patrol_target"));
                arr && arr->value.size() == 3) {
                target = glm::ivec3(arr->value[0], arr->value[1], arr->value[2]);
            }
            patrol->RestorePatrolState(target, tag.GetValue<int8_t>("PatrolLeader", 0) != 0,
                                       tag.GetValue<int8_t>("Patrolling", 0) != 0);
            if (auto* raider = dynamic_cast<Raider*>(patrol)) {
                raider->SetWave(tag.GetValue<int32_t>("Wave", 0));
                raider->SetCanJoinRaid(tag.GetValue<int8_t>("CanJoinRaid", 0) != 0);
            }
        }
        // MC Warden.readAdditionalSaveData: "listener", a fresh Data when absent.
        if (auto* warden = dynamic_cast<Warden*>(&mob)) warden->SetVibrationData(ReadVibrationData(tag, "listener"));

        if (auto* proj = dynamic_cast<Projectile*>(&mob)) {
            ReadProjectileLayer(tag, *proj);
            if (auto* hurting = dynamic_cast<HurtingProjectile*>(proj)) {
                hurting->SetAccelerationPower(
                    tag.GetValue<double>("acceleration_power", 0.1));
            }
            if (auto* arrow = dynamic_cast<Arrow*>(proj)) ReadArrowLayer(tag, *arrow);
        }

        // MC readAdditionalSaveData: FromBucket (getBooleanOr false); the
        // tadpole's Age (0) and AgeLocked (false).
        if (auto* fish = dynamic_cast<Fish*>(&mob)) {
            fish->SetFromBucket(tag.GetValue<int8_t>("FromBucket", 0) != 0);
        } else if (auto* axolotl = dynamic_cast<Axolotl*>(&mob)) {
            axolotl->SetFromBucket(tag.GetValue<int8_t>("FromBucket", 0) != 0);
        } else if (auto* tadpole = dynamic_cast<Tadpole*>(&mob)) {
            tadpole->SetAge(tag.GetValue<int32_t>("Age", 0));
            tadpole->SetAgeLocked(tag.GetValue<int8_t>("AgeLocked", 0) != 0);
        }

        // Boats and minecarts (common/entity/vehicle).
        if (IsVehicleEntityType(mob.GetType())) {
            if (auto* vehicle = dynamic_cast<VehicleEntity*>(&mob)) ReadVehicleLayer(tag, *vehicle);
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
            // MC IronGolem.readAdditionalSaveData: getBooleanOr("PlayerCreated", false).
            case EntityTypeId::IronGolem:
                if (auto* g = dynamic_cast<IronGolem*>(&mob)) {
                    g->SetPlayerCreated(tag.GetValue<int8_t>("PlayerCreated", 0) != 0);
                }
                break;
            // MC SnowGolem.readAdditionalSaveData: getBooleanOr("Pumpkin", true).
            case EntityTypeId::SnowGolem:
                if (auto* g = dynamic_cast<SnowGolem*>(&mob)) {
                    g->SetPumpkin(tag.GetValue<int8_t>("Pumpkin", 1) != 0);
                }
                break;
            // MC CopperGolem.readAdditionalSaveData: getLongOr("next_weather_age",
            // -1) and "weather_state" (absent: UNAFFECTED). A pre-release
            // save's integer ordinal is read as CopperGolemWeatherStateFix
            // converts it.
            case EntityTypeId::CopperGolem:
                if (auto* g = dynamic_cast<CopperGolem*>(&mob)) {
                    g->SetNextWeatheringTick(tag.GetValue<int64_t>("next_weather_age",
                                                                   CopperGolem::kUnsetWeatheringTick));
                    CopperGolem::WeatherState state = CopperGolem::WeatherState::Unaffected;
                    if (tag.HasTag("weather_state")) {
                        const std::string name = tag.GetValue<std::string>("weather_state", "");
                        if (!name.empty()) {
                            state = CopperGolem::WeatherStateFromName(StripNamespace(name));
                        } else {
                            const int ordinal = tag.GetValue<int32_t>("weather_state", 0);
                            state = ordinal >= 1 && ordinal <= 3 ? static_cast<CopperGolem::WeatherState>(ordinal)
                                                                 : CopperGolem::WeatherState::Unaffected;
                        }
                    }
                    g->SetWeatherState(state);
                }
                break;
            case EntityTypeId::Chicken:
                if (auto* c = dynamic_cast<Chicken*>(&mob)) {
                    c->SetChickenJockey(tag.GetValue<int8_t>("IsChickenJockey", 0) != 0);
                    if (tag.HasTag("variant")) c->SetVariantByte(TemperatureVariantFromId(tag.GetValue<std::string>("variant", "")));
                    if (uint8_t sv = 0; FarmSoundVariants::FromName(FarmSoundVariants::Mob::Chicken, tag.GetValue<std::string>("sound_variant", ""), sv)) c->SetSoundVariant(sv);
                }
                break;
            case EntityTypeId::Cow:
                if (auto* c = dynamic_cast<Cow*>(&mob)) {
                    if (tag.HasTag("variant")) c->SetVariantByte(TemperatureVariantFromId(tag.GetValue<std::string>("variant", "")));
                    if (uint8_t sv = 0; FarmSoundVariants::FromName(FarmSoundVariants::Mob::Cow, tag.GetValue<std::string>("sound_variant", ""), sv)) c->SetSoundVariant(sv);
                }
                break;
            case EntityTypeId::Pig:
                if (auto* p = dynamic_cast<Pig*>(&mob)) {
                    if (tag.HasTag("variant")) p->SetVariantByte(TemperatureVariantFromId(tag.GetValue<std::string>("variant", "")));
                    if (uint8_t sv = 0; FarmSoundVariants::FromName(FarmSoundVariants::Mob::Pig, tag.GetValue<std::string>("sound_variant", ""), sv)) p->SetSoundVariant(sv);
                }
                break;
            // MC MushroomCow.readAdditionalSaveData: absent or unknown →
            // Variant.DEFAULT (red).
            case EntityTypeId::Mooshroom:
                if (auto* m = dynamic_cast<Mooshroom*>(&mob)) {
                    m->SetVariant(tag.GetValue<std::string>("Type", "red") == "brown" ? Mooshroom::Variant::Brown
                                                                                     : Mooshroom::Variant::Red);
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
                if (auto* zv = dynamic_cast<ZombieVillager*>(&mob)) ReadZombieVillagerNbt(tag, *zv);
                break;
            // MC Villager / AbstractVillager (VillagerNbt.hpp).
            case EntityTypeId::Villager:
                if (auto* v = dynamic_cast<Villager*>(&mob)) ReadVillagerNbt(tag, *v);
                break;
            case EntityTypeId::WanderingTrader:
                if (auto* t = dynamic_cast<WanderingTrader*>(&mob)) ReadWanderingTraderNbt(tag, *t);
                break;
            case EntityTypeId::Fox:
                if (auto* f = dynamic_cast<Fox*>(&mob)) {
                    f->SetSleeping   (tag.GetValue<int8_t>("Sleeping", 0) != 0);
                    f->SetSitting    (tag.GetValue<int8_t>("Sitting", 0) != 0);
                    f->SetIsCrouching(tag.GetValue<int8_t>("Crouching", 0) != 0);
                    f->SetVariant(static_cast<Fox::Variant>(
                        EnumIndex(kFoxVariantNames, tag.GetValue<std::string>("Type", ""))));
                    // MC readAdditionalSaveData: clearTrusted, then each entry
                    // through addTrustedEntity.
                    f->ClearTrusted();
                    if (auto list = As<LT>(tag.GetTag("Trusted"))) {
                        for (const auto& elem : list->value) {
                            auto arr = As<::World::NBTTagIntArray>(elem);
                            if (!arr || arr->value.size() != 4) continue;
                            int32_t words[4];
                            for (int i = 0; i < 4; ++i) words[i] = arr->value[i];
                            f->AddTrustedUuid(UuidFromIntArray(words));
                        }
                    }
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
                    // "RabbitType": an unknown id is Variant.DEFAULT (brown).
                    if (tag.HasTag("RabbitType")) {
                        const int32_t id = tag.GetValue<int32_t>("RabbitType", 0);
                        r->SetVariant(Rabbit::IsValidVariant(id) ? static_cast<Rabbit::Variant>(id) : Rabbit::Variant::Brown);
                    }
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
                    if (uint8_t sv = 0; FarmSoundVariants::FromName(FarmSoundVariants::Mob::Cat, tag.GetValue<std::string>("sound_variant", ""), sv)) c->SetSoundVariant(sv);
                    // MC: `.orElse(DEFAULT_COLLAR_COLOR)` — red when absent.
                    c->SetCollarColor(static_cast<uint8_t>(
                        tag.GetValue<int8_t>("CollarColor", static_cast<int8_t>(kDyeColorRed))));
                }
                break;
            // MC Frog.readAdditionalSaveData: an unknown key keeps the
            // default (temperate), as VariantUtils.readVariant's ifPresent.
            case EntityTypeId::Frog:
                if (auto* frog = dynamic_cast<Frog*>(&mob)) {
                    Frog::Variant v;
                    if (Frog::VariantFromName(tag.GetValue<std::string>("variant", ""), v)) frog->SetVariant(v);
                }
                break;
            case EntityTypeId::Wolf:
                if (auto* wolf = dynamic_cast<Wolf*>(&mob)) {
                    // MC readAdditionalSaveData order: variant, CollarColor,
                    // anger (ReadNeutral above), sound_variant. A key the
                    // registry does not hold (puglin, or a datapack coat)
                    // leaves the default, exactly as MC's ifPresent does.
                    WolfVariants::Variant coat;
                    if (WolfVariants::FromName(tag.GetValue<std::string>("variant", ""), coat)) {
                        wolf->SetVariant(coat);
                    }
                    wolf->SetCollarColor(static_cast<uint8_t>(
                        tag.GetValue<int8_t>("CollarColor", static_cast<int8_t>(kDyeColorRed))));
                    WolfSoundVariants::SoundVariant sound;
                    if (WolfSoundVariants::FromName(tag.GetValue<std::string>("sound_variant", ""), sound)) {
                        wolf->SetSoundVariant(sound);
                    }
                    if (auto eq = As<CT>(tag.GetTag("equipment"))) {
                        if (auto body = As<CT>(eq->GetTag("body"))) {
                            wolf->SetBodyArmorItem(ReadItemStack(*body));
                        }
                    }
                }
                break;
            case EntityTypeId::Llama:
            case EntityTypeId::TraderLlama:
                if (auto* llama = dynamic_cast<Llama*>(&mob)) {
                    llama->SetTemper(tag.GetValue<int32_t>("Temper", 0));
                    llama->SetTamed (tag.GetValue<int8_t>("Tame", 0) != 0);
                    Uuid owner{};
                    if (ReadUuid(tag, "Owner", owner)) llama->SetOwnerUuid(owner);
                    if (tag.HasTag("Strength")) llama->SetStrength(tag.GetValue<int32_t>("Strength", 1));
                    // MC: Variant.LEGACY_CODEC, else Variant.DEFAULT (creamy).
                    llama->SetVariant(Llama::VariantById(tag.GetValue<int32_t>("Variant", 0)));
                    // After the strength: its columns size the chest.
                    ReadMountChest(tag, *llama);
                }
                if (auto* trader = dynamic_cast<TraderLlama*>(&mob)) {
                    trader->SetDespawnDelay(tag.GetValue<int32_t>("DespawnDelay",
                                                                  TraderLlama::kDefaultDespawnDelay));
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
                    h->SetBred      (tag.GetValue<int8_t>("Bred", 0) != 0);
                    h->SetTemper    (tag.GetValue<int32_t>("Temper", 0));
                    h->SetTamedHorse(tag.GetValue<int8_t>("Tame", 0) != 0);
                    Uuid owner{};
                    if (ReadUuid(tag, "Owner", owner)) h->SetOwnerUuid(owner);
                    // AbstractChestedHorse (the donkey, the mule).
                    ReadMountChest(tag, *h);
                }
                // MC Horse.readAdditionalSaveData.
                if (auto* horse = dynamic_cast<Horse*>(&mob)) {
                    horse->SetTypeVariant(tag.GetValue<int32_t>("Variant", 0));
                }
                // MC SkeletonHorse.readAdditionalSaveData.
                if (auto* s = dynamic_cast<SkeletonHorse*>(&mob)) {
                    s->SetTrap(tag.GetValue<int8_t>("SkeletonTrap", 0) != 0);
                    s->SetTrapTime(tag.GetValue<int32_t>("SkeletonTrapTime", 0));
                }
                break;
            case EntityTypeId::HappyGhast:
                // MC HappyGhast.readAdditionalSaveData.
                if (auto* g = dynamic_cast<HappyGhast*>(&mob)) {
                    g->SetServerStillTimeout(tag.GetValue<int32_t>("still_timeout", 0));
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
                    b->SetStayOutOfHiveCountdown(tag.GetValue<int32_t>("CannotEnterHiveTicks", 0));
                    if (auto hive = As<::World::NBTTagIntArray>(tag.GetTag("hive_pos"));
                        hive && hive->value.size() == 3) {
                        b->SetHivePos(glm::ivec3(hive->value[0], hive->value[1], hive->value[2]));
                    } else {
                        b->ClearHivePos();
                    }
                }
                break;
            case EntityTypeId::Dolphin:
                if (auto* d = dynamic_cast<Dolphin*>(&mob)) {
                    d->SetMoistness(tag.GetValue<int32_t>("Moistness", 2400));
                    d->SetGotFish(tag.GetValue<int8_t>("GotFish", 0) != 0);
                }
                break;
            case EntityTypeId::Pufferfish:
                if (auto* p = dynamic_cast<Pufferfish*>(&mob)) {
                    p->SetPuffState(tag.GetValue<int32_t>("PuffState", 0));
                }
                break;
            case EntityTypeId::TropicalFish:
                // MC readAdditionalSaveData: "Variant" orElse DEFAULT_VARIANT
                // (KOB, white, white).
                // Codec.INT reads any numeric tag (a hand-typed {Variant:5b}).
                if (auto* t = dynamic_cast<TropicalFish*>(&mob)) {
                    const ::World::NBTTagPtr v = tag.GetTag("Variant");
                    t->SetPackedVariant(v
                        ? static_cast<int32_t>(static_cast<int64_t>(NumberOf(v)))
                        : TropicalFishVariants::Pack(TropicalFishVariants::kDefaultVariant));
                }
                break;
            case EntityTypeId::Salmon:
                // MC readAdditionalSaveData: "type" orElse Variant.DEFAULT
                // (medium); an unknown name is the default too.
                if (auto* sal = dynamic_cast<Salmon*>(&mob)) {
                    const int size = Salmon::SizeFromName(tag.GetValue<std::string>("type", "medium"));
                    sal->SetSize(size >= 0 ? size : Salmon::kMedium);
                }
                break;
            case EntityTypeId::Allay:
                if (auto* a = dynamic_cast<Allay*>(&mob)) ReadAllayData(tag, *a);
                break;
            case EntityTypeId::Camel:
            case EntityTypeId::CamelHusk:   // MC CamelHusk extends Camel
                if (auto* c = dynamic_cast<Camel*>(&mob)) {
                    // MC Camel.readAdditionalSaveData: a negative tick is a
                    // seated camel — the pose goes with it.
                    const int64_t poseTick = tag.GetValue<int64_t>("LastPoseTick", 0);
                    if (poseTick < 0) c->SetPose(Pose::Sitting);
                    c->SetLastPoseChangeTick(poseTick);
                }
                break;
            case EntityTypeId::Axolotl:
                if (auto* a = dynamic_cast<Axolotl*>(&mob)) {
                    const int32_t v = tag.GetValue<int32_t>("Variant", 0);
                    a->SetVariant(static_cast<Axolotl::Variant>(
                        (v >= 0 && v < 5) ? v : 0));
                }
                break;
            // MC Parrot.readAdditionalSaveData: absent → Variant.DEFAULT
            // (red_blue); an out-of-range id CLAMPs (ByIdMap).
            case EntityTypeId::Parrot:
                if (auto* p = dynamic_cast<Parrot*>(&mob)) {
                    p->SetVariant(Parrot::VariantById(tag.GetValue<int32_t>("Variant", 0)));
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
                    c->SetPowered(tag.GetValue<int8_t>("powered", 0) != 0);
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
            case EntityTypeId::ZombieNautilus:
                if (auto* zn = dynamic_cast<ZombieNautilus*>(&mob)) {
                    // VariantUtils.readVariant: an unknown id keeps the current.
                    std::string id = tag.GetValue<std::string>("variant", "");
                    if (id.rfind("minecraft:", 0) == 0) id = id.substr(10);
                    if (id == "warm") zn->SetVariantByte(1);
                    else if (id == "temperate") zn->SetVariantByte(0);
                }
                break;
            case EntityTypeId::Shulker:
                if (auto* sh = dynamic_cast<Shulker*>(&mob)) {
                    sh->SetAttachFace(tag.GetValue<int8_t>("AttachFace", 0));
                    // Through SetRawPeekAmount, which also refreshes the
                    // closed-lid armour modifier a raw assignment would skip.
                    sh->SetRawPeekAmount(tag.GetValue<int8_t>("Peek", 0));
                    sh->SetColor(static_cast<uint8_t>(tag.GetValue<int8_t>("Color", 16)));
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
            case EntityTypeId::Painting:
                if (auto* p = dynamic_cast<Painting*>(&mob)) {
                    // MC Painting.readAdditionalSaveData. The cell is only
                    // trusted within 16 blocks of the saved position (MC
                    // BlockAttachedEntity: "Block-attached entity at invalid
                    // position"); otherwise the painting keeps the cell its
                    // position lies in.
                    static constexpr Direction kFrom2D[4] = {
                        Direction::South, Direction::West, Direction::North, Direction::East };
                    // Direction.from2DDataValue: BY_2D_DATA[abs(value % 4)].
                    const int facing = tag.GetValue<int8_t>("facing", 0) % 4;
                    const Direction direction = kFrom2D[facing < 0 ? -facing : facing];
                    glm::ivec3 cell = p->BlockPosition();
                    if (auto arr = As<::World::NBTTagIntArray>(tag.GetTag("block_pos")); arr && arr->value.size() == 3) {
                        const glm::ivec3 stored(arr->value[0], arr->value[1], arr->value[2]);
                        const glm::dvec3 d = glm::dvec3(stored) - glm::dvec3(p->BlockPosition());
                        if (glm::dot(d, d) < 16.0 * 16.0) cell = stored;
                    }
                    if (const std::string id = tag.GetValue<std::string>("variant", ""); !id.empty()) {
                        const int index = PaintingVariants::IndexOf(id);
                        if (index >= 0) p->SetVariant(index);
                    }
                    p->SetHangingPos(cell);
                    p->SetDirection(direction);
                }
                break;
            case EntityTypeId::Cushion:
                if (auto* c = dynamic_cast<Cushion*>(&mob)) {
                    // MC Cushion.readAdditionalSaveData: "color", WHITE when
                    // absent or unknown. The cell is Cushion.setPos's from the
                    // loaded position (MC keeps a "block_pos" within 16
                    // blocks, which for a cushion is that same cell; nothing
                    // but spawn protection reads it).
                    DyeColor color = Cushion::kDefaultColor;
                    if (!Cushion::ColorFromName(tag.GetValue<std::string>("color", ""), color)) {
                        color = Cushion::kDefaultColor;
                    }
                    c->SetColor(color);
                    c->SetPos(c->position);
                }
                break;
            case EntityTypeId::OminousItemSpawner:
                if (auto* o = dynamic_cast<OminousItemSpawner*>(&mob)) {
                    // MC OminousItemSpawner.readAdditionalSaveData.
                    ItemStack item;
                    if (auto stored = As<CT>(tag.GetTag("item"))) item = ReadItemStack(*stored);
                    o->SetItem(item);
                    o->SetSpawnItemAfterTicks(tag.GetValue<int64_t>("spawn_item_after_ticks", 0));
                }
                break;
            case EntityTypeId::ItemFrame:
            case EntityTypeId::GlowItemFrame:
                if (auto* f = dynamic_cast<ItemFrame*>(&mob)) {
                    // MC ItemFrame.readAdditionalSaveData (and
                    // BlockAttachedEntity's cell, trusted within 16 blocks).
                    glm::ivec3 cell = f->BlockPosition();
                    if (auto arr = As<::World::NBTTagIntArray>(tag.GetTag("block_pos")); arr && arr->value.size() == 3) {
                        const glm::ivec3 stored(arr->value[0], arr->value[1], arr->value[2]);
                        const glm::dvec3 d = glm::dvec3(stored) - glm::dvec3(f->BlockPosition());
                        if (glm::dot(d, d) < 16.0 * 16.0) cell = stored;
                    }
                    if (auto item = As<CT>(tag.GetTag("Item"))) f->SetItemSilently(ReadItemStack(*item));
                    f->SetRotation(tag.GetValue<int8_t>("ItemRotation", 0), /*updateNeighbours=*/false);
                    f->SetDropChance(tag.GetValue<float>("ItemDropChance", 1.0f));
                    // Direction.from3DDataValue: BY_3D_DATA[abs(value % 6)];
                    // default DOWN.
                    const int facing = tag.GetValue<int8_t>("Facing", 0) % 6;
                    f->SetHangingPos(cell);
                    f->SetDirection(static_cast<Direction>(facing < 0 ? -facing : facing));
                    f->SetInvisible(tag.GetValue<int8_t>("Invisible", 0) != 0);
                    f->SetFixed(tag.GetValue<int8_t>("Fixed", 0) != 0);
                }
                break;
            case EntityTypeId::ArmorStand:
                if (auto* a = dynamic_cast<ArmorStand*>(&mob)) {
                    // MC ArmorStand.readAdditionalSaveData.
                    a->SetInvisible  (tag.GetValue<int8_t>("Invisible", 0) != 0);
                    a->SetSmall      (tag.GetValue<int8_t>("Small", 0) != 0);
                    a->SetShowArms   (tag.GetValue<int8_t>("ShowArms", 0) != 0);
                    a->SetDisabledSlots(tag.GetValue<int32_t>("DisabledSlots", 0));
                    a->SetNoBasePlate(tag.GetValue<int8_t>("NoBasePlate", 0) != 0);
                    a->SetMarker     (tag.GetValue<int8_t>("Marker", 0) != 0);
                    ArmorStand::Pose pose;   // each part keeps its default when absent
                    if (auto p = As<CT>(tag.GetTag("Pose"))) {
                        const auto rot = [&](const char* name, glm::vec3& out) {
                            auto list = As<LT>(p->GetTag(name));
                            if (!list || list->value.size() != 3) return;
                            for (int i = 0; i < 3; ++i) {
                                auto f = As<::World::NBTTagFloat>(list->value[static_cast<size_t>(i)]);
                                out[i] = f ? f->value : 0.0f;
                            }
                        };
                        rot("Head",     pose.head);
                        rot("Body",     pose.body);
                        rot("LeftArm",  pose.leftArm);
                        rot("RightArm", pose.rightArm);
                        rot("LeftLeg",  pose.leftLeg);
                        rot("RightLeg", pose.rightLeg);
                    }
                    a->SetPose(pose);
                    if (auto eq = As<CT>(tag.GetTag("equipment"))) {
                        static constexpr std::pair<const char*, EquipmentSlot> kSlots[6] = {
                            {"mainhand", EquipmentSlot::MAINHAND}, {"offhand", EquipmentSlot::OFFHAND},
                            {"feet", EquipmentSlot::FEET},         {"legs", EquipmentSlot::LEGS},
                            {"chest", EquipmentSlot::CHEST},       {"head", EquipmentSlot::HEAD},
                        };
                        for (const auto& [name, slot] : kSlots) {
                            if (auto it = As<CT>(eq->GetTag(name))) a->SetItemSlot(slot, ReadItemStack(*it));
                        }
                    }
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
                    ReadCarrierInventory(tag, p->GetInventory());
                }
                break;
            case EntityTypeId::Pillager:
                if (auto* p = dynamic_cast<Pillager*>(&mob)) {
                    ReadCarrierInventory(tag, p->GetInventory());
                    // MC Pillager.readAdditionalSaveData: setCanPickUpLoot(true).
                    p->SetCanPickUpLoot(true);
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
            // MC Hoglin.readAdditionalSaveData.
            case EntityTypeId::Hoglin:
                if (auto* h = dynamic_cast<Hoglin*>(&mob)) {
                    h->SetImmuneToZombification(tag.GetValue<int8_t>("IsImmuneToZombification", 0) != 0);
                    h->SetTimeInOverworld(tag.GetValue<int32_t>("TimeInOverworld", 0));
                    h->SetCannotBeHunted(tag.GetValue<int8_t>("CannotBeHunted", 0) != 0);
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
            case EntityTypeId::SplashPotion:
                if (auto* p = dynamic_cast<ThrownSplashPotion*>(&mob)) {
                    if (auto item = As<CT>(tag.GetTag("Item"))) {
                        p->SetItem(ReadItemStack(*item));
                    }
                }
                break;
            case EntityTypeId::FireworkRocket:
                if (auto* r = dynamic_cast<FireworkRocket*>(&mob)) {
                    r->SetLife(tag.GetValue<int32_t>("Life", 0));
                    r->SetLifetime(tag.GetValue<int32_t>("LifeTime", 0));
                    // FireworksItem, or the default rocket.
                    ItemStack item;
                    if (auto stack = As<CT>(tag.GetTag("FireworksItem"))) item = ReadItemStack(*stack);
                    r->SetItem(item);
                    r->SetShotAtAngle(tag.GetValue<int8_t>("ShotAtAngle", 0) != 0);
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
                    if (auto contents = tag.GetTag("potion_contents")) {
                        c->SetPotionContents(ReadPotionContents(*contents));
                    }
                }
                break;
            default:
                break;
        }

        // The mod mobs' own fields (Mob::LoadModNbt, ModMobNbt.hpp).
        {
            ModNbtTagAdapter modIn(tag);
            mob.LoadModNbt(modIn);
        }

        // Engine extra, LAST (over whatever the type's own fields set): the
        // eased animation state (Mob::GetRenderPhase — a wolf's beg tilt and
        // shake …) the owner's client showed at Save and Quit.
        if (auto list = As<LT>(tag.GetTag("obey_render_phase"))) {
            float phase[Mob::kRenderPhaseMax] = {};
            int n = 0;
            for (const auto& elem : list->value) {
                if (n >= Mob::kRenderPhaseMax) break;
                phase[n++] = static_cast<float>(NumberOf(elem));
            }
            if (n > 0) mob.SetRenderPhase(phase, n);
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
        WriteEntityTags(w, item.tags);

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
        ReadEntityTags(tag, out.tags);
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
        WriteEntityTags(w, orb.tags);

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
        ReadEntityTags(tag, out.tags);
        return true;
    }

    // ── Scoreboard tags (MC Entity "Tags") ─────────────────────────────────

    void WriteEntityTags(Nbt::Writer& w, const EntityTags& tags) {
        // MC Entity.saveWithoutId: `if (!this.tags.isEmpty()) output.store(
        // "Tags", TAG_LIST_CODEC, List.copyOf(this.tags))`.
        if (tags.Empty()) return;
        auto list = w.BeginList("Tags", Nbt::TagType::String);
        for (const std::string& t : tags.All()) w.ListString(list, t);
        w.EndList(list);
    }

    void ReadEntityTags(const ::World::NBTTagCompound& tag, EntityTags& out) {
        // MC Entity.load: the tags are cleared, then the saved list (at most
        // MAX_ENTITY_TAG_COUNT) read back.
        out.Clear();
        auto list = std::dynamic_pointer_cast<::World::NBTTagList>(tag.GetTag("Tags"));
        if (!list) return;
        for (const auto& element : list->value) {
            auto str = std::dynamic_pointer_cast<::World::NBTTagString>(element);
            if (str) out.Add(str->value);
        }
    }

    // ── Player effect list (PlayerDataStore) ───────────────────────────────
    // The same MobEffectInstance.CODEC list LivingEntity writes, for the
    // player file, which ServerPlayer (not a LivingEntity) writes itself.
    void WriteAttributeList(Nbt::Writer& w, const AttributeMap& map) {
        const auto& all = map.All();
        if (all.empty()) return;

        // The modifier's saved Identifier: the fixed table's for the engine's
        // own permanent ones, else a permanent modifier's recorded name.
        const auto savedId = [](Attribute attribute, const AttributeModifier& mod) -> std::string {
            if (const char* mcId = PersistentModifierMcId(attribute, mod.id)) return mcId;
            if (mod.permanent) return ModifierIdName(mod.id);
            return {};
        };

        auto list = w.BeginList("attributes", Nbt::TagType::Compound);
        for (const auto& inst : all) {
            const auto& def = kAttributeTable[static_cast<size_t>(inst.GetAttribute())];
            w.ListCompoundBegin(list);
            w.String("id",   std::string(kNamespace) + std::string(def.name));
            w.Double("base", inst.GetBaseValue());
            bool anyPersistent = false;
            for (const AttributeModifier& mod : inst.Modifiers()) {
                if (!savedId(inst.GetAttribute(), mod).empty()) { anyPersistent = true; break; }
            }
            if (anyPersistent) {
                auto mods = w.BeginList("modifiers", Nbt::TagType::Compound);
                for (const AttributeModifier& mod : inst.Modifiers()) {
                    const std::string id = savedId(inst.GetAttribute(), mod);
                    if (id.empty()) continue;
                    w.ListCompoundBegin(mods);
                    w.String("id", id);
                    w.Double("amount", mod.amount);
                    w.String("operation", OperationName(mod.operation));
                    w.ListCompoundEnd(mods);
                }
                w.EndList(mods);
            }
            w.ListCompoundEnd(list);
        }
        w.EndList(list);
    }

    void ReadAttributeList(const ::World::NBTTagCompound& tag, AttributeMap& map, bool onlyRegistered,
                           bool* customized) {
        auto list = As<LT>(tag.GetTag("attributes"));
        if (!list) return;
        for (const auto& elem : list->value) {
            auto c = As<CT>(elem);
            if (!c) continue;
            Attribute attr{};
            if (!Game::AttributeFromName(c->GetValue<std::string>("id", ""), attr)) continue;
            // A mob takes only the attributes it registered: SetBaseValue on
            // an absent one would give a zombie a jump_strength row it has
            // no business owning, and MC's own apply() ignores them the same
            // way.
            if (onlyRegistered && !map.Has(attr)) continue;
            const bool syncable = Network::IsClientSyncableAttribute(attr);
            const double base = c->GetValue<double>("base", map.GetBaseValue(attr));
            if (customized && syncable && map.Has(attr) && map.GetBaseValue(attr) != base) *customized = true;
            map.SetBaseValue(attr, base);
            // The permanent modifiers: the engine's fixed ones (see
            // kPersistentModifiers) by their table id, every other
            // Identifier as a permanent named modifier (/attribute's). An
            // id MC wrote for a modifier this engine rebuilds itself (an
            // effect's, worn gear's) never reaches a save, so it is not
            // mistaken for one here.
            if (auto mods = As<LT>(c->GetTag("modifiers"))) {
                for (const auto& modElem : mods->value) {
                    auto m = As<CT>(modElem);
                    if (!m) continue;
                    const std::string name = m->GetValue<std::string>("id", "");
                    if (name.empty() || !IsValidIdentifier(name)) continue;
                    AttributeModifier mod;
                    mod.amount = m->GetValue<double>("amount", 0.0);
                    mod.operation = OperationFromName(m->GetValue<std::string>("operation", "add_value"));
                    ModifierId fixed{};
                    if (PersistentModifierFromMcId(attr, name, fixed)) {
                        mod.id = static_cast<uint32_t>(fixed);
                    } else {
                        mod.id = static_cast<uint32_t>(NamedModifierId(name));
                        mod.permanent = true;
                        if (customized && syncable) *customized = true;
                    }
                    map.RemoveModifier(attr, static_cast<ModifierId>(mod.id));
                    map.AddModifier(attr, mod);
                }
            }
        }
    }

    void WriteActiveEffects(Nbt::Writer& w, const std::vector<MobEffectInstance>& effects) {
        if (effects.empty()) return;   // MC omits the key when there are none
        auto list = w.BeginList("active_effects", Nbt::TagType::Compound);
        for (const auto& e : effects) {
            w.ListCompoundBegin(list);
            WriteEffectBody(w, e);
            w.ListCompoundEnd(list);
        }
        w.EndList(list);
    }

    std::vector<MobEffectInstance> ReadActiveEffects(const ::World::NBTTagCompound& tag) {
        std::vector<MobEffectInstance> restored;
        if (auto list = As<LT>(tag.GetTag("active_effects"))) {
            restored.reserve(list->value.size());
            for (const auto& elem : list->value) {
                auto c = As<CT>(elem);
                if (!c) continue;
                MobEffectInstance inst{};
                if (ReadEffectBody(*c, inst)) restored.push_back(std::move(inst));
            }
        }
        return restored;
    }

} // namespace Game::Anvil
