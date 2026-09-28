// File: src/common/entity/decoration/Cushion.hpp
//
// MC 26.3 net.minecraft.world.entity.decoration.Cushion — a dyed 1 × 1/4
// seat resting on the top of a block. A player right-clicks it to sit
// (startRiding), sneaks to get up; any hit, push, move, fire, lava or
// lightning breaks it into its item; it pops when the block under it goes.
//
// GEOMETRY (EntityType CUSHION: sized(1.0, 0.25)): `position` is the centre
// of the bottom face, the box the type's dimensions around it
// (Cushion.recalculateBoundingBox → makeBoundingBox). The attached cell
// (BlockAttachedEntity.pos) is BlockPos.containing(position) — the cell the
// cushion sits IN, above its support. yRot is the placing player's facing
// snapped to a horizontal direction; the renderer turns the model by it.
//
// RIDING: through the engine's riding system (Server::PlayerRiding), which
// puts the sitting player's view in the cushion's passenger list as MC puts
// the Player — one seat (Entity.canAddPassenger), taken while IsVehicle().
// The seat is MC's passenger attachment AT_HEIGHT (0, 0.25, 0) minus the
// player's vehicle attachment (Avatar.DEFAULT_VEHICLE_ATTACHMENT, 0.6).
//
// WIRE: the colour rides the variant byte (DyeColor ordinal); the facing,
// yRot. SAVE (MC 26.3): "color" (DyeColor name) and "block_pos" — EntityNbt.
#pragma once

#include "common/entity/decoration/BlockAttachedEntity.hpp"
#include "common/entity/PlayerRideable.hpp"
#include "common/entity/Item.hpp"
#include "common/physics/Physics.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace Game {

    struct IBlockAccess;
    // MC DyeColor — the ordinals of SignBlockEntity.hpp's Game::DyeColor
    // (declared opaquely so this header stays light).
    enum class DyeColor : uint8_t;

    class Cushion final : public BlockAttachedEntity, public PlayerRideable {
    public:
        explicit Cushion(EntityLevel* level);

        // MC Cushion.DEFAULT_COLOR and LIGHTNING_DROP_INVULNERABLE_TICKS.
        static constexpr DyeColor kDefaultColor = static_cast<DyeColor>(0);   // WHITE
        static constexpr int kLightningDropInvulnerableTicks = 20;
        // EntityType CUSHION sized(1.0F, 0.25F).
        static constexpr double kWidth  = 1.0;
        static constexpr double kHeight = 0.25;
        // Avatar.DEFAULT_VEHICLE_ATTACHMENT — where a player's feet sit
        // below the seat.
        static constexpr double kPlayerVehicleAttachmentY = 0.6;
        // CushionItem.COLLISION_SHAPE_RAYCAST_EPSILON.
        static constexpr double kCollisionShapeRaycastEpsilon = 0.001;

        // ── Colour (MC DATA_COLOR) ─────────────────────────────────────────
        DyeColor GetColor() const { return m_color; }
        void SetColor(DyeColor color) { m_color = color; }

        // Items.CUSHION.pick(color) — the item for a colour, and back again
        // (MC's CUSHION_COLOR component, which every cushion item carries
        // as its colour). nullopt for any other item.
        static ItemID ItemFor(DyeColor color);
        static std::optional<DyeColor> ColorOfItem(ItemID item);
        static bool IsCushionItem(ItemID item) { return ColorOfItem(item).has_value(); }
        // DyeColor.CODEC — the colour as its name ("light_blue"), for the
        // "color" save key. ColorFromName is false for an unknown name.
        static const char* ColorName(DyeColor color);
        static bool ColorFromName(std::string_view name, DyeColor& out);

        // MC Cushion.setPos: the raw position, then the attached cell and
        // the box (BlockAttachedEntity.setPos → recalculateBoundingBox).
        void SetPos(const glm::dvec3& pos);
        // The type's box around a bottom-centre position
        // (EntityType.getSpawnAABB / EntityDimensions.makeBoundingBox).
        static AABBd MakeBoundingBox(const glm::dvec3& pos);

        // ── Placement and survival (MC Cushion statics) ────────────────────
        // canBePlacedAt: survives there and its resting face is not buried.
        static bool CanBePlacedAt(const IBlockAccess& level, const AABBd& box);
        // wouldSurviveAt: an anchor under it and not wholly inside
        // suffocating blocks.
        static bool WouldSurviveAt(const IBlockAccess& level, const AABBd& box);
        bool Survives() const override;

        // ── Breaking ───────────────────────────────────────────────────────
        // MC Cushion.dropItem: the break sound (and particles), then the
        // cushion item carrying this one's name — unless entity drops are
        // off or a creative player broke it.
        void DropItem(Entity* causedBy) override;
        // MC Cushion.hurtServer: a player who may not build (adventure,
        // spectator) cannot break it; past that, BlockAttachedEntity's.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        // MC Cushion.thunderHit: killed, and its item dropped.
        void ThunderHit(Entity* bolt) override;
        // MC Cushion.destroyIfInFire: a fire block inside the (deflated)
        // box breaks it — run after placement and at every check interval.
        void DestroyIfInFire();

        // MC getPickResult.
        ItemStack PickResult() const { return ItemStack(ItemFor(m_color), 1); }

        // ── Seat ───────────────────────────────────────────────────────────
        // MC isVehicle(): someone sits here.
        bool IsOccupied() const { return IsVehicle(); }
        // MC positionRider for a player: the seat (passenger attachment
        // AT_HEIGHT) minus the player's vehicle attachment — the rider's
        // feet.
        glm::dvec3 PlayerRiderPosition() const override {
            return position + glm::dvec3(0.0, kHeight - kPlayerVehicleAttachmentY, 0.0);
        }
        // MC Cushion.removePassenger: the get-up sound (server, unless the
        // cushion is itself going away).
        void OnPassengerRemoved(Entity& passenger) override;
        // MC Entity.getDismountLocationForPassenger: centred, on top of the
        // box.
        glm::dvec3 DismountLocation() const {
            return glm::dvec3(position.x, position.y + kHeight, position.z);
        }

        // ── Wire ───────────────────────────────────────────────────────────
        uint8_t GetVariantByte() const override { return static_cast<uint8_t>(m_color); }
        // Client: the colour arrives after the position; the cell and box
        // are rebuilt from it.
        void SetVariantByte(uint8_t v) override;

        float BaseBbWidth() const override { return static_cast<float>(kWidth); }
        float BaseBbHeight() const override { return static_cast<float>(kHeight); }

    protected:
        // MC Cushion.tickAtCheckInterval: the fluid it sits in acts on it
        // (lava burns it away), then the fire check.
        void TickAtCheckInterval() override;

    private:
        void ShowBreakingParticles();
        void RecalculateBoundingBox();

        DyeColor m_color = kDefaultColor;
    };

} // namespace Game
