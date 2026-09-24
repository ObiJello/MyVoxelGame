#pragma once
#include <string_view>
#include <utility>
#include <vector>

namespace console {
// Returns an atlas upload only when the original frame-cycle logic requests one.
class TextureAnimation {
public:
    TextureAnimation(int frameCount, std::vector<std::pair<int,int>> schedule = {});
    int tick(); // -1 means no upload
private:
    int frameCount_, frame, subFrame = 0;
    std::vector<std::pair<int,int>> schedule_;
};
struct LiquidAnimationSpec {
    std::string_view name;
    int x, y, size, frames;
    std::vector<std::pair<int,int>> schedule;
};
std::vector<LiquidAnimationSpec> liquidAnimationSpecs();
}
