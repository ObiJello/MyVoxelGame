// File: src/common/entity/PlayerRideableJumping.hpp
//
// MC net.minecraft.world.entity.PlayerRideableJumping — a mount the steering
// player charges a jump (or a dash) on: the horse family's leap, the camel's
// dash. The steering client holds the jump key to charge the bar
// (Client::Vehicles, MC LocalPlayer.aiStep's jumpRidingScale), and on release
// calls OnPlayerJump with the charge (0..100+) — the client simulates the
// mount, so the jump itself happens there — and tells the server
// (PlayerCommandC2S START_RIDING_JUMP), whose HandleStartJump plays the
// server's half (the horse rears and neighs, the camel's dash cooldown and
// sound). HandleStopJump is MC's STOP_RIDING_JUMP (the camel's dash end).
#pragma once

namespace Game {

    class PlayerRideableJumping {
    public:
        virtual ~PlayerRideableJumping() = default;

        // The steering side (the client): charge the jump with the bar's
        // reading ×100.
        virtual void OnPlayerJump(int jumpAmount) = 0;
        // May the jump be charged now (the horse: saddled; the camel: not
        // sitting, not dashing)?
        virtual bool CanJump() const = 0;
        // The server's half of the jump.
        virtual void HandleStartJump(int jumpScale) = 0;
        virtual void HandleStopJump() = 0;
        // Ticks until the next jump may charge — the camel's dash cooldown
        // (the bar shows its cooldown sprite while > 0).
        virtual int  GetJumpCooldown() const { return 0; }
    };

} // namespace Game
