#include "levelgen/density/DensityFunctionCompiler.h"
#include "levelgen/density/CoreFunctions.h"

#include <stdexcept>

// Reference: densityfunction.DensityFunctionCompiler (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

namespace {

// DensityFunctionCompiler.PreparedCache: a deduplicated cache, compiled once.
class PreparedCache final : public DensityFunction {
public:
    PreparedCache(int id, Interval range, int domainAxes, DensitySamplerPtr cachingSampler)
        : m_id(id), m_range(range), m_domainAxes(domainAxes), m_cachingSampler(std::move(cachingSampler)) {}

    DensitySamplerPtr compileSampler(CompileContext&) const override { return m_cachingSampler; }
    DensityFunctionPtr rewriteChildren(const DfRewriteRule&) const override { return self(); }
    Interval range() const override { return m_range; }
    int domainAxes() const override { return m_domainAxes; }
    std::string typeId() const override { return "prepared_cache"; }
    bool equals(const DensityFunction& other) const override {
        const auto* o = dynamic_cast<const PreparedCache*>(&other);
        return o != nullptr && m_id == o->m_id && m_range == o->m_range &&
               m_domainAxes == o->m_domainAxes && m_cachingSampler == o->m_cachingSampler;
    }
    size_t hash() const override { return hashCombine(0x15, std::hash<int>()(m_id)); }

private:
    int m_id;
    Interval m_range;
    int m_domainAxes;
    DensitySamplerPtr m_cachingSampler;
};

} // namespace

// The first optimizer pass: inline references and deduplicate caches,
// recursively (Java's anonymous DfRewriteRule in the constructor).
class DensityFunctionCompiler::OptimizerRule final : public DfRewriteRule {
public:
    explicit OptimizerRule(DensityFunctionCompiler& compiler) : m_compiler(compiler) {}
    DensityFunctionPtr rewrite(const DensityFunctionPtr& input) const override {
        DensityFunctionPtr function = DfRewriteRule::inlineReference().rewrite(input);
        if (dynamic_cast<const CacheFunction*>(function.get()) != nullptr) {
            return m_compiler.reuseOrPrepareCache(function);
        }
        return function->rewriteChildren(*this);
    }
private:
    DensityFunctionCompiler& m_compiler;
};

DensityFunctionCompiler::DensityFunctionCompiler(CompileContext& context) : m_context(context) {
    m_cacheRule = std::make_unique<OptimizerRule>(*this);
    m_optimizerRule = std::make_unique<SequenceRewriteRule>(
        std::vector<const DfRewriteRule*>{m_cacheRule.get(), &DfRewriteRule::sliceUniformAxes()});
}

const DensitySampler& DensityFunctionCompiler::getSampler(const DensityFunctionPtr& function) {
    return *getSamplerPtr(function);
}

DensitySamplerPtr DensityFunctionCompiler::getSamplerPtr(const DensityFunctionPtr& function) {
    {
        std::lock_guard<std::mutex> lock(m_samplersMutex);
        auto it = m_samplers.find(function);
        if (it != m_samplers.end()) return it->second;
    }
    DensitySamplerPtr sampler = optimizeAndCompile(function);
    std::lock_guard<std::mutex> lock(m_samplersMutex);
    // computeIfAbsent: the first compiled sampler wins.
    auto [it, inserted] = m_samplers.emplace(function, sampler);
    return it->second;
}

DensitySamplerPtr DensityFunctionCompiler::optimizeAndCompile(const DensityFunctionPtr& function) {
    std::lock_guard<std::recursive_mutex> lock(m_compileLock);
    DensityFunctionPtr optimizedFunction = m_optimizerRule->rewrite(function);
    return optimizedFunction->compileSampler(m_context);
}

DensityFunctionPtr DensityFunctionCompiler::reuseOrPrepareCache(const DensityFunctionPtr& cacheFunction) {
    const auto* cache = static_cast<const CacheFunction*>(cacheFunction.get());
    auto it = m_preparedCaches.find(cache->input());
    if (it != m_preparedCaches.end()) return it->second;
    DensityFunctionPtr prepared = prepareCache(cacheFunction);
    m_preparedCaches.emplace(cache->input(), prepared);
    return prepared;
}

DensityFunctionPtr DensityFunctionCompiler::prepareCache(const DensityFunctionPtr& cacheFunction) {
    const auto* cache = static_cast<const CacheFunction*>(cacheFunction.get());
    const int id = m_nextCacheId++;
    DensityFunctionPtr input = m_optimizerRule->rewrite(cache->input());
    auto cachingSampler = std::make_shared<CachingDensitySampler>(id, input->compileSampler(m_context));
    return std::make_shared<PreparedCache>(id, input->range(), input->domainAxes(), cachingSampler);
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
