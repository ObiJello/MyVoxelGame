// File: src/common/network/packets/game/UpdateAttributesS2CPacket.hpp
//
// MC ClientboundUpdateAttributesPacket: a living entity's client-syncable
// attributes — each one's base value and modifier stack. MC sends every
// syncable attribute when a watcher starts tracking the entity
// (ServerEntity.sendPairingData) and the changed ones afterwards
// (sendDirtyEntityData). Here the mobs whose client copy needs them opt in
// (Mob::SyncsAttributesToClient — the mounts a client steers: an equine's
// rolled speed and jump move it on the steering client, its rolled health
// fills the rider's vehicle hearts), plus any mob whose attributes were
// customized (/attribute — Mob::ShouldSyncAttributes), and a change resends
// the whole set. A player's own attributes go to its own client under the
// player's id (ServerPlayer::syncAttributesToClient) — the base values and
// own modifiers its prediction folds the worn items and effects onto.
//
// Trailing (wire-safe) block: the Identifier of every modifier id in the
// packet that has one (Game::ModifierIdName), so the client can name them —
// /attribute's Tab completion of `modifier remove <id>`.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/entity/Attributes.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Network {

    // MC Attribute.isClientSyncable (Attributes.java `.setSyncable(true)`),
    // for the attributes this engine carries.
    inline bool IsClientSyncableAttribute(Game::Attribute attribute) {
        switch (attribute) {
            case Game::Attribute::AirDragModifier:
            case Game::Attribute::Armor:
            case Game::Attribute::ArmorToughness:
            case Game::Attribute::AttackSpeed:
            case Game::Attribute::Bounciness:
            case Game::Attribute::BelowNameDistance:
            case Game::Attribute::BlockBreakSpeed:
            case Game::Attribute::BlockInteractionRange:
            case Game::Attribute::BurningTime:
            case Game::Attribute::CameraDistance:
            case Game::Attribute::EntityInteractionRange:
            case Game::Attribute::ExplosionKnockbackResistance:
            case Game::Attribute::FallDamageMultiplier:
            case Game::Attribute::FlyingSpeed:
            case Game::Attribute::FrictionModifier:
            case Game::Attribute::Gravity:
            case Game::Attribute::JumpStrength:
            case Game::Attribute::Luck:
            case Game::Attribute::MaxAbsorption:
            case Game::Attribute::MaxHealth:
            case Game::Attribute::MiningEfficiency:
            case Game::Attribute::MovementEfficiency:
            case Game::Attribute::MovementSpeed:
            case Game::Attribute::NameTagDistance:
            case Game::Attribute::OxygenBonus:
            case Game::Attribute::SafeFallDistance:
            case Game::Attribute::Scale:
            case Game::Attribute::SneakingSpeed:
            case Game::Attribute::StepHeight:
            case Game::Attribute::SubmergedMiningSpeed:
            case Game::Attribute::SweepingDamageRatio:
            case Game::Attribute::WaterMovementEfficiency:
                return true;
            default:
                return false;
        }
    }

    struct UpdateAttributesS2CPacket {
        struct Entry {
            Game::Attribute                      attribute = Game::Attribute::MaxHealth;
            double                               base = 0.0;
            std::vector<Game::AttributeModifier> modifiers;
        };
        int32_t            entityId = 0;
        std::vector<Entry> attributes;
        // (modifier id, Identifier) for the modifiers above that have one.
        std::vector<std::pair<uint32_t, std::string>> modifierNames;
    };

    // The syncable attributes of `map`, in its own order.
    inline std::vector<UpdateAttributesS2CPacket::Entry> SyncableAttributes(const Game::AttributeMap& map) {
        std::vector<UpdateAttributesS2CPacket::Entry> out;
        for (const Game::AttributeInstance& inst : map.All()) {
            if (!IsClientSyncableAttribute(inst.GetAttribute())) continue;
            out.push_back({inst.GetAttribute(), inst.GetBaseValue(), inst.Modifiers()});
        }
        return out;
    }

    // A signature of the syncable attributes (FNV-1a over the bases and the
    // modifier stacks) — what the tracker compares to notice a change.
    inline uint64_t SyncableAttributesSignature(const Game::AttributeMap& map) {
        uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](const void* data, size_t size) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; ++i) {
                h ^= bytes[i];
                h *= 1099511628211ull;
            }
        };
        for (const Game::AttributeInstance& inst : map.All()) {
            if (!IsClientSyncableAttribute(inst.GetAttribute())) continue;
            const uint8_t attribute = static_cast<uint8_t>(inst.GetAttribute());
            const double base = inst.GetBaseValue();
            mix(&attribute, sizeof(attribute));
            mix(&base, sizeof(base));
            for (const Game::AttributeModifier& mod : inst.Modifiers()) {
                const uint8_t op = static_cast<uint8_t>(mod.operation);
                mix(&mod.id, sizeof(mod.id));
                mix(&mod.amount, sizeof(mod.amount));
                mix(&op, sizeof(op));
            }
        }
        return h;
    }

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const UpdateAttributesS2CPacket& p) {
            PacketBuffer b;
            b.WriteVarInt(static_cast<uint32_t>(p.entityId));
            b.WriteVarInt(static_cast<uint32_t>(p.attributes.size()));
            for (const auto& entry : p.attributes) {
                b.WriteByte(static_cast<uint8_t>(entry.attribute));
                b.WriteDouble(entry.base);
                b.WriteVarInt(static_cast<uint32_t>(entry.modifiers.size()));
                for (const Game::AttributeModifier& mod : entry.modifiers) {
                    b.WriteVarInt(mod.id);
                    b.WriteDouble(mod.amount);
                    b.WriteByte(static_cast<uint8_t>(mod.operation));
                }
            }
            // The trailing names block, from the packet's own list or, when
            // the sender left it empty, from the process-wide name table.
            std::vector<std::pair<uint32_t, std::string>> names = p.modifierNames;
            if (names.empty()) {
                for (const auto& entry : p.attributes) {
                    for (const Game::AttributeModifier& mod : entry.modifiers) {
                        std::string name = Game::ModifierIdName(mod.id);
                        if (!name.empty()) names.emplace_back(mod.id, std::move(name));
                    }
                }
            }
            b.WriteVarInt(static_cast<uint32_t>(names.size()));
            for (const auto& [id, name] : names) {
                b.WriteVarInt(id);
                b.WriteString(name);
            }
            return b.GetData();
        }

        inline UpdateAttributesS2CPacket DeserializeUpdateAttributesS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            UpdateAttributesS2CPacket p;
            p.entityId = static_cast<int32_t>(r.ReadVarInt());
            const uint32_t count = r.ReadVarInt();
            for (uint32_t i = 0; i < count && r.Remaining() > 0; ++i) {
                UpdateAttributesS2CPacket::Entry entry;
                const uint8_t attribute = r.ReadByte();
                entry.base = r.ReadDouble();
                const uint32_t modCount = r.ReadVarInt();
                for (uint32_t m = 0; m < modCount && r.Remaining() > 0; ++m) {
                    Game::AttributeModifier mod;
                    mod.id = r.ReadVarInt();
                    mod.amount = r.ReadDouble();
                    const uint8_t op = r.ReadByte();
                    mod.operation = op <= static_cast<uint8_t>(Game::AttributeOperation::AddMultipliedTotal)
                        ? static_cast<Game::AttributeOperation>(op) : Game::AttributeOperation::AddValue;
                    entry.modifiers.push_back(mod);
                }
                // An attribute this build does not know is skipped whole.
                if (attribute >= static_cast<uint8_t>(Game::Attribute::Count)) continue;
                entry.attribute = static_cast<Game::Attribute>(attribute);
                p.attributes.push_back(std::move(entry));
            }
            if (r.Remaining() > 0) {
                const uint32_t nameCount = r.ReadVarInt();
                for (uint32_t i = 0; i < nameCount && r.Remaining() > 0; ++i) {
                    const uint32_t id = r.ReadVarInt();
                    p.modifierNames.emplace_back(id, r.ReadString());
                }
            }
            return p;
        }

    } // namespace Serialization

} // namespace Network
