#include "random/MarsagliaPolarGaussian.h"
#include "random/XoroshiroRandomSource.h"
#include "math/Mth.h"
#include <cmath>

namespace minecraft {

MarsagliaPolarGaussian::MarsagliaPolarGaussian(XoroshiroRandomSource* randomSource)
    : randomSource(randomSource)
    , nextNextGaussian(0.0)
    , haveNextNextGaussian(false) {
    // Reference: MarsagliaPolarGaussian.java lines 11-13
}

void MarsagliaPolarGaussian::reset() {
    // Reference: MarsagliaPolarGaussian.java lines 15-17
    haveNextNextGaussian = false;
}

// Helper: compute nextDouble matching Java's BitRandomSource.nextDouble() pattern.
// Java's WorldgenRandom inherits BitRandomSource.nextDouble() which calls:
//   next(26) and next(27), consuming TWO generator.nextLong() calls.
// NOT XoroshiroRandomSource.nextDouble() which uses nextBits(53) = ONE call.
//
// In Java, MarsagliaPolarGaussian is constructed with `this` (the WorldgenRandom/LegacyRandomSource),
// so randomSource.nextDouble() dispatches to BitRandomSource.nextDouble().
// Reference: LegacyRandomSource.java line 15: new MarsagliaPolarGaussian(this)
static double bitRandomSourceNextDouble(XoroshiroRandomSource* source) {
    // Reference: BitRandomSource.java nextDouble():
    //   int high = this.next(26);
    //   return (double)(((long)high << 27) + (long)this.next(27)) * 1.1102230246251565E-16;
    // Where next(bits) = (int)(randomSource.nextLong() >>> (64 - bits))
    int32_t high = static_cast<int32_t>(static_cast<uint64_t>(source->nextLong()) >> (64 - 26));
    int32_t low = static_cast<int32_t>(static_cast<uint64_t>(source->nextLong()) >> (64 - 27));
    int64_t combined = (static_cast<int64_t>(high) << 27) + static_cast<int64_t>(low);
    return static_cast<double>(combined) * 0x1.0p-53;
}

double MarsagliaPolarGaussian::nextGaussian() {
    // Reference: MarsagliaPolarGaussian.java lines 19-38
    if (haveNextNextGaussian) {
        haveNextNextGaussian = false;
        return nextNextGaussian;
    } else {
        double x;
        double y;
        double radiusSquared;
        do {
            // CRITICAL: Must use BitRandomSource.nextDouble() pattern (2 nextLong calls),
            // NOT XoroshiroRandomSource.nextDouble() (1 nextLong call).
            // Java's MarsagliaPolarGaussian calls randomSource.nextDouble() where randomSource
            // is the WorldgenRandom (which inherits BitRandomSource.nextDouble()).
            x = static_cast<double>(2.0F) * bitRandomSourceNextDouble(randomSource) - static_cast<double>(1.0F);
            y = static_cast<double>(2.0F) * bitRandomSourceNextDouble(randomSource) - static_cast<double>(1.0F);
            radiusSquared = Mth::square(x) + Mth::square(y);
        } while (radiusSquared >= static_cast<double>(1.0F) || radiusSquared == static_cast<double>(0.0F));

        // CRITICAL: Java uses (double)-2.0F - float literal cast to double
        double multiplier = std::sqrt(static_cast<double>(-2.0F) * std::log(radiusSquared) / radiusSquared);
        nextNextGaussian = y * multiplier;
        haveNextNextGaussian = true;
        return x * multiplier;
    }
}

} // namespace minecraft
