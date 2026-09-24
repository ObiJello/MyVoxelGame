#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
class Layer;
namespace console {
enum class BiomeScale { Normal, Large, Legacy11 };
class BiomeGenerator {
public:
    explicit BiomeGenerator(std::int64_t seed,BiomeScale scale=BiomeScale::Normal);
    std::vector<std::uint8_t> area(int x,int z,int width,int depth,bool raw=false);
private:
    std::shared_ptr<Layer> raw_,zoomed_;
    std::mutex mutex_;
};
}
