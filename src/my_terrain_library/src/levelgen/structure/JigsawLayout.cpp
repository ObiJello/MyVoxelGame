#include "levelgen/structure/StructureLayouts.h"

#include "levelgen/structure/JigsawTemplates.h"
#include "levelgen/structure/TemplatePool.h"
#include "levelgen/structure/PieceBehaviors.h"
#include "levelgen/ChunkGenerator.h"

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <vector>

// Reference: net/minecraft/world/level/levelgen/structure/pools/
// JigsawPlacement.java (full port; draw-order notes in the memory file).
// Free space uses an exact box-set model instead of Java's discretized
// VoxelShapes: inputs here are axis-aligned boxes on quarter-integer coords,
// where the discrete join semantics reduce to positive-volume overlap tests
// (verified empirically by the byte gates; a full BitSet port is the fallback
// if a gate ever disagrees).

namespace minecraft {
namespace levelgen {
namespace structure {

namespace {

struct DBox {
    double minX, minY, minZ, maxX, maxY, maxZ;
};

DBox boxOf(const BoundingBox& b) {
    // Reference: AABB.of(BoundingBox) - max corner is max + 1.
    return {static_cast<double>(b.minX), static_cast<double>(b.minY), static_cast<double>(b.minZ),
            static_cast<double>(b.maxX + 1), static_cast<double>(b.maxY + 1), static_cast<double>(b.maxZ + 1)};
}

DBox deflate(const DBox& b, double amount) {
    return {b.minX + amount, b.minY + amount, b.minZ + amount,
            b.maxX - amount, b.maxY - amount, b.maxZ - amount};
}

bool strictOverlap(const DBox& a, const DBox& b) {
    return a.minX < b.maxX && a.maxX > b.minX
        && a.minY < b.maxY && a.maxY > b.minY
        && a.minZ < b.maxZ && a.maxZ > b.minZ;
}

// free = outer minus union(occupied). fits(b) == "b entirely inside free".
struct FreeSpace {
    DBox outer;
    std::vector<DBox> occupied;

    bool fits(const DBox& b) const {
        if (b.minX < outer.minX || b.minY < outer.minY || b.minZ < outer.minZ
            || b.maxX > outer.maxX || b.maxY > outer.maxY || b.maxZ > outer.maxZ) {
            return false;
        }
        for (const DBox& occ : occupied) {
            if (strictOverlap(b, occ)) return false;
        }
        return true;
    }
    void occupy(const DBox& b) { occupied.push_back(b); }
};

// Reference: SequencedPriorityIterator - highest priority first, FIFO within.
template <typename T>
struct SequencedPriorityQueue {
    std::map<int, std::deque<T>> queues;
    bool empty() const {
        for (const auto& [prio, queue] : queues) {
            if (!queue.empty()) return false;
        }
        return true;
    }
    void add(T value, int priority) { queues[priority].push_back(std::move(value)); }
    T next() {
        auto it = queues.rbegin();
        while (it->second.empty()) ++it;
        T value = std::move(it->second.front());
        it->second.pop_front();
        return value;
    }
};

const char* elementTypeId(PoolElementKind kind) {
    switch (kind) {
        case PoolElementKind::SINGLE: return "minecraft:single_pool_element";
        case PoolElementKind::LEGACY_SINGLE: return "minecraft:legacy_single_pool_element";
        case PoolElementKind::FEATURE: return "minecraft:feature_pool_element";
        case PoolElementKind::LIST: return "minecraft:list_pool_element";
        case PoolElementKind::EMPTY: return "minecraft:empty_pool_element";
    }
    return "?";
}

// Reference: StructurePoolElement.getBoundingBox per subclass.
BoundingBox elementBox(const PoolElement& element, int32_t x, int32_t y, int32_t z, int rotation) {
    switch (element.kind) {
        case PoolElementKind::SINGLE:
        case PoolElementKind::LEGACY_SINGLE:
            return JigsawTemplates::elementBoundingBox(element.location, x, y, z, rotation);
        case PoolElementKind::FEATURE:
            // size ZERO -> box = pos..pos (maxima = pos + 0).
            return BoundingBox(x, y, z, x, y, z);
        case PoolElementKind::LIST: {
            BoundingBox result(0, 0, 0, 0, 0, 0);
            bool first = true;
            for (const PoolElement& sub : element.listElements) {
                if (sub.kind == PoolElementKind::EMPTY) continue;
                BoundingBox b = elementBox(sub, x, y, z, rotation);
                if (first) { result = b; first = false; }
                else {
                    result.minX = std::min(result.minX, b.minX);
                    result.minY = std::min(result.minY, b.minY);
                    result.minZ = std::min(result.minZ, b.minZ);
                    result.maxX = std::max(result.maxX, b.maxX);
                    result.maxY = std::max(result.maxY, b.maxY);
                    result.maxZ = std::max(result.maxZ, b.maxZ);
                }
            }
            return result;
        }
        case PoolElementKind::EMPTY:
            break;
    }
    return BoundingBox(x, y, z, x, y, z);
}

// Reference: getShuffledJigsawBlocks per subclass (shuffle + stable
// selection-priority sort for template elements; feature = 1 synthetic).
std::vector<PlacedJigsaw> shuffledJigsaws(const PoolElement& element,
                                          int32_t x, int32_t y, int32_t z, int rotation,
                                          LegacyRandomSource& random) {
    std::vector<PlacedJigsaw> jigsaws;
    switch (element.kind) {
        case PoolElementKind::SINGLE:
        case PoolElementKind::LEGACY_SINGLE:
            jigsaws = JigsawTemplates::getJigsaws(element.location, x, y, z, rotation);
            break;
        case PoolElementKind::FEATURE: {
            PlacedJigsaw synthetic;
            synthetic.x = x; synthetic.y = y; synthetic.z = z;
            synthetic.front = D_DOWN;
            synthetic.top = D_SOUTH;
            synthetic.name = "minecraft:bottom";
            synthetic.pool = "minecraft:empty";
            synthetic.target = "minecraft:empty";
            synthetic.rollable = true;  // vertical front -> ROLLABLE default
            jigsaws.push_back(std::move(synthetic));
            break;
        }
        case PoolElementKind::LIST:
            if (!element.listElements.empty()) {
                return shuffledJigsaws(element.listElements.front(), x, y, z, rotation, random);
            }
            break;
        case PoolElementKind::EMPTY:
            break;
    }
    // Util.shuffle (reverse Fisher-Yates; no draws for size <= 1)...
    for (int i = static_cast<int>(jigsaws.size()); i > 1; --i) {
        int swapTo = random.nextInt(i);
        std::swap(jigsaws[static_cast<size_t>(i - 1)], jigsaws[static_cast<size_t>(swapTo)]);
    }
    // ...then STABLE sort by selectionPriority DESC.
    std::stable_sort(jigsaws.begin(), jigsaws.end(),
                     [](const PlacedJigsaw& a, const PlacedJigsaw& b) {
                         return a.selectionPriority > b.selectionPriority;
                     });
    return jigsaws;
}

// Reference: StructureTemplatePool.getShuffledTemplates = Util.shuffledCopy.
std::vector<const PoolElement*> shuffledTemplates(const TemplatePool& pool,
                                                  LegacyRandomSource& random) {
    std::vector<const PoolElement*> copy = pool.templates;
    for (int i = static_cast<int>(copy.size()); i > 1; --i) {
        int swapTo = random.nextInt(i);
        std::swap(copy[static_cast<size_t>(i - 1)], copy[static_cast<size_t>(swapTo)]);
    }
    return copy;
}

BoundingBox elementBox(const PoolElement& element, int32_t x, int32_t y, int32_t z, int rotation);

// Reference: StructureTemplatePool.getMaxSize - max bbox Y SPAN over non-empty
// elements at rotation NONE (features count as span 1).
int poolMaxSize(const TemplatePool& pool) {
    int maxSize = 0;
    for (const auto& owned : pool.ownedElements) {
        if (owned->kind == PoolElementKind::EMPTY) continue;
        maxSize = std::max(maxSize, elementBox(*owned, 0, 0, 0, 0).getYSpan());
    }
    return maxSize;
}

struct PlacedPiece {
    const PoolElement* element;
    int32_t posX, posY, posZ;
    int rotation;
    BoundingBox box;
    int groundLevelDelta;
    std::vector<std::array<int, 4>> junctions;  // x, sourceGroundY, z, deltaY
};

struct PieceState {
    size_t pieceIndex;
    std::shared_ptr<FreeSpace> free;
    int depth;
};

// Reference: Rotation.getShuffled = Util.shuffledCopy(values(), random).
std::array<int, 4> shuffledRotations(LegacyRandomSource& random) {
    std::array<int, 4> rotations = {0, 1, 2, 3};
    for (int i = 4; i > 1; --i) {
        int swapTo = random.nextInt(i);
        std::swap(rotations[static_cast<size_t>(i - 1)], rotations[static_cast<size_t>(swapTo)]);
    }
    return rotations;
}

// Reference: JigsawBlock.canAttach.
bool canAttach(const PlacedJigsaw& source, const PlacedJigsaw& target) {
    return source.front == oppositeDir(target.front)
        && (source.rollable || source.top == target.top)
        && source.target == target.name;
}

struct Placer {
    int maxDepth;
    ChunkGenerator* generator;
    RandomState* randomState;
    LegacyRandomSource& random;
    std::vector<PlacedPiece>& pieces;
    std::function<const std::string&(const std::string&)> aliasLookup;
    SequencedPriorityQueue<PieceState> placing;

    void tryPlacingChildren(size_t sourceIndex, std::shared_ptr<FreeSpace> contextFree,
                            int depth, bool doExpansionHack) {
        // Copy source fields (pieces vector may grow).
        const PoolElement* sourceElement = pieces[sourceIndex].element;
        int32_t srcX = pieces[sourceIndex].posX;
        int32_t srcY = pieces[sourceIndex].posY;
        int32_t srcZ = pieces[sourceIndex].posZ;
        int sourceRotation = pieces[sourceIndex].rotation;
        std::string sourceProjection = sourceElement->projection;
        bool sourceRigid = sourceProjection == "rigid";
        BoundingBox sourceBB = pieces[sourceIndex].box;
        int32_t sourceBoxY = sourceBB.minY;
        std::shared_ptr<FreeSpace> sourceFree;  // lazily created

        for (const PlacedJigsaw& sourceJigsaw :
             shuffledJigsaws(*sourceElement, srcX, srcY, srcZ, sourceRotation, random)) {
            int sourceDirection = sourceJigsaw.front;
            int32_t tjx = sourceJigsaw.x + dirStepX(sourceDirection);
            int32_t tjy = sourceJigsaw.y + dirStepY(sourceDirection);
            int32_t tjz = sourceJigsaw.z + dirStepZ(sourceDirection);
            int32_t sourceJigsawLocalY = sourceJigsaw.y - sourceBoxY;
            int32_t sourceJigsawBaseHeight = INT32_MIN;

            const std::string& resolvedPool = aliasLookup(sourceJigsaw.pool);
            if (!TemplatePools::exists(resolvedPool)) continue;  // warn-skip
            const TemplatePool& targetPool = TemplatePools::byName(resolvedPool);
            if (targetPool.size() == 0 && targetPool.name != "minecraft:empty") continue;
            if (!TemplatePools::exists(targetPool.fallback)) continue;
            const TemplatePool& fallback = TemplatePools::byName(targetPool.fallback);
            if (fallback.size() == 0 && fallback.name != "minecraft:empty") continue;

            bool attachInsideSource = tjx >= sourceBB.minX && tjx <= sourceBB.maxX
                                   && tjy >= sourceBB.minY && tjy <= sourceBB.maxY
                                   && tjz >= sourceBB.minZ && tjz <= sourceBB.maxZ;
            std::shared_ptr<FreeSpace> childrenFree;
            if (attachInsideSource) {
                if (!sourceFree) {
                    sourceFree = std::make_shared<FreeSpace>();
                    sourceFree->outer = boxOf(sourceBB);
                }
                childrenFree = sourceFree;
            } else {
                childrenFree = contextFree;
            }

            std::vector<const PoolElement*> targetPieces;
            if (depth != maxDepth) {
                auto poolTemplates = shuffledTemplates(targetPool, random);
                targetPieces.insert(targetPieces.end(), poolTemplates.begin(), poolTemplates.end());
            }
            auto fallbackTemplates = shuffledTemplates(fallback, random);
            targetPieces.insert(targetPieces.end(), fallbackTemplates.begin(), fallbackTemplates.end());
            int placementPriority = sourceJigsaw.placementPriority;

            bool placedForThisJigsaw = false;
            for (const PoolElement* targetElement : targetPieces) {
                if (targetElement->kind == PoolElementKind::EMPTY) break;

                for (int targetRotation : shuffledRotations(random)) {
                    std::vector<PlacedJigsaw> targetJigsaws =
                        shuffledJigsaws(*targetElement, 0, 0, 0, targetRotation, random);
                    BoundingBox hackBox = elementBox(*targetElement, 0, 0, 0, targetRotation);
                    int expandTo = 0;
                    if (doExpansionHack && hackBox.getYSpan() <= 16) {
                        for (const PlacedJigsaw& tj : targetJigsaws) {
                            int32_t px = tj.x + dirStepX(tj.front);
                            int32_t py = tj.y + dirStepY(tj.front);
                            int32_t pz = tj.z + dirStepZ(tj.front);
                            bool inside = px >= hackBox.minX && px <= hackBox.maxX
                                       && py >= hackBox.minY && py <= hackBox.maxY
                                       && pz >= hackBox.minZ && pz <= hackBox.maxZ;
                            if (!inside) continue;
                            int childPoolSize = 0, childFallbackSize = 0;
                            const std::string& childPoolName = aliasLookup(tj.pool);
                            if (TemplatePools::exists(childPoolName)) {
                                const TemplatePool& childPool = TemplatePools::byName(childPoolName);
                                childPoolSize = poolMaxSize(childPool);
                                if (TemplatePools::exists(childPool.fallback)) {
                                    childFallbackSize = poolMaxSize(TemplatePools::byName(childPool.fallback));
                                }
                            }
                            expandTo = std::max(expandTo, std::max(childPoolSize, childFallbackSize));
                        }
                    }

                    for (const PlacedJigsaw& targetJigsaw : targetJigsaws) {
                        if (!canAttach(sourceJigsaw, targetJigsaw)) continue;
                        int32_t rawX = tjx - targetJigsaw.x;
                        int32_t rawY = tjy - targetJigsaw.y;
                        int32_t rawZ = tjz - targetJigsaw.z;
                        BoundingBox rawTargetBB = elementBox(*targetElement, rawX, rawY, rawZ, targetRotation);
                        int32_t rawTargetY = rawTargetBB.minY;
                        bool targetRigid = targetElement->projection == "rigid";
                        // targetJigsaw was queried at position ZERO, so its
                        // coords ARE the rotated-local coordinates.
                        int32_t targetJigsawLocalY = targetJigsaw.y;
                        int32_t deltaY = sourceJigsawLocalY - targetJigsawLocalY
                                       + dirStepY(sourceDirection);
                        int32_t targetBoxY;
                        if (sourceRigid && targetRigid) {
                            targetBoxY = sourceBoxY + deltaY;
                        } else {
                            if (sourceJigsawBaseHeight == INT32_MIN) {
                                sourceJigsawBaseHeight = generator->getBaseHeight(
                                    sourceJigsaw.x, sourceJigsaw.z,
                                    Heightmap::Types::WORLD_SURFACE_WG, randomState);
                            }
                            targetBoxY = sourceJigsawBaseHeight - targetJigsawLocalY;
                        }
                        int32_t yOffset = targetBoxY - rawTargetY;
                        BoundingBox targetBB = rawTargetBB;
                        targetBB.move(0, yOffset, 0);
                        int32_t targetPosX = rawX, targetPosY = rawY + yOffset, targetPosZ = rawZ;
                        if (expandTo > 0) {
                            int newSize = std::max(expandTo + 1, targetBB.maxY - targetBB.minY);
                            // Reference: encapsulate a point at minY + newSize.
                            targetBB.maxY = std::max(targetBB.maxY, targetBB.minY + newSize);
                        }

                        DBox targetBox = boxOf(targetBB);
                        if (!childrenFree->fits(deflate(targetBox, 0.25))) continue;
                        childrenFree->occupy(targetBox);

                        int sourceGroundLevelDelta = pieces[sourceIndex].groundLevelDelta;
                        int targetGroundLevelDelta = targetRigid
                            ? sourceGroundLevelDelta - deltaY : 1;  // element delta = 1 always

                        PlacedPiece targetPiece;
                        targetPiece.element = targetElement;
                        targetPiece.posX = targetPosX;
                        targetPiece.posY = targetPosY;
                        targetPiece.posZ = targetPosZ;
                        targetPiece.rotation = targetRotation;
                        targetPiece.box = targetBB;
                        targetPiece.groundLevelDelta = targetGroundLevelDelta;

                        int32_t junctionY;
                        if (sourceRigid) {
                            junctionY = sourceBoxY + sourceJigsawLocalY;
                        } else if (targetRigid) {
                            junctionY = targetBoxY + targetJigsawLocalY;
                        } else {
                            if (sourceJigsawBaseHeight == INT32_MIN) {
                                sourceJigsawBaseHeight = generator->getBaseHeight(
                                    sourceJigsaw.x, sourceJigsaw.z,
                                    Heightmap::Types::WORLD_SURFACE_WG, randomState);
                            }
                            junctionY = sourceJigsawBaseHeight + deltaY / 2;
                        }

                        pieces[sourceIndex].junctions.push_back(
                            {tjx, junctionY - sourceJigsawLocalY + sourceGroundLevelDelta, tjz, deltaY});
                        targetPiece.junctions.push_back(
                            {sourceJigsaw.x, junctionY - targetJigsawLocalY + targetGroundLevelDelta,
                             sourceJigsaw.z, -deltaY});

                        pieces.push_back(std::move(targetPiece));
                        if (depth + 1 <= maxDepth) {
                            placing.add(PieceState{pieces.size() - 1, childrenFree, depth + 1},
                                        placementPriority);
                        }
                        placedForThisJigsaw = true;
                        break;
                    }
                    if (placedForThisJigsaw) break;
                }
                if (placedForThisJigsaw) break;
            }
        }
    }
};

} // namespace

namespace StructureLayouts {

bool generateJigsaw(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out,
                    const std::function<bool(int x, int y, int z)>& validBiomeAt) {
    // Reference: JigsawStructure.findGenerationPoint + JigsawPlacement.addPieces.
    // Height sample FIRST (uniform draws; absolute does not), then the alias
    // lookup (positional random, separate stream), then the center rotation.
    int32_t height;
    if (info.jigsawStartHeightUniform) {
        // Reference: UniformHeight.sample - Mth.randomBetweenInclusive.
        int32_t lo = info.jigsawStartHeightMin, hi = info.jigsawStartHeightMax;
        height = lo > hi ? lo : ctx.random.nextInt(hi - lo + 1) + lo;
    } else {
        height = info.jigsawStartHeightAbsolute;  // ConstantHeight: no draws
    }
    int32_t posX = ctx.chunkX * 16;
    int32_t posZ = ctx.chunkZ * 16;

    // Reference: PoolAliasLookup.create(poolAliases, startPos, seed) -
    // RandomSource.create(seed).forkPositional().at(startPos); bindings
    // resolved in list order (weighted picks draw from THAT random).
    std::map<std::string, std::string> aliasMap;
    if (info.jigsawHasAliases) {
        LegacyRandomSource base(ctx.seed);
        LegacyRandomSource aliasRandom = base.forkPositional().at(posX, height, posZ);
        std::function<void(const StructureInfo::PoolAlias&)> resolve =
            [&](const StructureInfo::PoolAlias& binding) {
            if (binding.type == "minecraft:direct") {
                aliasMap[binding.alias] = binding.target;
            } else if (binding.type == "minecraft:random") {
                int total = 0;
                for (const auto& [pool, weight] : binding.targets) total += weight;
                int pick = aliasRandom.nextInt(total);
                for (const auto& [pool, weight] : binding.targets) {
                    pick -= weight;
                    if (pick < 0) { aliasMap[binding.alias] = pool; break; }
                }
            } else if (binding.type == "minecraft:random_group") {
                int total = 0;
                for (const auto& [group, weight] : binding.groups) total += weight;
                int pick = aliasRandom.nextInt(total);
                for (const auto& [group, weight] : binding.groups) {
                    pick -= weight;
                    if (pick < 0) {
                        for (const auto& sub : group) resolve(sub);
                        break;
                    }
                }
            }
        };
        for (const auto& binding : info.jigsawAliases) resolve(binding);
    }
    auto aliasLookup = [&aliasMap](const std::string& pool) -> const std::string& {
        auto it = aliasMap.find(pool);
        return it == aliasMap.end() ? pool : it->second;
    };

    int centerRotation = ctx.random.nextInt(4);  // Rotation.getRandom
    const TemplatePool& centerPool = TemplatePools::byName(aliasLookup(info.jigsawStartPool));
    if (centerPool.size() == 0) return false;
    const PoolElement* centerElement =
        centerPool.templates[static_cast<size_t>(ctx.random.nextInt(centerPool.size()))];
    if (centerElement->kind == PoolElementKind::EMPTY) return false;

    // Optional start-jigsaw anchoring (ancient_city).
    int32_t anchorX = posX, anchorY = height, anchorZ = posZ;
    if (!info.jigsawStartJigsawName.empty()) {
        bool found = false;
        for (const PlacedJigsaw& jigsaw :
             shuffledJigsaws(*centerElement, posX, height, posZ, centerRotation, ctx.random)) {
            if (jigsaw.name == info.jigsawStartJigsawName) {
                anchorX = jigsaw.x; anchorY = jigsaw.y; anchorZ = jigsaw.z;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    int32_t localAnchorX = anchorX - posX, localAnchorY = anchorY - height, localAnchorZ = anchorZ - posZ;
    int32_t adjX = posX - localAnchorX, adjY = height - localAnchorY, adjZ = posZ - localAnchorZ;

    BoundingBox centerBox = elementBox(*centerElement, adjX, adjY, adjZ, centerRotation);
    int32_t centerX = (centerBox.maxX + centerBox.minX) / 2;
    int32_t centerZ = (centerBox.maxZ + centerBox.minZ) / 2;
    int32_t bottomY;
    if (info.jigsawProjectToHeightmap.empty()) {
        bottomY = adjY;
    } else {
        Heightmap::Types type = info.jigsawProjectToHeightmap == "OCEAN_FLOOR_WG"
            ? Heightmap::Types::OCEAN_FLOOR_WG : Heightmap::Types::WORLD_SURFACE_WG;
        // getFirstFreeHeight == getBaseHeight exactly (no -1).
        bottomY = height + ctx.generator->getBaseHeight(centerX, centerZ, type, ctx.randomState);
    }
    int centerGroundLevelDelta = 1;  // element getGroundLevelDelta always 1
    int32_t oldAbsoluteGroundY = centerBox.minY + centerGroundLevelDelta;
    int32_t moveY = bottomY - oldAbsoluteGroundY;
    centerBox.move(0, moveY, 0);
    int32_t centerPosY = adjY + moveY;

    // Dimension padding reject (heightAccessor.getMaxY = minY + height - 1).
    if (info.jigsawPaddingBottom != 0 || info.jigsawPaddingTop != 0) {
        int32_t minYPad = ctx.generator->getLevelMinY() + info.jigsawPaddingBottom;
        int32_t maxYPad = ctx.generator->getLevelMinY() + ctx.generator->getLevelHeight() - 1
                        - info.jigsawPaddingTop;
        if (centerBox.minY < minYPad || centerBox.maxY > maxYPad) return false;
    }

    int32_t centerY = bottomY + localAnchorY;
    // Stub biome check at (centerX, centerY, centerZ) BEFORE the placer runs.
    if (!validBiomeAt(centerX, centerY, centerZ)) return false;

    std::vector<PlacedPiece> pieces;
    PlacedPiece center;
    center.element = centerElement;
    center.posX = adjX; center.posY = centerPosY; center.posZ = adjZ;
    center.rotation = centerRotation;
    center.box = centerBox;
    center.groundLevelDelta = centerGroundLevelDelta;
    pieces.push_back(std::move(center));

    if (info.jigsawMaxDepth > 0) {
        // Free-space outer box (AABB in the Java code).
        // Reference: the AABB uses heightAccessor.getMaxY() + 1 - padding.top
        // = minY + height - padding.top.
        DBox big{static_cast<double>(centerX - info.jigsawMaxDistanceH),
                 static_cast<double>(std::max(centerY - info.jigsawMaxDistanceV,
                                              ctx.generator->getLevelMinY() + info.jigsawPaddingBottom)),
                 static_cast<double>(centerZ - info.jigsawMaxDistanceH),
                 static_cast<double>(centerX + info.jigsawMaxDistanceH + 1),
                 static_cast<double>(std::min(centerY + info.jigsawMaxDistanceV + 1,
                                              ctx.generator->getLevelMinY() + ctx.generator->getLevelHeight()
                                              - info.jigsawPaddingTop)),
                 static_cast<double>(centerZ + info.jigsawMaxDistanceH + 1)};
        auto free = std::make_shared<FreeSpace>();
        free->outer = big;
        free->occupy(boxOf(centerBox));

        Placer placer{info.jigsawMaxDepth, ctx.generator, ctx.randomState, ctx.random, pieces,
                      aliasLookup, {}};
        placer.tryPlacingChildren(0, free, 0, info.jigsawExpansionHack);
        while (!placer.placing.empty()) {
            PieceState state = placer.placing.next();
            placer.tryPlacingChildren(state.pieceIndex, state.free, state.depth,
                                      info.jigsawExpansionHack);
        }
    }

    out.pieces.clear();
    out.pieces.reserve(pieces.size());
    out.behaviors.clear();
    out.behaviors.reserve(pieces.size());
    bool keepLiquids = info.jigsawLiquidSettings != "ignore_waterlogging";
    static const char* rotNames[4] = {"NONE", "CLOCKWISE_90", "CLOCKWISE_180", "COUNTERCLOCKWISE_90"};
    for (const PlacedPiece& piece : pieces) {
        StructurePieceData data;
        data.pieceType = "minecraft:jigsaw";
        data.boundingBox = piece.box;
        data.rotation = rotNames[piece.rotation & 3];
        data.genDepth = 0;
        std::ostringstream detail;
        detail << elementTypeId(piece.element->kind) << ':'
               << (piece.element->location.empty() ? "-" : piece.element->location)
               << '#' << piece.groundLevelDelta;
        if (!piece.junctions.empty()) {
            detail << "#J:";
            bool first = true;
            for (const auto& j : piece.junctions) {
                if (!first) detail << '|';
                first = false;
                detail << j[0] << ';' << j[1] << ';' << j[2] << ';' << j[3];
            }
        }
        data.detail = detail.str();
        data.poolElement = true;
        data.rigidProjection = piece.element->projection == "rigid";
        data.groundLevelDelta = piece.groundLevelDelta;
        data.junctions = piece.junctions;
        out.pieces.push_back(std::move(data));
        out.behaviors.push_back(PieceBehaviors::jigsawPiece(
            *piece.element, core::BlockPos(piece.posX, piece.posY, piece.posZ),
            piece.rotation, keepLiquids));
    }
    return !out.pieces.empty();
}

} // namespace StructureLayouts

} // namespace structure
} // namespace levelgen
} // namespace minecraft
