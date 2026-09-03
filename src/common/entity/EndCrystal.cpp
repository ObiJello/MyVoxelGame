// File: src/common/entity/EndCrystal.cpp
#include "common/entity/EndCrystal.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/DragonFight.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/Explosion.hpp"

namespace Game {

    EndCrystal::EndCrystal(EntityLevel* level)
        : Mob(EntityTypeId::EndCrystal, level, NoAiTag{}) {
        // MC: this.time = this.random.nextInt(100000) — desynchronises the
        // bob/rotation of a ring of crystals.
        if (m_level) time = m_level->Random().NextInt(100000);

        // No goals, no brain, no SetTarget, no ClearReferenceTo override —
        // the dying-mob reference sweep is a provable no-op for this type.
        // (The DRAGON's pointer at a crystal is the dragon's to clear.)
        // See Entity::HoldsEntityRefs and PrimedTnt's identical opt-out.
        ClearHoldsEntityRefs();
    }

    void EndCrystal::Tick() {
        // MC EndCrystal.tick — no super, no gravity, no mover: the crystal
        // sits where it was placed. ++time is both sides' animation clock;
        // the fire block underneath is the server's.
        ++time;

        if (m_level && !m_level->IsClientSide() &&
            m_level->DragonFight() != nullptr && m_level->Blocks()) {
            // MC: inside a dragon fight, a crystal keeps its fire lit — the
            // block AT the crystal's position, as BaseFireBlock.getState
            // places it (the End has no soul soil, so plain fire always).
            const glm::ivec3 pos = BlockPosition();
            if (m_level->Blocks()->GetBlock(pos.x, pos.y, pos.z) == BlockID::Air) {
                m_level->SetBlock(pos, BlockID::Fire);
            }
        }
    }

    bool EndCrystal::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC EndCrystal.hurtServer, transcribed. The crystal has no health:
        // any hit that gets past the guards destroys it outright.
        (void)amount;
        if (!m_level || m_level->IsClientSide() || IsRemoved()) return false;

        // MC isInvulnerableToBase: fireImmune covers fire, the invulnerable
        // flag (the four ritual crystals) covers everything but the void.
        if (source == MobDamageSource::Fire) return false;
        if (IsInvulnerable() && source != MobDamageSource::Void) return false;

        // MC: the dragon cannot pop its own crystals (wing sweeps).
        if (attacker != nullptr && dynamic_cast<EnderDragon*>(attacker) != nullptr) {
            return false;
        }

        // MC removes FIRST, so the blast's own entity sweep does not find the
        // crystal that produced it.
        Remove(RemovalReason::Killed);

        if (source != MobDamageSource::Explosion) {
            // MC: level.explode(this, source, null, x, y, z, 6.0F, false,
            // ExplosionInteraction.BLOCK). Queued like TNT's — the resolve
            // runs this same tick, before MobManager sweeps this entity, so
            // `source = this` is still alive when the apply reads it.
            ExplosionParams params;
            params.center = position;
            params.radius = 6.0f;
            params.source = this;
            params.attributedTo = attacker;
            params.interaction = ExplosionInteraction::Block;
            params.fire = false;
            m_level->QueueExplosion(params);
        }

        OnDestroyedBy(attacker);
        return true;
    }

    void EndCrystal::OnDestroyedBy(Entity* attacker) {
        // MC EndCrystal.onDestroyedBy → dragonFight.onCrystalDestroyed.
        if (IDragonFight* fight = m_level ? m_level->DragonFight() : nullptr) {
            fight->OnCrystalDestroyed(*this, attacker);
        }
    }

    void EndCrystal::SetBeamTarget(const glm::ivec3& target) {
        if (m_hasBeamTarget && m_beamTarget == target) return;
        m_beamTarget = target;
        m_hasBeamTarget = true;
        m_beamDirty = true;
    }

    void EndCrystal::ClearBeamTarget() {
        if (!m_hasBeamTarget) return;
        m_hasBeamTarget = false;
        m_beamDirty = true;
    }

} // namespace Game
