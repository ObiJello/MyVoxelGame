// File: src/server/entity/ServerEntityTracker.cpp
#include "server/entity/ServerEntityTracker.hpp"
#include "server/world/ticketing/ChunkTicketManager.hpp"
#include "common/entity/FallingBlockEntity.hpp"
#include "common/entity/PrimedTnt.hpp"
#include "common/entity/ArmorStand.hpp"
#include "common/entity/vehicle/VehicleEntity.hpp"
#include "common/network/packets/game/VehiclePackets.hpp"
#include "server/entity/PlayerRiding.hpp"
#include "common/entity/decoration/ItemFrame.hpp"
#include "common/entity/OminousItemSpawner.hpp"
#include "common/network/packets/game/ItemFrameDataS2CPacket.hpp"
#include "common/network/packets/game/FishingHookDataS2CPacket.hpp"
#include "common/entity/projectile/FishingHook.hpp"
#include "common/network/packets/game/SetEntityLinkS2CPacket.hpp"
#include "common/network/packets/game/BodyArmorS2CPacket.hpp"
#include "common/network/packets/game/UpdateAttributesS2CPacket.hpp"
#include "common/network/packets/game/FireworkRocketDataS2CPacket.hpp"
#include "common/entity/projectile/FireworkRocket.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/network/packets/game/ArmorStandDataS2CPacket.hpp"
#include "common/entity/EndCrystal.hpp"
#include "common/network/packets/game/DragonPackets.hpp"
#include "common/network/packets/game/RemoveEntitiesS2CPacket.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/core/Mth.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <algorithm>
#include <climits>
#include <cmath>

namespace Server {

    namespace {
        constexpr uint8_t kFlagBaby       = 0x01;
        constexpr uint8_t kFlagAggressive = 0x02;
        constexpr uint8_t kFlagOnFire     = 0x04;
        constexpr uint8_t kFlagAgeLocked  = 0x08;   // MC AgeableMob.AGE_LOCKED
        // MC DATA_LIVING_ENTITY_FLAGS bits 1 (using an item) and 2 (with the
        // off hand) — a spear-charging mob's lowered spear.
        constexpr uint8_t kFlagUsingItem  = 0x10;
        constexpr uint8_t kFlagUseOffhand = 0x20;

        int64_t Encode(double v) { return Network::EncodeEntityPos(v); }
    }

    uint8_t ServerEntityTracker::PackFlags(const Game::Mob& mob) {
        uint8_t flags = 0;
        if (mob.IsBaby())       flags |= kFlagBaby;
        if (mob.IsAggressive()) flags |= kFlagAggressive;
        if (mob.IsOnFire())     flags |= kFlagOnFire;
        if (mob.IsAgeLocked())  flags |= kFlagAgeLocked;
        if (mob.IsUsingItem()) {
            flags |= kFlagUsingItem;
            if (mob.GetUsedItemHand() == Game::EquipmentSlot::OFFHAND) flags |= kFlagUseOffhand;
        }
        return flags;
    }

    Network::ArmorStandDataS2CPacket ServerEntityTracker::BuildArmorStandData(const Game::ArmorStand& stand,
                                                                             int32_t id) {
        Network::ArmorStandDataS2CPacket p;
        p.entityId = id;
        const Game::ArmorStand::Pose& pose = stand.GetPose();
        p.poses = { pose.head, pose.body, pose.leftArm, pose.rightArm, pose.leftLeg, pose.rightLeg };
        static constexpr Game::EquipmentSlot kSlots[6] = {
            Game::EquipmentSlot::MAINHAND, Game::EquipmentSlot::OFFHAND, Game::EquipmentSlot::FEET,
            Game::EquipmentSlot::LEGS,     Game::EquipmentSlot::CHEST,   Game::EquipmentSlot::HEAD,
        };
        for (int i = 0; i < 6; ++i) p.equipment[static_cast<size_t>(i)] = stand.GetItemBySlot(kSlots[i]);
        return p;
    }

    uint8_t ServerEntityTracker::VariantData(const Game::Mob& mob) {
        // Whatever the type says its variant is — a sheep's wool byte, a
        // slime's size. Zero for everything without one.
        return mob.GetVariantByte();
    }

    namespace {
        // A firework rocket's synched data (FireworkRocketDataS2CPacket.hpp).
        Network::FireworkRocketDataS2CPacket BuildFireworkRocketData(const Game::FireworkRocket& rocket, int32_t id) {
            Network::FireworkRocketDataS2CPacket p;
            p.entityId = id;
            p.item = rocket.GetItem();
            p.attachedToId = rocket.GetAttachedToId();
            p.shotAtAngle = rocket.IsShotAtAngle();
            p.lifetime = rocket.GetLifetime();
            return p;
        }

        // A fishing bobber's synched state (FishingHookDataS2CPacket.hpp).
        Network::FishingHookDataS2CPacket BuildFishingHookData(const Game::FishingHook& hook, int32_t id) {
            Network::FishingHookDataS2CPacket p;
            p.entityId = id;
            p.ownerId  = hook.GetOwnerNetId();
            p.hookedId = hook.GetHookedNetId();
            if (hook.IsBiting()) p.flags |= Network::FishingHookDataS2CPacket::kFlagBiting;
            if (hook.GetOwnerHand() != 0) p.flags |= Network::FishingHookDataS2CPacket::kFlagOffhand;
            return p;
        }
    }

    Network::AddEntityS2CPacket ServerEntityTracker::BuildAddPacket(const Game::Mob& mob,
                                                                    const glm::dvec3& base) {
        Network::AddEntityS2CPacket p;
        p.entityId   = mob.GetId();
        p.entityType = static_cast<uint16_t>(mob.GetType());
        // The DELTA BASE, not the live position.
        //
        // Movement packets carry an offset from the last position the server
        // sent, and that base is shared by every watcher. A client seeded from
        // the live position instead would apply subsequent deltas against a
        // different origin than the server encoded them from — so a player who
        // started tracking mid-flight would see the mob drift further and
        // further from where it actually is. MC's sendPairingData sends
        // positionCodec.getBase() for exactly this reason.
        p.position   = base;
        p.velocity   = glm::vec3(mob.velocity);
        p.yRot       = Game::Mth::PackDegrees(mob.yRot);
        p.xRot       = Game::Mth::PackDegrees(mob.xRot);
        p.yHeadRot   = Game::Mth::PackDegrees(mob.GetYHeadRot());
        // The exact angles and the body yaw (appended field — see the
        // packet): what the entity looks like on first sight is what it
        // looks like here, to the degree, body included.
        p.hasExactRot   = true;
        p.exactYRot     = mob.yRot;
        p.exactXRot     = mob.xRot;
        p.exactYHeadRot = mob.GetYHeadRot();
        p.exactYBodyRot = mob.yBodyRot;
        p.walkPosition  = mob.walkAnimation.position;
        p.walkSpeed     = mob.walkAnimation.speed;
        p.walkSpeedOld  = mob.walkAnimation.speedOld;
        p.walkScale     = mob.walkAnimation.positionScale;
        p.animAge       = mob.viewAge >= 0 ? mob.viewAge + mob.tickCount : -1;
        {
            float phase[Game::Mob::kRenderPhaseMax] = {};
            const int n = std::clamp(mob.GetRenderPhase(phase), 0, Game::Mob::kRenderPhaseMax);
            p.renderPhaseCount = static_cast<uint8_t>(n);
            for (int i = 0; i < n; ++i) p.renderPhase[i] = phase[i];
        }
        p.health     = mob.GetHealth();
        p.flags      = PackFlags(mob);
        p.variantData = VariantData(mob);
        p.pose       = static_cast<uint8_t>(mob.GetPose());
        p.animState  = mob.GetAnimStateByte();
        // Riding link on first sight — MC sends ClientboundSetPassengersPacket
        // in sendPairingData; here the same fact rides the add packet.
        p.vehicleId  = mob.GetVehicle() ? mob.GetVehicle()->GetId() : -1;
        p.scale      = mob.scale;
        // MC sendPairingData's entity data: the effect particles and the
        // invisible / glowing flags on first sight.
        {
            const Game::EffectVisuals& visuals = mob.GetEffectVisuals();
            p.effectFlags     = visuals.flags;
            p.effectParticles = visuals.particles;
        }
        // MC DATA_CUSTOM_NAME / DATA_CUSTOM_NAME_VISIBLE on first sight.
        p.customName        = mob.GetCustomName();
        p.customNameVisible = mob.IsCustomNameVisible();

        // The block a block-shaped entity carries. Sent once, on the add
        // packet, because neither entity's state changes after spawn — a
        // falling sand block is sand for its whole life, and a primed TNT is
        // TNT. (The TNT's FUSE does change, and rides `animState` above.)
        if (const auto* falling = dynamic_cast<const Game::FallingBlockEntity*>(&mob)) {
            p.blockStateRaw = falling->CarriedState().RawId();
        } else if (const auto* tnt = dynamic_cast<const Game::PrimedTnt*>(&mob)) {
            p.blockStateRaw = tnt->CarriedState().RawId();
        } else {
            // A mob whose carried block can change (the sulfur cube): the
            // same field on first sight, then SetEntityData's copy on change.
            p.blockStateRaw = mob.GetCarriedBlockRaw();
        }
        return p;
    }

    void ServerEntityTracker::EmitTo(uint32_t connectionId, Network::PacketId id,
                                     std::vector<uint8_t> payload,
                                     EntityPacketOut::Kind kind,
                                     std::vector<EntityPacketOut>& out) {
        EntityPacketOut packet;
        packet.kind = kind;
        packet.connectionId = connectionId;
        packet.packetId = id;
        packet.payload = std::move(payload);
        out.push_back(std::move(packet));
    }

    namespace {
        // A boat's / minecart's synched entity data (VehicleDataS2C).
        std::vector<uint8_t> VehicleDataPayload(const Game::VehicleEntity& vehicle, int32_t id) {
            Game::VehicleSyncedData d;
            vehicle.FillSyncedData(d);
            Network::VehicleDataS2CPacket p;
            p.entityId = id;
            p.hurtTime = d.hurtTime;
            p.hurtDir = d.hurtDir;
            p.damage = d.damage;
            p.paddleLeft = d.paddleLeft;
            p.paddleRight = d.paddleRight;
            p.paddleReverse = d.paddleReverse;
            p.bubbleTime = d.bubbleTime;
            p.hasCustomDisplay = d.hasCustomDisplay;
            p.customDisplay = d.customDisplay;
            p.displayOffset = d.displayOffset;
            p.hasFuel = d.hasFuel;
            return Network::Serialization::Serialize(p);
        }

        // MC ClientboundSetPassengersPacket for a vehicle: the passengers in
        // seat order, a player by its player id.
        std::vector<uint8_t> SetPassengersPayload(const Game::VehicleEntity& vehicle, int32_t id) {
            Network::SetPassengersS2CPacket p;
            p.vehicleId = id;
            vehicle.CollectPassengerWireIds(p.passengerIds, &PlayerRiding::PlayerIdOfView);
            return Network::Serialization::Serialize(p);
        }

        // A mob's passengers as the wire names them (players by player id).
        std::vector<int32_t> PassengerWireIds(const Game::Entity& vehicle) {
            std::vector<int32_t> ids;
            for (const Game::Entity* p : vehicle.GetPassengers()) {
                if (!p) continue;
                if (p->IsPlayer()) {
                    const int32_t pid = PlayerRiding::PlayerIdOfView(*p);
                    if (pid > 0) ids.push_back(pid);
                } else {
                    ids.push_back(p->GetId());
                }
            }
            return ids;
        }
        bool HasPlayerWireId(const std::vector<int32_t>& ids) {
            for (int32_t id : ids) if (id > 0 && id < Game::kItemEntityIdBase) return true;
            return false;
        }

        // MC ChunkMap.TrackedEntity.getEffectiveRange — this entity's own
        // tracking range, widened to the largest of anything riding it, so a
        // small mob on a big vehicle is not culled before its ride is. MC
        // walks getIndirectPassengers(); the recursion here is the same walk.
        int EffectiveTrackingRangeBlocks(const Game::Entity& e) {
            int best = e.TypeInfo().clientTrackingRange * 16;
            for (const Game::Entity* p : e.GetPassengers()) {
                if (!p) continue;
                best = std::max(best, EffectiveTrackingRangeBlocks(*p));
            }
            return best;
        }

    } // namespace

    void ServerEntityTracker::Tick(const MobManager& mobs, ServerLevelBridge& level,
                                   const std::vector<TrackedPlayer>& players,
                                   const ChunkTicketManager* tickets,
                                   std::vector<EntityPacketOut>& out) {
        PROFILE_ZONE_N("EntityTracker");

        // Per-tick move batching: one MoveEntityS2C per recipient rather than
        // one per mob. A player watching 60 mobs gets one packet, not 60.
        std::unordered_map<uint32_t, Network::MoveEntityS2CPacket> moveBatches;

        // Did the player set change since last tick? Only then does every
        // tracked mob need its watcher set scrubbed of departed connections.
        bool playersChanged = players.size() != m_lastPlayerIds.size();
        if (!playersChanged) {
            for (size_t i = 0; i < players.size(); ++i) {
                if (players[i].connectionId != m_lastPlayerIds[i]) { playersChanged = true; break; }
            }
        }
        if (playersChanged) {
            m_lastPlayerIds.clear();
            for (const auto& p : players) m_lastPlayerIds.push_back(p.connectionId);
        }

        // Per-chunk gates, memoised on the last chunk seen. Mobs are walked
        // in insertion order, which for anything spawned in bulk — a
        // collapsing structure, a summoned pile — means long runs in the same
        // chunk, so the two hash lookups per mob per player become one per
        // run. At a hundred thousand entities the tracker was 18 ms a tick.
        Game::Math::ChunkPos memoChunk{INT32_MIN, INT32_MIN};
        bool memoTicking = false;
        thread_local std::vector<uint8_t> t_memoSent;
        t_memoSent.assign(players.size(), 0);

        // A connection may appear more than once — one entry per chunk loader
        // (its own view plus portal far sides). Visibility is the OR over a
        // connection's entries, decided once per connection, so the second
        // entry can never undo what the first added in the same tick.
        thread_local std::vector<size_t>  t_firstOf;   // index of the first entry with this connection
        thread_local std::vector<uint8_t> t_anyInRange;
        t_firstOf.assign(players.size(), 0);
        for (size_t i = 0; i < players.size(); ++i) {
            t_firstOf[i] = i;
            for (size_t j = 0; j < i; ++j) {
                if (players[j].connectionId == players[i].connectionId) { t_firstOf[i] = t_firstOf[j]; break; }
            }
        }

        // The ordered list, not the node map: sequential memory instead of a
        // cache miss per mob.
        for (Game::Mob* mobPtr : mobs.List()) {
            const Game::Mob& mob = *mobPtr;
            const int32_t id = mob.GetId();
            const auto [trackedIt, firstSight] = m_tracked.try_emplace(id);
            Tracked& tracked = trackedIt->second;

            // A parked mob (see PrimedTnt::Tick) moves nothing and sends
            // nothing; run its full bookkeeping — the per-player visibility
            // walk included — on every fourth tick only, staggered by id.
            // Worst case a player walking up to a parked pile sees entities
            // appear 150 ms late. At a million parked TNT this is most of
            // the tracker's tick.
            if (!firstSight && mob.physicsParked && !mob.needsSync && !mob.hurtMarked &&
                mob.GetLeashHolderNetId() == tracked.lastLeashHolderId &&
                ((static_cast<uint32_t>(tracked.tickCount) + static_cast<uint32_t>(id)) & 3u) != 0u) {
                ++tracked.tickCount;
                continue;
            }

            if (firstSight) {
                // Seed the delta base from where the mob actually is, so the
                // first AddEntity and the first delta agree.
                tracked.baseX = Encode(mob.position.x);
                tracked.baseY = Encode(mob.position.y);
                tracked.baseZ = Encode(mob.position.z);
                tracked.lastYRot = Game::Mth::PackDegrees(mob.yRot);
                tracked.lastXRot = Game::Mth::PackDegrees(mob.xRot);
                tracked.lastYHeadRot = Game::Mth::PackDegrees(mob.GetYHeadRot());
                tracked.wasOnGround = mob.onGround;
            }

            const glm::dvec3 basePos(Network::DecodeEntityPos(tracked.baseX),
                                     Network::DecodeEntityPos(tracked.baseY),
                                     Network::DecodeEntityPos(tracked.baseZ));

            // ── Watch set ──────────────────────────────────────────────────
            // MC ChunkMap.TrackedEntity.updatePlayer:1388-1409.
            const int effectiveRange = EffectiveTrackingRangeBlocks(mob);

            const Game::Math::ChunkPos mobChunk{
                static_cast<int>(std::floor(mob.position.x)) >> 4,
                static_cast<int>(std::floor(mob.position.z)) >> 4};
            if (mobChunk.x != memoChunk.x || mobChunk.z != memoChunk.z) {
                memoChunk = mobChunk;
                memoTicking = (tickets == nullptr) ||
                              tickets->IsEntityTickingAfterUpdates(mobChunk);
                for (size_t pi = 0; pi < players.size(); ++pi) {
                    const auto& p = players[pi];
                    t_memoSent[pi] = (p.sentChunks == nullptr) ||
                                     (p.sentChunks->count(mobChunk) != 0) ? 1 : 0;
                }
            }

            // ── Lead (MC ClientboundSetEntityLinkPacket) ──────────────────
            // A holder change goes to everyone already watching (MC sends it
            // from setLeashedTo / dropLeash); a watcher added below gets the
            // current link with its AddEntity instead (sendPairingData).
            // Checked before the watch-set walk so nobody gets it twice.
            const int32_t leashHolderId = mob.GetLeashHolderNetId();
            if (leashHolderId != tracked.lastLeashHolderId) {
                tracked.lastLeashHolderId = leashHolderId;
                const auto payload = Network::Serialization::Serialize(
                    Network::SetEntityLinkS2CPacket{id, leashHolderId});
                for (uint32_t connId : tracked.watchers) {
                    EmitTo(connId, Network::PacketId::SetEntityLinkS2C,
                           payload, EntityPacketOut::Kind::Data, out);
                }
            }

            t_anyInRange.assign(players.size(), 0);
            for (size_t pi = 0; pi < players.size(); ++pi) {
                const auto& p = players[pi];
                const glm::dvec3& playerPos = p.position;

                // MC: min(getEffectiveRange(), playerViewDistance * 16).
                // Without the clamp a player on render distance 4 still
                // received updates for entities 8 chunks away — entities they
                // have no chunks for and cannot see.
                const double visibleRange =
                    std::min(static_cast<double>(effectiveRange),
                             static_cast<double>(p.viewDistance) * 16.0);
                const double rangeSq = visibleRange * visibleRange;

                const double dx = playerPos.x - mob.position.x;
                const double dz = playerPos.z - mob.position.z;
                // Horizontal only, like MC — a player directly above a mob at
                // build height still tracks it.
                const bool inRangeXZ = (dx * dx + dz * dz) <= rangeSq;

                // MC ChunkMap.isChunkTracked: the entity's chunk must already
                // be WITH the player. Ours is the post-send set, so this is
                // exactly vanilla's `contains(view) && !chunkSender.isPending`.
                //
                // This is what stops entity updates outrunning their terrain.
                // During world load the client was being handed entities for
                // chunks it had not received yet — extra traffic at precisely
                // the moment it was already struggling to keep up with chunks.
                const bool chunkTracked = t_memoSent[pi] != 0;

                if (inRangeXZ && chunkTracked) t_anyInRange[t_firstOf[pi]] = 1;
            }

            for (size_t pi = 0; pi < players.size(); ++pi) {
                if (t_firstOf[pi] != pi) continue;   // decided with its first entry
                const uint32_t connId = players[pi].connectionId;
                const bool inRange = t_anyInRange[pi] != 0;
                const bool watching = tracked.watchers.count(connId) != 0;

                if (inRange && !watching) {
                    tracked.watchers.insert(connId);
                    EmitTo(connId, Network::PacketId::AddEntityS2C,
                           Network::Serialization::Serialize(BuildAddPacket(mob, basePos)),
                           EntityPacketOut::Kind::Add, out);
                    // MC ServerEntity.sendPairingData: a leashed mob's link
                    // follows its add.
                    if (leashHolderId != -1) {
                        EmitTo(connId, Network::PacketId::SetEntityLinkS2C,
                               Network::Serialization::Serialize(
                                   Network::SetEntityLinkS2CPacket{id, leashHolderId}),
                               EntityPacketOut::Kind::Data, out);
                    }
                    // MC sends a crystal's DATA_BEAM_TARGET with the entity
                    // data on tracking start; here it is its own packet (see
                    // DragonPackets.hpp). Only when a target is set — the
                    // client default is "no beam".
                    // An armor stand's poses and equipment follow its add
                    // packet (ArmorStandDataS2C) — its flags byte is the
                    // add packet's own variant byte.
                    if (const auto* stand = dynamic_cast<const Game::ArmorStand*>(&mob)) {
                        EmitTo(connId, Network::PacketId::ArmorStandDataS2C,
                               Network::Serialization::Serialize(BuildArmorStandData(*stand, id)),
                               EntityPacketOut::Kind::Data, out);
                    }
                    // A boat's or minecart's synched data and its seat
                    // order (MC sendPairingData: the entity data and
                    // ClientboundSetPassengersPacket).
                    if (Game::IsVehicleEntityType(mob.GetType())) {
                        if (const auto* vehicle = dynamic_cast<const Game::VehicleEntity*>(&mob)) {
                            EmitTo(connId, Network::PacketId::VehicleDataS2C, VehicleDataPayload(*vehicle, id),
                                   EntityPacketOut::Kind::Data, out);
                            EmitTo(connId, Network::PacketId::SetPassengersS2C, SetPassengersPayload(*vehicle, id),
                                   EntityPacketOut::Kind::Data, out);
                        }
                    }
                    // A mount with players aboard: its seat order (MC
                    // sendPairingData's ClientboundSetPassengersPacket).
                    else if (mob.IsVehicle()) {
                        const std::vector<int32_t> wire = PassengerWireIds(mob);
                        if (HasPlayerWireId(wire)) {
                            EmitTo(connId, Network::PacketId::SetPassengersS2C,
                                   Network::Serialization::Serialize(Network::SetPassengersS2CPacket{id, wire}),
                                   EntityPacketOut::Kind::Data, out);
                        }
                    }
                    // A fishing bobber's owner, hooked entity and bite (MC
                    // sends the owner as the add packet's data, the rest as
                    // entity data) follow its add (FishingHookDataS2C).
                    if (mob.GetType() == Game::EntityTypeId::FishingBobber) {
                        EmitTo(connId, Network::PacketId::FishingHookDataS2C,
                               Network::Serialization::Serialize(BuildFishingHookData(
                                   static_cast<const Game::FishingHook&>(mob), id)),
                               EntityPacketOut::Kind::Data, out);
                    }
                    // A firework rocket's item, attachment and angle flag (MC
                    // entity data) follow its add (FireworkRocketDataS2C) —
                    // the client needs the stack for its look and its
                    // explosions, the attachment to ride with its glider.
                    if (mob.GetType() == Game::EntityTypeId::FireworkRocket) {
                        EmitTo(connId, Network::PacketId::FireworkRocketDataS2C,
                               Network::Serialization::Serialize(BuildFireworkRocketData(
                                   static_cast<const Game::FireworkRocket&>(mob), id)),
                               EntityPacketOut::Kind::Data, out);
                    }
                    // An item frame's framed item follows the same way
                    // (ItemFrameDataS2C) — only when there is one; the
                    // client's default is an empty frame.
                    if (const auto* frame = dynamic_cast<const Game::ItemFrame*>(&mob);
                        frame && !frame->GetItem().IsEmpty()) {
                        EmitTo(connId, Network::PacketId::ItemFrameDataS2C,
                               Network::Serialization::Serialize(Network::ItemFrameDataS2CPacket{id, frame->GetItem()}),
                               EntityPacketOut::Kind::Data, out);
                    }
                    // An ominous item spawner's item (MC DATA_ITEM) rides the
                    // same "displayed item" packet.
                    if (const auto* spawner = dynamic_cast<const Game::OminousItemSpawner*>(&mob);
                        spawner && !spawner->GetItem().IsEmpty()) {
                        EmitTo(connId, Network::PacketId::ItemFrameDataS2C,
                               Network::Serialization::Serialize(Network::ItemFrameDataS2CPacket{id, spawner->GetItem()}),
                               EntityPacketOut::Kind::Data, out);
                    }
                    // A mob's worn / held humanoid equipment (BodyArmorS2C
                    // with the slot — MC ClientboundSetEquipmentPacket), one
                    // packet per non-empty slot; the client's default is bare.
                    for (int slotIndex = 0; slotIndex < Game::Mob::kEquipmentSlotCount; ++slotIndex) {
                        const auto slot = static_cast<Game::EquipmentSlot>(slotIndex);
                        const Game::ItemStack& stack = mob.GetEquipment(slot);
                        if (stack.IsEmpty()) continue;
                        Network::BodyArmorS2CPacket equipment{id, stack};
                        equipment.slot = slot;
                        EmitTo(connId, Network::PacketId::BodyArmorS2C,
                               Network::Serialization::Serialize(equipment),
                               EntityPacketOut::Kind::Data, out);
                    }
                    // MC ServerEntity.sendPairingData: the syncable
                    // attributes (UpdateAttributesS2C) of a mob whose client
                    // copy needs them (a mount's rolled speed / jump / health).
                    if (mob.ShouldSyncAttributes()) {
                        Network::UpdateAttributesS2CPacket attributes;
                        attributes.entityId = id;
                        attributes.attributes = Network::SyncableAttributes(mob.Attributes());
                        if (!attributes.attributes.empty()) {
                            EmitTo(connId, Network::PacketId::UpdateAttributesS2C,
                                   Network::Serialization::Serialize(attributes),
                                   EntityPacketOut::Kind::Data, out);
                        }
                    }
                    // An allay's held item (BodyArmorS2C, slot MAINHAND) —
                    // only when it holds one; the client's default is empty.
                    if (mob.GetType() == Game::EntityTypeId::Allay) {
                        if (const auto* allay = dynamic_cast<const Game::Allay*>(&mob);
                            allay && !allay->GetMainHandItem().IsEmpty()) {
                            Network::BodyArmorS2CPacket held{id, allay->GetMainHandItem()};
                            held.slot = Game::EquipmentSlot::MAINHAND;
                            EmitTo(connId, Network::PacketId::BodyArmorS2C,
                                   Network::Serialization::Serialize(held),
                                   EntityPacketOut::Kind::Data, out);
                        }
                    }
                    // A wolf's armour (BodyArmorS2C) — only when it wears
                    // some; the client's default is an empty BODY slot.
                    if (mob.GetType() == Game::EntityTypeId::Wolf) {
                        const auto& wolf = static_cast<const Game::Wolf&>(mob);
                        if (wolf.IsWearingBodyArmor()) {
                            EmitTo(connId, Network::PacketId::BodyArmorS2C,
                                   Network::Serialization::Serialize(
                                       Network::BodyArmorS2CPacket{id, wolf.GetBodyArmorItem()}),
                                   EntityPacketOut::Kind::Data, out);
                        }
                    }
                    // A wither's side-head targets (MC DATA_TARGET_B / _C) —
                    // only when a head has one; the client's default is none.
                    if (mob.GetType() == Game::EntityTypeId::Wither) {
                        const auto& wither = static_cast<const Game::Wither&>(mob);
                        if (wither.GetHeadTargetId(0) != 0 || wither.GetHeadTargetId(1) != 0) {
                            Network::WitherHeadTargetsS2CPacket heads;
                            heads.entityId = id;
                            heads.targets[0] = wither.GetHeadTargetId(0);
                            heads.targets[1] = wither.GetHeadTargetId(1);
                            EmitTo(connId, Network::PacketId::WitherHeadTargetsS2C,
                                   Network::Serialization::Serialize(heads),
                                   EntityPacketOut::Kind::Data, out);
                        }
                    }
                    if (const auto* crystal =
                            dynamic_cast<const Game::EndCrystal*>(&mob);
                        crystal && crystal->HasBeamTarget()) {
                        Network::EndCrystalBeamS2CPacket beam;
                        beam.entityId = id;
                        beam.hasTarget = true;
                        beam.target = crystal->BeamTarget();
                        EmitTo(connId, Network::PacketId::EndCrystalBeamS2C,
                               Network::Serialization::Serialize(beam),
                               EntityPacketOut::Kind::Data, out);
                    }
                } else if (!inRange && watching) {
                    tracked.watchers.erase(connId);
                    Network::RemoveEntitiesS2CPacket removal;
                    removal.entityIds.push_back(id);
                    EmitTo(connId, Network::PacketId::EntityDestroy,
                           Network::Serialization::Serialize(removal),
                           EntityPacketOut::Kind::Remove, out);
                }
            }

            // Drop watchers who are no longer connected at all — only on a
            // tick the player set changed; otherwise no watcher can have left.
            if (playersChanged) {
                for (auto it = tracked.watchers.begin(); it != tracked.watchers.end();) {
                    const bool stillHere = std::any_of(players.begin(), players.end(),
                        [&](const TrackedPlayer& p) { return p.connectionId == *it; });
                    it = stillHere ? std::next(it) : tracked.watchers.erase(it);
                }
            }

            // A wither whose side-head targets changed this tick tells every
            // current watcher.
            if (mobPtr->GetType() == Game::EntityTypeId::Wither) {
                auto& wither = static_cast<Game::Wither&>(*mobPtr);
                if (wither.ConsumeHeadTargetsDirty()) {
                    Network::WitherHeadTargetsS2CPacket heads;
                    heads.entityId = id;
                    heads.targets[0] = wither.GetHeadTargetId(0);
                    heads.targets[1] = wither.GetHeadTargetId(1);
                    const auto payload = Network::Serialization::Serialize(heads);
                    for (uint32_t connId : tracked.watchers) {
                        EmitTo(connId, Network::PacketId::WitherHeadTargetsS2C,
                               payload, EntityPacketOut::Kind::Data, out);
                    }
                }
            }

            // A crystal whose beam target changed this tick (the respawn
            // ritual retargets them mid-flight) tells every current watcher.
            if (auto* crystal = dynamic_cast<Game::EndCrystal*>(mobPtr);
                crystal && crystal->ConsumeBeamDirty()) {
                Network::EndCrystalBeamS2CPacket beam;
                beam.entityId = id;
                beam.hasTarget = crystal->HasBeamTarget();
                beam.target = crystal->BeamTarget();
                const auto payload = Network::Serialization::Serialize(beam);
                for (uint32_t connId : tracked.watchers) {
                    EmitTo(connId, Network::PacketId::EndCrystalBeamS2C,
                           payload, EntityPacketOut::Kind::Data, out);
                }
            }

            // An armor stand whose poses or equipment changed this tick (a
            // swap, a break, /data) tells every current watcher.
            // An item frame whose item changed this tick (placed, knocked
            // out) tells every current watcher.
            if (auto* frame = dynamic_cast<Game::ItemFrame*>(mobPtr);
                frame && frame->ConsumeItemDirty()) {
                const auto payload = Network::Serialization::Serialize(
                    Network::ItemFrameDataS2CPacket{id, frame->GetItem()});
                for (uint32_t connId : tracked.watchers) {
                    EmitTo(connId, Network::PacketId::ItemFrameDataS2C,
                           payload, EntityPacketOut::Kind::Data, out);
                }
            }
            // The same for an ominous item spawner that let its item go.
            if (auto* spawner = dynamic_cast<Game::OminousItemSpawner*>(mobPtr);
                spawner && spawner->ConsumeItemDirty()) {
                const auto payload = Network::Serialization::Serialize(
                    Network::ItemFrameDataS2CPacket{id, spawner->GetItem()});
                for (uint32_t connId : tracked.watchers) {
                    EmitTo(connId, Network::PacketId::ItemFrameDataS2C,
                           payload, EntityPacketOut::Kind::Data, out);
                }
            }

            // Humanoid equipment that changed this tick (equipped from a
            // table, dropped preserved, emptied at death) tells every current
            // watcher, slot by slot.
            // Syncable attributes that changed since the last look (a bred
            // foal's inherited speed, an effect's modifier) tell every
            // current watcher — MC sendDirtyEntityData's attribute half. The
            // first look only records: every watcher so far had them at
            // pairing.
            if (mobPtr->ShouldSyncAttributes()) {
                const uint64_t signature = Network::SyncableAttributesSignature(mobPtr->Attributes());
                // A mob that only now syncs (customized after its watchers
                // paired without its attributes) sends on its first look.
                const bool pairedWithout = !tracked.attributesSignatureKnown && !mobPtr->SyncsAttributesToClient();
                if ((pairedWithout || (tracked.attributesSignatureKnown && signature != tracked.lastAttributesSignature)) &&
                    !tracked.watchers.empty()) {
                    Network::UpdateAttributesS2CPacket attributes;
                    attributes.entityId = id;
                    attributes.attributes = Network::SyncableAttributes(mobPtr->Attributes());
                    const auto payload = Network::Serialization::Serialize(attributes);
                    for (uint32_t connId : tracked.watchers) {
                        EmitTo(connId, Network::PacketId::UpdateAttributesS2C,
                               payload, EntityPacketOut::Kind::Data, out);
                    }
                }
                tracked.lastAttributesSignature = signature;
                tracked.attributesSignatureKnown = true;
            }

            if (const uint8_t equipmentMask = mobPtr->ConsumeEquipmentDirtyMask()) {
                for (int slotIndex = 0; slotIndex < Game::Mob::kEquipmentSlotCount; ++slotIndex) {
                    if ((equipmentMask & (1u << slotIndex)) == 0) continue;
                    const auto slot = static_cast<Game::EquipmentSlot>(slotIndex);
                    Network::BodyArmorS2CPacket equipment{id, mobPtr->GetEquipment(slot)};
                    equipment.slot = slot;
                    const auto payload = Network::Serialization::Serialize(equipment);
                    for (uint32_t connId : tracked.watchers) {
                        EmitTo(connId, Network::PacketId::BodyArmorS2C,
                               payload, EntityPacketOut::Kind::Data, out);
                    }
                }
            }

            // An allay whose held item changed this tick (given, taken
            // back, dropped at death) tells every current watcher.
            if (mobPtr->GetType() == Game::EntityTypeId::Allay) {
                if (auto* allay = dynamic_cast<Game::Allay*>(mobPtr); allay && allay->ConsumeHandItemDirty()) {
                    Network::BodyArmorS2CPacket held{id, allay->GetMainHandItem()};
                    held.slot = Game::EquipmentSlot::MAINHAND;
                    const auto payload = Network::Serialization::Serialize(held);
                    for (uint32_t connId : tracked.watchers) {
                        EmitTo(connId, Network::PacketId::BodyArmorS2C,
                               payload, EntityPacketOut::Kind::Data, out);
                    }
                }
            }

            // A firework rocket whose synched data changed this tick tells
            // every current watcher.
            if (mobPtr->GetType() == Game::EntityTypeId::FireworkRocket) {
                auto& rocket = static_cast<Game::FireworkRocket&>(*mobPtr);
                if (rocket.ConsumeDataDirty()) {
                    const auto payload = Network::Serialization::Serialize(BuildFireworkRocketData(rocket, id));
                    for (uint32_t connId : tracked.watchers) {
                        EmitTo(connId, Network::PacketId::FireworkRocketDataS2C,
                               payload, EntityPacketOut::Kind::Data, out);
                    }
                }
            }

            // A fishing bobber whose synched state changed this tick (hooked,
            // unhooked, a bite starting or ending) tells every current watcher.
            if (mobPtr->GetType() == Game::EntityTypeId::FishingBobber) {
                auto& hook = static_cast<Game::FishingHook&>(*mobPtr);
                if (hook.ConsumeSyncDirty()) {
                    const auto payload = Network::Serialization::Serialize(BuildFishingHookData(hook, id));
                    for (uint32_t connId : tracked.watchers) {
                        EmitTo(connId, Network::PacketId::FishingHookDataS2C,
                               payload, EntityPacketOut::Kind::Data, out);
                    }
                }
            }

            // A wolf whose armour changed this tick (equipped, worn down,
            // repaired, sheared off, broken) tells every current watcher.
            if (mobPtr->GetType() == Game::EntityTypeId::Wolf) {
                auto& wolf = static_cast<Game::Wolf&>(*mobPtr);
                if (wolf.ConsumeBodyArmorDirty()) {
                    const auto payload = Network::Serialization::Serialize(
                        Network::BodyArmorS2CPacket{id, wolf.GetBodyArmorItem()});
                    for (uint32_t connId : tracked.watchers) {
                        EmitTo(connId, Network::PacketId::BodyArmorS2C,
                               payload, EntityPacketOut::Kind::Data, out);
                    }
                }
            }

            // A vehicle's synched data or passenger list changed: every
            // current watcher (MC sendDirtyEntityData / the passengers
            // packet ServerEntity.sendChanges sends on a change).
            if (Game::IsVehicleEntityType(mobPtr->GetType())) {
                if (const auto* vehicle = dynamic_cast<const Game::VehicleEntity*>(mobPtr)) {
                    if (vehicle->ConsumeDataDirty()) {
                        const auto payload = VehicleDataPayload(*vehicle, id);
                        for (uint32_t connId : tracked.watchers) {
                            EmitTo(connId, Network::PacketId::VehicleDataS2C, payload, EntityPacketOut::Kind::Data, out);
                        }
                    }
                    if (vehicle->ConsumePassengersDirty(&PlayerRiding::PlayerIdOfView)) {
                        const auto payload = SetPassengersPayload(*vehicle, id);
                        for (uint32_t connId : tracked.watchers) {
                            EmitTo(connId, Network::PacketId::SetPassengersS2C, payload, EntityPacketOut::Kind::Data, out);
                        }
                    }
                }
            }

            // Any other mob's passenger order, when it changes and a player
            // is (or was) aboard — the mounts a player rides and steers.
            if (!Game::IsVehicleEntityType(mobPtr->GetType()) &&
                (mobPtr->IsVehicle() || !tracked.lastPassengerWire.empty())) {
                std::vector<int32_t> wire = PassengerWireIds(*mobPtr);
                if (wire != tracked.lastPassengerWire &&
                    (HasPlayerWireId(wire) || HasPlayerWireId(tracked.lastPassengerWire))) {
                    const auto payload = Network::Serialization::Serialize(Network::SetPassengersS2CPacket{id, wire});
                    for (uint32_t connId : tracked.watchers) {
                        EmitTo(connId, Network::PacketId::SetPassengersS2C, payload, EntityPacketOut::Kind::Data, out);
                    }
                }
                tracked.lastPassengerWire = std::move(wire);
            }

            if (auto* stand = dynamic_cast<Game::ArmorStand*>(mobPtr);
                stand && stand->ConsumeDataDirty()) {
                const auto payload = Network::Serialization::Serialize(BuildArmorStandData(*stand, id));
                for (uint32_t connId : tracked.watchers) {
                    EmitTo(connId, Network::PacketId::ArmorStandDataS2C,
                           payload, EntityPacketOut::Kind::Data, out);
                }
            }

            if (tracked.watchers.empty()) { ++tracked.tickCount; continue; }

            // ── Simulation gate (MC ChunkMap.tick:1175) ────────────────────
            //
            //   if (sectionPosChanged || entity.needsSync
            //       || distanceManager.inEntityTickingRange(chunk))
            //       trackedEntity.serverEntity.sendChanges();
            //
            // An entity in a chunk that is LOADED but not entity-ticking costs
            // no packets at all. That is the largest of MC's volume filters and
            // the one we were missing entirely: every tracked mob produced
            // updates regardless of whether its chunk was being simulated, so a
            // player standing at the edge of their render distance paid for
            // every mob out to the tracking range.
            //
            // The section-change arm matters on its own: an entity that crossed
            // a chunk boundary while outside the ticking range still gets one
            // update, so watchers see it move rather than jump when it later
            // re-enters.
            const bool sectionPosChanged = (mobChunk.x != tracked.lastChunkX ||
                                            mobChunk.z != tracked.lastChunkZ);
            tracked.lastChunkX = mobChunk.x;
            tracked.lastChunkZ = mobChunk.z;

            // Lock-free: TickMobs ran RunAllUpdates before any of this, the
            // same contract MobManager::Tick relies on. IsEntityTicking took
            // the ticket manager's mutex once per entity per tick.
            const bool entityTicking = memoTicking;

            if (!entityTicking && !sectionPosChanged && !mob.needsSync) {
                ++tracked.tickCount;
                continue;
            }

            // ── Movement ───────────────────────────────────────────────────
            ++tracked.tickCount;
            ++tracked.teleportDelay;

            // ── Synched data ───────────────────────────────────────────────
            const uint8_t flags = PackFlags(mob);
            const uint8_t variant = VariantData(mob);
            const auto hurtTime = static_cast<uint8_t>(std::clamp(mob.hurtTime, 0, 255));
            const auto deathTime = static_cast<uint8_t>(std::clamp(mob.deathTime, 0, 255));

            uint8_t swell = 0, swellDir = 0;
            // Type-id test, not RTTI. This ran a dynamic_cast on EVERY mob
            // every tick purely to read creeper swell — 10,240 RTTI graph walks
            // a second with 512 primed TNT alone. Valid here and NOT in the
            // explosion (see the note there): no Creeper subclass exists, so
            // the type id answers exactly what the cast did.
            if (mob.GetType() == Game::EntityTypeId::Creeper) {
                const auto* creeper = static_cast<const Game::Creeper*>(&mob);
                swellDir = creeper->GetSwellDir() > 0 ? 1 : 0;
                // Derived from the render fraction rather than read directly:
                // the fuse counter is private to Creeper, and the client only
                // needs enough resolution to drive the flash.
                swell = static_cast<uint8_t>(
                    std::clamp(creeper->GetSwelling(0.0f) * (Game::Creeper::kMaxSwell - 2),
                               0.0f, 255.0f));
            }

            const uint8_t pose = static_cast<uint8_t>(mob.GetPose());
            const uint8_t animState = mob.GetAnimStateByte();
            // Riding link: -1 when not a passenger. A change here is a mount
            // or dismount — MC's ClientboundSetPassengersPacket moment.
            const int32_t vehicleId =
                mob.GetVehicle() ? mob.GetVehicle()->GetId() : -1;
            const uint32_t carriedBlock = mob.GetCarriedBlockRaw();
            // MC updateDirtyEffects → DATA_EFFECT_PARTICLES / shared flags.
            const Game::EffectVisuals& effectVisuals = mob.GetEffectVisuals();

            const bool dataChanged =
                effectVisuals != tracked.lastEffectVisuals ||
                mob.GetCustomName() != tracked.lastCustomName ||
                mob.IsCustomNameVisible() != tracked.lastCustomNameVisible ||
                flags != tracked.lastFlags || variant != tracked.lastVariant ||
                carriedBlock != tracked.lastCarriedBlock ||
                hurtTime != tracked.lastHurtTime || deathTime != tracked.lastDeathTime ||
                swell != tracked.lastSwell || pose != tracked.lastPose ||
                (animState != tracked.lastAnimState && !mob.AnimStateTicksOnClient()) ||
                vehicleId != tracked.lastVehicleId ||
                std::abs(mob.scale - tracked.lastScale) > 1.0e-4f ||
                std::abs(mob.GetHealth() - tracked.lastHealth) > 1.0e-4f;


            // Two INDEPENDENT gates, and keeping them independent is the whole
            // point of this block.
            //
            // MC ServerEntity.sendChanges calls sendDirtyEntityData() whenever
            // the synched data is dirty, but its MOVEMENT is separately guarded
            // inside that branch by sentPosition/shouldSendRotation. Folding
            // the dirty-data clause into one shared gate looks equivalent and
            // is not: primed TNT's fuse rides `animState`, so its data is dirty
            // EVERY tick, and a shared gate therefore promoted it to a full
            // position update every tick as well — 10x the movement traffic
            // for the one entity type most likely to exist in the hundreds.
            // With 512 TNT that is ~30k packets/s against a 2048-deep client
            // queue, which overflows, drops CHUNK packets with everything else,
            // and stalls world load behind a 30-second timeout.
            //
            // So: data goes out when it is dirty (vanilla, and cheap — it is
            // one small packet), movement stays on the update interval.
            const int updateInterval = std::max(1, mob.TypeInfo().updateInterval);
            const bool dueThisTick = (tracked.tickCount % updateInterval) == 0;
            const bool sendMovement = dueThisTick || mob.needsSync;
            // hurtMarked is its own third reason to get here: MC handles it
            // OUTSIDE the movement gate entirely (sendChanges:222-226), every
            // tick, because a damage flinch delayed by up to updateInterval
            // ticks reads as the hit not registering. Three sites set it
            // without needsSync — the damage push itself, a mob attack's
            // upward knock, and the ravager fling.
            if (!sendMovement && !dataChanged && !mob.hurtMarked) {
                continue;
            }

            const int8_t yRotN     = Game::Mth::PackDegrees(mob.yRot);
            const int8_t xRotN     = Game::Mth::PackDegrees(mob.xRot);
            const int8_t yHeadRotN = Game::Mth::PackDegrees(mob.GetYHeadRot());

            // Every movement predicate is ANDed with sendMovement, so a tick
            // that got here only because the synched data was dirty emits the
            // data packet and nothing else. Gating the predicates rather than
            // wrapping the block keeps the delta-base bookkeeping below on its
            // existing paths.
            const bool rotationChanged = sendMovement && (
                std::abs(yRotN - tracked.lastYRot) >= 1 ||
                std::abs(xRotN - tracked.lastXRot) >= 1 ||
                std::abs(yHeadRotN - tracked.lastYHeadRot) >= 1);

            const int64_t curX = Encode(mob.position.x);
            const int64_t curY = Encode(mob.position.y);
            const int64_t curZ = Encode(mob.position.z);

            const int64_t dX = curX - tracked.baseX;
            const int64_t dY = curY - tracked.baseY;
            const int64_t dZ = curZ - tracked.baseZ;

            // MC compares the DECODED delta against a squared tolerance rather
            // than testing the integers for zero, so sub-quantum jitter does
            // not generate traffic.
            const double ddx = Network::DecodeEntityPos(dX);
            const double ddy = Network::DecodeEntityPos(dY);
            const double ddz = Network::DecodeEntityPos(dZ);
            const bool positionChanged =
                (ddx * ddx + ddy * ddy + ddz * ddz) >= kPositionTolerance;

            // Staggered by id: a million entities spawned the same tick all
            // shared a resync phase, and the tick where they aligned emitted
            // one 12 MB move batch — past the 2 MB frame cap, which reads as
            // a corrupt stream and force-disconnects the client.
            const bool forcePos =
                ((tracked.tickCount + static_cast<uint32_t>(id)) % kForcedPosUpdatePeriod) == 0;
            const bool sendPos = sendMovement && (positionChanged || forcePos);

            const bool deltaTooBig = sendMovement && (std::abs(dX) > kMaxDelta ||
                                                      std::abs(dY) > kMaxDelta ||
                                                      std::abs(dZ) > kMaxDelta);
            const bool forceTeleport =
                sendMovement && tracked.teleportDelay > kForcedTeleportPeriod;
            const bool groundChanged =
                sendMovement && tracked.wasOnGround != mob.onGround;
            // Through an immersive surface since the last pass: a full sync
            // whatever the distance, flagged so the watchers snap to it.
            const bool crossedSurface = mob.portal.ConsumeCrossedSurface();

            if (mob.IsPassenger()) {
                // MC ServerEntity.sendChanges' passenger branch: NO position
                // packets — every watcher derives the rider's position from
                // the vehicle via client-side PositionRider, and a competing
                // interpolation target is exactly the rider jitter MC avoids.
                // Rotation still travels (a jockey looks around), and the
                // delta base advances silently so the post-dismount resync
                // below starts from the truth.
                if (rotationChanged) {
                    Network::MoveEntityS2CPacket::Entry e;
                    e.entityId = id;
                    e.onGround = mob.onGround;
                    e.mask = 0x02;
                    e.yRot = yRotN;
                    e.xRot = xRotN;
                    e.yHeadRot = yHeadRotN;
                    for (uint32_t connId : tracked.watchers) {
                        moveBatches[connId].entries.push_back(e);
                    }
                    tracked.lastYRot = yRotN; tracked.lastXRot = xRotN;
                    tracked.lastYHeadRot = yHeadRotN;
                }
                tracked.baseX = curX; tracked.baseY = curY; tracked.baseZ = curZ;
                tracked.wasOnGround = mob.onGround;
                tracked.wasRiding = true;

            } else if (deltaTooBig || forceTeleport || groundChanged ||
                       tracked.wasRiding || crossedSurface) {
                Network::EntityPositionSyncS2CPacket p;
                p.entityId = id;
                p.position = mob.position;
                p.velocity = glm::vec3(mob.velocity);
                p.yRot = yRotN;
                p.xRot = xRotN;
                p.yHeadRot = yHeadRotN;
                p.onGround = mob.onGround;
                if (crossedSurface) p.flags |= Network::EntityPositionSyncS2CPacket::kFlagPortalCrossing;

                const auto payload = Network::Serialization::Serialize(p);
                for (uint32_t connId : tracked.watchers) {
                    EmitTo(connId, Network::PacketId::EntityPositionSyncS2C, payload,
                           EntityPacketOut::Kind::PositionSync, out);
                }

                tracked.baseX = curX; tracked.baseY = curY; tracked.baseZ = curZ;
                tracked.lastYRot = yRotN; tracked.lastXRot = xRotN;
                tracked.lastYHeadRot = yHeadRotN;
                tracked.wasOnGround = mob.onGround;
                tracked.teleportDelay = 0;
                // MC clears wasRiding at the end of every non-passenger
                // update; here the sync branch is the only one reachable while
                // it is set, so this is the same thing.
                tracked.wasRiding = false;

            } else if (sendPos || rotationChanged) {
                Network::MoveEntityS2CPacket::Entry e;
                e.entityId = id;
                e.onGround = mob.onGround;

                if (sendPos) {
                    e.mask |= 0x01;
                    e.dx = static_cast<int16_t>(dX);
                    e.dy = static_cast<int16_t>(dY);
                    e.dz = static_cast<int16_t>(dZ);
                }
                if (rotationChanged) {
                    e.mask |= 0x02;
                    e.yRot = yRotN;
                    e.xRot = xRotN;
                    e.yHeadRot = yHeadRotN;
                }

                for (uint32_t connId : tracked.watchers) {
                    moveBatches[connId].entries.push_back(e);
                }

                // The base only advances when a POSITION actually went out —
                // advancing it on a rotation-only packet would silently drop
                // the movement since the last send.
                if (sendPos) {
                    tracked.baseX = curX; tracked.baseY = curY; tracked.baseZ = curZ;
                }
                if (rotationChanged) {
                    tracked.lastYRot = yRotN; tracked.lastXRot = xRotN;
                    tracked.lastYHeadRot = yHeadRotN;
                }
            }

            // ── Velocity ───────────────────────────────────────────────────
            // Only when something asked for it (knockback, jump, leap). Sending
            // velocity every tick would undo the client's own simulation. Not
            // for passengers — MC's passenger branch sends no motion (RideTick
            // zeroes it every tick on both sides anyway).
            // NOT gated on sendMovement — see the hurtMarked note on the gate.
            if ((mob.needsSync || mob.hurtMarked) && !mob.IsPassenger()) {
                Network::SetEntityMotionS2CPacket p;
                p.entityId = id;
                p.velocity = glm::vec3(mob.velocity);

                const auto payload = Network::Serialization::Serialize(p);
                for (uint32_t connId : tracked.watchers) {
                    EmitTo(connId, Network::PacketId::SetEntityMotionS2C, payload,
                           EntityPacketOut::Kind::Motion, out);
                }
            }

            // CONSUME the impulse flags. MC does exactly this — ServerEntity
            // .sendChanges:219 clears needsSync at the end of the movement
            // branch and :223-226 clears hurtMarked as it sends the motion
            // packet — and leaving them set is not a small leak, it is
            // permanent.
            //
            // Both flags mean "an impulse happened THIS tick". Nothing else in
            // the engine cleared them, so the first time anything pushed an
            // entity — a blast, a knockback, a jump — that entity started
            // sending a SetEntityMotion packet every single tick for the rest
            // of its life, and taking the movement branch every tick with it.
            // A few hundred primed TNT caught in each other's blasts is then
            // tens of thousands of packets a second, which overruns the
            // client's 2048-deep incoming queue; the drops take CHUNK packets
            // with them, so world load stalls out to its 30-second timeout and
            // the player cannot move until it fires.
            mob.needsSync  = false;
            mob.hurtMarked = false;

            if (dataChanged) {
                Network::SetEntityDataS2CPacket p;
                p.entityId = id;
                p.health = mob.GetHealth();
                p.flags = flags;
                p.variantData = variant;
                p.hurtTime = hurtTime;
                p.deathTime = deathTime;
                p.swellDir = swellDir;
                p.swell = swell;
                p.pose = pose;
                p.animState = animState;
                p.vehicleId = vehicleId;
                p.scale     = mob.scale;
                p.blockStateRaw = carriedBlock;
                p.effectFlags     = effectVisuals.flags;
                p.effectParticles = effectVisuals.particles;
                p.customName        = mob.GetCustomName();
                p.customNameVisible = mob.IsCustomNameVisible();

                const auto payload = Network::Serialization::Serialize(p);
                for (uint32_t connId : tracked.watchers) {
                    EmitTo(connId, Network::PacketId::SetEntityDataS2C, payload,
                           EntityPacketOut::Kind::Data, out);
                }

                tracked.lastHealth = mob.GetHealth();
                tracked.lastFlags = flags;
                tracked.lastVariant = variant;
                tracked.lastHurtTime = hurtTime;
                tracked.lastDeathTime = deathTime;
                tracked.lastSwell = swell;
                tracked.lastPose = pose;
                tracked.lastAnimState = animState;
                tracked.lastCarriedBlock = carriedBlock;
                tracked.lastVehicleId = vehicleId;
                tracked.lastScale     = mob.scale;
                tracked.lastEffectVisuals = effectVisuals;
                tracked.lastCustomName        = mob.GetCustomName();
                tracked.lastCustomNameVisible = mob.IsCustomNameVisible();
            }
        }

        // Flush the batched move packets, chunked well under the reader's
        // 2 MB frame cap (an entry is ~13 bytes; 32k entries is ~430 KB).
        constexpr size_t kMoveEntriesPerPacket = 32768;
        for (auto& [connId, batch] : moveBatches) {
            if (batch.entries.empty()) continue;
            if (batch.entries.size() <= kMoveEntriesPerPacket) {
                EmitTo(connId, Network::PacketId::MoveEntityS2C,
                       Network::Serialization::Serialize(batch),
                       EntityPacketOut::Kind::Move, out);
                continue;
            }
            Network::MoveEntityS2CPacket part;
            for (size_t i = 0; i < batch.entries.size(); i += kMoveEntriesPerPacket) {
                part.entries.assign(
                    batch.entries.begin() + static_cast<std::ptrdiff_t>(i),
                    batch.entries.begin() + static_cast<std::ptrdiff_t>(
                        std::min(batch.entries.size(), i + kMoveEntriesPerPacket)));
                EmitTo(connId, Network::PacketId::MoveEntityS2C,
                       Network::Serialization::Serialize(part),
                       EntityPacketOut::Kind::Move, out);
            }
        }

        // ── Entity events raised by the entity system this tick ────────────
        // Normally already drained by IntegratedServer's pre-removal
        // FlushEntityEvents call (see the header note); this catches events
        // raised after that point and costs nothing when the vector is
        // empty.
        FlushEntityEvents(level, out);
    }

    void ServerEntityTracker::FlushEntityEvents(ServerLevelBridge& level,
                                                std::vector<EntityPacketOut>& out,
                                                bool keepUntracked) {
        auto& events = level.DrainEvents();
        size_t kept = 0;
        for (const auto& ev : events) {
            const auto it = m_tracked.find(ev.entityId);
            if (it == m_tracked.end()) {
                // Not tracked yet: held (in order, compacted to the front)
                // for Tick()'s closing flush, or dropped by that flush.
                if (keepUntracked) events[kept++] = ev;
                continue;
            }

            Network::EntityEventS2CPacket p;
            p.entityId = ev.entityId;
            p.event = ev.event;

            const auto payload = Network::Serialization::Serialize(p);
            for (uint32_t connId : it->second.watchers) {
                EmitTo(connId, Network::PacketId::EntityEventS2C, payload,
                       EntityPacketOut::Kind::Event, out);
            }
        }
        events.resize(kept);
    }

    void ServerEntityTracker::RemoveEntities(const std::vector<int32_t>& entityIds,
                                             std::vector<EntityPacketOut>& out) {
        // One RemoveEntities packet per watcher for the whole tick's removals
        // (MC's ClientboundRemoveEntitiesPacket carries a list for exactly
        // this). RemoveEntity-per-id emitted one packet per entity, which at
        // the tail of a mass detonation was a hundred thousand packets in a
        // handful of ticks.
        std::unordered_map<uint32_t, std::vector<int32_t>> perWatcher;
        for (int32_t id : entityIds) {
            const auto it = m_tracked.find(id);
            if (it == m_tracked.end()) continue;
            for (uint32_t connId : it->second.watchers) perWatcher[connId].push_back(id);
            m_tracked.erase(it);
        }
        // Chunked so a single packet stays far below the frame size limit.
        constexpr size_t kIdsPerPacket = 4096;
        for (auto& [connId, ids] : perWatcher) {
            for (size_t i = 0; i < ids.size(); i += kIdsPerPacket) {
                Network::RemoveEntitiesS2CPacket removal;
                removal.entityIds.assign(ids.begin() + static_cast<std::ptrdiff_t>(i),
                                         ids.begin() + static_cast<std::ptrdiff_t>(
                                             std::min(ids.size(), i + kIdsPerPacket)));
                EmitTo(connId, Network::PacketId::EntityDestroy,
                       Network::Serialization::Serialize(removal),
                       EntityPacketOut::Kind::Remove, out);
            }
        }
    }

    void ServerEntityTracker::RemoveEntity(int32_t entityId, std::vector<EntityPacketOut>& out) {
        const auto it = m_tracked.find(entityId);
        if (it == m_tracked.end()) return;

        Network::RemoveEntitiesS2CPacket removal;
        removal.entityIds.push_back(entityId);
        const auto payload = Network::Serialization::Serialize(removal);

        for (uint32_t connId : it->second.watchers) {
            EmitTo(connId, Network::PacketId::EntityDestroy, payload,
                   EntityPacketOut::Kind::Remove, out);
        }
        m_tracked.erase(it);
    }

    const std::unordered_set<uint32_t>* ServerEntityTracker::WatchersOf(int32_t entityId) const {
        const auto it = m_tracked.find(entityId);
        return it == m_tracked.end() ? nullptr : &it->second.watchers;
    }

    void ServerEntityTracker::RemovePlayer(uint32_t connectionId) {
        for (auto& [id, tracked] : m_tracked) tracked.watchers.erase(connectionId);
    }

    void ServerEntityTracker::Clear() { m_tracked.clear(); }

} // namespace Server
