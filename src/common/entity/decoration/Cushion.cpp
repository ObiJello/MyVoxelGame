// File: src/common/entity/decoration/Cushion.cpp
#include "common/entity/decoration/Cushion.hpp"
#include "common/particle/ParticleOptions.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/PrimedTnt.hpp"
#include "common/entity/projectile/Projectile.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/entity/SignBlockEntity.hpp"   // Game::DyeColor, kDyeColorCount
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/fluid/FluidState.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

namespace Game {

    namespace {

        // Items.CUSHION — ItemIds.CUSHION = createSimpleColored("cushion"),
        // one item per DyeColor in DyeColor order.
        constexpr ItemID kCushionItems[kDyeColorCount] = {
            Items::WhiteCushion, Items::OrangeCushion, Items::MagentaCushion, Items::LightBlueCushion,
            Items::YellowCushion, Items::LimeCushion, Items::PinkCushion, Items::GrayCushion,
            Items::LightGrayCushion, Items::CyanCushion, Items::PurpleCushion, Items::BlueCushion,
            Items::BrownCushion, Items::GreenCushion, Items::RedCushion, Items::BlackCushion,
        };
        static_assert(Items::BlackCushion - Items::WhiteCushion == kDyeColorCount - 1,
                      "the cushion items are one contiguous DyeColor-ordered run");

        // AABB.nextDeflated / Math.nextUp-nextDown on every face.
        AABBd NextDeflated(const AABBd& b) {
            constexpr double kInf = 1.0e300;
            return AABBd::FromMinMax(
                glm::dvec3(std::nextafter(b.min.x, kInf), std::nextafter(b.min.y, kInf), std::nextafter(b.min.z, kInf)),
                glm::dvec3(std::nextafter(b.max.x, -kInf), std::nextafter(b.max.y, -kInf), std::nextafter(b.max.z, -kInf)));
        }

        int FloorI(double v) { return static_cast<int>(std::floor(v)); }

        // MC BlockBehaviour.isSuffocating. The 26.3 default is
        // `state.is(CAUSES_SUFFOCATION) && isCollisionShapeFullBlock`, where
        // #causes_suffocation is #blocks_motion; Blocks.java overrides it to
        // `never` for the leaves, mangrove roots, glass, stained and tinted
        // glass, the moving piston and the copper grates, and to `always`
        // for farmland, the dirt path, soul sand and mud. Resolved once per
        // BlockID; the collision-shape half is per state.
        enum class SuffocationRule : uint8_t { Default, Never, Always };

        const std::vector<SuffocationRule>& SuffocationRules() {
            static std::vector<SuffocationRule> table;
            static std::once_flag once;
            std::call_once(once, [] {
                table.assign(BlockRegistry::Size, SuffocationRule::Default);
                const auto endsWith = [](const std::string& s, std::string_view suffix) {
                    return s.size() >= suffix.size() &&
                           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
                };
                for (size_t i = 0; i < BlockRegistry::Size; ++i) {
                    const std::string& slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
                    if (slug.empty()) continue;
                    if (endsWith(slug, "leaves") || slug == "mangrove_roots" || slug == "glass" ||
                        endsWith(slug, "_stained_glass") || slug == "tinted_glass" ||
                        slug == "moving_piston" || endsWith(slug, "copper_grate")) {
                        table[i] = SuffocationRule::Never;
                    } else if (slug == "farmland" || slug == "dirt_path" || slug == "soul_sand" || slug == "mud") {
                        table[i] = SuffocationRule::Always;
                    }
                }
            });
            return table;
        }

        bool IsSuffocating(BlockState state) {
            const BlockID id = state.Block();
            const auto& rules = SuffocationRules();
            const size_t i = static_cast<size_t>(id);
            const SuffocationRule rule = i < rules.size() ? rules[i] : SuffocationRule::Default;
            if (rule == SuffocationRule::Never) return false;
            if (rule == SuffocationRule::Always) return true;
            if (!BlockBlocksMotion(id)) return false;
            const BlockRegistry::BlockShape* box = BlockRegistry::GetSingleCollisionBox(state);
            return box && box->min.x <= 0.0f && box->min.y <= 0.0f && box->min.z <= 0.0f &&
                   box->max.x >= 1.0f && box->max.y >= 1.0f && box->max.z >= 1.0f;
        }

        // MC Cushion.hasAnchorBelow: something whose (outline) shape's bounds
        // reach the 1/64 slab under the cushion's bottom face.
        bool HasAnchorBelow(const IBlockAccess& level, const AABBd& box) {
            constexpr double kSlab = 0.015625;
            const AABBd anchor = AABBd::FromMinMax(
                glm::dvec3(box.min.x, box.min.y - kSlab, box.min.z),
                glm::dvec3(std::nextafter(box.max.x, -1.0e300), box.min.y, std::nextafter(box.max.z, -1.0e300)));
            // findBlocksIn(anchorBox.expandTowards(0, -0.125, 0)).
            const glm::ivec3 lo(FloorI(anchor.min.x), FloorI(anchor.min.y - 0.125), FloorI(anchor.min.z));
            const glm::ivec3 hi(FloorI(anchor.max.x), FloorI(anchor.max.y), FloorI(anchor.max.z));
            for (int x = lo.x; x <= hi.x; ++x) {
                for (int y = lo.y; y <= hi.y; ++y) {
                    for (int z = lo.z; z <= hi.z; ++z) {
                        const BlockState state = level.GetBlockState(x, y, z);
                        // Empty outline shapes: air, the liquids (LiquidBlock
                        // .getShape), the bubble column and the light block
                        // (empty to anyone not holding one).
                        const BlockID id = state.Block();
                        if (id == BlockID::Air || id == BlockID::Water || id == BlockID::Lava ||
                            id == BlockID::ResonantWater || id == BlockID::BubbleColumn ||
                            id == BlockID::Light) {
                            continue;
                        }
                        const glm::ivec3 cell(x, y, z);
                        const BlockRegistry::BlockShapeSet shape = BlockRegistry::GetBlockShapeSetAt(level, cell, state);
                        if (shape.count == 0) continue;
                        // shape.bounds(): the union's extent.
                        glm::vec3 mn(1.0f), mx(0.0f);
                        bool any = false;
                        for (const BlockRegistry::BlockShape& s : shape) {
                            if (s.max.x <= s.min.x || s.max.y <= s.min.y || s.max.z <= s.min.z) continue;
                            mn = glm::min(mn, s.min);
                            mx = glm::max(mx, s.max);
                            any = true;
                        }
                        if (!any) continue;
                        const AABBd bounds = AABBd::FromMinMax(glm::dvec3(cell) + glm::dvec3(mn),
                                                               glm::dvec3(cell) + glm::dvec3(mx));
                        if (bounds.Intersects(anchor)) return true;
                    }
                }
            }
            return false;
        }

        // MC Cushion.isCoveredBySuffocatingBlocks: EVERY cell the deflated
        // box touches suffocates.
        bool IsCoveredBySuffocatingBlocks(const IBlockAccess& level, const AABBd& box) {
            const AABBd d = NextDeflated(box);
            for (int x = FloorI(d.min.x); x <= FloorI(d.max.x); ++x) {
                for (int y = FloorI(d.min.y); y <= FloorI(d.max.y); ++y) {
                    for (int z = FloorI(d.min.z); z <= FloorI(d.max.z); ++z) {
                        if (!IsSuffocating(level.GetBlockState(x, y, z))) return false;
                    }
                }
            }
            return true;
        }

        // MC Cushion.isAnchorBuried: the 1/64 slice at the bottom of the box,
        // minus every block collision shape (Shapes.join ONLY_FIRST), is
        // empty. Exact: the slice is cut on every collider face it contains
        // and each resulting cell's centre is tested for cover.
        bool IsAnchorBuried(const IBlockAccess& level, const AABBd& box) {
            constexpr double kSlab = 0.015625;
            const AABBd slice = NextDeflated(AABBd::FromMinMax(
                box.min, glm::dvec3(box.max.x, box.min.y + kSlab, box.max.z)));
            PhysicsContext context;
            context.blockAccess = &level;
            std::vector<AABBd> colliders;
            CollectBlockColliders(slice, context, colliders);
            if (colliders.empty()) return false;

            std::vector<double> cuts[3];
            for (int a = 0; a < 3; ++a) {
                cuts[a].push_back(slice.min[a]);
                cuts[a].push_back(slice.max[a]);
                for (const AABBd& c : colliders) {
                    if (c.min[a] > slice.min[a] && c.min[a] < slice.max[a]) cuts[a].push_back(c.min[a]);
                    if (c.max[a] > slice.min[a] && c.max[a] < slice.max[a]) cuts[a].push_back(c.max[a]);
                }
                std::sort(cuts[a].begin(), cuts[a].end());
                cuts[a].erase(std::unique(cuts[a].begin(), cuts[a].end()), cuts[a].end());
            }
            for (size_t i = 0; i + 1 < cuts[0].size(); ++i) {
                for (size_t j = 0; j + 1 < cuts[1].size(); ++j) {
                    for (size_t k = 0; k + 1 < cuts[2].size(); ++k) {
                        const glm::dvec3 c((cuts[0][i] + cuts[0][i + 1]) * 0.5,
                                           (cuts[1][j] + cuts[1][j + 1]) * 0.5,
                                           (cuts[2][k] + cuts[2][k + 1]) * 0.5);
                        bool covered = false;
                        for (const AABBd& col : colliders) {
                            if (c.x > col.min.x && c.x < col.max.x && c.y > col.min.y && c.y < col.max.y &&
                                c.z > col.min.z && c.z < col.max.z) {
                                covered = true;
                                break;
                            }
                        }
                        if (!covered) return false;   // some of the resting face is exposed
                    }
                }
            }
            return true;
        }

    } // namespace

    Cushion::Cushion(EntityLevel* level)
        : BlockAttachedEntity(EntityTypeId::Cushion, level) {
        RecalculateBoundingBox();
    }

    // ── Colour ─────────────────────────────────────────────────────────────

    ItemID Cushion::ItemFor(DyeColor color) {
        const size_t i = static_cast<size_t>(color);
        return i < static_cast<size_t>(kDyeColorCount) ? kCushionItems[i] : kCushionItems[0];
    }

    std::optional<DyeColor> Cushion::ColorOfItem(ItemID item) {
        if (item < Items::WhiteCushion || item > Items::BlackCushion) return std::nullopt;
        return static_cast<DyeColor>(item - Items::WhiteCushion);
    }

    const char* Cushion::ColorName(DyeColor color) { return DyeColorName(color); }

    bool Cushion::ColorFromName(std::string_view name, DyeColor& out) { return DyeColorFromName(name, out); }

    void Cushion::SetVariantByte(uint8_t v) {
        m_color = v < kDyeColorCount ? static_cast<DyeColor>(v) : kDefaultColor;
        RecalculateBoundingBox();
    }

    // ── Geometry ───────────────────────────────────────────────────────────

    AABBd Cushion::MakeBoundingBox(const glm::dvec3& pos) {
        const double hw = kWidth * 0.5;
        return AABBd::FromMinMax(glm::dvec3(pos.x - hw, pos.y, pos.z - hw),
                                 glm::dvec3(pos.x + hw, pos.y + kHeight, pos.z + hw));
    }

    void Cushion::RecalculateBoundingBox() {
        m_pos = glm::ivec3(FloorI(position.x), FloorI(position.y), FloorI(position.z));
        SetFixedBoundingBox(MakeBoundingBox(position));
    }

    void Cushion::SetPos(const glm::dvec3& pos) {
        position = pos;
        oldPosition = pos;
        RecalculateBoundingBox();
    }

    // ── Placement and survival ─────────────────────────────────────────────

    bool Cushion::CanBePlacedAt(const IBlockAccess& level, const AABBd& box) {
        return WouldSurviveAt(level, box) && !IsAnchorBuried(level, box);
    }

    bool Cushion::WouldSurviveAt(const IBlockAccess& level, const AABBd& box) {
        return HasAnchorBelow(level, box) && !IsCoveredBySuffocatingBlocks(level, box);
    }

    bool Cushion::Survives() const {
        if (!m_level || !m_level->Blocks()) return false;
        return WouldSurviveAt(*m_level->Blocks(), GetAABBd());
    }

    // ── Ticking ────────────────────────────────────────────────────────────

    void Cushion::TickAtCheckInterval() {
        if (!m_level || m_level->IsClientSide() || !m_level->Blocks()) return;
        const IBlockAccess& blocks = *m_level->Blocks();
        // blockPosition(): the cell the bottom face is in.
        const glm::ivec3 cell(FloorI(position.x), FloorI(position.y), FloorI(position.z));
        const FluidState fluid = GetFluidState(blocks, cell);
        if (!fluid.IsEmpty()) {
            // collidedWithFluid: the fluid's box in its cell against ours.
            const float height = FluidHeight(blocks, cell, fluid);
            const AABBd fluidBox = AABBd::FromMinMax(glm::dvec3(cell),
                                                     glm::dvec3(cell) + glm::dvec3(1.0, height, 1.0));
            if (height > 0.0f && fluidBox.Intersects(GetAABBd()) && fluid.Is(FluidType::Lava)) {
                // LavaFluid.entityInside → Entity.lavaHurt: set alight for
                // 15 s, then lava damage 4 — which breaks the cushion — and
                // the burn hiss. (Water's EXTINGUISH has nothing to put out.)
                IgniteForSeconds(15.0f);
                if (Hurt(MobDamageSource::Lava, 4.0f, nullptr) && !IsSilent()) {
                    m_level->PlaySound(nullptr, position, SoundEvents::GENERIC_BURN, GetSoundSource(),
                                       0.4f, 2.0f + m_level->Random().NextFloat() * 0.4f);
                }
            }
        }
        DestroyIfInFire();
    }

    void Cushion::DestroyIfInFire() {
        if (IsRemoved() || !m_level || m_level->IsClientSide() || !m_level->Blocks()) return;
        const IBlockAccess& blocks = *m_level->Blocks();
        // findBlocksIn(getBoundingBox().nextDeflated()) filtered on #fire:
        // the first fire cell is a 1-point inFire hit, and the walk stops.
        const AABBd d = NextDeflated(GetAABBd());
        for (int x = FloorI(d.min.x); x <= FloorI(d.max.x); ++x) {
            for (int y = FloorI(d.min.y); y <= FloorI(d.max.y); ++y) {
                for (int z = FloorI(d.min.z); z <= FloorI(d.max.z); ++z) {
                    const BlockID id = blocks.GetBlock(x, y, z);
                    if (id == BlockID::Fire || id == BlockID::SoulFire) {
                        Hurt(MobDamageSource::Fire, 1.0f, nullptr);
                        return;
                    }
                }
            }
        }
    }

    // ── Breaking ───────────────────────────────────────────────────────────

    bool Cushion::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // isBreakingDeniedFor / isBreakingDeniedAtPosFor: the damage's
        // CAUSING entity — a projectile's shooter, primed TNT's igniter — if
        // a player who may not build. (mayInteract, spawn protection, has no
        // counterpart here.)
        Entity* cause = attacker;
        if (auto* projectile = dynamic_cast<Projectile*>(attacker)) cause = projectile->GetOwner();
        else if (auto* tnt = dynamic_cast<PrimedTnt*>(attacker))   cause = tnt->GetOwner();
        if (cause && cause->IsPlayer() && !cause->MayBuild()) return false;
        return BlockAttachedEntity::Hurt(source, amount, attacker);
    }

    void Cushion::ThunderHit(Entity* bolt) {
        if (IsRemoved() || !m_level || m_level->IsClientSide()) return;
        Kill();
        // MC gives the dropped item LIGHTNING_DROP_INVULNERABLE_TICKS so the
        // same bolt cannot burn it; item entities here are not Entities and
        // the bolt's entity sweep never reaches them, so the drop is safe
        // without it.
        DropItem(bolt);
    }

    void Cushion::ShowBreakingParticles() {
        // MC: level.sendParticles(BLOCK <colour>_wool, x, y + 2/3·h, z, 10,
        // w/4, h/4, w/4, 0.05).
        if (!m_level) return;
        static const BlockID kWool[16] = {
            BlockID::WhiteWool,     BlockID::OrangeWool,    BlockID::MagentaWool, BlockID::LightBlueWool,
            BlockID::YellowWool,    BlockID::LimeWool,      BlockID::PinkWool,    BlockID::GrayWool,
            BlockID::LightGrayWool, BlockID::CyanWool,      BlockID::PurpleWool,  BlockID::BlueWool,
            BlockID::BrownWool,     BlockID::GreenWool,     BlockID::RedWool,     BlockID::BlackWool,
        };
        const BlockID wool = kWool[static_cast<uint8_t>(m_color) & 0x0F];
        const double w = static_cast<double>(GetBbWidth()), h = static_cast<double>(GetBbHeight());
        m_level->SendParticles(ParticleOptions::Block(BlockStates::Default(wool)), position.x,
                               position.y + h * 0.6666666666666666, position.z, 10, w / 4.0, h / 4.0, w / 4.0,
                               0.05);
    }

    void Cushion::OnPassengerRemoved(Entity& passenger) {
        (void)passenger;
        // MC Cushion.removePassenger: `!isClientSide() && getRemovalReason()
        // == null` — the get-up sound, unless the cushion itself is going.
        if (m_level && !m_level->IsClientSide() && !IsRemoved()) {
            PlaySound(SoundEvents::CUSHION_GET_UP, 1.0f, 1.0f);
        }
    }

    void Cushion::DropItem(Entity* causedBy) {
        if (!m_level) return;
        PlaySound(SoundEvents::CUSHION_BREAK, 1.0f, 1.0f);
        ShowBreakingParticles();
        if (!m_level->DoEntityDrops()) return;
        // Player.hasInfiniteMaterials — a creative player's break drops
        // nothing.
        if (causedBy && causedBy->IsPlayer() && causedBy->IsCreative()) return;
        // getCushionItemStackWithData: the colour's item, named as this
        // cushion is. spawnAtLocation(level, stack): at the position, no
        // lift.
        ItemStack stack(ItemFor(m_color), 1);
        if (const auto& name = GetCustomName()) stack.components.set(DataComponents::CUSTOM_NAME, *name);
        m_level->SpawnItemStackDrop(position, stack);
    }

} // namespace Game
