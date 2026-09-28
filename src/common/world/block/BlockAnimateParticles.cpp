// File: src/common/world/block/BlockAnimateParticles.cpp
#include "common/world/block/BlockAnimateParticles.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/block/FallingBlock.hpp"
#include "common/world/block/RedstoneWire.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/World.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Game {

    namespace {

        using K = ParticleKind;

        // ── Block queries the animateTicks make ───────────────────────────

        BlockState StateAt(const IBlockAccess& b, const glm::ivec3& p) { return b.GetBlockState(p.x, p.y, p.z); }

        std::string_view Prop(BlockState s, std::string_view name) { return s.GetValueByName(name); }
        bool IsTrue(BlockState s, std::string_view name) { return s.GetValueByName(name) == "true"; }
        int IntProp(BlockState s, std::string_view name) {
            const std::string_view v = s.GetValueByName(name);
            int out = 0;
            for (char c : v) {
                if (c < '0' || c > '9') break;
                out = out * 10 + (c - '0');
            }
            return out;
        }

        Direction DirectionFromName(std::string_view name) {
            if (name == "down")  return Direction::Down;
            if (name == "up")    return Direction::Up;
            if (name == "north") return Direction::North;
            if (name == "south") return Direction::South;
            if (name == "west")  return Direction::West;
            return Direction::East;
        }

        glm::ivec3 Step(Direction d) { return glm::ivec3(StepX(d), StepY(d), StepZ(d)); }

        // BlockState.canOcclude — the Properties' occlusion flag: every block
        // not built with noOcclusion(), i.e. the opaque render layer.
        bool CanOcclude(BlockState s) {
            if (s.Block() == BlockID::Air) return false;
            return BlockRegistry::Get(s.Block()).renderLayer == RenderLayer::Opaque;
        }

        // BlockState.isSolidRender: an occluding full cube.
        bool IsSolidRender(BlockState s) {
            return CanOcclude(s) && BlockRegistry::IsOcclusionFullCube(s);
        }

        // BlockState.isFaceSturdy(level, pos, face): some box of the shape
        // covers the whole face on that side.
        bool IsFaceSturdy(const IBlockAccess& b, const glm::ivec3& pos, BlockState s, Direction face) {
            if (s.Block() == BlockID::Air) return false;
            constexpr float e = 0.0001f;
            for (const auto& box : BlockRegistry::GetBlockShapeSetAt(b, pos, s)) {
                const bool fullX = box.min.x <= e && box.max.x >= 1.0f - e;
                const bool fullY = box.min.y <= e && box.max.y >= 1.0f - e;
                const bool fullZ = box.min.z <= e && box.max.z >= 1.0f - e;
                switch (face) {
                    case Direction::Down:  if (box.min.y <= e && fullX && fullZ) return true; break;
                    case Direction::Up:    if (box.max.y >= 1.0f - e && fullX && fullZ) return true; break;
                    case Direction::North: if (box.min.z <= e && fullX && fullY) return true; break;
                    case Direction::South: if (box.max.z >= 1.0f - e && fullX && fullY) return true; break;
                    case Direction::West:  if (box.min.x <= e && fullY && fullZ) return true; break;
                    case Direction::East:  if (box.max.x >= 1.0f - e && fullY && fullZ) return true; break;
                }
            }
            return false;
        }

        // Block.isFaceFull(collisionShape, UP).
        bool CollisionTopFull(BlockState s) {
            if (s.Block() == BlockID::Air || !BlockRegistry::HasCollision(s.Block())) return false;
            return BlockRegistry::GetBlockCollisionShapeSet(s).IsFaceSturdyUp();
        }

        // BlockState.isCollisionShapeFullBlock.
        bool CollisionFullBlock(BlockState s) {
            if (s.Block() == BlockID::Air || !BlockRegistry::HasCollision(s.Block())) return false;
            return BlockRegistry::GetBlockCollisionShapeSet(s).IsFullCube();
        }

        // Direction.getRandom: VALUES[random.nextInt(6)].
        Direction RandomDirection(JavaRandom& random) { return static_cast<Direction>(random.NextInt(6)); }

        const std::string& Slug(BlockID id) { return BlockRegistry::Get(id).registrySlug; }

        bool EndsWith(const std::string& s, std::string_view suffix) {
            return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        // A block tag, resolved once per tag into a per-BlockID table (the
        // data pack is read lazily by DataTags; this runs long after startup).
        class TagTable {
        public:
            explicit TagTable(const char* tag) : m_tag(tag) {}
            bool Has(BlockID id) {
                if (m_table.empty()) {
                    m_table.assign(BlockRegistry::Size, 0);
                    for (size_t i = 0; i < BlockRegistry::Size; ++i) {
                        const std::string& slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
                        if (slug.empty()) continue;
                        const auto& tags = DataTags::TagsFor(DataTags::Registry::Block, slug);
                        m_table[i] = std::find(tags.begin(), tags.end(), m_tag) != tags.end() ? 1 : 0;
                    }
                }
                const size_t i = static_cast<size_t>(id);
                return i < m_table.size() && m_table[i] != 0;
            }
        private:
            std::string          m_tag;
            std::vector<uint8_t> m_table;
        };
        TagTable& PowerProviders()   { static TagTable t("#minecraft:enchantment_power_provider"); return t; }
        TagTable& PowerTransmitters() { static TagTable t("#minecraft:enchantment_power_transmitter"); return t; }
        TagTable& Impermeable()      { static TagTable t("#minecraft:impermeable"); return t; }

        // ParticleUtils.spawnParticleBelow.
        void SpawnParticleBelow(EntityLevel& level, const glm::ivec3& pos, JavaRandom& random,
                                const ParticleOptions& particle) {
            level.AddParticle(particle, pos.x + random.NextDouble(), pos.y - 0.05, pos.z + random.NextDouble(),
                              0.0, 0.0, 0.0);
        }

        // DustParticleOptions.REDSTONE.
        ParticleOptions RedstoneDust() { return ParticleOptions::Dust(16711680u, 1.0f); }

        // ── FireBlock.getIgniteOdds > 0 (FireBlock.bootStrap) ─────────────
        bool IsFlammable(BlockState s) {
            if (IsTrue(s, "waterlogged")) return false;
            static const std::unordered_set<std::string> kNames = {
                "bamboo_mosaic", "bamboo_block", "stripped_bamboo_block", "mangrove_roots", "bookshelf", "tnt",
                "short_grass", "fern", "dead_bush", "short_dry_grass", "tall_dry_grass", "sunflower", "lilac",
                "rose_bush", "peony", "red_shrub", "tall_grass", "large_fern", "dandelion", "golden_dandelion",
                "poppy", "open_eyeblossom", "closed_eyeblossom", "blue_orchid", "allium", "azure_bluet",
                "red_tulip", "orange_tulip", "white_tulip", "pink_tulip", "oxeye_daisy", "cornflower",
                "lily_of_the_valley", "torchflower", "pitcher_plant", "wither_rose", "pink_petals", "wildflowers",
                "leaf_litter", "cactus_flower", "vine", "coal_block", "hay_block", "target", "pale_moss_block",
                "pale_moss_carpet", "pale_hanging_moss", "dried_kelp_block", "bamboo", "scaffolding", "lectern",
                "composter", "sweet_berry_bush", "beehive", "bee_nest", "azalea_leaves",
                "flowering_azalea_leaves", "cave_vines", "cave_vines_plant", "spore_blossom", "azalea",
                "flowering_azalea", "big_dripleaf", "big_dripleaf_stem", "small_dripleaf", "hanging_roots",
                "glow_lichen", "firefly_bush", "bush", "red_poplar_leaves", "orange_poplar_leaves",
                "yellow_poplar_leaves",
            };
            static const char* const kWoods[] = {"oak", "spruce", "birch", "jungle", "acacia", "cherry", "dark_oak",
                                                 "pale_oak", "poplar", "mangrove", "bamboo"};
            const std::string& slug = Slug(s.Block());
            if (slug.empty()) return false;
            if (kNames.count(slug)) return true;
            // Blocks.WOOL / WOOL_STAIRS / WOOL_SLAB / CARPET (the sixteen dyes).
            if (EndsWith(slug, "_wool") || EndsWith(slug, "_wool_stairs") || EndsWith(slug, "_wool_slab")) return true;
            if (EndsWith(slug, "_carpet") && slug != "moss_carpet" && slug != "pale_moss_carpet") return true;
            for (const char* wood : kWoods) {
                const std::string w(wood);
                for (const char* kind : {"_planks", "_slab", "_fence_gate", "_fence", "_stairs", "_log", "_wood",
                                         "_leaves", "_shelf"}) {
                    if (slug == w + kind || slug == "stripped_" + w + kind) return true;
                }
            }
            return false;
        }

        // ── Torches ───────────────────────────────────────────────────────

        // TorchBlock.animateTick with its flameParticle.
        template <K Flame>
        void TorchParticles(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom&) {
            const double x = pos.x + 0.5, y = pos.y + 0.7, z = pos.z + 0.5;
            level.AddParticle(ParticleOptions(K::Smoke), x, y, z, 0.0, 0.0, 0.0);
            level.AddParticle(ParticleOptions(Flame), x, y, z, 0.0, 0.0, 0.0);
        }

        // WallTorchBlock.animateTick.
        template <K Flame>
        void WallTorchParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom&) {
            const Direction opposite = Opposite(DirectionFromName(Prop(state, "facing")));
            const double x = pos.x + 0.5 + 0.27 * StepX(opposite);
            const double y = pos.y + 0.7 + 0.22;
            const double z = pos.z + 0.5 + 0.27 * StepZ(opposite);
            level.AddParticle(ParticleOptions(K::Smoke), x, y, z, 0.0, 0.0, 0.0);
            level.AddParticle(ParticleOptions(Flame), x, y, z, 0.0, 0.0, 0.0);
        }

        // RedstoneTorchBlock.animateTick.
        void RedstoneTorchParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            if (!IsTrue(state, "lit")) return;
            const double x = pos.x + 0.5 + (random.NextDouble() - 0.5) * 0.2;
            const double y = pos.y + 0.7 + (random.NextDouble() - 0.5) * 0.2;
            const double z = pos.z + 0.5 + (random.NextDouble() - 0.5) * 0.2;
            level.AddParticle(RedstoneDust(), x, y, z, 0.0, 0.0, 0.0);
        }

        // RedstoneWallTorchBlock.animateTick.
        void RedstoneWallTorchParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            if (!IsTrue(state, "lit")) return;
            const Direction opposite = Opposite(DirectionFromName(Prop(state, "facing")));
            const double x = pos.x + 0.5 + (random.NextDouble() - 0.5) * 0.2 + 0.27 * StepX(opposite);
            const double y = pos.y + 0.7 + (random.NextDouble() - 0.5) * 0.2 + 0.22;
            const double z = pos.z + 0.5 + (random.NextDouble() - 0.5) * 0.2 + 0.27 * StepZ(opposite);
            level.AddParticle(RedstoneDust(), x, y, z, 0.0, 0.0, 0.0);
        }

        // ── Redstone ──────────────────────────────────────────────────────

        // RedStoneOreBlock.animateTick → spawnParticles (lit).
        void RedstoneOreParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom&) {
            if (!IsTrue(state, "lit")) return;
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            JavaRandom& random = level.Random();   // spawnParticles draws level.getRandom()
            for (int d = 0; d < 6; ++d) {
                const Direction dir = static_cast<Direction>(d);
                if (IsSolidRender(StateAt(*blocks, pos + Step(dir)))) continue;
                const Axis axis = AxisOf(dir);
                const double dx = axis == Axis::X ? 0.5 + 0.5625 * StepX(dir) : static_cast<double>(random.NextFloat());
                const double dy = axis == Axis::Y ? 0.5 + 0.5625 * StepY(dir) : static_cast<double>(random.NextFloat());
                const double dz = axis == Axis::Z ? 0.5 + 0.5625 * StepZ(dir) : static_cast<double>(random.NextFloat());
                level.AddParticle(RedstoneDust(), pos.x + dx, pos.y + dy, pos.z + dz, 0.0, 0.0, 0.0);
            }
        }

        // RedstoneWireBlock.spawnParticlesAlongLine.
        void WireLine(EntityLevel& level, JavaRandom& random, const glm::ivec3& pos, uint32_t color,
                      Direction side, Direction along, float from, float to) {
            const float span = to - from;
            if (random.NextFloat() >= 0.2f * span) return;
            constexpr float kSideOfBlock = 0.4375f;
            const float positionOnLine = from + span * random.NextFloat();
            const double x = 0.5 + static_cast<double>(kSideOfBlock * static_cast<float>(StepX(side))) +
                             static_cast<double>(positionOnLine * static_cast<float>(StepX(along)));
            const double y = 0.5 + static_cast<double>(kSideOfBlock * static_cast<float>(StepY(side))) +
                             static_cast<double>(positionOnLine * static_cast<float>(StepY(along)));
            const double z = 0.5 + static_cast<double>(kSideOfBlock * static_cast<float>(StepZ(side))) +
                             static_cast<double>(positionOnLine * static_cast<float>(StepZ(along)));
            level.AddParticle(ParticleOptions::Dust(color, 1.0f), pos.x + x, pos.y + y, pos.z + z, 0.0, 0.0, 0.0);
        }

        // RedstoneWireBlock.animateTick: the dust along each connection.
        void RedstoneWireParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            const int power = IntProp(state, "power");
            if (power == 0) return;
            const uint32_t color = RedstoneWireColorForPower(power) & 0xFFFFFFu;
            // Direction.Plane.HORIZONTAL: NORTH, EAST, SOUTH, WEST.
            static constexpr Direction kHorizontal[4] = {Direction::North, Direction::East, Direction::South,
                                                         Direction::West};
            static constexpr const char* kNames[4] = {"north", "east", "south", "west"};
            for (int i = 0; i < 4; ++i) {
                const Direction horizontal = kHorizontal[i];
                const std::string_view connection = Prop(state, kNames[i]);
                if (connection == "up") {
                    WireLine(level, random, pos, color, horizontal, Direction::Up, -0.5f, 0.5f);
                    WireLine(level, random, pos, color, Direction::Down, horizontal, 0.0f, 0.5f);   // case fall-through
                } else if (connection == "side") {
                    WireLine(level, random, pos, color, Direction::Down, horizontal, 0.0f, 0.5f);
                } else {
                    WireLine(level, random, pos, color, Direction::Down, horizontal, 0.0f, 0.3f);
                }
            }
        }

        // RepeaterBlock.animateTick (powered).
        void RepeaterParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            if (!IsTrue(state, "powered")) return;
            const Direction direction = DirectionFromName(Prop(state, "facing"));
            const double x = pos.x + 0.5 + (random.NextDouble() - 0.5) * 0.2;
            const double y = pos.y + 0.4 + (random.NextDouble() - 0.5) * 0.2;
            const double z = pos.z + 0.5 + (random.NextDouble() - 0.5) * 0.2;
            float offset = -5.0f;
            if (random.NextBool()) offset = static_cast<float>(IntProp(state, "delay") * 2 - 1);
            offset /= 16.0f;
            const double xo = static_cast<double>(offset * static_cast<float>(StepX(direction)));
            const double zo = static_cast<double>(offset * static_cast<float>(StepZ(direction)));
            level.AddParticle(RedstoneDust(), x + xo, y, z + zo, 0.0, 0.0, 0.0);
        }

        // LeverBlock.animateTick → makeParticle(state, level, pos, 0.5).
        void LeverParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            if (!IsTrue(state, "powered") || !(random.NextFloat() < 0.25f)) return;
            const Direction facing = DirectionFromName(Prop(state, "facing"));
            const Direction opposite = Opposite(facing);
            // FaceAttachedHorizontalDirectionalBlock.getConnectedDirection.
            const std::string_view face = Prop(state, "face");
            const Direction connected = face == "ceiling" ? Direction::Down
                                      : face == "floor"   ? Direction::Up : facing;
            const Direction oppositeConnect = Opposite(connected);
            const double x = pos.x + 0.5 + 0.1 * StepX(opposite) + 0.2 * StepX(oppositeConnect);
            const double y = pos.y + 0.5 + 0.1 * StepY(opposite) + 0.2 * StepY(oppositeConnect);
            const double z = pos.z + 0.5 + 0.1 * StepZ(opposite) + 0.2 * StepZ(oppositeConnect);
            level.AddParticle(ParticleOptions::Dust(16711680u, 0.5f), x, y, z, 0.0, 0.0, 0.0);
        }

        // LightningRodBlock.animateTick: sparks on the topmost rod in a storm.
        void LightningRodParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom&) {
            if (!level.IsThundering()) return;
            JavaRandom& random = level.Random();
            if (static_cast<int64_t>(random.NextInt(200)) > level.GetGameTime() % 200) return;
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            // pos.y == WORLD_SURFACE height - 1: nothing but air above.
            for (int y = pos.y + 1; y <= World::MAX_Y; ++y) {
                if (blocks->GetBlock(pos.x, y, pos.z) != BlockID::Air) return;
            }
            const Axis axis = AxisOf(DirectionFromName(Prop(state, "facing")));
            const bool stepX = axis == Axis::X, stepY = axis == Axis::Y, stepZ = axis == Axis::Z;
            const auto nextDouble = [&random](double min, double max) { return random.NextDouble() * (max - min) + min; };
            const int count = random.NextInt(2) + 1;   // UniformInt.of(1, 2)
            for (int i = 0; i < count; ++i) {
                const double x = pos.x + 0.5 + nextDouble(-1.0, 1.0) * (stepX ? 0.5 : 0.125);
                const double y = pos.y + 0.5 + nextDouble(-1.0, 1.0) * (stepY ? 0.5 : 0.125);
                const double z = pos.z + 0.5 + nextDouble(-1.0, 1.0) * (stepZ ? 0.5 : 0.125);
                const double xs = stepX ? nextDouble(-1.0, 1.0) : 0.0;
                const double ys = stepY ? nextDouble(-1.0, 1.0) : 0.0;
                const double zs = stepZ ? nextDouble(-1.0, 1.0) : 0.0;
                level.AddParticle(ParticleOptions(K::ElectricSpark), x, y, z, xs, ys, zs);
            }
        }

        // ── Fire ──────────────────────────────────────────────────────────

        // BaseFireBlock.animateTick's particles (its crackle is the sound
        // module's): smoke hugging whatever flammable faces a floating fire
        // clings to, else a sheet of smoke over a grounded one.
        template <bool Soul>
        void FireParticles(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            // SoulFireBlock.canBurn is true for everything.
            const auto canBurn = [](BlockState s) { return Soul || IsFlammable(s); };
            const glm::ivec3 below = pos - glm::ivec3(0, 1, 0);
            const BlockState belowState = StateAt(*blocks, below);
            const auto smoke = [&](double x, double y, double z) {
                level.AddParticle(ParticleOptions(K::LargeSmoke), x, y, z, 0.0, 0.0, 0.0);
            };
            if (!canBurn(belowState) && !IsFaceSturdy(*blocks, below, belowState, Direction::Up)) {
                if (canBurn(StateAt(*blocks, pos + glm::ivec3(-1, 0, 0)))) {
                    for (int i = 0; i < 2; ++i) {
                        const double x = pos.x + random.NextDouble() * 0.10000000149011612;
                        const double y = pos.y + random.NextDouble();
                        smoke(x, y, pos.z + random.NextDouble());
                    }
                }
                if (canBurn(StateAt(*blocks, pos + glm::ivec3(1, 0, 0)))) {
                    for (int i = 0; i < 2; ++i) {
                        const double x = (pos.x + 1) - random.NextDouble() * 0.10000000149011612;
                        const double y = pos.y + random.NextDouble();
                        smoke(x, y, pos.z + random.NextDouble());
                    }
                }
                if (canBurn(StateAt(*blocks, pos + glm::ivec3(0, 0, -1)))) {
                    for (int i = 0; i < 2; ++i) {
                        const double x = pos.x + random.NextDouble();
                        const double y = pos.y + random.NextDouble();
                        smoke(x, y, pos.z + random.NextDouble() * 0.10000000149011612);
                    }
                }
                if (canBurn(StateAt(*blocks, pos + glm::ivec3(0, 0, 1)))) {
                    for (int i = 0; i < 2; ++i) {
                        const double x = pos.x + random.NextDouble();
                        const double y = pos.y + random.NextDouble();
                        smoke(x, y, (pos.z + 1) - random.NextDouble() * 0.10000000149011612);
                    }
                }
                if (canBurn(StateAt(*blocks, pos + glm::ivec3(0, 1, 0)))) {
                    for (int i = 0; i < 2; ++i) {
                        const double x = pos.x + random.NextDouble();
                        const double y = (pos.y + 1) - random.NextDouble() * 0.10000000149011612;
                        smoke(x, y, pos.z + random.NextDouble());
                    }
                }
            } else {
                for (int i = 0; i < 3; ++i) {
                    const double x = pos.x + random.NextDouble();
                    const double y = pos.y + random.NextDouble() * 0.5 + 0.5;
                    smoke(x, y, pos.z + random.NextDouble());
                }
            }
        }

        // CampfireBlock.animateTick: a lit (non-soul) campfire's lava pops.
        void CampfireParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            if (!IsTrue(state, "lit")) return;
            if (random.NextInt(5) != 0) return;
            for (int i = 0; i < random.NextInt(1) + 1; ++i) {
                level.AddParticle(ParticleOptions(K::Lava), pos.x + 0.5, pos.y + 0.5, pos.z + 0.5,
                                  static_cast<double>(random.NextFloat() / 2.0f), 5.0E-5,
                                  static_cast<double>(random.NextFloat() / 2.0f));
            }
        }

        // ── Furnaces, the brewing stand ───────────────────────────────────

        // FurnaceBlock / BlastFurnaceBlock: smoke (and a furnace's flame) out
        // of the front, `height` sixteenths up.
        template <bool Flame, int Height>
        void CookerParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            if (!IsTrue(state, "lit")) return;
            const double x = pos.x + 0.5, y = pos.y, z = pos.z + 0.5;
            const Direction direction = DirectionFromName(Prop(state, "facing"));
            const Axis axis = AxisOf(direction);
            const double ss = random.NextDouble() * 0.6 - 0.3;
            const double dx = axis == Axis::X ? StepX(direction) * 0.52 : ss;
            const double dy = random.NextDouble() * static_cast<double>(Height) / 16.0;
            const double dz = axis == Axis::Z ? StepZ(direction) * 0.52 : ss;
            level.AddParticle(ParticleOptions(K::Smoke), x + dx, y + dy, z + dz, 0.0, 0.0, 0.0);
            if (Flame) level.AddParticle(ParticleOptions(K::Flame), x + dx, y + dy, z + dz, 0.0, 0.0, 0.0);
        }

        // SmokerBlock.animateTick: smoke out of the top.
        void SmokerParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom&) {
            if (!IsTrue(state, "lit")) return;
            level.AddParticle(ParticleOptions(K::Smoke), pos.x + 0.5, pos.y + 1.1, pos.z + 0.5, 0.0, 0.0, 0.0);
        }

        // BrewingStandBlock.animateTick.
        void BrewingStandParticles(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            const double x = pos.x + 0.4 + static_cast<double>(random.NextFloat()) * 0.2;
            const double y = pos.y + 0.7 + static_cast<double>(random.NextFloat()) * 0.3;
            const double z = pos.z + 0.4 + static_cast<double>(random.NextFloat()) * 0.2;
            level.AddParticle(ParticleOptions(K::Smoke), x, y, z, 0.0, 0.0, 0.0);
        }

        // ── Candles (AbstractCandleBlock.addParticlesAndSound's particles) ─
        void CandleParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            if (!IsTrue(state, "lit")) return;
            // CandleBlock.PARTICLE_OFFSETS by candle count; a candle cake's
            // one flame sits on top (8, 16, 8).
            static const glm::dvec3 kOne[]   = {{8, 8, 8}};
            static const glm::dvec3 kTwo[]   = {{6, 7, 8}, {10, 8, 7}};
            static const glm::dvec3 kThree[] = {{8, 5, 10}, {6, 7, 8}, {9, 8, 7}};
            static const glm::dvec3 kFour[]  = {{7, 5, 9}, {10, 7, 9}, {6, 7, 6}, {9, 8, 6}};
            static const glm::dvec3 kCake[]  = {{8, 16, 8}};
            const glm::dvec3* offsets = kOne;
            int count = 1;
            if (EndsWith(Slug(state.Block()), "candle_cake")) {
                offsets = kCake;
            } else {
                switch (std::clamp(IntProp(state, "candles"), 1, 4)) {
                    case 2: offsets = kTwo; count = 2; break;
                    case 3: offsets = kThree; count = 3; break;
                    case 4: offsets = kFour; count = 4; break;
                    default: break;
                }
            }
            for (int i = 0; i < count; ++i) {
                const glm::dvec3 p = glm::dvec3(pos) + offsets[i] * 0.0625;
                if (random.NextFloat() < 0.3f) {
                    level.AddParticle(ParticleOptions(K::Smoke), p.x, p.y, p.z, 0.0, 0.0, 0.0);
                }
                level.AddParticle(ParticleOptions(K::SmallFlame), p.x, p.y, p.z, 0.0, 0.0, 0.0);
            }
        }

        // ── Portals and the End ───────────────────────────────────────────

        // NetherPortalBlock.animateTick: four motes, pushed across the plane.
        void NetherPortalParticles(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            const IBlockAccess* blocks = level.Blocks();
            const bool eastWest = blocks &&
                (blocks->GetBlock(pos.x - 1, pos.y, pos.z) == BlockID::NetherPortal ||
                 blocks->GetBlock(pos.x + 1, pos.y, pos.z) == BlockID::NetherPortal);
            for (int i = 0; i < 4; ++i) {
                double x = pos.x + random.NextDouble();
                const double y = pos.y + random.NextDouble();
                double z = pos.z + random.NextDouble();
                double xa = (static_cast<double>(random.NextFloat()) - 0.5) * 0.5;
                const double ya = (static_cast<double>(random.NextFloat()) - 0.5) * 0.5;
                double za = (static_cast<double>(random.NextFloat()) - 0.5) * 0.5;
                const int flip = random.NextInt(2) * 2 - 1;
                if (!eastWest) {
                    x = pos.x + 0.5 + 0.25 * flip;
                    xa = static_cast<double>(random.NextFloat() * 2.0f * static_cast<float>(flip));
                } else {
                    z = pos.z + 0.5 + 0.25 * flip;
                    za = static_cast<double>(random.NextFloat() * 2.0f * static_cast<float>(flip));
                }
                level.AddParticle(ParticleOptions(K::Portal), x, y, z, xa, ya, za);
            }
        }

        // EnderChestBlock.animateTick.
        void EnderChestParticles(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            for (int i = 0; i < 3; ++i) {
                const int flipX = random.NextInt(2) * 2 - 1;
                const int flipZ = random.NextInt(2) * 2 - 1;
                const double x = pos.x + 0.5 + 0.25 * flipX;
                const double y = static_cast<double>(static_cast<float>(pos.y) + random.NextFloat());
                const double z = pos.z + 0.5 + 0.25 * flipZ;
                const double xa = static_cast<double>(random.NextFloat() * static_cast<float>(flipX));
                const double ya = (static_cast<double>(random.NextFloat()) - 0.5) * 0.125;
                const double za = static_cast<double>(random.NextFloat() * static_cast<float>(flipZ));
                level.AddParticle(ParticleOptions(K::Portal), x, y, z, xa, ya, za);
            }
        }

        // EndPortalBlock.animateTick.
        void EndPortalParticles(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            const double x = pos.x + random.NextDouble();
            const double z = pos.z + random.NextDouble();
            level.AddParticle(ParticleOptions(K::Smoke), x, pos.y + 0.8, z, 0.0, 0.0, 0.0);
        }

        // EndGatewayBlock.animateTick: one mote per face the gateway shows
        // (TheEndGatewayBlockEntity.getParticleAmount).
        void EndGatewayParticles(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            int count = 0;
            for (int d = 0; d < 6; ++d) {
                const BlockState n = StateAt(*blocks, pos + Step(static_cast<Direction>(d)));
                if (!IsSolidRender(n) && n.Block() != BlockID::EndGateway) ++count;
            }
            for (int i = 0; i < count; ++i) {
                double x = pos.x + random.NextDouble();
                const double y = pos.y + random.NextDouble();
                double z = pos.z + random.NextDouble();
                double xa = (random.NextDouble() - 0.5) * 0.5;
                const double ya = (random.NextDouble() - 0.5) * 0.5;
                double za = (random.NextDouble() - 0.5) * 0.5;
                const int flip = random.NextInt(2) * 2 - 1;
                if (random.NextBool()) {
                    z = pos.z + 0.5 + 0.25 * flip;
                    za = static_cast<double>(random.NextFloat() * 2.0f * static_cast<float>(flip));
                } else {
                    x = pos.x + 0.5 + 0.25 * flip;
                    xa = static_cast<double>(random.NextFloat() * 2.0f * static_cast<float>(flip));
                }
                level.AddParticle(ParticleOptions(K::Portal), x, y, z, xa, ya, za);
            }
        }

        // EndRodBlock.animateTick.
        void EndRodParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            const Direction direction = DirectionFromName(Prop(state, "facing"));
            const double x = pos.x + 0.55 - static_cast<double>(random.NextFloat() * 0.1f);
            const double y = pos.y + 0.55 - static_cast<double>(random.NextFloat() * 0.1f);
            const double z = pos.z + 0.55 - static_cast<double>(random.NextFloat() * 0.1f);
            const double r = static_cast<double>(0.4f - (random.NextFloat() + random.NextFloat()) * 0.4f);
            if (random.NextInt(5) == 0) {
                const double vx = random.NextGaussian() * 0.005;
                const double vy = random.NextGaussian() * 0.005;
                const double vz = random.NextGaussian() * 0.005;
                level.AddParticle(ParticleOptions(K::EndRod), x + StepX(direction) * r, y + StepY(direction) * r,
                                  z + StepZ(direction) * r, vx, vy, vz);
            }
        }

        // RespawnAnchorBlock.animateTick's reverse-portal motes.
        void RespawnAnchorParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            if (IntProp(state, "charges") == 0) return;
            const double x = pos.x + 0.5 + (0.5 - random.NextDouble());
            const double y = pos.y + 1.0;
            const double z = pos.z + 0.5 + (0.5 - random.NextDouble());
            const double ya = static_cast<double>(random.NextFloat()) * 0.04;
            level.AddParticle(ParticleOptions(K::ReversePortal), x, y, z, 0.0, ya, 0.0);
        }

        // ── Drips ─────────────────────────────────────────────────────────

        // CryingObsidianBlock.animateTick: a tear off any open side.
        void CryingObsidianParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            if (random.NextInt(5) != 0) return;
            const Direction dir = RandomDirection(random);
            if (dir == Direction::Up) return;
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            const glm::ivec3 relative = pos + Step(dir);
            const BlockState rs = StateAt(*blocks, relative);
            if (CanOcclude(state) && IsFaceSturdy(*blocks, relative, rs, Opposite(dir))) return;
            const double xo = StepX(dir) == 0 ? random.NextDouble() : 0.5 + StepX(dir) * 0.6;
            const double yo = StepY(dir) == 0 ? random.NextDouble() : 0.5 + StepY(dir) * 0.6;
            const double zo = StepZ(dir) == 0 ? random.NextDouble() : 0.5 + StepZ(dir) * 0.6;
            level.AddParticle(ParticleOptions(K::DrippingObsidianTear), pos.x + xo, pos.y + yo, pos.z + zo, 0.0, 0.0, 0.0);
        }

        // WetSpongeBlock.animateTick.
        void WetSpongeParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            const Direction direction = RandomDirection(random);
            if (direction == Direction::Up) return;
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            const glm::ivec3 relative = pos + Step(direction);
            const BlockState rs = StateAt(*blocks, relative);
            if (CanOcclude(state) && IsFaceSturdy(*blocks, relative, rs, Opposite(direction))) return;
            double xx = pos.x, yy = pos.y, zz = pos.z;
            if (direction == Direction::Down) {
                yy -= 0.05;
                xx += random.NextDouble();
                zz += random.NextDouble();
            } else {
                yy += random.NextDouble() * 0.8;
                if (AxisOf(direction) == Axis::X) {
                    zz += random.NextDouble();
                    if (direction == Direction::East) xx += 1.0;
                    else xx += 0.05;
                } else {
                    xx += random.NextDouble();
                    if (direction == Direction::South) zz += 1.0;
                    else zz += 0.05;
                }
            }
            level.AddParticle(ParticleOptions(K::DrippingWater), xx, yy, zz, 0.0, 0.0, 0.0);
        }

        // BeehiveBlock.animateTick → trySpawnDripParticles (honey level ≥ 5).
        void BeehiveParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            if (IntProp(state, "honey_level") < 5) return;
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            JavaRandom& levelRandom = level.Random();
            for (int i = 0; i < random.NextInt(1) + 1; ++i) {
                if (!FluidStateOf(state).IsEmpty() || levelRandom.NextFloat() < 0.3f) continue;
                const auto shapes = BlockRegistry::GetBlockCollisionShapeSet(state);
                if (shapes.count == 0) continue;
                glm::dvec3 mn(1.0), mx(0.0);
                for (const auto& b : shapes) { mn = glm::min(mn, glm::dvec3(b.min)); mx = glm::max(mx, glm::dvec3(b.max)); }
                if (mx.y < 1.0 || Impermeable().Has(state.Block())) continue;
                double height;
                if (mn.y > 0.0) {
                    height = pos.y + mn.y - 0.05;
                } else {
                    const glm::ivec3 below = pos - glm::ivec3(0, 1, 0);
                    const BlockState bs = StateAt(*blocks, below);
                    // An empty collision shape's top is -infinity.
                    double belowTop = -1.0;
                    if (bs.Block() != BlockID::Air && BlockRegistry::HasCollision(bs.Block())) {
                        for (const auto& b : BlockRegistry::GetBlockCollisionShapeSet(bs)) {
                            belowTop = std::max(belowTop, static_cast<double>(b.max.y));
                        }
                    }
                    if (!((belowTop < 1.0 || !CollisionFullBlock(bs)) && FluidStateOf(bs).IsEmpty())) continue;
                    height = pos.y - 0.05;
                }
                const double x = pos.x + mn.x + (mx.x - mn.x) * levelRandom.NextDouble();
                const double z = pos.z + mn.z + (mx.z - mn.z) * levelRandom.NextDouble();
                level.AddParticle(ParticleOptions(K::DrippingHoney), x, height, z, 0.0, 0.0, 0.0);
            }
        }

        // PointedDripstoneBlock.animateTick: a free-hanging stalactite tip
        // drips what sits above its root (water or lava; with nothing, the
        // dimension's default — lava in the nether — only 2 % of the time).
        void DripstoneParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            // isFreeHangingStalactite: pointing down, a tip, not waterlogged.
            if (Prop(state, "vertical_direction") != "down" || Prop(state, "thickness") != "tip" ||
                IsTrue(state, "waterlogged")) {
                return;
            }
            const float roll = random.NextFloat();
            if (roll > 0.12f) return;
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            glm::ivec3 root = pos;
            bool found = false;
            for (int i = 1; i < 11; ++i) {
                const glm::ivec3 p = pos + glm::ivec3(0, i, 0);
                const BlockState s = StateAt(*blocks, p);
                if (!s.Is(BlockID::PointedDripstone)) { root = p - glm::ivec3(0, 1, 0); found = true; break; }
                if (Prop(s, "vertical_direction") != "down") break;
            }
            if (!found) return;
            const glm::ivec3 above = root + glm::ivec3(0, 1, 0);
            const bool nether = level.Dimension() == DimensionId::Nether;
            FluidType fluid = StateAt(*blocks, above).Is(BlockID::Mud) && !nether ? FluidType::Water
                                                                                 : GetFluidState(*blocks, above).type;
            const bool fillsCauldron = fluid == FluidType::Water || fluid == FluidType::Lava;
            if (!(roll < 0.02f || fillsCauldron)) return;
            K kind;
            if (fluid == FluidType::Empty) kind = nether ? K::DrippingDripstoneLava : K::DrippingDripstoneWater;
            else kind = fluid == FluidType::Lava ? K::DrippingDripstoneLava : K::DrippingDripstoneWater;
            const glm::vec3 offset = BlockRegistry::GetBlockOffset(BlockID::PointedDripstone, pos.x, pos.z);
            level.AddParticle(ParticleOptions(kind), pos.x + 0.5 + offset.x, pos.y + 0.3125 - 0.0625,
                              pos.z + 0.5 + offset.z, 0.0, 0.0, 0.0);
        }

        // ── Leaves ────────────────────────────────────────────────────────

        // LeavesBlock.makeDrippingWaterParticles.
        void LeafDrips(EntityLevel& level, const glm::ivec3& pos, JavaRandom& random) {
            if (!level.IsRainingAt(pos + glm::ivec3(0, 1, 0))) return;
            if (random.NextInt(15) != 1) return;
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            const glm::ivec3 below = pos - glm::ivec3(0, 1, 0);
            const BlockState bs = StateAt(*blocks, below);
            if (!CanOcclude(bs) || !IsFaceSturdy(*blocks, below, bs, Direction::Up)) {
                SpawnParticleBelow(level, pos, random, ParticleOptions(K::DrippingWater));
            }
        }

        // FallingParticlesLeavesBlock.makeFallingLeavesParticles.
        bool LeafFallRoll(EntityLevel& level, const glm::ivec3& pos, JavaRandom& random, float chance) {
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return false;
            if (random.NextFloat() >= chance) return false;
            return !CollisionTopFull(StateAt(*blocks, pos - glm::ivec3(0, 1, 0)));
        }

        // LeavesBlock (spruce): the drips only.
        void PlainLeavesParticles(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            LeafDrips(level, pos, random);
        }

        // TintedParticleLeavesBlock (oak, birch, jungle, acacia, dark oak,
        // mangrove): leaves in the block's world tint.
        void TintedLeavesParticles(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            LeafDrips(level, pos, random);
            if (!LeafFallRoll(level, pos, random, 0.01f)) return;
            const int tint = level.GetClientLeafTintColor(pos);
            const uint32_t argb = tint < 0 ? 0xFFFFFFFFu : (0xFF000000u | static_cast<uint32_t>(tint));
            SpawnParticleBelow(level, pos, random, ParticleOptions::Color(K::TintedLeaves, argb));
        }

        // UntintedParticleLeavesBlock with its own particle and chance.
        template <K Leaf, int ChancePerMille>
        void UntintedLeavesParticles(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            LeafDrips(level, pos, random);
            if (!LeafFallRoll(level, pos, random, static_cast<float>(ChancePerMille) / 1000.0f)) return;
            SpawnParticleBelow(level, pos, random, ParticleOptions(Leaf));
        }

        // The azaleas: UntintedParticleLeavesBlock(0.01, TINTED_LEAVES in a
        // fixed -9399763).
        void AzaleaLeavesParticles(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            LeafDrips(level, pos, random);
            if (!LeafFallRoll(level, pos, random, 0.01f)) return;
            SpawnParticleBelow(level, pos, random,
                               ParticleOptions::Color(K::TintedLeaves, static_cast<uint32_t>(-9399763)));
        }

        // ── Plants and the rest ───────────────────────────────────────────

        // MyceliumBlock.animateTick.
        void MyceliumParticles(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            if (random.NextInt(10) == 0) {
                level.AddParticle(ParticleOptions(K::Mycelium), pos.x + random.NextDouble(), pos.y + 1.1,
                                  pos.z + random.NextDouble(), 0.0, 0.0, 0.0);
            }
        }

        // SporeBlossomBlock.animateTick.
        void SporeBlossomParticles(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            level.AddParticle(ParticleOptions(K::FallingSporeBlossom), pos.x + random.NextDouble(), pos.y + 0.7,
                              pos.z + random.NextDouble(), 0.0, 0.0, 0.0);
            const IBlockAccess* blocks = level.Blocks();
            for (int i = 0; i < 14; ++i) {
                // Mth.nextInt(random, -10, 10) = random.nextInt(21) - 10.
                const int dx = random.NextInt(21) - 10;
                const int dy = random.NextInt(10);
                const int dz = random.NextInt(21) - 10;
                const glm::ivec3 ambient(pos.x + dx, pos.y - dy, pos.z + dz);
                if (blocks && CollisionFullBlock(StateAt(*blocks, ambient))) continue;
                level.AddParticle(ParticleOptions(K::SporeBlossomAir), ambient.x + random.NextDouble(),
                                  ambient.y + random.NextDouble(), ambient.z + random.NextDouble(), 0.0, 0.0, 0.0);
            }
        }

        // FireflyBushBlock.animateTick's fireflies (dark enough, 70 %).
        void FireflyBushParticles(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            if (level.GetMaxLocalRawBrightness(pos.x, pos.y, pos.z) <= 13 && random.NextDouble() <= 0.7) {
                const double x = pos.x + random.NextDouble() * 10.0 - 5.0;
                const double y = pos.y + random.NextDouble() * 5.0;
                const double z = pos.z + random.NextDouble() * 10.0 - 5.0;
                level.AddParticle(ParticleOptions(K::Firefly), x, y, z, 0.0, 0.0, 0.0);
            }
        }

        // WitherRoseBlock.animateTick: smoke around the (offset) flower.
        void WitherRoseParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            const IBlockAccess* blocks = level.Blocks();
            glm::dvec3 center(0.5, 0.0, 0.5);
            if (blocks) {
                const auto shapes = BlockRegistry::GetBlockShapeSetAt(*blocks, pos, state);
                if (shapes.count > 0) {
                    glm::dvec3 mn(1.0), mx(0.0);
                    for (const auto& b : shapes) { mn = glm::min(mn, glm::dvec3(b.min)); mx = glm::max(mx, glm::dvec3(b.max)); }
                    center = (mn + mx) * 0.5;
                }
            }
            // The flower's XZ offset rides the shape (BlockBehaviour.OffsetType.XZ).
            const glm::vec3 offset = BlockRegistry::GetBlockOffset(state.Block(), pos.x, pos.z);
            const double x = pos.x + center.x + offset.x;
            const double z = pos.z + center.z + offset.z;
            for (int i = 0; i < 3; ++i) {
                if (random.NextBool()) {
                    level.AddParticle(ParticleOptions(K::Smoke), x + random.NextDouble() / 5.0,
                                      pos.y + (0.5 - random.NextDouble()), z + random.NextDouble() / 5.0, 0.0, 0.0, 0.0);
                }
            }
        }

        // BrushableBlock.animateTick: dust under a suspended suspicious block.
        void BrushableParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            if (random.NextInt(16) != 0) return;
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            if (!FallingBlockIsFree(StateAt(*blocks, pos - glm::ivec3(0, 1, 0)))) return;
            const double xx = pos.x + random.NextDouble();
            const double zz = pos.z + random.NextDouble();
            level.AddParticle(ParticleOptions::Block(K::FallingDust, state), xx, pos.y - 0.05, zz, 0.0, 0.0, 0.0);
        }

        // BubbleColumnBlock.animateTick: the whirlpool or the rising column.
        void BubbleColumnParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            const double x = pos.x, y = pos.y, z = pos.z;
            if (IsTrue(state, "drag")) {
                level.AddAlwaysVisibleParticle(ParticleOptions(K::CurrentDown), x + 0.5, y + 0.8, z, 0.0, 0.0, 0.0);
            } else {
                level.AddAlwaysVisibleParticle(ParticleOptions(K::BubbleColumnUp), x + 0.5, y, z + 0.5, 0.0, 0.04, 0.0);
                level.AddAlwaysVisibleParticle(ParticleOptions(K::BubbleColumnUp), x + random.NextFloat(),
                                               y + random.NextFloat(), z + random.NextFloat(), 0.0, 0.04, 0.0);
            }
        }

        // DriedGhastBlock.animateTick's particles.
        void DriedGhastParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            const double x = pos.x + 0.5, y = pos.y + 0.5, z = pos.z + 0.5;
            if (!IsTrue(state, "waterlogged")) {
                if (random.NextInt(6) == 0) {
                    level.AddParticle(ParticleOptions(K::WhiteSmoke), x, y, z, 0.0, 0.02, 0.0);
                }
            } else if (random.NextInt(6) == 0) {
                const double px = x + static_cast<double>((random.NextFloat() * 2.0f - 1.0f) / 3.0f);
                const double pz = z + static_cast<double>((random.NextFloat() * 2.0f - 1.0f) / 3.0f);
                level.AddParticle(ParticleOptions(K::HappyVillager), px, y + 0.4, pz, 0.0,
                                  static_cast<double>(random.NextFloat()), 0.0);
            }
        }

        // SculkSensorBlock.animateTick (the calibrated one inherits it): an
        // ACTIVE sensor glows off its sides.
        void SculkSensorParticles(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            if (Prop(state, "sculk_sensor_phase") != "active") return;
            const Direction dir = RandomDirection(random);
            if (dir == Direction::Up || dir == Direction::Down) return;
            const double x = pos.x + 0.5 + (StepX(dir) == 0 ? 0.5 - random.NextDouble() : StepX(dir) * 0.6);
            const double y = pos.y + 0.25;
            const double z = pos.z + 0.5 + (StepZ(dir) == 0 ? 0.5 - random.NextDouble() : StepZ(dir) * 0.6);
            const double ya = static_cast<double>(random.NextFloat()) * 0.04;
            // DustColorTransitionOptions.SCULK_TO_REDSTONE.
            level.AddParticle(ParticleOptions::DustColorTransition(3790560u, 16711680u, 1.0f), x, y, z, 0.0, ya, 0.0);
        }

        // EnchantingTableBlock.animateTick: glyphs from each bookshelf.
        void EnchantingTableParticles(EntityLevel& level, const glm::ivec3& pos, BlockState, JavaRandom& random) {
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return;
            // BOOKSHELF_OFFSETS: BlockPos.betweenClosed(-2, 0, -2, 2, 1, 2)
            // filtered to the ring |x| == 2 || |z| == 2 (x fastest, then y,
            // then z — BlockPos.betweenClosed's order).
            for (int oz = -2; oz <= 2; ++oz) {
                for (int oy = 0; oy <= 1; ++oy) {
                    for (int ox = -2; ox <= 2; ++ox) {
                        if (std::abs(ox) != 2 && std::abs(oz) != 2) continue;
                        if (random.NextInt(16) != 0) continue;
                        // EnchantingTableBlock.isValidBookShelf.
                        if (!PowerProviders().Has(blocks->GetBlock(pos.x + ox, pos.y + oy, pos.z + oz))) continue;
                        if (!PowerTransmitters().Has(blocks->GetBlock(pos.x + ox / 2, pos.y + oy, pos.z + oz / 2))) continue;
                        level.AddParticle(ParticleOptions(K::Enchant), pos.x + 0.5, pos.y + 2.0, pos.z + 0.5,
                                          static_cast<double>(static_cast<float>(ox) + random.NextFloat()) - 0.5,
                                          static_cast<double>(static_cast<float>(oy) - random.NextFloat() - 1.0f),
                                          static_cast<double>(static_cast<float>(oz) + random.NextFloat()) - 0.5);
                    }
                }
            }
        }

        // ── Chaining ──────────────────────────────────────────────────────
        //
        // animateTick is a plain function pointer; a block that already has
        // one keeps it, called first, then the particles.
        std::array<BlockAnimateTickFn, BlockRegistry::Size> s_previous{};
        std::array<BlockAnimateTickFn, BlockRegistry::Size> s_particles{};

        void Chained(EntityLevel& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            const size_t i = static_cast<size_t>(state.Block());
            if (i >= BlockRegistry::Size) return;
            if (s_previous[i]) s_previous[i](level, pos, state, random);
            if (s_particles[i]) s_particles[i](level, pos, state, random);
        }

        void Attach(std::array<Block, BlockRegistry::Size>& blocks, BlockID id, BlockAnimateTickFn fn) {
            const size_t i = static_cast<size_t>(id);
            if (i >= BlockRegistry::Size || id == BlockID::Air) return;
            if (blocks[i].animateTick == &Chained) return;   // attached twice: keep the first
            s_previous[i] = blocks[i].animateTick;
            s_particles[i] = fn;
            blocks[i].animateTick = &Chained;
        }

    } // namespace

    void RegisterBlockAnimateParticles(std::array<Block, BlockRegistry::Size>& blocks) {
        using B = BlockID;
        // Torches.
        Attach(blocks, B::Torch,           &TorchParticles<K::Flame>);
        Attach(blocks, B::WallTorch,       &WallTorchParticles<K::Flame>);
        Attach(blocks, B::SoulTorch,       &TorchParticles<K::SoulFireFlame>);
        Attach(blocks, B::SoulWallTorch,   &WallTorchParticles<K::SoulFireFlame>);
        Attach(blocks, B::CopperTorch,     &TorchParticles<K::CopperFireFlame>);
        Attach(blocks, B::CopperWallTorch, &WallTorchParticles<K::CopperFireFlame>);
        Attach(blocks, B::RedstoneTorch,     &RedstoneTorchParticles);
        Attach(blocks, B::RedstoneWallTorch, &RedstoneWallTorchParticles);
        // Redstone.
        Attach(blocks, B::RedstoneOre,          &RedstoneOreParticles);
        Attach(blocks, B::DeepslateRedstoneOre, &RedstoneOreParticles);
        Attach(blocks, B::RedstoneWire,         &RedstoneWireParticles);
        Attach(blocks, B::Repeater,             &RepeaterParticles);
        Attach(blocks, B::Lever,                &LeverParticles);
        // Fire, the campfire's pops (the soul campfire spawns none).
        Attach(blocks, B::Fire,     &FireParticles<false>);
        Attach(blocks, B::SoulFire, &FireParticles<true>);
        Attach(blocks, B::Campfire, &CampfireParticles);
        // Cookers and the brewing stand.
        Attach(blocks, B::Furnace,      &CookerParticles<true, 6>);
        Attach(blocks, B::BlastFurnace, &CookerParticles<false, 9>);
        Attach(blocks, B::Smoker,       &SmokerParticles);
        Attach(blocks, B::BrewingStand, &BrewingStandParticles);
        // Portals and the End.
        Attach(blocks, B::NetherPortal,  &NetherPortalParticles);
        Attach(blocks, B::EnderChest,    &EnderChestParticles);
        Attach(blocks, B::EndPortal,     &EndPortalParticles);
        Attach(blocks, B::EndGateway,    &EndGatewayParticles);
        Attach(blocks, B::EndRod,        &EndRodParticles);
        Attach(blocks, B::RespawnAnchor, &RespawnAnchorParticles);
        // Drips.
        Attach(blocks, B::CryingObsidian,   &CryingObsidianParticles);
        Attach(blocks, B::WetSponge,        &WetSpongeParticles);
        Attach(blocks, B::Beehive,          &BeehiveParticles);
        Attach(blocks, B::BeeNest,          &BeehiveParticles);
        Attach(blocks, B::PointedDripstone, &DripstoneParticles);
        // Leaves (Blocks.java's leaf classes and chances).
        Attach(blocks, B::SpruceLeaves,  &PlainLeavesParticles);
        Attach(blocks, B::OakLeaves,     &TintedLeavesParticles);
        Attach(blocks, B::BirchLeaves,   &TintedLeavesParticles);
        Attach(blocks, B::JungleLeaves,  &TintedLeavesParticles);
        Attach(blocks, B::AcaciaLeaves,  &TintedLeavesParticles);
        Attach(blocks, B::DarkOakLeaves, &TintedLeavesParticles);
        Attach(blocks, B::MangroveLeaves, &TintedLeavesParticles);
        Attach(blocks, B::CherryLeaves,  &UntintedLeavesParticles<K::CherryLeaves, 100>);
        Attach(blocks, B::PaleOakLeaves, &UntintedLeavesParticles<K::PaleOakLeaves, 20>);
        Attach(blocks, B::RedPoplarLeaves,    &UntintedLeavesParticles<K::RedPoplarLeaves, 10>);
        Attach(blocks, B::OrangePoplarLeaves, &UntintedLeavesParticles<K::OrangePoplarLeaves, 10>);
        Attach(blocks, B::YellowPoplarLeaves, &UntintedLeavesParticles<K::YellowPoplarLeaves, 10>);
        Attach(blocks, B::AzaleaLeaves,          &AzaleaLeavesParticles);
        Attach(blocks, B::FloweringAzaleaLeaves, &AzaleaLeavesParticles);
        // Plants and the rest.
        Attach(blocks, B::Mycelium,        &MyceliumParticles);
        Attach(blocks, B::SporeBlossom,    &SporeBlossomParticles);
        Attach(blocks, B::FireflyBush,     &FireflyBushParticles);
        Attach(blocks, B::WitherRose,      &WitherRoseParticles);
        Attach(blocks, B::SuspiciousSand,  &BrushableParticles);
        Attach(blocks, B::SuspiciousGravel, &BrushableParticles);
        Attach(blocks, B::BubbleColumn,    &BubbleColumnParticles);
        Attach(blocks, B::DriedGhast,      &DriedGhastParticles);
        Attach(blocks, B::SculkSensor,           &SculkSensorParticles);
        Attach(blocks, B::CalibratedSculkSensor, &SculkSensorParticles);
        Attach(blocks, B::EnchantingTable, &EnchantingTableParticles);
        // Every candle colour and every candle cake (AbstractCandleBlock),
        // and every lightning rod (the copper ages and their waxed forms).
        for (size_t i = 0; i < BlockRegistry::Size; ++i) {
            const std::string& slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
            if (slug == "candle" || EndsWith(slug, "_candle") || EndsWith(slug, "candle_cake")) {
                Attach(blocks, static_cast<BlockID>(i), &CandleParticles);
            } else if (EndsWith(slug, "lightning_rod")) {
                Attach(blocks, static_cast<BlockID>(i), &LightningRodParticles);
            }
        }
    }

} // namespace Game
