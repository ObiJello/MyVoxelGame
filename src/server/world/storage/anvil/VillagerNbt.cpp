// File: src/server/world/storage/anvil/VillagerNbt.cpp
#include "server/world/storage/anvil/VillagerNbt.hpp"

#include "server/world/storage/anvil/ItemStackNbt.hpp"

#include "common/core/Uuid.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/brain/Brain.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/npc/Villager.hpp"
#include "common/world/level/DimensionId.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Game::Anvil {

    namespace {

        using CT = ::World::NBTTagCompound;
        using LT = ::World::NBTTagList;

        template <typename Tag>
        std::shared_ptr<Tag> As(const ::World::NBTTagPtr& t) {
            return std::dynamic_pointer_cast<Tag>(t);
        }

        std::string Namespaced(std::string_view id) {
            return "minecraft:" + std::string(id);
        }

        std::string_view StripNamespace(std::string_view name) {
            const size_t colon = name.find(':');
            return colon == std::string_view::npos ? name : name.substr(colon + 1);
        }

        // Numeric NBT of any width — MC's getIntOr / getLongOr accept any
        // numeric tag.
        std::optional<int64_t> ReadNumber(const ::World::NBTTagPtr& t) {
            if (auto v = As<::World::NBTTagLong>(t))  return v->value;
            if (auto v = As<::World::NBTTagInt>(t))   return v->value;
            if (auto v = As<::World::NBTTagShort>(t)) return v->value;
            if (auto v = As<::World::NBTTagByte>(t))  return v->value;
            return std::nullopt;
        }
        int64_t ReadNumberOr(const CT& tag, const char* key, int64_t fallback) {
            const auto v = ReadNumber(tag.GetTag(key));
            return v ? *v : fallback;
        }

        // ── VillagerData (MC VillagerData.CODEC) ─────────────────────────

        void WriteVillagerData(Nbt::Writer& w, const VillagerData& d) {
            w.BeginCompound("VillagerData");
            w.Int("level", d.level);
            w.String("profession", Namespaced(VillagerProfessionId(d.profession)));
            w.String("type", Namespaced(VillagerTypeId(d.type)));
            w.EndCompound();
        }

        // Every field optional with MC's defaults (plains / none / 1).
        std::optional<VillagerData> ReadVillagerData(const CT& tag) {
            auto c = As<CT>(tag.GetTag("VillagerData"));
            if (!c) return std::nullopt;
            VillagerData d;
            d.type = VillagerType::Plains;
            d.profession = VillagerProfession::None;
            d.level = 1;
            VillagerType type;
            if (ParseVillagerType(StripNamespace(c->GetValue<std::string>("type", "")), type)) d.type = type;
            VillagerProfession profession;
            if (ParseVillagerProfession(StripNamespace(c->GetValue<std::string>("profession", "")), profession)) {
                d.profession = profession;
            }
            // MC VillagerData's constructor: level = max(1, level).
            d.level = static_cast<int>(std::max<int64_t>(1, ReadNumberOr(*c, "level", 1)));
            return d;
        }

        // ── Offers (MC MerchantOffers.CODEC) ─────────────────────────────

        // An ItemCost is {id, count, components} — the ItemStack shape.
        void WriteCost(Nbt::Writer& w, const char* key, const ItemCost& cost) {
            w.BeginCompound(key);
            WriteItemStackBody(w, cost.AsStack());
            w.EndCompound();
        }

        std::optional<ItemCost> ReadCost(const CT& parent, const char* key) {
            auto c = As<CT>(parent.GetTag(key));
            if (!c) return std::nullopt;
            const ItemStack stack = ReadItemStack(*c);
            if (stack.IsEmpty()) return std::nullopt;
            ItemCost cost(stack.itemId, stack.count > 0 ? stack.count : 1);
            cost.components = stack.components;
            return cost;
        }

        void WriteOffers(Nbt::Writer& w, const MerchantOffers& offers) {
            w.BeginCompound("Offers");
            auto list = w.BeginList("Recipes", Nbt::TagType::Compound);
            for (const MerchantOffer& o : offers) {
                w.ListCompoundBegin(list);
                WriteCost(w, "buy", o.GetItemCostA());
                if (o.GetItemCostB()) WriteCost(w, "buyB", *o.GetItemCostB());
                w.BeginCompound("sell");
                WriteItemStackBody(w, o.GetResult());
                w.EndCompound();
                w.Int  ("uses",            o.GetUses());
                w.Int  ("maxUses",         o.GetMaxUses());
                w.Bool ("rewardExp",       o.ShouldRewardExp());
                w.Int  ("specialPrice",    o.GetSpecialPriceDiff());
                w.Int  ("demand",          o.GetDemand());
                w.Float("priceMultiplier", o.GetPriceMultiplier());
                w.Int  ("xp",              o.GetXp());
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
            w.EndCompound();
        }

        std::optional<MerchantOffers> ReadOffers(const CT& tag) {
            auto offersTag = As<CT>(tag.GetTag("Offers"));
            if (!offersTag) return std::nullopt;
            MerchantOffers offers;
            auto recipes = As<LT>(offersTag->GetTag("Recipes"));
            if (!recipes) return offers;
            for (const auto& elem : recipes->value) {
                auto r = As<CT>(elem);
                if (!r) continue;
                // MC: buy and sell are required fields; an entry without them
                // fails to decode and is dropped.
                const auto buy = ReadCost(*r, "buy");
                auto sellTag = As<CT>(r->GetTag("sell"));
                if (!buy || !sellTag) continue;
                const ItemStack sell = ReadItemStack(*sellTag);
                if (sell.IsEmpty()) continue;
                MerchantOffer offer(*buy, ReadCost(*r, "buyB"), sell,
                                    static_cast<int>(ReadNumberOr(*r, "uses", 0)),
                                    static_cast<int>(ReadNumberOr(*r, "maxUses", 4)),
                                    static_cast<int>(ReadNumberOr(*r, "xp", 1)),
                                    r->GetValue<float>("priceMultiplier", 0.0f),
                                    static_cast<int>(ReadNumberOr(*r, "demand", 0)));
                offer.SetRewardExp(ReadNumberOr(*r, "rewardExp", 1) != 0);
                offer.SetSpecialPriceDiff(static_cast<int>(ReadNumberOr(*r, "specialPrice", 0)));
                offers.push_back(std::move(offer));
            }
            return offers;
        }

        // ── Gossips (MC GossipContainer.CODEC) ───────────────────────────

        void WriteGossips(Nbt::Writer& w, const GossipContainer& gossips) {
            auto list = w.BeginList("Gossips", Nbt::TagType::Compound);
            for (const GossipContainer::Entry& e : gossips.Unpack()) {
                w.ListCompoundBegin(list);
                int32_t words[4];
                UuidToIntArray(e.target, words);
                w.IntArray("Target", words, 4);
                w.String("Type", GetGossipTypeInfo(e.type).id);
                w.Int("Value", e.value);
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }

        void ReadGossips(const CT& tag, GossipContainer& gossips) {
            gossips.Clear();
            auto list = As<LT>(tag.GetTag("Gossips"));
            if (!list) return;
            for (const auto& elem : list->value) {
                auto c = As<CT>(elem);
                if (!c) continue;
                auto target = As<::World::NBTTagIntArray>(c->GetTag("Target"));
                GossipType type;
                if (!target || target->value.size() != 4 ||
                    !ParseGossipType(StripNamespace(c->GetValue<std::string>("Type", "")), type)) {
                    continue;
                }
                const int value = static_cast<int>(ReadNumberOr(*c, "Value", 0));
                if (value <= 0) continue;   // ExtraCodecs.POSITIVE_INT
                int32_t words[4];
                for (int i = 0; i < 4; ++i) words[i] = target->value[static_cast<size_t>(i)];
                GossipContainer::Entry entry;
                entry.target = UuidFromIntArray(words);
                entry.type = type;
                entry.value = value;
                gossips.Put(entry);
            }
        }

        // ── Brain memories (MC Brain.Packed / MemoryMap) ─────────────────

        enum class MemoryCodec : uint8_t { GlobalPos, Long, Bool };
        struct SavedMemory {
            MemoryModule module;
            const char*  id;
            MemoryCodec  codec;
        };
        // Villager.MEMORY_TYPES with a codec (MemoryModuleType.register(id,
        // codec)): the ones MC writes.
        constexpr SavedMemory kSavedMemories[] = {
            { MemoryModule::Home,                   "minecraft:home",                    MemoryCodec::GlobalPos },
            { MemoryModule::JobSite,                "minecraft:job_site",                MemoryCodec::GlobalPos },
            { MemoryModule::PotentialJobSite,       "minecraft:potential_job_site",      MemoryCodec::GlobalPos },
            { MemoryModule::MeetingPoint,           "minecraft:meeting_point",           MemoryCodec::GlobalPos },
            { MemoryModule::GolemDetectedRecently,  "minecraft:golem_detected_recently", MemoryCodec::Bool },
            { MemoryModule::DangerDetectedRecently, "minecraft:danger_detected_recently", MemoryCodec::Bool },
            { MemoryModule::LastSlept,              "minecraft:last_slept",              MemoryCodec::Long },
            { MemoryModule::LastWoken,              "minecraft:last_woken",              MemoryCodec::Long },
            { MemoryModule::LastWorkedAtPoi,        "minecraft:last_worked_at_poi",      MemoryCodec::Long },
        };

        DimensionId LevelDimension(const Villager& v) {
            const EntityLevel* level = v.Level();
            return level ? level->Dimension() : DimensionId::Overworld;
        }

    } // namespace

    // ── Villager ─────────────────────────────────────────────────────────

    void WriteVillagerBrain(Nbt::Writer& w, const Villager& villager) {
        w.BeginCompound("Brain");
        w.BeginCompound("memories");
        if (const Brain* brain = villager.GetBrain()) {
            const std::string dimension(DimensionRegistryName(LevelDimension(villager)));
            for (const SavedMemory& m : kSavedMemories) {
                if (!brain->IsRegistered(m.module) || !brain->HasMemoryValue(m.module)) continue;
                const int64_t ttl = brain->GetTimeUntilExpiry(m.module);
                switch (m.codec) {
                    case MemoryCodec::GlobalPos: {
                        const auto pos = brain->GetBlockPos(m.module);
                        if (!pos) continue;
                        w.BeginCompound(m.id);
                        w.BeginCompound("value");
                        const int32_t p[3] = { pos->x, pos->y, pos->z };
                        w.IntArray("pos", p, 3);
                        w.String("dimension", dimension);
                        w.EndCompound();
                        break;
                    }
                    case MemoryCodec::Long: {
                        const auto v = brain->GetLong(m.module);
                        if (!v) continue;
                        w.BeginCompound(m.id);
                        w.Long("value", *v);
                        break;
                    }
                    case MemoryCodec::Bool: {
                        const auto v = brain->GetBool(m.module);
                        if (!v) continue;
                        w.BeginCompound(m.id);
                        w.Bool("value", *v);
                        break;
                    }
                }
                if (ttl != ExpirableValue::kNoExpiry) w.Long("ttl", ttl);
                w.EndCompound();
            }
        }
        w.EndCompound();
        w.EndCompound();
    }

    void WriteVillagerNbt(Nbt::Writer& w, const Villager& villager) {
        // AbstractVillager: Offers (only once rolled), Inventory.
        if (const auto& offers = villager.PeekOffers()) WriteOffers(w, *offers);
        {
            auto list = w.BeginList("Inventory", Nbt::TagType::Compound);
            const SimpleContainer& inv = villager.GetInventory();
            for (int i = 0; i < inv.GetContainerSize(); ++i) {
                const ItemStack& stack = inv.GetItem(i);
                if (stack.IsEmpty()) continue;
                w.ListCompoundBegin(list);
                WriteItemStackBody(w, stack);
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }
        // Villager.
        WriteVillagerData(w, villager.GetVillagerData());
        w.Bool("VillagerDataFinalized", villager.GetVillagerDataFinalized());
        w.Byte("FoodLevel", static_cast<int8_t>(villager.GetFoodLevel()));
        WriteGossips(w, villager.GetGossips());
        w.Int ("Xp",              villager.GetVillagerXp());
        w.Long("LastRestock",     villager.GetLastRestockGameTime());
        w.Long("LastGossipDecay", villager.GetLastGossipDecayTime());
        w.Int ("RestocksToday",   villager.GetRestocksToday());
    }

    void ReadVillagerNbt(const CT& tag, Villager& villager) {
        // LivingEntity: the Brain's memories (MC makeBrain(packed)).
        if (Brain* brain = villager.GetBrain()) {
            auto brainTag = As<CT>(tag.GetTag("Brain"));
            auto memories = brainTag ? As<CT>(brainTag->GetTag("memories")) : nullptr;
            const DimensionId here = LevelDimension(villager);
            if (memories) {
                for (const SavedMemory& m : kSavedMemories) {
                    if (!brain->IsRegistered(m.module)) continue;
                    auto entry = As<CT>(memories->GetTag(m.id));
                    if (!entry) continue;
                    const int64_t ttl = entry->HasTag("ttl") ? ReadNumberOr(*entry, "ttl", 0)
                                                             : ExpirableValue::kNoExpiry;
                    switch (m.codec) {
                        case MemoryCodec::GlobalPos: {
                            auto value = As<CT>(entry->GetTag("value"));
                            auto pos = value ? As<::World::NBTTagIntArray>(value->GetTag("pos")) : nullptr;
                            if (!pos || pos->value.size() != 3) break;
                            // A claim in another dimension cannot be honoured
                            // by this level's POIs (MC's ValidateNearbyPoi
                            // would drop it on the first tick).
                            const auto dim = DimensionFromRegistryName(value->GetValue<std::string>("dimension", ""));
                            if (dim && *dim != here) break;
                            brain->SetMemoryWithExpiry(
                                m.module, glm::ivec3(pos->value[0], pos->value[1], pos->value[2]), ttl);
                            break;
                        }
                        case MemoryCodec::Long:
                            if (const auto v = ReadNumber(entry->GetTag("value"))) {
                                brain->SetMemoryWithExpiry(m.module, static_cast<int64_t>(*v), ttl);
                            }
                            break;
                        case MemoryCodec::Bool:
                            if (const auto v = ReadNumber(entry->GetTag("value"))) {
                                brain->SetMemoryWithExpiry(m.module, *v != 0, ttl);
                            }
                            break;
                    }
                }
            }
        }

        // Villager: the data FIRST — SetVillagerData drops offers on a
        // profession change, and the saved offers belong to the saved job.
        const std::optional<VillagerData> data = ReadVillagerData(tag);
        if (ReadNumberOr(tag, "VillagerDataFinalized", 0) != 0 || data) {
            villager.SetVillagerDataFinalized(true);
            VillagerData d = data ? *data : VillagerData{};
            if (!data) { d.type = VillagerType::Plains; d.profession = VillagerProfession::None; d.level = 1; }
            villager.SetVillagerData(d);
        }

        // AbstractVillager: Offers, Inventory.
        villager.SetOffers(ReadOffers(tag));
        if (auto list = As<LT>(tag.GetTag("Inventory"))) {
            SimpleContainer& inv = villager.GetInventory();
            for (int i = 0; i < inv.GetContainerSize(); ++i) inv.SetItem(i, ItemStack{});
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

        villager.SetFoodLevel(static_cast<int>(static_cast<int8_t>(ReadNumberOr(tag, "FoodLevel", 0))));
        ReadGossips(tag, villager.GetGossips());
        villager.SetVillagerXp(static_cast<int>(ReadNumberOr(tag, "Xp", 0)));
        villager.SetLastRestockGameTime(ReadNumberOr(tag, "LastRestock", 0));
        villager.SetLastGossipDecayTime(ReadNumberOr(tag, "LastGossipDecay", 0));
        villager.SetRestocksToday(static_cast<int>(ReadNumberOr(tag, "RestocksToday", 0)));

        // MC: `if (level instanceof ServerLevel) refreshBrain(serverLevel)`.
        const EntityLevel* level = villager.Level();
        if (level && !level->IsClientSide()) {
            villager.RefreshBrain();
            // The claims the memories name re-take their tickets on the
            // first tick (the POI records are rebuilt from blocks).
            villager.MarkPoiTicketsForRestore();
        }
    }

    // ── ZombieVillager ───────────────────────────────────────────────────

    void WriteZombieVillagerNbt(Nbt::Writer& w, const ZombieVillager& zombie) {
        WriteVillagerData(w, zombie.GetVillagerData());
        w.Bool("VillagerDataFinalized", zombie.GetVillagerDataFinalized());
        w.Int("Xp", 0);
    }

    void ReadZombieVillagerNbt(const CT& tag, ZombieVillager& zombie) {
        const std::optional<VillagerData> data = ReadVillagerData(tag);
        if (ReadNumberOr(tag, "VillagerDataFinalized", 0) != 0 || data) {
            zombie.SetVillagerDataFinalized(true);
            if (data) zombie.SetVillagerData(*data);
            else      zombie.RerollVillagerData();
        }
    }

} // namespace Game::Anvil
