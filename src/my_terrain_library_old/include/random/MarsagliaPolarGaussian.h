#ifndef MARSAGLIAPOLARGAUSSIAN_H
#define MARSAGLIAPOLARGAUSSIAN_H

namespace minecraft {

// Forward declaration
class XoroshiroRandomSource;

/**
 * Marsaglia polar method for generating Gaussian (normal) distributed random numbers.
 * This is used by XoroshiroRandomSource for nextGaussian().
 *
 * Reference: net/minecraft/world/level/levelgen/MarsagliaPolarGaussian.java
 */
class MarsagliaPolarGaussian {
public:
    /**
     * Construct with a reference to the random source.
     * Reference: MarsagliaPolarGaussian.java lines 11-13
     */
    explicit MarsagliaPolarGaussian(XoroshiroRandomSource* randomSource);

    /**
     * Reset the cached gaussian value.
     * Reference: MarsagliaPolarGaussian.java lines 15-17
     */
    void reset();

    /**
     * Generate the next gaussian-distributed random number.
     * Uses the Marsaglia polar method which generates two values at a time.
     * Reference: MarsagliaPolarGaussian.java lines 19-38
     */
    double nextGaussian();

private:
    XoroshiroRandomSource* randomSource;
    double nextNextGaussian;
    bool haveNextNextGaussian;
};

} // namespace minecraft

#endif // MARSAGLIAPOLARGAUSSIAN_H
