#include "levelgen/density/DfRewriteRule.h"
#include "levelgen/density/CoreFunctions.h"

// Reference: densityfunction.DfRewriteRule (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

const DfRewriteRule& DfRewriteRule::inlineReference() {
    static const LambdaRewriteRule rule([](const DensityFunctionPtr& function) -> DensityFunctionPtr {
        if (const auto* reference = dynamic_cast<const ReferenceFunction*>(function.get())) {
            return reference->target();
        }
        return function;
    });
    return rule;
}

const DfRewriteRule& DfRewriteRule::sliceUniformAxes() {
    static const SliceUniformAxes rule(ALL_AXES);
    return rule;
}

DensityFunctionPtr SliceUniformAxes::rewrite(const DensityFunctionPtr& function) const {
    if (shouldSkip(*function)) return function;
    const int domainAxes = function->domainAxes();
    if (m_parentDomainAxes == domainAxes) {
        return function->rewriteChildren(*this);
    }
    const SliceUniformAxes childRule(domainAxes);
    DensityFunctionPtr newFunction = function->rewriteChildren(childRule);
    const int removedAxes = m_parentDomainAxes & ~domainAxes;
    return removeAxes(newFunction, removedAxes);
}

DensityFunctionPtr SliceUniformAxes::removeAxes(DensityFunctionPtr function, int axes) const {
    const int filteredAxes = axes & ~getExistingRemovedAxes(function);
    if ((filteredAxes & AXIS_X) != 0) function = std::make_shared<SliceFunction>(Axis::X, 0, function);
    if ((filteredAxes & AXIS_Z) != 0) function = std::make_shared<SliceFunction>(Axis::Z, 0, function);
    if ((filteredAxes & AXIS_Y) != 0) function = std::make_shared<SliceFunction>(Axis::Y, 0, function);
    return function;
}

int SliceUniformAxes::getExistingRemovedAxes(DensityFunctionPtr function) {
    int axes = 0;
    while (const auto* slice = dynamic_cast<const SliceFunction*>(function.get())) {
        axes |= axesFrom(slice->axis());
        function = slice->input();
    }
    return axes;
}

bool SliceUniformAxes::shouldSkip(const DensityFunction& function) {
    // `function instanceof ConstantFunction || function instanceof GradientFunction`
    const std::string type = function.typeId();
    return type == "minecraft:constant" || type == "minecraft:gradient";
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
