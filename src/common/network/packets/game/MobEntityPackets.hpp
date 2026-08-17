// File: src/common/network/packets/game/MobEntityPackets.hpp
//
// The mob entity packet family, ported from MC's clientbound entity packets.
//
// Grouped in one header rather than split one-per-file (the convention for the
// older packets in this directory) because they share a quantisation contract
// that only makes sense read together: get the fixed-point scale wrong in one
// and the others still decode, they just disagree by a fraction of a block.
//
// ── Quantisation (MC VecDeltaCodec / Mth.packDegrees) ─────────────────────
//
//   position delta   int16, units of 1/4096 block  -> +-7.99975 blocks max
//   rotation         int8,  units of 1/256 turn    -> 1.40625 degrees
//
// The delta cap is why MoveEntityPos exists alongside EntityPositionSync: any
// movement that would overflow a short falls back to the full-precision packet.
// The sender (ServerEntityTracker) owns that decision; a decoder that assumed
// deltas always fit would silently teleport mobs backwards on fast movement.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <glm/glm.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Network {

    // Shared scale constants. Both sides must agree exactly.
    inline constexpr double kEntityPosScale = 4096.0;

    inline int64_t EncodeEntityPos(double v) {
        return static_cast<int64_t>(std::llround(v * kEntityPosScale));
    }
    inline double DecodeEntityPos(int64_t v) {
        return static_cast<double>(v) / kEntityPosScale;
    }

    // MC ClientboundAddEntityPacket. Full state for an entity the client has
    // never seen.
    struct AddEntityS2CPacket {
        int32_t    entityId = 0;
        uint16_t   entityType = 0;      // Game::EntityTypeId
        glm::dvec3 position{0.0};
        glm::vec3  velocity{0.0f};      // blocks per TICK
        int8_t     yRot = 0;            // packed degrees
        int8_t     xRot = 0;
        int8_t     yHeadRot = 0;
        // Everything the client needs to render correctly on the very first
        // frame. MC sends these as synched data in a follow-up packet; folding
        // them in avoids a frame where a baby cow renders adult-sized.
        float      health = 0.0f;
        uint8_t    flags = 0;           // bit0 baby, bit1 aggressive, bit2 on fire
        uint8_t    variantData = 0;     // sheep wool byte; unused by other types
        // MC DATA_POSE. Drives the episodic animation timers — a frog croaks
        // because this turned CROAKING, not because a timer was sent.
        uint8_t    pose = 0;            // Game::Pose ordinal
        // MC gives each mob its own synched enum for the state its animations
        // key on (Armadillo.ARMADILLO_STATE, Bat's resting flag). One byte
        // covers every such enum this port has, and the meaning is per-type —
        // the client dispatches on the entity class, exactly as MC does.
        uint8_t    animState = 0;
    };

    // MC ClientboundMoveEntityPacket.Pos / .Rot / .PosRot, merged into one
    // packet with a mask. Three separate packet ids would each carry the same
    // 1-byte header for no gain at this scale.
    struct MoveEntityS2CPacket {
        struct Entry {
            int32_t entityId = 0;
            uint8_t mask = 0;           // bit0 position, bit1 rotation
            int16_t dx = 0, dy = 0, dz = 0;   // 1/4096 block
            int8_t  yRot = 0, xRot = 0, yHeadRot = 0;
            bool    onGround = false;
        };
        // Batched per chunk, matching how ItemEntityMoveS2C already works —
        // a busy chunk sends one packet, not one per mob.
        std::vector<Entry> entries;
    };

    // MC ClientboundEntityPositionSyncPacket. Full-precision teleport, used
    // when a delta would overflow or on the periodic forced resync.
    struct EntityPositionSyncS2CPacket {
        int32_t    entityId = 0;
        glm::dvec3 position{0.0};
        glm::vec3  velocity{0.0f};
        int8_t     yRot = 0, xRot = 0, yHeadRot = 0;
        bool       onGround = false;
    };

    // MC ClientboundSetEntityMotionPacket.
    struct SetEntityMotionS2CPacket {
        int32_t   entityId = 0;
        glm::vec3 velocity{0.0f};
    };

    // MC ClientboundSetEntityDataPacket, reduced to the fields this port's mobs
    // actually synchronise. A generic key/value channel would be more faithful
    // but every consumer here is a fixed field, so the wire stays flat.
    struct SetEntityDataS2CPacket {
        int32_t entityId = 0;
        float   health = 0.0f;
        uint8_t flags = 0;          // same bits as AddEntity
        uint8_t variantData = 0;
        uint8_t hurtTime = 0;       // drives the red flash
        uint8_t deathTime = 0;      // drives the fall-over animation
        uint8_t swellDir = 0;       // creeper: 0 = shrinking, 1 = swelling
        uint8_t swell = 0;          // creeper fuse progress, 0..30
        uint8_t pose = 0;           // Game::Pose ordinal — see AddEntity
        uint8_t animState = 0;      // per-type animation state — see AddEntity
    };

    // MC ClientboundEntityEventPacket. One byte: 3 death, 60 poof, 10 eat,
    // 18 hearts.
    struct EntityEventS2CPacket {
        int32_t entityId = 0;
        uint8_t event = 0;
    };

    // MC ClientboundHurtAnimationPacket. Sent to the VICTIM: the hurt flash
    // other players see rides the position broadcast, but your own camera tilt
    // needs the direction the hit came from, which nothing else carries.
    //
    // `yaw` is MC's hurtDir — atan2(dz, dx) in degrees MINUS the victim's own
    // yaw, i.e. the attacker's bearing relative to the way you are facing. That
    // is what makes the camera tip away from the blow rather than always
    // rolling the same way (GameRenderer.bobHurt conjugates the roll by it).
    struct HurtAnimationS2CPacket {
        int32_t entityId = 0;
        float   yaw = 0.0f;
    };

    // MC ServerboundInteractPacket, attack branch only. `useOffhand` and the
    // interact-at vector are omitted because nothing consumes them yet.
    struct InteractC2SPacket {
        enum class Action : uint8_t { Interact = 0, Attack = 1 };
        int32_t entityId = 0;
        Action  action = Action::Attack;
        bool    sneaking = false;
        // MC reads this off its own player; movement is client-authoritative
        // here, so it has to travel. See IntegratedServer::HandleInteract for
        // why trusting it is safe.
        bool    sprinting = false;
    };

    namespace Serialization {

        // ── AddEntity ──────────────────────────────────────────────────────
        inline std::vector<uint8_t> Serialize(const AddEntityS2CPacket& p) {
            Network::PacketBuffer b;
            b.WriteVarInt(static_cast<uint32_t>(p.entityId));
            b.WriteVarInt(p.entityType);
            b.WriteDouble(p.position.x);
            b.WriteDouble(p.position.y);
            b.WriteDouble(p.position.z);
            b.WriteFloat(p.velocity.x);
            b.WriteFloat(p.velocity.y);
            b.WriteFloat(p.velocity.z);
            b.WriteByte(static_cast<uint8_t>(p.yRot));
            b.WriteByte(static_cast<uint8_t>(p.xRot));
            b.WriteByte(static_cast<uint8_t>(p.yHeadRot));
            b.WriteFloat(p.health);
            b.WriteByte(p.flags);
            b.WriteByte(p.variantData);
            b.WriteByte(p.pose);
            b.WriteByte(p.animState);
            return b.GetData();
        }

        inline AddEntityS2CPacket DeserializeAddEntityS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader r(data);
            AddEntityS2CPacket p;
            p.entityId   = static_cast<int32_t>(r.ReadVarInt());
            p.entityType = static_cast<uint16_t>(r.ReadVarInt());
            p.position.x = r.ReadDouble();
            p.position.y = r.ReadDouble();
            p.position.z = r.ReadDouble();
            p.velocity.x = r.ReadFloat();
            p.velocity.y = r.ReadFloat();
            p.velocity.z = r.ReadFloat();
            p.yRot       = static_cast<int8_t>(r.ReadByte());
            p.xRot       = static_cast<int8_t>(r.ReadByte());
            p.yHeadRot   = static_cast<int8_t>(r.ReadByte());
            p.health     = r.ReadFloat();
            p.flags      = r.ReadByte();
            p.variantData = r.ReadByte();
            p.pose       = r.ReadByte();
            p.animState  = r.ReadByte();
            return p;
        }

        // ── MoveEntity ─────────────────────────────────────────────────────
        inline std::vector<uint8_t> Serialize(const MoveEntityS2CPacket& p) {
            Network::PacketBuffer b;
            b.WriteVarInt(static_cast<uint32_t>(p.entries.size()));
            for (const auto& e : p.entries) {
                b.WriteVarInt(static_cast<uint32_t>(e.entityId));
                b.WriteByte(e.mask);
                if (e.mask & 0x01) {
                    // PacketBuffer's short is unsigned; the two's-complement
                    // bit pattern round-trips exactly, so the cast pair below is
                    // the whole of the signed handling.
                    b.WriteShort(static_cast<uint16_t>(e.dx));
                    b.WriteShort(static_cast<uint16_t>(e.dy));
                    b.WriteShort(static_cast<uint16_t>(e.dz));
                }
                if (e.mask & 0x02) {
                    b.WriteByte(static_cast<uint8_t>(e.yRot));
                    b.WriteByte(static_cast<uint8_t>(e.xRot));
                    b.WriteByte(static_cast<uint8_t>(e.yHeadRot));
                }
                b.WriteByte(e.onGround ? 1 : 0);
            }
            return b.GetData();
        }

        inline MoveEntityS2CPacket DeserializeMoveEntityS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader r(data);
            MoveEntityS2CPacket p;
            const uint32_t count = r.ReadVarInt();
            p.entries.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                MoveEntityS2CPacket::Entry e;
                e.entityId = static_cast<int32_t>(r.ReadVarInt());
                e.mask = r.ReadByte();
                if (e.mask & 0x01) {
                    e.dx = static_cast<int16_t>(r.ReadShort());
                    e.dy = static_cast<int16_t>(r.ReadShort());
                    e.dz = static_cast<int16_t>(r.ReadShort());
                }
                if (e.mask & 0x02) {
                    e.yRot     = static_cast<int8_t>(r.ReadByte());
                    e.xRot     = static_cast<int8_t>(r.ReadByte());
                    e.yHeadRot = static_cast<int8_t>(r.ReadByte());
                }
                e.onGround = r.ReadByte() != 0;
                p.entries.push_back(e);
            }
            return p;
        }

        // ── EntityPositionSync ─────────────────────────────────────────────
        inline std::vector<uint8_t> Serialize(const EntityPositionSyncS2CPacket& p) {
            Network::PacketBuffer b;
            b.WriteVarInt(static_cast<uint32_t>(p.entityId));
            b.WriteDouble(p.position.x);
            b.WriteDouble(p.position.y);
            b.WriteDouble(p.position.z);
            b.WriteFloat(p.velocity.x);
            b.WriteFloat(p.velocity.y);
            b.WriteFloat(p.velocity.z);
            b.WriteByte(static_cast<uint8_t>(p.yRot));
            b.WriteByte(static_cast<uint8_t>(p.xRot));
            b.WriteByte(static_cast<uint8_t>(p.yHeadRot));
            b.WriteByte(p.onGround ? 1 : 0);
            return b.GetData();
        }

        inline EntityPositionSyncS2CPacket
        DeserializeEntityPositionSyncS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader r(data);
            EntityPositionSyncS2CPacket p;
            p.entityId   = static_cast<int32_t>(r.ReadVarInt());
            p.position.x = r.ReadDouble();
            p.position.y = r.ReadDouble();
            p.position.z = r.ReadDouble();
            p.velocity.x = r.ReadFloat();
            p.velocity.y = r.ReadFloat();
            p.velocity.z = r.ReadFloat();
            p.yRot       = static_cast<int8_t>(r.ReadByte());
            p.xRot       = static_cast<int8_t>(r.ReadByte());
            p.yHeadRot   = static_cast<int8_t>(r.ReadByte());
            p.onGround   = r.ReadByte() != 0;
            return p;
        }

        // ── SetEntityMotion ────────────────────────────────────────────────
        inline std::vector<uint8_t> Serialize(const SetEntityMotionS2CPacket& p) {
            Network::PacketBuffer b;
            b.WriteVarInt(static_cast<uint32_t>(p.entityId));
            b.WriteFloat(p.velocity.x);
            b.WriteFloat(p.velocity.y);
            b.WriteFloat(p.velocity.z);
            return b.GetData();
        }

        inline SetEntityMotionS2CPacket
        DeserializeSetEntityMotionS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader r(data);
            SetEntityMotionS2CPacket p;
            p.entityId   = static_cast<int32_t>(r.ReadVarInt());
            p.velocity.x = r.ReadFloat();
            p.velocity.y = r.ReadFloat();
            p.velocity.z = r.ReadFloat();
            return p;
        }

        // ── SetEntityData ──────────────────────────────────────────────────
        inline std::vector<uint8_t> Serialize(const SetEntityDataS2CPacket& p) {
            Network::PacketBuffer b;
            b.WriteVarInt(static_cast<uint32_t>(p.entityId));
            b.WriteFloat(p.health);
            b.WriteByte(p.flags);
            b.WriteByte(p.variantData);
            b.WriteByte(p.hurtTime);
            b.WriteByte(p.deathTime);
            b.WriteByte(p.swellDir);
            b.WriteByte(p.swell);
            b.WriteByte(p.pose);
            b.WriteByte(p.animState);
            return b.GetData();
        }

        inline SetEntityDataS2CPacket
        DeserializeSetEntityDataS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader r(data);
            SetEntityDataS2CPacket p;
            p.entityId    = static_cast<int32_t>(r.ReadVarInt());
            p.health      = r.ReadFloat();
            p.flags       = r.ReadByte();
            p.variantData = r.ReadByte();
            p.hurtTime    = r.ReadByte();
            p.deathTime   = r.ReadByte();
            p.swellDir    = r.ReadByte();
            p.swell       = r.ReadByte();
            p.pose        = r.ReadByte();
            p.animState   = r.ReadByte();
            return p;
        }

        // ── EntityEvent ────────────────────────────────────────────────────
        inline std::vector<uint8_t> Serialize(const EntityEventS2CPacket& p) {
            Network::PacketBuffer b;
            b.WriteVarInt(static_cast<uint32_t>(p.entityId));
            b.WriteByte(p.event);
            return b.GetData();
        }

        inline EntityEventS2CPacket
        DeserializeEntityEventS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader r(data);
            EntityEventS2CPacket p;
            p.entityId = static_cast<int32_t>(r.ReadVarInt());
            p.event    = r.ReadByte();
            return p;
        }

        // ── HurtAnimation ──────────────────────────────────────────────────
        inline std::vector<uint8_t> Serialize(const HurtAnimationS2CPacket& p) {
            Network::PacketBuffer b;
            b.WriteVarInt(static_cast<uint32_t>(p.entityId));
            b.WriteFloat(p.yaw);
            return b.GetData();
        }

        inline HurtAnimationS2CPacket
        DeserializeHurtAnimationS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader r(data);
            HurtAnimationS2CPacket p;
            p.entityId = static_cast<int32_t>(r.ReadVarInt());
            p.yaw      = r.ReadFloat();
            return p;
        }

        // ── Interact ───────────────────────────────────────────────────────
        inline std::vector<uint8_t> Serialize(const InteractC2SPacket& p) {
            Network::PacketBuffer b;
            b.WriteVarInt(static_cast<uint32_t>(p.entityId));
            b.WriteByte(static_cast<uint8_t>(p.action));
            b.WriteByte(p.sneaking ? 1 : 0);
            b.WriteByte(p.sprinting ? 1 : 0);
            return b.GetData();
        }

        inline InteractC2SPacket DeserializeInteractC2S(const std::vector<uint8_t>& data) {
            Network::PacketReader r(data);
            InteractC2SPacket p;
            p.entityId = static_cast<int32_t>(r.ReadVarInt());
            p.action   = static_cast<InteractC2SPacket::Action>(r.ReadByte());
            p.sneaking = r.ReadByte() != 0;
            p.sprinting = r.ReadByte() != 0;
            return p;
        }

    } // namespace Serialization

} // namespace Network
