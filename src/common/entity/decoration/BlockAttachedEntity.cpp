// File: src/common/entity/decoration/BlockAttachedEntity.cpp
#include "common/entity/decoration/BlockAttachedEntity.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"

#include "common/entity/ArmorStand.hpp"
#include "common/entity/EndCrystal.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/FallingBlockEntity.hpp"
#include "common/entity/PrimedTnt.hpp"
#include "common/entity/projectile/Projectile.hpp"

namespace Game {

    BlockAttachedEntity::BlockAttachedEntity(EntityTypeId type, EntityLevel* level)
        : Mob(type, level, NoAiTag{}) {
        // No health to speak of; the living attributes exist only because the
        // Mob pipeline reads them.
        CreateLivingAttributes(m_attributes);
        m_health = GetMaxHealth();
        ClearHoldsEntityRefs();
    }

    void BlockAttachedEntity::Tick() {
        // MC BlockAttachedEntity.tick — the server's alone.
        if (!m_level || m_level->IsClientSide()) return;
        // checkBelowWorld.
        if (position.y < static_cast<double>(m_level->GetMinY() - 64)) {
            Remove(RemovalReason::Discarded);
            return;
        }
        if (m_ticksSinceLastCheck++ >= kCheckInterval) {
            m_ticksSinceLastCheck = 0;
            TickAtCheckInterval();
            if (!IsRemoved() && !SurvivesCheck()) {
                Remove(RemovalReason::Discarded);
                DropItem(nullptr);
            }
        }
    }

    void BlockAttachedEntity::Kill(Entity* attributedTo) {
        // MC kill(level, attributedTo): onKilled, remove(KILLED), then
        // gameEvent(ENTITY_DIE, attributedTo, else this).
        OnKilled();
        Remove(RemovalReason::Killed);
        GameEvent(GameEventId::EntityDie, attributedTo ? attributedTo : this);
    }

    bool BlockAttachedEntity::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        (void)amount;
        if (!m_level || m_level->IsClientSide()) return false;
        // isInvulnerableToBase: the invulnerable flag holds against all but
        // the void and a creative player (source.isCreativePlayer()).
        const bool creativePlayer = attacker && attacker->IsPlayer() && attacker->IsCreative();
        if (IsInvulnerable() && source != MobDamageSource::Void && !creativePlayer) return false;
        // Mobs break block-attached entities only while mob griefing is on.
        // MC tests source.getEntity() — the CAUSING entity: a projectile's
        // shooter, primed TNT's igniter — for `instanceof Mob`. The engine's
        // non-mob entities ride the Mob class (see the header), so they are
        // resolved to their cause, or passed over, first.
        if (!m_level->MobGriefing() && attacker && !attacker->IsPlayer()) {
            Entity* cause = attacker;
            if (auto* projectile = dynamic_cast<Projectile*>(attacker)) cause = projectile->GetOwner();
            else if (auto* tnt = dynamic_cast<PrimedTnt*>(attacker))   cause = tnt->GetOwner();
            const bool causeIsMob = cause && !cause->IsPlayer() && dynamic_cast<Mob*>(cause) &&
                                    !dynamic_cast<Projectile*>(cause) && !dynamic_cast<PrimedTnt*>(cause) &&
                                    !dynamic_cast<FallingBlockEntity*>(cause) && !dynamic_cast<EndCrystal*>(cause) &&
                                    !dynamic_cast<BlockAttachedEntity*>(cause) && !dynamic_cast<ArmorStand*>(cause);
            if (causeIsMob) return false;
        }
        if (!IsRemoved()) {
            Kill(attacker);   // MC kill(level, source.getEntity())
            DropItem(attacker);
        }
        return true;
    }

    bool BlockAttachedEntity::SkipAttackInteraction(Entity& source) {
        // (mayInteract — spawn protection — has no counterpart here.)
        if (!source.IsPlayer()) return false;
        return Hurt(MobDamageSource::PlayerAttack, 0.0f, &source);
    }

} // namespace Game
