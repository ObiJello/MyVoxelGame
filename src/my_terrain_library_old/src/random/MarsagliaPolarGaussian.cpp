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
            // CRITICAL: Java uses (double)2.0F - float literal cast to double
            x = static_cast<double>(2.0F) * randomSource->nextDouble() - static_cast<double>(1.0F);
            y = static_cast<double>(2.0F) * randomSource->nextDouble() - static_cast<double>(1.0F);
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
