// File: src/common/network/packets/game/MerchantPackets.hpp
//
// The trading packets:
//
//   MerchantOffersS2C  MC ClientboundMerchantOffersPacket(containerId, offers,
//                      villagerLevel, villagerXp, showProgress, canRestock) —
//                      sent right after the merchant menu opens and again
//                      whenever the villager's offers change while it is open
//                      (a trade used one up, a level-up added a tier).
//                      Wire: VarInt containerId; the offers (MC
//                      MerchantOffers.STREAM_CODEC — VarInt count, then per
//                      offer: cost A, result, optional cost B, outOfStock,
//                      uses, maxUses, xp, specialPrice, priceMultiplier,
//                      demand); VarInt level; VarInt xp; byte showProgress;
//                      byte canRestock.
//
//   SelectTradeC2S     MC ServerboundSelectTradePacket(item) — the player
//                      clicked a trade in the list. Wire: VarInt item.
#pragma once

#include "common/entity/npc/MerchantOffer.hpp"
#include "common/network/PacketRegistry.hpp"

#include <cstdint>
#include <vector>

namespace Network {

    struct MerchantOffersS2CPacket {
        uint32_t              containerId = 0;
        Game::MerchantOffers  offers;
        int32_t               villagerLevel = 0;
        int32_t               villagerXp = 0;
        bool                  showProgress = false;
        bool                  canRestock = false;
    };

    struct SelectTradeC2SPacket {
        int32_t item = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const MerchantOffersS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteVarInt(packet.containerId);
            Game::WriteMerchantOffers(buffer, packet.offers);
            buffer.WriteVarInt(static_cast<uint32_t>(packet.villagerLevel));
            buffer.WriteVarInt(static_cast<uint32_t>(packet.villagerXp));
            buffer.WriteByte(packet.showProgress ? 1 : 0);
            buffer.WriteByte(packet.canRestock ? 1 : 0);
            return buffer.GetData();
        }

        inline MerchantOffersS2CPacket DeserializeMerchantOffersS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            MerchantOffersS2CPacket packet;
            packet.containerId   = reader.ReadVarInt();
            packet.offers        = Game::ReadMerchantOffers(reader);
            packet.villagerLevel = static_cast<int32_t>(reader.ReadVarInt());
            packet.villagerXp    = static_cast<int32_t>(reader.ReadVarInt());
            packet.showProgress  = reader.ReadByte() != 0;
            packet.canRestock    = reader.ReadByte() != 0;
            return packet;
        }

        inline std::vector<uint8_t> Serialize(const SelectTradeC2SPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteVarInt(static_cast<uint32_t>(packet.item));
            return buffer.GetData();
        }

        inline SelectTradeC2SPacket DeserializeSelectTradeC2S(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            SelectTradeC2SPacket packet;
            packet.item = static_cast<int32_t>(reader.ReadVarInt());
            return packet;
        }

    } // namespace Serialization

} // namespace Network
