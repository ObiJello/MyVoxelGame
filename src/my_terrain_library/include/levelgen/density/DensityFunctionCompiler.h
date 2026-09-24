#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"
#include "levelgen/density/SamplerContext.h"

#include <memory>
#include <mutex>
#include <unordered_map>

// Reference: densityfunction.DensityFunctionCompiler (26.3).
//
// Compiles each density function once: inline registry references, replace
// every `cache` by a prepared cache (one per structurally-equal input, each
// with its own cache id), slice uniform axes, then compileSampler. Samplers
// are memoised by structural equality of the function (Java's
// ConcurrentHashMap<DensityFunction, DensitySampler>).

namespace minecraft {
namespace levelgen {
namespace density {

class DensityFunctionCompiler {
public:
    explicit DensityFunctionCompiler(CompileContext& context);

    const DensitySampler& getSampler(const DensityFunctionPtr& function);
    DensitySamplerPtr getSamplerPtr(const DensityFunctionPtr& function);

private:
    class OptimizerRule;
    friend class OptimizerRule;

    DensitySamplerPtr optimizeAndCompile(const DensityFunctionPtr& function);
    DensityFunctionPtr reuseOrPrepareCache(const DensityFunctionPtr& cache);
    DensityFunctionPtr prepareCache(const DensityFunctionPtr& cache);

    CompileContext& m_context;
    std::unique_ptr<DfRewriteRule> m_optimizerRule;
    std::unique_ptr<DfRewriteRule> m_cacheRule;

    std::mutex m_samplersMutex;
    std::unordered_map<DensityFunctionPtr, DensitySamplerPtr, DensityFunctionHash, DensityFunctionEquals> m_samplers;

    std::recursive_mutex m_compileLock;
    std::unordered_map<DensityFunctionPtr, DensityFunctionPtr, DensityFunctionHash, DensityFunctionEquals> m_preparedCaches;
    int m_nextCacheId = 0;
};

// DensitySamplerSet: the samplers of one context (RandomState
// .samplersWithContext).
class DensitySamplerSet {
public:
    DensitySamplerSet(DensityFunctionCompiler& compiler, SamplerContext& context)
        : m_compiler(&compiler), m_context(&context) {}

    BoundSampler get(const DensityFunctionPtr& function) const {
        return BoundSampler{&m_compiler->getSampler(function), m_context};
    }
    float sampleValue(const DensityFunctionPtr& function, int blockX, int blockY, int blockZ) const {
        return m_compiler->getSampler(function).sampleValue(*m_context, blockX, blockY, blockZ);
    }
    SamplerContext& context() const { return *m_context; }

private:
    DensityFunctionCompiler* m_compiler;
    SamplerContext* m_context;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
