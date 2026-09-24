#pragma once

#include "levelgen/density/DensityFunction.h"

#include <functional>
#include <vector>

// Reference: densityfunction.DfRewriteRule (26.3) - a pass over a density
// function tree. INLINE_REFERENCE replaces a registry reference by its
// target; SliceUniformAxes wraps every subtree that ignores an axis its parent
// uses in a SliceFunction pinned to coordinate 0, so the subtree is sampled
// once per column (or per layer) instead of once per cell.

namespace minecraft {
namespace levelgen {
namespace density {

class DfRewriteRule {
public:
    virtual ~DfRewriteRule() = default;
    virtual DensityFunctionPtr rewrite(const DensityFunctionPtr& function) const = 0;

    static const DfRewriteRule& inlineReference();
    static const DfRewriteRule& sliceUniformAxes();
};

// A rule from a callable (Java's lambda rules).
class LambdaRewriteRule final : public DfRewriteRule {
public:
    explicit LambdaRewriteRule(std::function<DensityFunctionPtr(const DensityFunctionPtr&)> fn)
        : m_fn(std::move(fn)) {}
    DensityFunctionPtr rewrite(const DensityFunctionPtr& function) const override { return m_fn(function); }
private:
    std::function<DensityFunctionPtr(const DensityFunctionPtr&)> m_fn;
};

// DfRewriteRule.sequence(rules...): each rule applied to the previous result.
class SequenceRewriteRule final : public DfRewriteRule {
public:
    explicit SequenceRewriteRule(std::vector<const DfRewriteRule*> rules) : m_rules(std::move(rules)) {}
    DensityFunctionPtr rewrite(const DensityFunctionPtr& function) const override {
        DensityFunctionPtr result = function;
        for (const DfRewriteRule* rule : m_rules) result = rule->rewrite(result);
        return result;
    }
private:
    std::vector<const DfRewriteRule*> m_rules;
};

class SliceUniformAxes final : public DfRewriteRule {
public:
    explicit SliceUniformAxes(int parentDomainAxes) : m_parentDomainAxes(parentDomainAxes) {}
    DensityFunctionPtr rewrite(const DensityFunctionPtr& function) const override;
    int parentDomainAxes() const { return m_parentDomainAxes; }
private:
    DensityFunctionPtr removeAxes(DensityFunctionPtr function, int axes) const;
    static int getExistingRemovedAxes(DensityFunctionPtr function);
    static bool shouldSkip(const DensityFunction& function);
    int m_parentDomainAxes;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
