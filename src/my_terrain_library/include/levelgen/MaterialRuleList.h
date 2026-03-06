#pragma once

#include "levelgen/BlockStateFiller.h"
#include <vector>

namespace minecraft {
namespace levelgen {

/**
 * MaterialRuleList - Chain of responsibility for block decision rules
 * Reference: MaterialRuleList.java lines 1-20
 *
 * This class implements the chain of responsibility pattern for block placement.
 * It contains a list of BlockStateFiller rules and tries each one in order,
 * returning the first non-null result.
 *
 * Typical rule order:
 * 1. Aquifer - determines water/lava/air
 * 2. OreVeinifier - adds ore veins (if enabled)
 * 3. Other rules...
 */
class MaterialRuleList : public BlockStateFiller {
private:
    std::vector<BlockStateFiller*> m_materialRuleList;

public:
    /**
     * Construct a MaterialRuleList with the given rules.
     *
     * @param rules Vector of BlockStateFiller rules to apply in order
     *              NOTE: MaterialRuleList does NOT take ownership - caller manages memory
     */
    explicit MaterialRuleList(const std::vector<BlockStateFiller*>& rules);

    /**
     * Destructor - does not delete rules (caller owns them)
     */
    ~MaterialRuleList() override = default;

    /**
     * Calculate block type by trying each rule in order.
     * Returns the first non-null result.
     *
     * Reference: MaterialRuleList.java lines 9-18
     *
     * @param context The density function context
     * @return The block type from the first matching rule, or nullptr if no rules match
     */
    BlockState* calculate(
        const density::DensityFunction::FunctionContext& context
    ) const override;
};

} // namespace levelgen
} // namespace minecraft
