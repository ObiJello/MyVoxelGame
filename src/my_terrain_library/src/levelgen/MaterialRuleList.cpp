#include "levelgen/MaterialRuleList.h"
#include "world/level/block/Blocks.h"

namespace minecraft {
namespace levelgen {

// Reference: MaterialRuleList.java lines 5-8
MaterialRuleList::MaterialRuleList(const std::vector<BlockStateFiller*>& rules)
    : m_materialRuleList(rules)
{
}

// Reference: MaterialRuleList.java lines 9-18
BlockState* MaterialRuleList::calculate(
    const density::DensityFunction::FunctionContext& context
) const {
    // Try each rule in order, return first non-null result
    for (BlockStateFiller* rule : m_materialRuleList) {
        BlockState* result = rule->calculate(context);
        if (result != nullptr) {
            return result;
        }
    }

    // No rules matched, return nullptr (caller will use default block)
    return nullptr;
}

} // namespace levelgen
} // namespace minecraft
