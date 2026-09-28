// File: src/common/entity/PlayerRideable.hpp
//
// A MOB a player can sit on — the equines (HorseTaming: horse, donkey, mule,
// skeleton and zombie horse, llama, trader llama) — in the engine's one riding
// system (Server::PlayerRiding).
//
// Players are not Entities here, but their server-side PlayerEntityView is,
// and the riding system puts that view in the mob's passenger list exactly as
// MC puts the Player there: the link is Entity::m_passengers / m_vehicle,
// mirrored on the player by ServerPlayer::getVehicleId (what the save and the
// PlayerMountS2C broadcast read). So a rideable mob asks MC's questions of its
// own passenger list — isVehicle(), getFirstPassenger() instanceof Player —
// and throwing the rider off is MC's ejectPassengers.
//
// What this interface adds is only the seat: where a player's FEET go on the
// mob (the client puts its own player there too, from its copy of the mob).
#pragma once

#include <cstdint>
#include <glm/glm.hpp>

namespace Game {

    class Entity;
    class LivingEntity;

    class PlayerRideable {
    public:
        virtual ~PlayerRideable() = default;

        // MC positionRider for a player passenger: the rider's FEET —
        // getPassengerRidingPosition(player) minus the player's vehicle
        // attachment (Avatar.DEFAULT_VEHICLE_ATTACHMENT, 0.6 up).
        virtual glm::dvec3 PlayerRiderPosition() const = 0;

        // The rider was put down (any reason). Default: nothing.
        virtual void OnPlayerRiderDismounted() {}

        // MC Avatar.DEFAULT_VEHICLE_ATTACHMENT.y.
        static constexpr double kPlayerVehicleAttachmentY = 0.6;

        // The first passenger of `self` when it is a player (its view), else
        // null — MC getFirstPassenger() instanceof Player.
        static LivingEntity* FirstPlayerPassenger(const Entity& self);
        // Any player aboard `self`.
        static bool HasPlayerPassenger(const Entity& self);
        // Throw every player off `self` (MC ejectPassengers for the player
        // seat): each rider's view StopRiding()s, which the riding system
        // turns into the dismount.
        static void EjectPlayerPassengers(Entity& self);
    };

} // namespace Game
