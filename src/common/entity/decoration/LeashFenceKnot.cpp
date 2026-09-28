// File: src/common/entity/decoration/LeashFenceKnot.cpp
#include "common/entity/decoration/LeashFenceKnot.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/Leashable.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/CrossCollision.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace Game {

    namespace {

        // Every live knot, client and server alike (the client mirrors knots
        // too, so the registry is shared across threads). Knots are few —
        // one per tied fence post — so a flat list is the whole index.
        struct KnotRegistry {
            std::mutex                   mutex;
            std::vector<LeashFenceKnot*> knots;
        };

        KnotRegistry& Registry() {
            static KnotRegistry registry;
            return registry;
        }

    } // namespace

    LeashFenceKnot::LeashFenceKnot(EntityLevel* level)
        : BlockAttachedEntity(EntityTypeId::LeashKnot, level) {
        KnotRegistry& r = Registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.knots.push_back(this);
    }

    LeashFenceKnot::~LeashFenceKnot() {
        KnotRegistry& r = Registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.knots.erase(std::remove(r.knots.begin(), r.knots.end(), this), r.knots.end());
    }

    std::unique_ptr<LeashFenceKnot> LeashFenceKnot::Create(EntityLevel* level, const glm::ivec3& pos) {
        auto knot = std::make_unique<LeashFenceKnot>(level);
        knot->SetKnotPos(pos);
        return knot;
    }

    LeashFenceKnot* LeashFenceKnot::Find(const EntityLevel* level, const glm::ivec3& pos) {
        if (!level) return nullptr;
        KnotRegistry& r = Registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        for (LeashFenceKnot* knot : r.knots) {
            // Level first: a knot of another level (the client's mirror, on
            // another thread) is never read past its level pointer, which is
            // fixed from construction.
            if (knot->Level() != level) continue;
            if (!knot->IsRemoved() && knot->GetBlockPos() == pos) return knot;
        }
        return nullptr;
    }

    void LeashFenceKnot::SetKnotPos(const glm::ivec3& pos) {
        // MC recalculateBoundingBox: the feet at the cell's (0.5, 0.375,
        // 0.5); the type's box stands on them.
        m_pos = pos;
        position = glm::dvec3(pos.x + 0.5, pos.y + kOffsetY, pos.z + 0.5);
        oldPosition = position;
    }

    void LeashFenceKnot::SetVariantByte(uint8_t) {
        // Client: the cell from the position the server sent.
        SetKnotPos(glm::ivec3(static_cast<int>(std::floor(position.x)),
                              static_cast<int>(std::floor(position.y)),
                              static_cast<int>(std::floor(position.z))));
    }

    bool LeashFenceKnot::Survives() const {
        // MC: level.getBlockState(pos).is(BlockTags.FENCES).
        if (!m_level || !m_level->Blocks()) return false;
        return IsFenceBlock(m_level->Blocks()->GetBlock(m_pos.x, m_pos.y, m_pos.z));
    }

    void LeashFenceKnot::DropItem(Entity* causedBy) {
        (void)causedBy;
        PlaySound(SoundEvents::LEAD_UNTIED, 1.0f, 1.0f);
    }

    void LeashFenceKnot::PlayPlacementSound() {
        PlaySound(SoundEvents::LEAD_TIED, 1.0f, 1.0f);
    }

    void LeashFenceKnot::NotifyLeasheeRemoved(Mob& leashee) {
        (void)leashee;
        // MC: `if (Leashable.leashableLeashedTo(this).isEmpty()) discard()`.
        std::vector<Mob*> still;
        Leash::LeashableLeashedTo(*this, still);
        if (still.empty()) Discard();
    }

} // namespace Game
