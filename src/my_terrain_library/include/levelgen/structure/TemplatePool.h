#pragma once

#include <memory>
#include <string>
#include <vector>

// Reference: net/minecraft/world/level/levelgen/structure/pools/
// StructureTemplatePool.java + StructurePoolElement subclasses, loaded from
// data/minecraft/worldgen/template_pool/**.json (188 files).
//
// Parity-relevant: the pool's `templates` list is the element list EXPANDED
// BY WEIGHT (element repeated weight times, in file order), so
// getRandomTemplate = templates[nextInt(size)] and getShuffledTemplates =
// Util.shuffledCopy(templates, random).

namespace minecraft {
namespace levelgen {
namespace structure {

// Element kinds (registry ids shortened).
enum class PoolElementKind { SINGLE, LEGACY_SINGLE, FEATURE, LIST, EMPTY };

struct PoolElement {
    PoolElementKind kind = PoolElementKind::EMPTY;
    std::string location;    // template id for single/legacy ("" otherwise)
    std::string processors;  // processor list id or "" (inline empty)
    std::string projection;  // "rigid" or "terrain_matching"
    std::string feature;     // feature id for FEATURE elements
    std::vector<PoolElement> listElements;  // LIST elements

    bool isEmpty() const { return kind == PoolElementKind::EMPTY; }
};

struct TemplatePool {
    std::string name;      // "minecraft:village/plains/town_centers"
    std::string fallback;  // pool id
    // Weight-expanded flat list; entries POINT INTO ownedElements.
    std::vector<const PoolElement*> templates;
    std::vector<std::unique_ptr<PoolElement>> ownedElements;

    int size() const { return static_cast<int>(templates.size()); }
};

namespace TemplatePools {

/** Pool by id; loads + caches all pools lazily. Throws on unknown id. */
const TemplatePool& byName(const std::string& name);

/** True when the id resolves to a loadable pool file (or the builtin empty). */
bool exists(const std::string& name);

/** The builtin "minecraft:empty" pool (zero elements). */
const TemplatePool& empty();

} // namespace TemplatePools

} // namespace structure
} // namespace levelgen
} // namespace minecraft
