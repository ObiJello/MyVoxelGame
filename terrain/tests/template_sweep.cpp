// Load EVERY structure template through the TemplateEngine loader and report
// palette entries the block registry cannot resolve (the exact failure that
// aborts worldgen in the wild: "palette entry X has properties the
// registered block lacks").
#include "levelgen/structure/TemplateEngine.h"
#include "world/level/block/Blocks.h"
#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using minecraft::world::level::block::Blocks;

int main() {
    Blocks::bootstrap();
    // Find data/minecraft/structure via the same walk-up the library uses:
    fs::path root = fs::current_path();
    fs::path structDir;
    for (fs::path p = root; !p.empty() && p != p.root_path(); p = p.parent_path()) {
        if (fs::exists(p / "data/minecraft/structure")) {
            structDir = p / "data/minecraft/structure";
            break;
        }
    }
    if (structDir.empty()) { std::printf("no data dir found\n"); return 2; }

    int total = 0, failed = 0;
    for (const auto& e : fs::recursive_directory_iterator(structDir)) {
        if (!e.is_regular_file() || e.path().extension() != ".nbt") continue;
        // Template id: minecraft:<relpath-without-.nbt>
        std::string rel = fs::relative(e.path(), structDir).string();
        rel = rel.substr(0, rel.size() - 4);
        std::string id = "minecraft:" + rel;
        ++total;
        try {
            (void)minecraft::levelgen::structure::TemplateEngine::get(id);
        } catch (const std::exception& ex) {
            ++failed;
            std::printf("FAIL %s: %s\n", id.c_str(), ex.what());
        }
    }
    std::printf("templates: %d, failures: %d\n", total, failed);
    return failed ? 1 : 0;
}
