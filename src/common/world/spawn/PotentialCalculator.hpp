// File: src/common/world/spawn/PotentialCalculator.hpp
//
// MC net.minecraft.world.level.PotentialCalculator — the "spawn cost" budget
// used by biomes with a `spawn_costs` block (soul sand valley, basalt deltas).
//
// Every counted mob of a charged type contributes a point charge; a candidate
// position may spawn only while the summed potential (each charge divided by
// straight-line distance) times the candidate's own charge stays within the
// type's energy budget. The effect is soft spacing: skeletons in a soul sand
// valley spread out instead of clustering, without any hard cap.
#pragma once

#include <glm/glm.hpp>

#include <cmath>
#include <limits>
#include <vector>

namespace Game {

    class PotentialCalculator {
    public:
        void AddCharge(const glm::ivec3& pos, double charge) {
            if (charge != 0.0) m_charges.push_back({ pos, charge });
        }

        // MC getPotentialEnergyChange(pos, charge): the summed potential at
        // `pos` scaled by the candidate's own charge. Infinite when a charge
        // sits exactly at `pos` (distSqr == 0), matching MC's explicit
        // POSITIVE_INFINITY.
        double GetPotentialEnergyChange(const glm::ivec3& pos, double charge) const {
            if (charge == 0.0) return 0.0;
            double potential = 0.0;
            for (const PointCharge& point : m_charges) {
                const double dx = point.pos.x - pos.x;
                const double dy = point.pos.y - pos.y;
                const double dz = point.pos.z - pos.z;
                const double distSqr = dx * dx + dy * dy + dz * dz;
                if (distSqr == 0.0) return std::numeric_limits<double>::infinity();
                potential += point.charge / std::sqrt(distSqr);
            }
            return potential * charge;
        }

    private:
        struct PointCharge {
            glm::ivec3 pos;
            double     charge;
        };
        std::vector<PointCharge> m_charges;
    };

} // namespace Game
