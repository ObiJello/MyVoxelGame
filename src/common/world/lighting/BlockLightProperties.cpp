// File: src/common/world/lighting/BlockLightProperties.cpp
#include "common/world/lighting/BlockLightProperties.hpp"

#include "common/world/block/BlockRegistry.hpp"
#include "common/core/Log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Game::Lighting {

    namespace {

        enum class EmissionRule : uint8_t {
            None, Constant, Lit, Candles, SeaPickle, RespawnAnchor, LightLevel,
            Berries, AnyFace, TrialSpawner, Vault,
        };
        enum class SkyDownRule : uint8_t { Default, True, False, FluidEmpty, NotWaterlogged };
        enum class ShapeRule : uint8_t { False, True, NotDouble, Extended };

        struct LightRow {
            std::string_view slug;
            EmissionRule emission;
            int value;
            bool canOcclude;
            SkyDownRule skyDown;
            int dampening;          // -1 = MC's default
            ShapeRule shape;
        };

        // Local aliases so the generated rows read like the Java they came from.
        constexpr EmissionRule None = EmissionRule::None, Constant = EmissionRule::Constant,
            Lit = EmissionRule::Lit, Candles = EmissionRule::Candles,
            SeaPickle = EmissionRule::SeaPickle, RespawnAnchor = EmissionRule::RespawnAnchor,
            LightLevel = EmissionRule::LightLevel, Berries = EmissionRule::Berries,
            AnyFace = EmissionRule::AnyFace, TrialSpawner = EmissionRule::TrialSpawner,
            Vault = EmissionRule::Vault;
        constexpr SkyDownRule Default = SkyDownRule::Default, FluidEmpty = SkyDownRule::FluidEmpty,
            NotWaterlogged = SkyDownRule::NotWaterlogged;
        constexpr ShapeRule NotDouble = ShapeRule::NotDouble, Extended = ShapeRule::Extended;

        // `True` / `False` name both a SkyDownRule and a ShapeRule in the
        // generated rows; the macro below picks the right enum by position.
        struct TF { bool v; };
        constexpr TF True{true}, False{false};
        constexpr SkyDownRule Sky(SkyDownRule r) { return r; }
        constexpr SkyDownRule Sky(TF t) { return t.v ? SkyDownRule::True : SkyDownRule::False; }
        constexpr ShapeRule Shp(ShapeRule r) { return r; }
        constexpr ShapeRule Shp(TF t) { return t.v ? ShapeRule::True : ShapeRule::False; }

        const LightRow kLightRows[] = {
#define BLOCK_LIGHT(slug, rule, value, occlude, sky, damp, shape) \
            { slug, rule, value, occlude, Sky(sky), damp, Shp(shape) },
#include "common/world/block/GeneratedBlockLight.inc"
#undef BLOCK_LIGHT
        };

        struct EngineRow { std::string_view slug; int emission; };
        const EngineRow kEngineRows[] = {
#define ENGINE_BLOCK_LIGHT(slug, emission) { slug, emission },
#include "common/world/block/EngineBlockLight.inc"
#undef ENGINE_BLOCK_LIGHT
        };

        // One 16x16 coverage grid per face, 1/16-block cells (every vanilla
        // shape that uses its shape for light is on that grid: slabs, stairs,
        // snow layers in eighths, farmland and paths at 15/16...). Row v, bit u.
        struct FaceMasks {
            std::array<std::array<uint16_t, 16>, 6> face{};
            std::array<bool, 6> full{};
            std::array<bool, 6> empty{};
        };

        std::vector<StateLightInfo> s_table;
        std::vector<FaceMasks>      s_faces;
        std::atomic<bool>           s_ready{false};

        // The tangent coordinates ShapeOcclusion uses — fixed per AXIS, so the
        // two faces meeting on one boundary plane rasterise onto one grid.
        void Rasterize(FaceMasks& out, int face, float u0, float v0, float u1, float v1) {
            const int iu0 = std::clamp(static_cast<int>(std::floor(u0 * 16.0f + 0.001f)), 0, 16);
            const int iv0 = std::clamp(static_cast<int>(std::floor(v0 * 16.0f + 0.001f)), 0, 16);
            const int iu1 = std::clamp(static_cast<int>(std::ceil(u1 * 16.0f - 0.001f)), 0, 16);
            const int iv1 = std::clamp(static_cast<int>(std::ceil(v1 * 16.0f - 0.001f)), 0, 16);
            for (int v = iv0; v < iv1; ++v) {
                for (int u = iu0; u < iu1; ++u) {
                    out.face[static_cast<size_t>(face)][static_cast<size_t>(v)] =
                        static_cast<uint16_t>(out.face[static_cast<size_t>(face)][static_cast<size_t>(v)] | (1u << u));
                }
            }
        }

        // MC VoxelShape.getFaceShape(direction): the boxes that reach the
        // boundary on that side, projected onto it.
        FaceMasks BuildFaceMasks(const BlockRegistry::BlockShapeSet& shape) {
            FaceMasks m;
            constexpr float lo = 0.0001f, hi = 0.9999f;
            for (const auto& b : shape) {
                // Down/Up: (x, z). North/South: (x, y). West/East: (y, z).
                if (b.min.y <= lo) Rasterize(m, static_cast<int>(Direction::Down),  b.min.x, b.min.z, b.max.x, b.max.z);
                if (b.max.y >= hi) Rasterize(m, static_cast<int>(Direction::Up),    b.min.x, b.min.z, b.max.x, b.max.z);
                if (b.min.z <= lo) Rasterize(m, static_cast<int>(Direction::North), b.min.x, b.min.y, b.max.x, b.max.y);
                if (b.max.z >= hi) Rasterize(m, static_cast<int>(Direction::South), b.min.x, b.min.y, b.max.x, b.max.y);
                if (b.min.x <= lo) Rasterize(m, static_cast<int>(Direction::West),  b.min.y, b.min.z, b.max.y, b.max.z);
                if (b.max.x >= hi) Rasterize(m, static_cast<int>(Direction::East),  b.min.y, b.min.z, b.max.y, b.max.z);
            }
            for (size_t f = 0; f < 6; ++f) {
                bool full = true, empty = true;
                for (uint16_t row : m.face[f]) {
                    if (row != 0xFFFFu) full = false;
                    if (row != 0) empty = false;
                }
                m.full[f] = full;
                m.empty[f] = empty;
            }
            return m;
        }

        int ParseInt(std::string_view v, int fallback) {
            if (v.empty()) return fallback;
            int n = 0;
            for (char c : v) {
                if (c < '0' || c > '9') return fallback;
                n = n * 10 + (c - '0');
            }
            return n;
        }

        int EvalEmission(EmissionRule rule, int value, BlockState state) {
            auto prop = [&](std::string_view name) { return state.GetValueByName(name); };
            // A rule whose property the state lacks (an engine block aliased
            // to a vanilla one with fewer properties) reads as "on": MC would
            // throw, and an always-lit twin is the useful reading.
            auto flag = [&](std::string_view name) {
                const std::string_view v = prop(name);
                return v.empty() || v == "true";
            };
            switch (rule) {
                case EmissionRule::None:     return 0;
                case EmissionRule::Constant: return value;
                case EmissionRule::Lit:      return flag("lit") ? value : 0;
                case EmissionRule::Candles:
                    return flag("lit") ? 3 * ParseInt(prop("candles"), 1) : 0;
                case EmissionRule::SeaPickle:
                    // SeaPickleBlock.isDead = !waterlogged.
                    return prop("waterlogged") == "true" ? 3 + 3 * ParseInt(prop("pickles"), 1) : 0;
                case EmissionRule::RespawnAnchor:
                    return static_cast<int>(std::floor(static_cast<float>(ParseInt(prop("charges"), 0)) / 4.0f * 15.0f));
                case EmissionRule::LightLevel: return ParseInt(prop("level"), 15);
                case EmissionRule::Berries:    return prop("berries") == "true" ? value : 0;
                case EmissionRule::AnyFace:
                    for (std::string_view f : {"north", "east", "south", "west", "up", "down"}) {
                        if (prop(f) == "true") return value;
                    }
                    return 0;
                case EmissionRule::TrialSpawner: {
                    const std::string_view s = prop("trial_spawner_state");
                    if (s == "waiting_for_players") return 4;
                    if (s == "active" || s == "waiting_for_reward_ejection" || s == "ejecting_reward") return 8;
                    return 0;
                }
                case EmissionRule::Vault:
                    return prop("vault_state") == "inactive" ? 6 : 12;
            }
            return 0;
        }

    } // namespace

    void BlockLightProperties::Init() {
        std::unordered_map<std::string_view, const LightRow*> bySlug;
        bySlug.reserve(std::size(kLightRows));
        for (const LightRow& r : kLightRows) bySlug.emplace(r.slug, &r);
        std::unordered_map<std::string_view, int> engineEmission;
        for (const EngineRow& r : kEngineRows) engineEmission[r.slug] = r.emission;

        const uint32_t total = BlockStates::Total();
        s_table.assign(total, StateLightInfo{});
        s_faces.clear();
        std::unordered_map<std::string, uint16_t> faceDedup;   // identical shapes share a slot

        size_t unmatched = 0, emitting = 0;
        for (size_t bi = 0; bi < BlockRegistry::Size; ++bi) {
            const BlockID id = static_cast<BlockID>(bi);
            const Block& block = BlockRegistry::Get(id);
            const auto it = bySlug.find(std::string_view(block.registrySlug));
            const LightRow* row = it != bySlug.end() ? it->second : nullptr;
            if (!row && id != BlockID::Air) ++unmatched;
            const auto eng = engineEmission.find(std::string_view(block.registrySlug));

            const uint32_t base = BlockStates::Base(id);
            const uint32_t count = BlockStates::Count(id);
            for (uint32_t s = 0; s < count; ++s) {
                const BlockState state = BlockState::FromRawId(base + s);
                if (state.RawId() >= total) break;
                StateLightInfo& info = s_table[state.RawId()];

                // Without a generated row the block is treated as MC's
                // defaults, occluding exactly when the engine draws it opaque.
                const bool canOcclude = row ? row->canOcclude : block.opaque;
                const bool hasFluid = BlockRegistry::ContainsWater(state) || id == BlockID::Lava;
                const BlockRegistry::BlockShapeSet shape = BlockRegistry::GetBlockShapeSet(state);
                const BlockRegistry::BlockShapeSet collision = BlockRegistry::GetBlockCollisionShapeSet(state);
                // The engine's shape table hands an element-less model the
                // full cube, and a hollow model its full bounding box. MC's
                // getShape for these is not full: the airs are Shapes.empty(),
                // the moving piston's comes from a block entity that does not
                // exist when BlockStateBase.initCache runs (empty), the
                // structure void is a 6/16 box, and the cauldrons and the
                // composter are a full box with the inside cut out. Read as
                // full cubes, air would stop sky light (propagatesSkylightDown
                // false, dampening 1) and every open column's sky source would
                // sit at the top of its highest non-empty section; a composter
                // would be solidRender (dampening 15).
                const std::string_view slug = block.registrySlug;
                const bool mcShapeNotFull = id == BlockID::Air || slug == "cave_air" || slug == "void_air" ||
                                            slug == "moving_piston" || slug == "structure_void" ||
                                            slug == "composter" || slug.ends_with("cauldron");
                const bool shapeFull = !mcShapeNotFull && shape.IsFullCube();
                // isCollisionShapeFullBlock. The engine's collision shape set
                // is the outline for most blocks, so a noCollision block (a
                // fluid, the airs) is excluded by its flag, not its boxes.
                const bool fullCollision = block.hasCollision && !mcShapeNotFull && collision.IsFullCube();
                // MC getOcclusionShape: getShape, except the lectern and the
                // sculk shrieker, which occlude with their collision shape.
                const bool occlusionFromCollision =
                    block.registrySlug == "lectern" || block.registrySlug == "sculk_shrieker";
                const BlockRegistry::BlockShapeSet& occlusion = occlusionFromCollision ? collision : shape;
                const bool solidRender = canOcclude && !mcShapeNotFull && occlusion.IsFullCube();

                // propagatesSkylightDown.
                bool skyDown = !shapeFull && !hasFluid;
                if (row) {
                    switch (row->skyDown) {
                        case SkyDownRule::Default: break;
                        case SkyDownRule::True:  skyDown = true; break;
                        case SkyDownRule::False: skyDown = false; break;
                        case SkyDownRule::FluidEmpty: skyDown = !hasFluid; break;
                        case SkyDownRule::NotWaterlogged: skyDown = state.GetValueByName("waterlogged") != "true"; break;
                    }
                }

                // getLightDampening.
                int dampening = solidRender ? 15 : (skyDown ? 0 : 1);
                if (row && row->dampening >= 0) dampening = row->dampening;

                // useShapeForLightOcclusion.
                bool useShape = false;
                if (row) {
                    switch (row->shape) {
                        case ShapeRule::False: break;
                        case ShapeRule::True: useShape = true; break;
                        case ShapeRule::NotDouble: useShape = state.GetValueByName("type") != "double"; break;
                        case ShapeRule::Extended: useShape = state.GetValueByName("extended") == "true"; break;
                    }
                }

                // Emission: the engine table wins, then the generated rule.
                int emission = 0;
                if (eng != engineEmission.end()) emission = eng->second;
                else if (row) emission = EvalEmission(row->emission, row->value, state);
                emission = std::clamp(emission, 0, 15);
                if (emission > 0) ++emitting;

                // emissiveRendering: MC's magma block (always) and sculk
                // sensors in their ACTIVE phase; the engine's full-bright
                // blocks (Block::emissive).
                bool emissiveRendering = block.emissive || block.registrySlug == "magma_block";
                if ((block.registrySlug == "sculk_sensor" || block.registrySlug == "calibrated_sculk_sensor") &&
                    state.GetValueByName("sculk_sensor_phase") == "active") {
                    emissiveRendering = true;
                }

                // getShadeBrightness: isCollisionShapeFullBlock ? 0.2 : 1.0,
                // and the classes that override it — TransparentBlock (glass)
                // and its stained / tinted / waterlogged (copper grate)
                // subclasses, BarrierBlock, LightBlock and StructureVoidBlock
                // answer 1.0; MudBlock and SoulSandBlock 0.2 (their collision
                // is 14/16); SnowLayerBlock 0.2 at 8 layers only.
                bool shadeDark = fullCollision;
                if (slug == "glass" || slug == "tinted_glass" || slug.ends_with("stained_glass") ||
                    slug.ends_with("copper_grate") || slug == "barrier" || slug == "light" ||
                    slug == "structure_void") {
                    shadeDark = false;
                } else if (slug == "mud" || slug == "soul_sand") {
                    shadeDark = true;
                } else if (slug == "snow") {
                    shadeDark = state.GetValueByName("layers") == "8";
                }

                info.emission = static_cast<uint8_t>(emission);
                info.dampening = static_cast<uint8_t>(std::clamp(dampening, 0, 15));
                info.flags = static_cast<uint8_t>(
                    (canOcclude ? kCanOcclude : 0) | (solidRender ? kSolidRender : 0) |
                    (skyDown ? kSkyDown : 0) | (useShape ? kUseShape : 0) |
                    (emissiveRendering ? kEmissiveRendering : 0) | (hasFluid ? kHasFluid : 0) |
                    (fullCollision ? kFullCollision : 0) | (shadeDark ? kShadeDark : 0));

                // Face shapes only where they matter (MC isEmptyShape false).
                if (canOcclude && useShape) {
                    FaceMasks masks = BuildFaceMasks(occlusion);
                    std::string key(reinterpret_cast<const char*>(masks.face.data()), sizeof(masks.face));
                    auto [fit, inserted] = faceDedup.emplace(std::move(key), 0);
                    if (inserted) {
                        s_faces.push_back(masks);
                        fit->second = static_cast<uint16_t>(s_faces.size());
                    }
                    info.faceSlot = fit->second;
                }
            }
        }
        s_ready.store(true, std::memory_order_release);
        Log::Info("[Light] block light properties: %u states, %zu emitting, %zu face shapes, %zu blocks on defaults",
                  total, emitting, s_faces.size(), unmatched);
    }

    bool BlockLightProperties::Ready() { return s_ready.load(std::memory_order_acquire); }

    const StateLightInfo& BlockLightProperties::Info(BlockState state) {
        static const StateLightInfo kAir{};
        const uint32_t id = state.RawId();
        return id < s_table.size() ? s_table[id] : kAir;
    }

    namespace {
        // MC Shapes.faceShapeOccludes(a, b): either face full, or the union
        // covers the square; two empty faces never occlude.
        bool FaceShapeOccludes(uint16_t slotA, int faceA, uint16_t slotB, int faceB) {
            const FaceMasks* a = slotA ? &s_faces[slotA - 1u] : nullptr;
            const FaceMasks* b = slotB ? &s_faces[slotB - 1u] : nullptr;
            if ((a && a->full[static_cast<size_t>(faceA)]) || (b && b->full[static_cast<size_t>(faceB)])) return true;
            const bool aEmpty = !a || a->empty[static_cast<size_t>(faceA)];
            const bool bEmpty = !b || b->empty[static_cast<size_t>(faceB)];
            if (aEmpty && bEmpty) return false;
            for (size_t v = 0; v < 16; ++v) {
                const uint16_t ra = aEmpty ? 0 : a->face[static_cast<size_t>(faceA)][v];
                const uint16_t rb = bEmpty ? 0 : b->face[static_cast<size_t>(faceB)][v];
                if (static_cast<uint16_t>(ra | rb) != 0xFFFFu) return false;
            }
            return true;
        }
    }

    bool BlockLightProperties::ShapeOccludes(BlockState from, BlockState to, Direction direction) {
        const uint16_t a = Info(from).faceSlot;
        const uint16_t b = Info(to).faceSlot;
        if (a == 0 && b == 0) return false;          // both isEmptyShape
        return FaceShapeOccludes(a, static_cast<int>(direction), b, static_cast<int>(Opposite(direction)));
    }

    bool BlockLightProperties::IsEdgeOccluded(BlockState top, BlockState bottom) {
        const StateLightInfo& b = Info(bottom);
        if (b.dampening != 0) return true;
        const uint16_t t = Info(top).faceSlot;
        if (t == 0 && b.faceSlot == 0) return false;
        return FaceShapeOccludes(t, static_cast<int>(Direction::Down), b.faceSlot, static_cast<int>(Direction::Up));
    }

    bool BlockLightProperties::HasDifferentLightProperties(BlockState oldState, BlockState newState) {
        if (newState == oldState) return false;
        const StateLightInfo& o = Info(oldState);
        const StateLightInfo& n = Info(newState);
        return n.dampening != o.dampening || n.emission != o.emission ||
               (n.flags & kUseShape) != 0 || (o.flags & kUseShape) != 0;
    }

} // namespace Game::Lighting
