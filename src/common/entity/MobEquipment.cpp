// File: src/common/entity/MobEquipment.cpp
//
// Mob's humanoid equipment (the Mob members declared in Mob.hpp's equipment
// block) and the EquipmentTable operations of MobEquipment.hpp. MC sources:
// Mob.setItemSlot / getItemBySlot, DropChances, LivingEntity.
// collectEquipmentChanges (the item attribute modifiers), EquipmentUser.equip
// / resolveSlot, EquipmentSlot.limit, Mob.dropPreservedEquipment and
// Mob.dropCustomDeathLoot's equipment loop.
#include "common/entity/MobEquipment.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/Attributes.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemAttributes.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/MountInventory.hpp"
#include "common/world/block/entity/SpawnerBlockEntity.hpp"
#include "common/world/enchantment/EnchantmentDefinitions.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/world/tags/DataTags.hpp"
#include "common/world/loot/ChestLootTables.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <vector>

namespace Game {

    namespace {

        const ItemStack kEmptyStack{};

        int SlotIndex(EquipmentSlot slot) {
            const int i = static_cast<int>(slot);
            return i >= 0 && i < Mob::kEquipmentSlotCount ? i : -1;
        }

        ModifierId EquipmentModifierId(int slot, int k) {
            return static_cast<ModifierId>(static_cast<uint32_t>(ModifierId::MobEquipmentBase) +
                                           static_cast<uint32_t>(slot) * 4u + static_cast<uint32_t>(k));
        }

        // The armour slot group a piece's modifiers apply in.
        bool ArmorGroupMatches(ArmorSlotGroup group, EquipmentSlot slot) {
            switch (group) {
                case ArmorSlotGroup::Head:  return slot == EquipmentSlot::HEAD;
                case ArmorSlotGroup::Chest: return slot == EquipmentSlot::CHEST;
                case ArmorSlotGroup::Legs:  return slot == EquipmentSlot::LEGS;
                case ArmorSlotGroup::Feet:  return slot == EquipmentSlot::FEET;
                case ArmorSlotGroup::Body:  return slot == EquipmentSlot::BODY;
            }
            return false;
        }

        // MC EquipmentSlot.limit: the armour slots hold one item; the hands
        // any count.
        ItemStack LimitForSlot(EquipmentSlot slot, const ItemStack& stack) {
            ItemStack out = stack;
            if (slot != EquipmentSlot::MAINHAND && slot != EquipmentSlot::OFFHAND && out.count > 1) out.count = 1;
            return out;
        }

    } // namespace

    // ── Mob storage ─────────────────────────────────────────────────────────

    const ItemStack& Mob::GetEquipment(EquipmentSlot slot) const {
        const int i = SlotIndex(slot);
        if (i < 0 || !m_equipment) return kEmptyStack;
        return m_equipment->items[i];
    }

    void Mob::SetEquipment(EquipmentSlot slot, const ItemStack& stack) {
        const int i = SlotIndex(slot);
        if (i < 0) return;
        if (!m_equipment && stack.IsEmpty()) return;
        const ItemStack oldStack = GetEquipment(slot);
        ReplaceEquipmentStack(slot, stack);
        // MC setItemSlot → onEquipItem(slot, old, new). A copy of the new
        // stack: an override may set another slot.
        const ItemStack newStack = GetEquipment(slot);
        OnEquipItem(slot, oldStack, newStack);
    }

    void Mob::ReplaceEquipmentStack(EquipmentSlot slot, const ItemStack& stack) {
        const int i = SlotIndex(slot);
        if (i < 0) return;
        if (!m_equipment) {
            if (stack.IsEmpty()) return;
            m_equipment = std::make_unique<EquipmentSlots>();
        }
        ItemStack& held = m_equipment->items[i];
        held = stack;
        if (held.count <= 0 || held.itemId == Items::Air) held = ItemStack{};
        m_equipment->dirtyMask |= static_cast<uint8_t>(1u << i);

        // LivingEntity.collectEquipmentChanges: the old item's modifiers off,
        // the new one's on — only those of the slot group the item is worn in
        // (a helmet held in a hand adds no armour).
        for (int k = 0; k < 3; ++k) {
            const ModifierId id = EquipmentModifierId(i, k);
            m_attributes.RemoveModifier(Attribute::Armor, id);
            m_attributes.RemoveModifier(Attribute::ArmorToughness, id);
            m_attributes.RemoveModifier(Attribute::KnockbackResistance, id);
            m_attributes.RemoveModifier(Attribute::AttackDamage, id);
            m_attributes.RemoveModifier(Attribute::AttackSpeed, id);
        }
        if (!held.IsEmpty()) {
            const auto add = [this, i](Attribute attribute, int k, double amount) {
                m_attributes.AddModifier(attribute, AttributeModifier{
                    static_cast<uint32_t>(EquipmentModifierId(i, k)), amount, AttributeOperation::AddValue });
            };
            if (slot == EquipmentSlot::MAINHAND && HasItemAttackAttributes(held.itemId)) {
                float damage = 0.0f, speed = 0.0f;
                GetItemAttackAttributes(held.itemId, damage, speed);
                add(Attribute::AttackDamage, 0, damage);
                add(Attribute::AttackSpeed, 1, speed);
            }
            if (const ItemArmorRow* row = GetItemArmorAttributes(held.itemId);
                row && ArmorGroupMatches(row->slot, slot)) {
                if (row->armor != 0.0f) add(Attribute::Armor, 0, row->armor);
                if (row->armorToughness != 0.0f) add(Attribute::ArmorToughness, 1, row->armorToughness);
                if (row->knockbackResistance != 0.0f) add(Attribute::KnockbackResistance, 2, row->knockbackResistance);
            }
        }
    }

    float Mob::GetEquipmentDropChance(EquipmentSlot slot) const {
        const int i = SlotIndex(slot);
        if (i < 0 || !m_equipment) return kDefaultEquipmentDropChance;
        return m_equipment->dropChances[i];
    }

    void Mob::SetEquipmentDropChance(EquipmentSlot slot, float chance) {
        const int i = SlotIndex(slot);
        if (i < 0) return;
        if (!m_equipment) {
            if (chance == kDefaultEquipmentDropChance) return;
            m_equipment = std::make_unique<EquipmentSlots>();
        }
        m_equipment->dropChances[i] = std::max(0.0f, chance);   // ExtraCodecs.NON_NEGATIVE_FLOAT
    }

    bool Mob::HasAnyEquipment() const {
        if (!m_equipment) return false;
        for (const ItemStack& stack : m_equipment->items) {
            if (!stack.IsEmpty()) return true;
        }
        return false;
    }

    bool Mob::HasValidEquippableItemForSlot(EquipmentSlot slot) const {
        // MC Mob.hasValidEquippableItemForSlot: hasItemInSlot(slot) &&
        // isEquippableInSlot(getItemBySlot(slot), slot) — so a saddle stops
        // counting the moment the slot does (a dead or baby mount).
        const ItemStack& stack = GetEquipment(slot);
        return !stack.IsEmpty() && IsEquippableInSlot(stack, slot);
    }

    bool Mob::IsSaddled() const {
        return HasValidEquippableItemForSlot(EquipmentSlot::SADDLE);
    }

    uint8_t Mob::ConsumeEquipmentDirtyMask() {
        if (!m_equipment) return 0;
        const uint8_t mask = m_equipment->dirtyMask;
        m_equipment->dirtyMask = 0;
        return mask;
    }

    ItemStack* Mob::EquipmentInSlot(EquipmentSlot slot) {
        const int i = SlotIndex(slot);
        if (i < 0 || !m_equipment || m_equipment->items[i].IsEmpty()) return nullptr;
        return &m_equipment->items[i];
    }

    void Mob::MoveEquipmentTo(Mob& to) {
        if (!m_equipment) return;
        for (int i = 0; i < kEquipmentSlotCount; ++i) {
            const auto slot = static_cast<EquipmentSlot>(i);
            const ItemStack stack = GetEquipment(slot);
            if (stack.IsEmpty()) continue;
            to.SetEquipment(slot, stack);
            to.SetEquipmentDropChance(slot, GetEquipmentDropChance(slot));
            SetEquipment(slot, ItemStack{});
        }
    }

    float Mob::GetArmorCoverPercentage() const {
        if (!m_equipment) return 0.0f;
        int worn = 0;
        for (const EquipmentSlot slot : { EquipmentSlot::FEET, EquipmentSlot::LEGS,
                                          EquipmentSlot::CHEST, EquipmentSlot::HEAD }) {
            if (!GetEquipment(slot).IsEmpty()) ++worn;
        }
        return static_cast<float>(worn) / 4.0f;
    }

    void Mob::OnEquipItem(EquipmentSlot slot, const ItemStack& oldStack, const ItemStack& newStack) {
        // MC LivingEntity.onEquipItem: server side, a real change, not on the
        // first tick (the spawn gear finalizeSpawn hands out is silent).
        if (!m_level || m_level->IsClientSide() || firstTick) return;
        if (IsSameItemSameComponents(oldStack, newStack)) return;
        const auto equippable = newStack.IsEmpty() ? std::nullopt : newStack.get(DataComponents::EQUIPPABLE);
        if (!IsSilent() && equippable && equippable->slot == slot) {
            // getEquipSound — the mounts answer their saddle sound here.
            m_level->PlaySeededSound(nullptr, position, GetEquipSound(slot, newStack, *equippable),
                                     GetSoundSource(), 1.0f, 1.0f, m_level->Random().NextLong());
        }
        // doesEmitEquipEvent (true for every mob but the armour-less ones MC
        // exempts — none of them wear anything here).
        GameEvent(equippable ? GameEventId::Equip : GameEventId::Unequip);
    }

    // ── Loot pickup ─────────────────────────────────────────────────────────

    EquipmentSlot Mob::GetEquipmentSlotForItem(const ItemStack& stack) const {
        const auto equippable = stack.get(DataComponents::EQUIPPABLE);
        return equippable && CanUseSlot(equippable->slot) ? equippable->slot : EquipmentSlot::MAINHAND;
    }

    bool Mob::IsEquippableInSlot(const ItemStack& stack, EquipmentSlot slot) const {
        const auto equippable = stack.get(DataComponents::EQUIPPABLE);
        if (!equippable) return slot == EquipmentSlot::MAINHAND && CanUseSlot(EquipmentSlot::MAINHAND);
        // slot == equippable.slot() && canUseSlot(slot) &&
        // equippable.canBeEquippedBy(typeHolder()) — the saddle's
        // #can_equip_saddle, horse armour's #can_wear_horse_armor, a carpet's
        // llama / trader llama.
        return slot == equippable->slot && CanUseSlot(equippable->slot) && SlotIndex(slot) >= 0 &&
               equippable->CanBeEquippedBy(TypeInfo().slug);
    }

    // ── Mount equipment ─────────────────────────────────────────────────────

    std::string Mob::GetEquipSound(EquipmentSlot slot, const ItemStack& stack,
                                   const Equippable& equippable) const {
        (void)slot; (void)stack;
        return equippable.equipSound;   // LivingEntity.getEquipSound
    }

    UseResult Mob::EquipOnTarget(const Equippable& equippable, ItemStack& stack) {
        // Equippable.equipOnTarget: isEquippableInSlot(stack, slot) &&
        // !hasItemInSlot(slot) && isAlive().
        if (!IsEquippableInSlot(stack, equippable.slot) || HasItemInSlot(equippable.slot) || !IsAlive()) {
            return UseResult::Pass;
        }
        if (m_level && !m_level->IsClientSide()) {
            // setItemSlot(slot, stack.split(1)) — onEquipItem plays the
            // (mount's) equip sound — then setGuaranteedDrop.
            ItemStack one = stack;
            one.count = 1;
            stack.count -= 1;
            if (stack.count <= 0) stack.Clear();
            SetEquipment(equippable.slot, one);
            SetGuaranteedDrop(equippable.slot);
        }
        return UseResult::Success;
    }

    bool Mob::CanEquipWithDispenser(const ItemStack& stack) const {
        // LivingEntity.canEquipWithDispenser (the spectator half is a
        // player's; a mob never is one).
        if (!IsAlive() || stack.IsEmpty()) return false;
        const auto equippable = stack.get(DataComponents::EQUIPPABLE);
        if (!equippable || !equippable->dispensable) return false;
        const EquipmentSlot slot = equippable->slot;
        if (SlotIndex(slot) < 0) return false;
        if (!CanUseSlot(slot) || !equippable->CanBeEquippedBy(TypeInfo().slug)) return false;
        return GetEquipment(slot).IsEmpty() && CanDispenserEquipIntoSlot(slot);
    }

    bool Mob::CanShearEquipment(const LivingEntity& player) const {
        (void)player;
        return !IsVehicle();   // Mob.canShearEquipment
    }

    bool Mob::AttemptToShearEquipment(LivingEntity& player, ItemStack& shears) {
        if (!m_level || m_level->IsClientSide()) return false;
        // Mob.attemptToShearEquipment: EquipmentSlot.VALUES in order
        // (MAINHAND, OFFHAND, FEET, LEGS, CHEST, HEAD, BODY, SADDLE) — the
        // first canBeSheared piece not held on by Curse of Binding.
        for (int i = 0; i < kEquipmentSlotCount; ++i) {
            const auto slot = static_cast<EquipmentSlot>(i);
            const ItemStack piece = GetEquipment(slot);
            if (piece.IsEmpty()) continue;
            const auto equippable = piece.get(DataComponents::EQUIPPABLE);
            if (!equippable || !equippable->canBeSheared) continue;
            if (EnchantmentHelper::HasPreventArmorChange(piece) && !player.IsCreative()) continue;

            // Mob.shearItem: wear the shears (hand.asEquipmentSlot — the
            // interaction is always the main hand here), the offset read
            // before the slot empties, the slot cleared, SHEAR, the piece
            // dropped at the passenger attachment point.
            HurtAndBreak(shears, 1, player, EquipmentSlot::MAINHAND);
            const glm::dvec3 offset = GetPassengerAttachmentPoint(*this);
            SetEquipment(slot, ItemStack{});
            GameEvent(GameEventId::Shear, &player);
            DropItemStackAt(m_level->Dimension(), position + offset, piece);
            // this.playSound(equippable.shearingSound()).
            PlaySound(equippable->shearingSound, 1.0f, 1.0f);
            return true;
        }
        return false;
    }

    void Mob::CreateMountInventory() {
        if (MountInventory* inventory = GetMountInventory()) {
            // AbstractHorse.getInventorySize: AbstractMountInventoryMenu
            // .getInventorySize(columns) = columns * 3.
            inventory->Create(std::max(0, GetInventoryColumns()) * 3);
        }
    }

    namespace {
        bool IsArmorSlot(EquipmentSlot slot) {
            return slot == EquipmentSlot::HEAD || slot == EquipmentSlot::CHEST ||
                   slot == EquipmentSlot::LEGS || slot == EquipmentSlot::FEET;
        }

        // MC Mob.getApproximateAttributeWith: the mob's base value for the
        // attribute (0 when it has none) through the item's ATTRIBUTE_
        // MODIFIERS for `slot` — the generated weapon / armour rows here, the
        // same data SetEquipment applies.
        double ApproximateAttributeWith(const AttributeMap& attributes, const ItemStack& stack,
                                        Attribute attribute, EquipmentSlot slot) {
            double value = attributes.Has(attribute) ? attributes.GetBaseValue(attribute) : 0.0;
            if (stack.IsEmpty()) return value;
            if (attribute == Attribute::AttackDamage) {
                if (slot == EquipmentSlot::MAINHAND && HasItemAttackAttributes(stack.itemId)) {
                    float damage = 0.0f, speed = 0.0f;
                    GetItemAttackAttributes(stack.itemId, damage, speed);
                    value += damage;
                }
                return value;
            }
            if (const ItemArmorRow* row = GetItemArmorAttributes(stack.itemId);
                row && ArmorGroupMatches(row->slot, slot)) {
                if (attribute == Attribute::Armor) value += row->armor;
                else if (attribute == Attribute::ArmorToughness) value += row->armorToughness;
            }
            return value;
        }

        bool IsInItemTag(const ItemStack& stack, const char* tag) {
            return tag && !stack.IsEmpty() &&
                   DataTags::HasTag(DataTags::Registry::Item, ItemRegistry::Slug(stack.itemId), tag);
        }
    }

    bool Mob::CanReplaceCurrentItem(const ItemStack& newStack, const ItemStack& current,
                                    EquipmentSlot slot) const {
        if (current.IsEmpty()) return true;
        if (IsArmorSlot(slot)) {
            // compareArmor: Curse of Binding pins the old piece; then armour,
            // then toughness, then the tie-break.
            if (EnchantmentHelper::HasPreventArmorChange(current)) return false;
            const double newDefense = ApproximateAttributeWith(m_attributes, newStack, Attribute::Armor, slot);
            const double oldDefense = ApproximateAttributeWith(m_attributes, current, Attribute::Armor, slot);
            const double newToughness = ApproximateAttributeWith(m_attributes, newStack, Attribute::ArmorToughness, slot);
            const double oldToughness = ApproximateAttributeWith(m_attributes, current, Attribute::ArmorToughness, slot);
            if (newDefense != oldDefense) return newDefense > oldDefense;
            if (newToughness != oldToughness) return newToughness > oldToughness;
            return CanReplaceEqualItem(newStack, current);
        }
        if (slot != EquipmentSlot::MAINHAND) return false;
        // compareWeapons: the preferred weapon tag first, then attack damage.
        if (const char* preferred = GetPreferredWeaponType()) {
            const bool currentPreferred = IsInItemTag(current, preferred);
            const bool newPreferred = IsInItemTag(newStack, preferred);
            if (currentPreferred && !newPreferred) return false;
            if (!currentPreferred && newPreferred) return true;
        }
        const double newAttack = ApproximateAttributeWith(m_attributes, newStack, Attribute::AttackDamage, slot);
        const double oldAttack = ApproximateAttributeWith(m_attributes, current, Attribute::AttackDamage, slot);
        if (newAttack != oldAttack) return newAttack > oldAttack;
        return CanReplaceEqualItem(newStack, current);
    }

    bool Mob::CanReplaceEqualItem(const ItemStack& newStack, const ItemStack& current) const {
        // More enchantments, then less wear, then a custom name.
        const auto enchantmentCount = [](const ItemStack& stack) -> size_t {
            const auto enchantments = stack.get(DataComponents::ENCHANTMENTS);
            return enchantments ? enchantments->size() : 0;
        };
        const size_t newCount = enchantmentCount(newStack);
        const size_t currentCount = enchantmentCount(current);
        if (newCount != currentCount) return newCount > currentCount;
        const int newDamage = GetDamageValue(newStack);
        const int currentDamage = GetDamageValue(current);
        if (newDamage != currentDamage) return newDamage < currentDamage;
        return newStack.components.get(DataComponents::CUSTOM_NAME).has_value() &&
               !current.components.get(DataComponents::CUSTOM_NAME).has_value();
    }

    ItemStack Mob::EquipItemIfPossible(const ItemStack& stack) {
        if (!m_level || m_level->IsClientSide() || stack.IsEmpty()) return ItemStack{};
        EquipmentSlot slot = GetEquipmentSlotForItem(stack);
        if (!IsEquippableInSlot(stack, slot)) return ItemStack{};
        ItemStack current = GetEquipment(slot);
        bool canReplace = CanReplaceCurrentItem(stack, current, slot);
        // A worse armour piece still fills an empty main hand.
        if (IsArmorSlot(slot) && !canReplace) {
            slot = EquipmentSlot::MAINHAND;
            current = GetEquipment(slot);
            canReplace = current.IsEmpty();
        }
        if (!canReplace || !CanHoldItem(stack)) return ItemStack{};

        // The displaced piece drops with the slot's chance, softened by 0.1
        // (max(nextFloat() - 0.1, 0) < chance) — a guaranteed piece always.
        const double dropChance = static_cast<double>(GetEquipmentDropChance(slot));
        if (!current.IsEmpty() &&
            static_cast<double>(std::max(m_level->Random().NextFloat() - 0.1f, 0.0f)) < dropChance) {
            DropItemStackAt(m_level->Dimension(), position, current);   // spawnAtLocation
        }
        const ItemStack toEquip = LimitForSlot(slot, stack);
        SetItemSlotAndDropWhenKilled(slot, toEquip);
        SetPersistenceRequired(true);
        return toEquip;
    }

    int Mob::TakeItemEntity(int32_t itemEntityId, int count) {
        if (!m_level || count <= 0) return 0;
        const int taken = m_level->TakeFromItemEntity(itemEntityId, count);
        // MC Mob.take → LivingEntity.take: the take packet to the watchers.
        if (taken > 0) m_level->NoteItemEntityTaken(itemEntityId, GetId(), taken);
        return taken;
    }

    void Mob::PickUpItem(int32_t itemEntityId, const ItemStack& stack) {
        // MC Mob.pickUpItem: equip a copy; what was equipped leaves the item
        // entity (take + shrink — the entity despawns once empty).
        const ItemStack equipped = EquipItemIfPossible(stack);
        if (equipped.IsEmpty()) return;
        OnItemPickup(itemEntityId, equipped);
        TakeItemEntity(itemEntityId, equipped.count);
    }

    void Mob::TickLooting() {
        if (!m_level || m_level->IsClientSide()) return;
        if (!CanPickUpLoot() || !IsAlive() || IsDeadOrDying() || !m_level->MobGriefing()) return;
        const glm::ivec3 reach = GetPickupReach();
        AABBd box = GetAABBd();
        box.min -= glm::dvec3(reach);
        box.max += glm::dvec3(reach);
        std::vector<EntityLevel::NearbyItemEntity> items;
        m_level->GetItemEntitiesInBox(box, items);
        for (const auto& item : items) {
            // !isRemoved && !getItem().isEmpty() && !hasPickUpDelay.
            if (!item.canPickUp) continue;
            const ItemStack* live = m_level->GetItemEntityStack(item.id);
            if (!live || live->IsEmpty()) continue;
            const ItemStack snapshot = *live;
            if (!WantsToPickUp(snapshot)) continue;
            PickUpItem(item.id, snapshot);
            if (!CanPickUpLoot() || IsRemoved()) break;
        }
    }

    // ── Spawn gear ──────────────────────────────────────────────────────────

    DifficultyInstance Mob::CurrentDifficulty() const {
        if (!m_level) return DifficultyInstance{};
        return m_level->GetCurrentDifficultyAt(BlockPosition());
    }

    ItemID Mob::GetEquipmentForSlot(EquipmentSlot slot, int type) {
        // MC Mob.getEquipmentForSlot, 26.3 table (copper sits between leather
        // and gold).
        static constexpr ItemID kHead[6] = { Items::LeatherHelmet, Items::CopperHelmet, Items::GoldenHelmet,
                                             Items::ChainmailHelmet, Items::IronHelmet, Items::DiamondHelmet };
        static constexpr ItemID kChest[6] = { Items::LeatherChestplate, Items::CopperChestplate,
                                              Items::GoldenChestplate, Items::ChainmailChestplate,
                                              Items::IronChestplate, Items::DiamondChestplate };
        static constexpr ItemID kLegs[6] = { Items::LeatherLeggings, Items::CopperLeggings, Items::GoldenLeggings,
                                             Items::ChainmailLeggings, Items::IronLeggings, Items::DiamondLeggings };
        static constexpr ItemID kFeet[6] = { Items::LeatherBoots, Items::CopperBoots, Items::GoldenBoots,
                                             Items::ChainmailBoots, Items::IronBoots, Items::DiamondBoots };
        if (type < 0 || type > 5) return Items::Air;
        switch (slot) {
            case EquipmentSlot::HEAD:  return kHead[type];
            case EquipmentSlot::CHEST: return kChest[type];
            case EquipmentSlot::LEGS:  return kLegs[type];
            case EquipmentSlot::FEET:  return kFeet[type];
            default:                   return Items::Air;
        }
    }

    void Mob::PopulateDefaultEquipmentSlots(JavaRandom& random, const DifficultyInstance& difficulty) {
        if (!(random.NextFloat() < 0.15f * difficulty.GetSpecialMultiplier())) return;
        int armorType = random.NextInt(3);
        for (int i = 1; static_cast<float>(i) <= 3.0f; ++i) {
            if (random.NextFloat() < 0.1087f) ++armorType;
        }
        const float partialChance =
            (m_level && m_level->GetDifficulty() == Difficulty::Hard) ? 0.1f : 0.25f;
        bool first = true;
        // EQUIPMENT_POPULATION_ORDER.
        static constexpr EquipmentSlot kOrder[4] = {
            EquipmentSlot::HEAD, EquipmentSlot::CHEST, EquipmentSlot::LEGS, EquipmentSlot::FEET };
        for (const EquipmentSlot slot : kOrder) {
            const bool empty = GetEquipment(slot).IsEmpty();
            if (!first && random.NextFloat() < partialChance) break;
            first = false;
            if (empty) {
                const ItemID equip = GetEquipmentForSlot(slot, armorType);
                if (equip != Items::Air) SetEquipment(slot, ItemStack(equip, 1));
            }
        }
    }

    void Mob::PopulateDefaultEquipmentEnchantments(JavaRandom& random, const DifficultyInstance& difficulty) {
        EnchantSpawnedWeapon(random, difficulty);
        // EquipmentSlot.VALUES order, HUMANOID_ARMOR type: FEET, LEGS,
        // CHEST, HEAD.
        for (const EquipmentSlot slot : { EquipmentSlot::FEET, EquipmentSlot::LEGS,
                                          EquipmentSlot::CHEST, EquipmentSlot::HEAD }) {
            EnchantSpawnedArmor(random, slot, difficulty);
        }
    }

    void Mob::EnchantSpawnedWeapon(JavaRandom& random, const DifficultyInstance& difficulty) {
        EnchantSpawnedEquipment(EquipmentSlot::MAINHAND, random, 0.25f, difficulty);
    }

    void Mob::EnchantSpawnedArmor(JavaRandom& random, EquipmentSlot slot, const DifficultyInstance& difficulty) {
        EnchantSpawnedEquipment(slot, random, 0.5f, difficulty);
    }

    void Mob::EnchantSpawnedEquipment(EquipmentSlot slot, JavaRandom& random, float chance,
                                      const DifficultyInstance& difficulty) {
        ItemStack stack = GetEquipment(slot);
        if (stack.IsEmpty() || !(random.NextFloat() < chance * difficulty.GetSpecialMultiplier())) return;
        // EnchantmentHelper.enchantItemFromProvider(MOB_SPAWN_EQUIPMENT):
        // data/minecraft/enchantment_provider/mob_spawn_equipment.json —
        // by_cost_with_difficulty over #on_mob_spawn_equipment, min_cost 5,
        // max_cost_span 17: cost = randomBetweenInclusive(5, 5 +
        // (int)(special * 17)), selectEnchantment at that cost, each result
        // upgraded onto the stack.
        static const std::vector<EnchantmentId> kCandidates =
            EnchantmentDefinitions::ResolveTagOrdered("minecraft:on_mob_spawn_equipment");
        constexpr int kMinCost = 5, kMaxCostSpan = 17;
        const int maxCost = kMinCost + static_cast<int>(difficulty.GetSpecialMultiplier() * static_cast<float>(kMaxCostSpan));
        const int cost = random.NextInt(maxCost - kMinCost + 1) + kMinCost;
        for (const EnchantmentInstance& instance : EnchantmentHelper::SelectEnchantment(random, stack, cost, kCandidates)) {
            EnchantmentHelper::Enchant(stack, instance.id, instance.level);
        }
        SetEquipment(slot, stack);
    }

    bool Mob::IsHalloween() {
        // MC SpecialDates.isHalloween (26.3): MonthDay October 31, now.
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        return local.tm_mon == 9 && local.tm_mday == 31;
    }

    void Mob::MaybeWearHalloweenHead(JavaRandom& random) {
        if (!GetEquipment(EquipmentSlot::HEAD).IsEmpty() || !IsHalloween()) return;
        if (!(random.NextFloat() < 0.25f)) return;
        const BlockID head = random.NextFloat() < 0.1f ? BlockID::JackOLantern : BlockID::CarvedPumpkin;
        SetEquipment(EquipmentSlot::HEAD, ItemStack(head, 1));
        SetEquipmentDropChance(EquipmentSlot::HEAD, 0.0f);
    }

    // ── MobEquipment ────────────────────────────────────────────────────────

    namespace MobEquipment {

        void Equip(Mob& mob, const std::string& lootTable, bool uniformDropChance, const float dropChances[8]) {
            (void)uniformDropChance;   // the per-slot array already says it
            EntityLevel* level = mob.Level();
            if (!level || level->IsClientSide() || lootTable.empty()) return;

            // The EQUIPMENT parameter set: ORIGIN at the mob, THIS_ENTITY it;
            // the level random (no table seed).
            ChestLoot::LootLevelContext context;
            context.dimensionId = DimensionToRaw(level->Dimension());
            context.origin = mob.position;
            std::vector<ItemStack> possibleEquipment;
            if (!ChestLoot::GetRandomItems(lootTable, level->Random(), 0.0f, possibleEquipment, &context)) return;

            std::vector<EquipmentSlot> insertedIntoSlots;
            const auto inserted = [&insertedIntoSlots](EquipmentSlot slot) {
                return std::find(insertedIntoSlots.begin(), insertedIntoSlots.end(), slot) != insertedIntoSlots.end();
            };
            for (const ItemStack& toEquip : possibleEquipment) {
                if (toEquip.IsEmpty()) continue;
                // EquipmentUser.resolveSlot: the item's EQUIPPABLE slot when
                // still free, else the main hand when still free.
                std::optional<EquipmentSlot> slot;
                if (const auto equippable = toEquip.get(DataComponents::EQUIPPABLE)) {
                    if (!inserted(equippable->slot)) slot = equippable->slot;
                } else if (!inserted(EquipmentSlot::MAINHAND)) {
                    slot = EquipmentSlot::MAINHAND;
                }
                if (!slot) continue;
                // BODY / SADDLE are not humanoid slots; nothing wears them here.
                if (SlotIndex(*slot) >= 0) {
                    mob.SetEquipment(*slot, LimitForSlot(*slot, toEquip));
                    const float chance = dropChances[static_cast<int>(*slot)];
                    if (!std::isnan(chance)) mob.SetEquipmentDropChance(*slot, chance);
                }
                insertedIntoSlots.push_back(*slot);
            }
        }

        void EquipFromSpawnData(Mob& mob, const SpawnData& data) {
            if (!data.hasEquipment) return;
            Equip(mob, data.equipmentLootTable, data.equipmentUniformDrop, data.equipmentDropChances);
        }

        void DropPreservedEquipment(Mob& mob) {
            EntityLevel* level = mob.Level();
            if (!level || level->IsClientSide()) return;
            for (int i = 0; i < Mob::kEquipmentSlotCount; ++i) {
                const auto slot = static_cast<EquipmentSlot>(i);
                const ItemStack stack = mob.GetEquipment(slot);
                if (stack.IsEmpty() || !(mob.GetEquipmentDropChance(slot) > 1.0f)) continue;
                mob.SetEquipment(slot, ItemStack{});
                DropItemStackAt(level->Dimension(), mob.position, stack);   // spawnAtLocation
            }
        }

        void DropEquipmentOnDeath(Mob& mob, bool killedByPlayer, int lootingLevel, bool killerIsPlayer) {
            EntityLevel* level = mob.Level();
            if (!level || level->IsClientSide() || !mob.HasAnyEquipment()) return;
            JavaRandom& random = level->Random();
            for (int i = 0; i < Mob::kEquipmentSlotCount; ++i) {
                const auto slot = static_cast<EquipmentSlot>(i);
                ItemStack stack = mob.GetEquipment(slot);
                float dropChance = mob.GetEquipmentDropChance(slot);
                if (dropChance == 0.0f) continue;
                const bool preserve = dropChance > 1.0f;
                // EnchantmentHelper.processEquipmentDropChance: Looting's
                // equipment_drops — +1% per level, for a player killer.
                if (killerIsPlayer && lootingLevel > 0) dropChance += 0.01f * static_cast<float>(lootingLevel);
                if (stack.IsEmpty()) continue;
                if (EnchantmentHelper::HasPreventEquipmentDrop(stack)) continue;
                if (!killedByPlayer && !preserve) continue;
                if (!(random.NextFloat() < dropChance)) continue;
                if (!preserve && IsDamageableItem(stack)) {
                    // A worn drop: maxDamage - nextInt(1 + nextInt(max(maxDamage - 3, 1))).
                    const int maxDamage = GetMaxDamage(stack);
                    const int inner = random.NextInt(std::max(maxDamage - 3, 1));
                    SetDamageValue(stack, maxDamage - random.NextInt(1 + inner));
                }
                DropItemStackAt(level->Dimension(), mob.position, stack);
                mob.SetEquipment(slot, ItemStack{});
            }
        }

        int ExperienceBonus(const Mob& mob, JavaRandom& random) {
            if (!mob.HasAnyEquipment()) return 0;
            int bonus = 0;
            for (int i = 0; i < Mob::kEquipmentSlotCount; ++i) {
                const auto slot = static_cast<EquipmentSlot>(i);
                if (!mob.GetEquipment(slot).IsEmpty() && mob.GetEquipmentDropChance(slot) <= 1.0f) {
                    bonus += 1 + random.NextInt(3);
                }
            }
            return bonus;
        }

    } // namespace MobEquipment

} // namespace Game
