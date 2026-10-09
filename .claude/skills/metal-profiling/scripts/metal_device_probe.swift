import Metal
let d = MTLCreateSystemDefaultDevice()!
print("device:", d.name)
for cs in d.counterSets ?? [] {
    print("set \(cs.name): \(cs.counters.count) counters ->", cs.counters.map{$0.name}.joined(separator: ", "))
}
for (n,p) in [("atStageBoundary",MTLCounterSamplingPoint.atStageBoundary),("atDrawBoundary",.atDrawBoundary),("atBlitBoundary",.atBlitBoundary),("atDispatchBoundary",.atDispatchBoundary)] {
    print(n, d.supportsCounterSampling(p))
}
