#include "levelgen/density/synth/NormalNoise.h"

// Reference: levelgen.Noises.instantiate (26.3):
//   holder.value().create(context.fromHashOf(name.identifier()))
// PositionalRandomFactory.fromHashOf(Identifier) hashes name.toString().

namespace minecraft {
namespace levelgen {
namespace density {
namespace synth {

NoisePtr instantiate(const NormalNoise& noise, const std::string& key,
                     const random::AnyPositionalRandomFactory& random) {
    random::AnyRandomSource source = random.fromHashOf(key);
    return noise.create(source);
}

} // namespace synth
} // namespace density
} // namespace levelgen
} // namespace minecraft
