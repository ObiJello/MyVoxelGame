#pragma once
#include <string_view>

namespace console {
struct PlayerHurtState {
    int health=20;
    int invulnerableTicks=0;
    int hurtTicks=0;
    int deathTicks=0;
    int lastHurt=0;
};

// Monster::doHurtTarget, Silverfish::checkHurtTarget and Slime::playerTouch.
int sourceMobMeleeDamage(std::wstring_view id,int slimeSize=1);
// Player::hurt difficulty scaling followed by Mob::hurt's 20-tick damage window.
bool applySourcePlayerHurt(PlayerHurtState& player,int damage,int difficulty,bool invulnerable);
// Mob::hurt's damage window alone, for sources that do not scale with
// difficulty (falling, drowning, lava, fire, suffocation, the void).
bool applySourceMobHurt(PlayerHurtState& player,int damage,bool invulnerable);
void tickSourcePlayerHurt(PlayerHurtState& player);
}
