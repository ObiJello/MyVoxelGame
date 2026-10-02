// File: src/client/renderer/entity/MobEquipmentLayers.cpp
//
// MobRenderer's equipment layers — see the "Equipment layers" block in
// MobRenderer.hpp. MC sources (client/renderer/entity/…):
//   layers/ItemInHandLayer, layers/CustomHeadLayer, layers/HumanoidArmorLayer
//   + layers/EquipmentLayerRenderer, layers/CrossedArmsItemLayer,
//   layers/WitchItemLayer, layers/FoxHeldItemLayer,
//   layers/DolphinCarryingItemLayer, layers/PandaHoldsItemLayer; the
//   renderers that register them (HumanoidMobRenderer, AbstractZombie- /
//   AbstractSkeletonRenderer, PiglinRenderer, ZombifiedPiglinRenderer,
//   ZombieVillagerRenderer, IllagerRenderer and its Evoker / Illusioner /
//   Vindicator / Pillager subclasses, VexRenderer, AllayRenderer,
//   VillagerRenderer, WanderingTraderRenderer, WitchRenderer, FoxRenderer,
//   DolphinRenderer, PandaRenderer); the item side is
//   renderer/item/ItemStackRenderState + the assets/items definitions
//   (bow / crossbow pull stages, the trident's and spears' in-hand models,
//   the shield) and resources/model/cuboid/ItemTransform.
#include "client/renderer/entity/MobRenderer.hpp"
#include "client/renderer/entity/ShieldTextures.hpp"
#include "common/core/Ease.hpp"
#include "common/entity/SpearItem.hpp"

#include "client/renderer/entity/BlockCubeEntityRenderer.hpp"
#include "client/renderer/entity/ModMobRender.hpp"
#include "client/resource/ResourcePacks.hpp"
#include "client/renderer/texture/AtlasBuilder.hpp"
#include "client/renderer/viewmodel/ItemMeshBuilder.hpp"
#include "common/data/DataComponents.hpp"
#include "common/data/components/BlockDataComponents.hpp"
#include "common/entity/DyeColorUtil.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    namespace {

        // HeldItemSpriteMesh's sprite lookup: textures/item, else
        // textures/block. Cached (render thread) — asked per held item per
        // frame; a resource-pack reload drops the answers.
        bool SpriteTextureExists(const std::string& name) {
            static std::unordered_map<std::string, bool> s_known;
            static int s_packGeneration = -1;
            if (Resources::CacheStale(s_packGeneration)) s_known.clear();
            if (const auto it = s_known.find(name); it != s_known.end()) return it->second;
            const bool exists =
                std::filesystem::exists(PlatformMain::GetAssetPath("assets/textures/item/" + name + ".png")) ||
                std::filesystem::exists(PlatformMain::GetAssetPath("assets/textures/block/" + name + ".png"));
            s_known.emplace(name, exists);
            return exists;
        }

        using Game::EntityTypeId;
        using Game::EquipmentSlot;
        using Ctx = ItemDisplay::Context;

        // ── Item model resolution (the assets/items definitions) ──────────

        struct ResolvedItem {
            enum class Kind { None, Sprite, Block, Trident, Shield, Banner } kind = Kind::None;
            std::string model;   // the display model, "item/bow"
            // Sprite layers, in draw order, with their ARGB tint (0 = none).
            std::vector<std::pair<std::string, uint32_t>> sprites;
            Game::BlockID block = Game::BlockID::Air;
        };

        bool ContextIsGroundLike(Ctx c) {
            // The display_context cases the trident / spear definitions
            // route to their plain model: gui, ground, fixed (on_shelf).
            return c == Ctx::Gui || c == Ctx::Ground || c == Ctx::Fixed;
        }

        // The DyeColor ordinal a "<colour>_banner" item names (BannerItem's
        // colour), -1 for anything else.
        int BannerBaseColor(const std::string& slug) {
            static constexpr const char* kDyeNames[16] = {
                "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black" };
            for (int i = 0; i < 16; ++i) {
                if (slug == std::string(kDyeNames[i]) + "_banner") return i;
            }
            return -1;
        }

        ResolvedItem ResolveItem(const Game::ItemStack& stack, Ctx context,
                                 const MobRenderer::ItemUseState& use) {
            ResolvedItem r;
            if (stack.IsEmpty()) return r;
            const Game::ItemID id = stack.itemId;
            const std::string slug(Game::ItemRegistry::Slug(id));
            if (slug.empty()) return r;
            const bool groundLike = ContextIsGroundLike(context);
            const auto single = [&](const std::string& model, const std::string& sprite) {
                r.kind = ResolvedItem::Kind::Sprite;
                r.model = model;
                r.sprites.emplace_back(sprite, 0u);
            };

            // items/trident.json: the flat item in gui / ground / fixed; the
            // TridentModel special in hand, trident_throwing while in use.
            if (id == Game::Items::Trident) {
                if (groundLike) { single("item/trident", "trident"); return r; }
                r.kind = ResolvedItem::Kind::Trident;
                r.model = use.usingItem ? "item/trident_throwing" : "item/trident_in_hand";
                return r;
            }
            // items/shield.json: the ShieldModel special, shield_blocking in
            // use.
            if (id == Game::Items::Shield) {
                r.kind = ResolvedItem::Kind::Shield;
                r.model = use.usingItem ? "item/shield_blocking" : "item/shield";
                return r;
            }
            // items/<x>_spear.json: the long in-hand model outside gui /
            // ground / fixed.
            if (!groundLike && slug.size() > 6 && slug.compare(slug.size() - 6, 6, "_spear") == 0 &&
                ItemDisplay::ModelExists("item/" + slug + "_in_hand") && SpriteTextureExists(slug + "_in_hand")) {
                single("item/" + slug + "_in_hand", slug + "_in_hand");
                return r;
            }
            // items/bow.json: using_item → range_dispatch use_duration × 0.05
            // over 0.65 / 0.9.
            if (id == Game::Items::Bow && use.usingItem) {
                const float pull = use.useTicks * 0.05f;
                const char* stage = pull < 0.65f ? "bow_pulling_0" : (pull < 0.9f ? "bow_pulling_1" : "bow_pulling_2");
                single(std::string("item/") + stage, stage);
                return r;
            }
            // items/crossbow.json: charge_type arrow / rocket, else
            // using_item → crossbow/pull over 0.58 / 1.0, else the standby.
            if (id == Game::Items::Crossbow) {
                if (const auto charged = stack.get(Game::DataComponents::CHARGED_PROJECTILES);
                    charged && !charged->IsEmpty()) {
                    if (charged->Contains(Game::Items::FireworkRocket)) single("item/crossbow_firework", "crossbow_firework");
                    else                                               single("item/crossbow_arrow", "crossbow_arrow");
                    return r;
                }
                if (use.usingItem) {
                    // CrossbowItem.getChargeDuration (Quick Charge through
                    // EnchantmentHelper.modifyCrossbowChargingTime).
                    const float duration = std::floor(
                        Game::EnchantmentHelper::ModifyCrossbowChargingTime(stack, 1.25f) * 20.0f);
                    const float pull = duration > 0.0f ? std::min(use.useTicks / duration, 1.0f) : 1.0f;
                    const char* stage = pull < 0.58f ? "crossbow_pulling_0"
                                      : (pull < 1.0f ? "crossbow_pulling_1" : "crossbow_pulling_2");
                    single(std::string("item/") + stage, stage);
                    return r;
                }
                single("item/crossbow", "crossbow_standby");
                return r;
            }

            // items/<colour>_banner.json: the minecraft:banner special model
            // on template_banner's display (the wall banner has no item).
            if (slug.size() > 7 && slug.compare(slug.size() - 7, 7, "_banner") == 0 &&
                slug.find("wall_") == std::string::npos && BannerBaseColor(slug) >= 0) {
                r.kind = ResolvedItem::Kind::Banner;
                r.model = "item/template_banner";
                return r;
            }

            // Everything else: items/<slug>.json → models/item/<slug> (a
            // block item without one uses its block model).
            r.model = ItemDisplay::ModelExists("item/" + slug) ? "item/" + slug : "block/" + slug;
            const Game::BlockID block = MobRenderer::ItemBlockShown(id);
            if (block != Game::BlockID::Air && !ItemDisplay::IsGenerated(r.model)) {
                r.kind = ResolvedItem::Kind::Block;
                r.block = block;
                return r;
            }
            const Game::Item& item = Game::ItemRegistry::Get(id);
            r.kind = ResolvedItem::Kind::Sprite;
            if (item.spriteLayers.size() > 1) {
                for (size_t i = 0; i < item.spriteLayers.size(); ++i) {
                    r.sprites.emplace_back(item.spriteLayers[i], Game::ResolveItemLayerTint(stack, i));
                }
            } else {
                // The engine's sprite for the item, else the model's layer0
                // (a torch's block texture).
                std::string sprite = MobRenderer::ItemSpriteName(id);
                if (sprite.empty() || !SpriteTextureExists(sprite)) sprite = ItemDisplay::SpriteLayer(r.model, 0);
                if (sprite.empty()) { r.kind = ResolvedItem::Kind::None; return r; }
                r.sprites.emplace_back(sprite, Game::ResolveItemLayerTint(stack, 0));
            }
            return r;
        }

        // Append `src` (0..1-cell vertices of an ItemCubeVert or
        // ModelVertex mesh) through `m`, optionally tinted.
        template <typename V>
        void AppendTransformed(const std::vector<V>& src, const std::vector<uint32_t>& srcIdx,
                               const glm::mat4& m, uint32_t tintARGB,
                               std::vector<ModelVertex>& verts, std::vector<uint32_t>& idx) {
            const uint8_t tr = tintARGB ? static_cast<uint8_t>((tintARGB >> 16) & 0xFF) : 255;
            const uint8_t tg = tintARGB ? static_cast<uint8_t>((tintARGB >> 8) & 0xFF) : 255;
            const uint8_t tb = tintARGB ? static_cast<uint8_t>(tintARGB & 0xFF) : 255;
            const auto base = static_cast<uint32_t>(verts.size());
            for (const V& v : src) {
                const glm::vec3 p = glm::vec3(m * glm::vec4(v.x, v.y, v.z, 1.0f));
                verts.push_back({ p.x, p.y, p.z, v.u, v.v,
                                  static_cast<uint8_t>((v.r * tr) / 255),
                                  static_cast<uint8_t>((v.g * tg) / 255),
                                  static_cast<uint8_t>((v.b * tb) / 255), v.a });
            }
            for (const uint32_t i : srcIdx) idx.push_back(base + i);
        }

        // MC SkullBlock.Types ordinal for a mob-head block item, -1 for
        // anything else (the dragon head included — no DragonHeadModel here).
        int SkullKindOf(Game::ItemID id) {
            if (!Game::ItemRegistry::IsBlockItem(id)) return -1;
            switch (Game::ItemRegistry::ToBlock(id)) {
                case Game::BlockID::SkeletonSkull:       return 0;
                case Game::BlockID::WitherSkeletonSkull: return 1;
                case Game::BlockID::PlayerHead:          return 2;
                case Game::BlockID::ZombieHead:          return 3;
                case Game::BlockID::CreeperHead:         return 4;
                case Game::BlockID::PiglinHead:          return 5;
                default:                                 return -1;
            }
        }
        bool IsSkullBlockItem(Game::ItemID id) {
            if (!Game::ItemRegistry::IsBlockItem(id)) return false;
            return SkullKindOf(id) >= 0 || Game::ItemRegistry::ToBlock(id) == Game::BlockID::DragonHead;
        }

        // SkullBlockRenderer.SKIN_BY_TYPE, and each model's sheet size.
        struct SkullInfo { const char* texture; float texW, texH; };
        constexpr SkullInfo kSkullInfo[6] = {
            { "assets/textures/entity/skeleton/skeleton.png",        64.0f, 32.0f },
            { "assets/textures/entity/skeleton/wither_skeleton.png", 64.0f, 32.0f },
            // PLAYER without a profile: DefaultPlayerSkin (no profiles here).
            { "assets/textures/entity/player/wide/steve.png",        64.0f, 64.0f },
            { "assets/textures/entity/zombie/zombie.png",            64.0f, 64.0f },
            { "assets/textures/entity/creeper/creeper.png",          64.0f, 32.0f },
            { "assets/textures/entity/piglin/piglin.png",            64.0f, 64.0f },
        };

        void AddCube(ModelPart* part, float texX, float texY, float ox, float oy, float oz,
                     float sx, float sy, float sz, float grow = 0.0f, bool mirror = false) {
            part->cubes.push_back(CubeDefinition{ ox, oy, oz, sx, sy, sz, texX, texY, grow, grow, grow, mirror });
        }

        // The worn-head render layer bits a type's renderer registers.
        bool HasCustomHeadLayer(EntityTypeId type) {
            switch (type) {
                // HumanoidMobRenderer.
                case EntityTypeId::Zombie: case EntityTypeId::Husk: case EntityTypeId::Drowned:
                case EntityTypeId::ZombieVillager: case EntityTypeId::Giant:
                case EntityTypeId::Skeleton: case EntityTypeId::Stray: case EntityTypeId::Bogged:
                case EntityTypeId::Parched: case EntityTypeId::WitherSkeleton:
                case EntityTypeId::Piglin: case EntityTypeId::PiglinBrute: case EntityTypeId::ZombifiedPiglin:
                // IllagerRenderer.
                case EntityTypeId::Pillager: case EntityTypeId::Vindicator:
                case EntityTypeId::Evoker: case EntityTypeId::Illusioner:
                // VillagerRenderer, WanderingTraderRenderer.
                case EntityTypeId::Villager: case EntityTypeId::WanderingTrader:
                // CopperGolemRenderer.
                case EntityTypeId::CopperGolem:
                    return true;
                default:
                    return false;
            }
        }

        // The renderers with an ItemInHandLayer over a humanoid arm pair.
        bool HasItemInHandLayer(EntityTypeId type) {
            switch (type) {
                case EntityTypeId::Zombie: case EntityTypeId::Husk: case EntityTypeId::Drowned:
                case EntityTypeId::ZombieVillager: case EntityTypeId::Giant:
                case EntityTypeId::Skeleton: case EntityTypeId::Stray: case EntityTypeId::Bogged:
                case EntityTypeId::Parched: case EntityTypeId::WitherSkeleton:
                case EntityTypeId::Piglin: case EntityTypeId::PiglinBrute: case EntityTypeId::ZombifiedPiglin:
                case EntityTypeId::Pillager: case EntityTypeId::Vindicator:
                case EntityTypeId::Evoker: case EntityTypeId::Illusioner:
                case EntityTypeId::Vex:
                    return true;
                default:
                    return false;
            }
        }

        // Game::EquipmentSlot HEAD's piece drawn by the armour layer
        // (HumanoidArmorLayer.shouldRender: EQUIPPABLE in HEAD with an asset).
        bool DrawnAsHeadArmor(const Game::ItemStack& head) {
            const auto equippable = head.get(Game::DataComponents::EQUIPPABLE);
            return equippable && equippable->slot == EquipmentSlot::HEAD &&
                   !MobRenderer::EquipmentAssetOf(head).empty();
        }

    } // namespace

    // ── AppendItem — MC ItemStackRenderState.submit ────────────────────────

    void MobRenderer::AppendItem(const glm::mat4& pose, const Game::ItemStack& stack,
                                 ItemDisplay::Context context, const ItemUseState& use,
                                 const EmitFn& emit) {
        if (stack.IsEmpty()) return;
        // ITEM_MODEL / CUSTOM_MODEL_DATA (Game::GetRenderStack).
        {
            Game::ItemStack scratch;
            const Game::ItemStack& drawn = Game::GetRenderStack(stack, scratch);
            if (&drawn != &stack) {
                AppendItem(pose, drawn, context, use, emit);
                return;
            }
        }
        const ResolvedItem item = ResolveItem(stack, context, use);
        if (item.kind == ResolvedItem::Kind::None) return;
        // ItemTransform.apply (the left-hand fix for THIRD_PERSON_LEFT_HAND),
        // ending in the model cell's -0.5 centring.
        const glm::mat4 m = ItemDisplay::Apply(pose, ItemDisplay::Get(item.model, context),
                                               ItemDisplay::IsLeftHand(context));
        switch (item.kind) {
            case ResolvedItem::Kind::Sprite: {
                // ItemModelGenerator's extruded layers: the sprite mesh is
                // [0,16]² pixels with its depth centred on 0; the generated
                // model sits at z 7.5..8.5 of the cell.
                const glm::mat4 sm = glm::scale(glm::translate(m, glm::vec3(0.0f, 0.0f, 0.5f)),
                                                glm::vec3(1.0f / 16.0f));
                for (const auto& [sprite, tint] : item.sprites) {
                    SpriteEntry* entry = EnsureSpriteGeometry(sprite);
                    if (!entry) continue;
                    const size_t first = m_indices.size();
                    AppendTransformed(entry->verts, entry->indices, sm, tint, m_verts, m_indices);
                    emit(entry->texture, first, false);
                }
                break;
            }
            case ResolvedItem::Kind::Block: {
                if (!g_atlasBuilder) return;
                std::vector<ItemCubeVert> bv;
                std::vector<uint32_t> bi;
                // The item's block model (a fence's _inventory model), else
                // the default state's.
                if (!BuildBlockModelMesh(item.block, std::string(), bv, bi) || bv.empty()) {
                    bv.clear();
                    bi.clear();
                    BlockCubeEntityRenderer::BuildStateMesh(Game::BlockStates::Default(item.block), bv, bi);
                }
                if (bv.empty()) return;
                const size_t first = m_indices.size();
                AppendTransformed(bv, bi, m, 0u, m_verts, m_indices);
                emit(g_atlasBuilder->GetBackendTextureHandle(), first, false);
                break;
            }
            case ResolvedItem::Kind::Trident: {
                // TridentSpecialRenderer: DEFAULT_TRANSFORMATION scale(1, -1,
                // -1), then TridentModel in pixels. The geometry beneath the
                // projectile wrappers (pivot/yaw/orient) is the raw vertical
                // trident; the wrappers only turn the FLYING one.
                if (!m_heldTridentModel) m_heldTridentModel = std::make_unique<TridentModel>();
                ModelPart* orient = m_heldTridentModel->Root().Find("orient");
                if (!orient) return;
                glm::mat4 tm = glm::scale(m, glm::vec3(1.0f, -1.0f, -1.0f));
                tm = glm::scale(tm, glm::vec3(1.0f / 16.0f));
                // MC's pole sits at PartPose.ZERO; the projectile model
                // offsets it -11.5 px to centre the flying trident on its
                // pivot — taken back off here, so the grip is the shaft's
                // end as the in-hand displays expect.
                tm = glm::translate(tm, glm::vec3(0.0f, 11.5f, 0.0f));
                const size_t first = m_indices.size();
                for (const auto& child : orient->children) {
                    child->Build(tm, m_heldTridentModel->TexWidth(), m_heldTridentModel->TexHeight(),
                                 m_verts, m_indices, m_heldTridentModel->CullBackFaces());
                }
                emit(LoadTexture("assets/textures/entity/trident.png"), first, false);
                break;
            }
            case ResolvedItem::Kind::Shield: {
                // ShieldSpecialRenderer: scale(1, -1, -1), ShieldModel (plate
                // 12x22x1, handle 2x6x6) on the shield's sheet — its
                // BANNER_PATTERNS / BASE_COLOR layers over shield_base
                // (ShieldTextures).
                if (!m_shieldModel) {
                    m_shieldModel = std::make_unique<ModelPart>();
                    ModelPart* plate = m_shieldModel->AddChild("plate", PartPose::Zero());
                    AddCube(plate, 0, 0, -6.0f, -11.0f, -2.0f, 12.0f, 22.0f, 1.0f);
                    ModelPart* handle = m_shieldModel->AddChild("handle", PartPose::Zero());
                    AddCube(handle, 26, 0, -1.0f, -3.0f, -1.0f, 2.0f, 6.0f, 6.0f);
                }
                glm::mat4 shm = glm::scale(m, glm::vec3(1.0f, -1.0f, -1.0f));
                shm = glm::scale(shm, glm::vec3(1.0f / 16.0f));
                const size_t first = m_indices.size();
                m_shieldModel->Build(shm, 64.0f, 64.0f, m_verts, m_indices);
                emit(ShieldTextures::ForStack(stack), first, false);
                break;
            }
            case ResolvedItem::Kind::Banner: {
                // BannerSpecialRenderer → BannerRenderer.submitSpecial: the
                // model transformation translate(0.5, 0, 0.5), scale(2/3,
                // -2/3, -2/3); BannerModel (pole 2x42x2 at texOffs 44,0; bar
                // 20x2x2 at 0,42) and BannerFlagModel (flag 20x40x1 at 0,0,
                // offset (0, -44, 0), setupAnim(phase 0): xRot (-0.0125 +
                // 0.01 cos 0) PI) on banner_base; then submitPatterns: the
                // flag again on banner/base tinted with the banner's colour,
                // and up to 16 BANNER_PATTERNS layers, each its pattern sheet
                // tinted with the layer's colour.
                if (!m_bannerModel) {
                    m_bannerModel = std::make_unique<ModelPart>();
                    ModelPart* pole = m_bannerModel->AddChild("pole", PartPose::Zero());
                    AddCube(pole, 44, 0, -1.0f, -42.0f, -1.0f, 2.0f, 42.0f, 2.0f);
                    ModelPart* bar = m_bannerModel->AddChild("bar", PartPose::Zero());
                    AddCube(bar, 0, 42, -10.0f, -44.0f, -1.0f, 20.0f, 2.0f, 2.0f);
                    m_bannerFlagModel = std::make_unique<ModelPart>();
                    ModelPart* flag = m_bannerFlagModel->AddChild("flag", PartPose::Offset(0.0f, -44.0f, 0.0f));
                    AddCube(flag, 0, 0, -10.0f, 0.0f, -2.0f, 20.0f, 40.0f, 1.0f);
                }
                if (ModelPart* flag = m_bannerFlagModel->Find("flag")) {
                    flag->ResetPose();
                    flag->xRot = (-0.0125f + 0.01f * std::cos(0.0f)) * 3.1415927f;
                }
                glm::mat4 bm = glm::translate(m, glm::vec3(0.5f, 0.0f, 0.5f));
                bm = glm::scale(bm, glm::vec3(0.6666667f, -0.6666667f, -0.6666667f));
                bm = glm::scale(bm, glm::vec3(1.0f / 16.0f));

                const TextureHandle baseSheet = LoadTexture("assets/textures/entity/banner_base.png");
                const auto drawFlag = [&](TextureHandle tex, uint32_t rgb) {
                    if (tex == INVALID_TEXTURE) return;
                    const size_t firstVert = m_verts.size();
                    const size_t first = m_indices.size();
                    m_bannerFlagModel->Build(bm, 64.0f, 64.0f, m_verts, m_indices);
                    for (size_t i = firstVert; i < m_verts.size(); ++i) {
                        m_verts[i].r = static_cast<uint8_t>((rgb >> 16) & 0xFF);
                        m_verts[i].g = static_cast<uint8_t>((rgb >> 8) & 0xFF);
                        m_verts[i].b = static_cast<uint8_t>(rgb & 0xFF);
                    }
                    emit(tex, first, false);
                };
                {
                    const size_t first = m_indices.size();
                    m_bannerModel->Build(bm, 64.0f, 64.0f, m_verts, m_indices);
                    emit(baseSheet, first, false);
                }
                drawFlag(baseSheet, 0xFFFFFF);
                const int baseColor = BannerBaseColor(std::string(Game::ItemRegistry::Slug(stack.itemId)));
                drawFlag(LoadTexture("assets/textures/entity/banner/base.png"),
                         Game::DyeTextureDiffuseColor(static_cast<uint8_t>(std::max(baseColor, 0))));
                if (const auto patterns = stack.get(Game::DataComponents::BANNER_PATTERNS)) {
                    const size_t count = std::min<size_t>(patterns->layers.size(), 16);
                    for (size_t i = 0; i < count; ++i) {
                        const Game::BannerPatternLayer& layer = patterns->layers[i];
                        // Sheets.getBannerSprite: entity/banner/<asset path>
                        // (every vanilla pattern's asset id is its own id).
                        std::string path = layer.pattern;
                        if (const size_t colon = path.find(':'); colon != std::string::npos) path = path.substr(colon + 1);
                        drawFlag(LoadTexture("assets/textures/entity/banner/" + path + ".png"),
                                 Game::DyeTextureDiffuseColor(layer.color));
                    }
                }
                break;
            }
            case ResolvedItem::Kind::None:
                break;
        }
    }

    // ── ItemInHandLayer ────────────────────────────────────────────────────

    bool MobRenderer::ArmItemPose(const EntityModel& model, Game::EntityTypeId type,
                                  const glm::mat4& entityMatrix, bool leftArm, bool babyGrip,
                                  glm::mat4& out, const glm::mat4* handOverride) const {
        glm::mat4 hand(1.0f);
        if (handOverride) {
            hand = *handOverride;
        } else if (!model.HandMatrix(leftArm, hand)) {
            return false;
        }
        glm::mat4 m = entityMatrix * hand;
        // VexModel.translateToHand: after the arm, scale 0.55 and a small
        // shove, mirrored for the left arm — in blocks (×16 in pixels).
        if (type == Game::EntityTypeId::Vex) {
            m = glm::scale(m, glm::vec3(0.55f));
            m = glm::translate(m, glm::vec3(leftArm ? -0.046875f : 0.046875f, -0.15625f, 0.078125f) * 16.0f);
        }
        // submitArmWithItem: X -90, Y 180, then the grip — (±1, 2, -10)
        // sixteenths, or the baby's (0, 1, -4.5) — and back to blocks.
        m = glm::rotate(m, glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        m = glm::rotate(m, glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        const float offX = babyGrip ? 0.0f : 1.0f;
        const float offY = babyGrip ? 1.0f : 2.0f;
        const float offZ = babyGrip ? -4.5f : -10.0f;
        m = glm::translate(m, glm::vec3((leftArm ? -1.0f : 1.0f) * offX, offY, offZ));
        out = glm::scale(m, glm::vec3(16.0f));
        return true;
    }

    void MobRenderer::ArmUseStates(const EntityRenderState& state, ItemUseState& right, ItemUseState& left) {
        // HumanoidRenderState.ticksUsingItem(arm): the clock on the arm the
        // used hand is (useItemHand.asArm(mainArm)); the swing and its STAB
        // type on the main arm (the swinging hand here is always the main).
        const bool mainArmRight = state.mainArm >= 0.5f;
        const bool usedArmRight = state.useItemHand == 0 ? mainArmRight : !mainArmRight;
        right = ItemUseState{};
        left  = ItemUseState{};
        ItemUseState& used = usedArmRight ? right : left;
        used.usingItem = state.isUsingItem;
        used.useTicks  = state.isUsingItem ? state.ticksUsingItem : 0.0f;
        for (ItemUseState* use : { &right, &left }) {
            use->swingAnimation = state.attackTime;
            use->ticksSinceKineticHitFeedback = state.ticksSinceKineticHitFeedback;
        }
        right.spearPose = state.rightArmPose == ArmPose::Spear;
        left.spearPose  = state.leftArmPose == ArmPose::Spear;
        ItemUseState& swinging = mainArmRight ? right : left;
        swinging.stabbing = state.swingAnimType >= 1.5f && state.attackTime > 0.0f;
    }

    void MobRenderer::AppendHeldItems(const EntityModel& model, Game::EntityTypeId type,
                                      const glm::mat4& entityMatrix,
                                      const Game::ItemStack& rightItem, const Game::ItemStack& leftItem,
                                      bool babyGrip, const ItemUseState& rightUse,
                                      const ItemUseState& leftUse, const EmitFn& emit) {
        const auto arm = [&](const Game::ItemStack& stack, bool left, const ItemUseState& use) {
            if (stack.IsEmpty()) return;
            glm::mat4 pose;
            if (!ArmItemPose(model, type, entityMatrix, left, babyGrip, pose)) return;
            // submitArmWithItem's spear motions, in blocks on the grip:
            const std::optional<Game::Spear::SpearDefinition> spear = Game::Spear::ForStack(stack);
            const float invert = left ? -1.0f : 1.0f;
            const auto rotateAround = [&pose](float degrees, const glm::vec3& axis, const glm::vec3& pivot) {
                pose = glm::translate(pose, pivot);
                pose = glm::rotate(pose, glm::radians(degrees), axis);
                pose = glm::translate(pose, -pivot);
            };
            // SpearAnimations.thirdPersonAttackItem — this arm's STAB swing.
            if (use.stabbing && use.swingAnimation > 0.0f) {
                const float jetForward = spear && spear->kineticWeapon ? spear->kineticWeapon->forwardMovement : 0.0f;
                const float attack  = Game::Ease::InQuad(Game::Ease::Progress(use.swingAnimation, 0.05f, 0.2f));
                const float retract = Game::Ease::InOutExpo(Game::Ease::Progress(use.swingAnimation, 0.4f, 1.0f));
                rotateAround(-70.0f * (attack - retract), glm::vec3(1.0f, 0.0f, 0.0f),
                             glm::vec3(0.0f, -0.125f, 0.125f));
                pose = glm::translate(pose, glm::vec3(0.0f, jetForward * (attack - retract), 0.0f));
            }
            // ArmPose.SPEAR.animateUseItem → SpearAnimations
            // .thirdPersonUseItem — the charging spear's raise, sway and
            // recoil on the arm in use.
            if (use.spearPose && use.useTicks != 0.0f && spear && spear->kineticWeapon) {
                const Game::Spear::UseParams p =
                    Game::Spear::UseParams::FromKineticWeapon(*spear->kineticWeapon, use.useTicks);
                const float attack  = Game::Ease::InQuad(Game::Ease::Progress(use.swingAnimation, 0.05f, 0.2f));
                const float retract = Game::Ease::InOutExpo(Game::Ease::Progress(use.swingAnimation, 0.4f, 1.0f));
                const float raiseModified = 1.0f - Game::Ease::OutBack(1.0f - p.raiseProgress);
                const float hitFeedback = Game::Spear::HitFeedbackAmount(use.ticksSinceKineticHitFeedback);
                pose = glm::translate(pose, glm::vec3(0.0f, -hitFeedback * 0.4f,
                    -spear->kineticWeapon->forwardMovement * (raiseModified - p.raiseBackProgress) + hitFeedback));
                rotateAround(-(70.0f * (p.raiseProgress - p.raiseBackProgress) - 40.0f * (attack - retract)),
                             glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -0.03125f, 0.125f));
                rotateAround(invert * 90.0f * (p.raiseProgress - p.swayProgress + 3.0f * retract + attack),
                             glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 0.125f));
            }
            AppendItem(pose, stack, left ? Ctx::ThirdPersonLeftHand : Ctx::ThirdPersonRightHand, use, emit);
        };
        arm(rightItem, false, rightUse);
        arm(leftItem, true, leftUse);
    }

    // ── CustomHeadLayer ────────────────────────────────────────────────────

    MobRenderer::HeadTransforms MobRenderer::HeadTransformsFor(Game::EntityTypeId type) {
        HeadTransforms t;
        switch (type) {
            // PiglinRenderer.PIGLIN_CUSTOM_HEAD_TRANSFORMS (the zombified
            // piglin and the brute share the renderer's constant).
            case EntityTypeId::Piglin: case EntityTypeId::PiglinBrute: case EntityTypeId::ZombifiedPiglin:
                t.horizontalScale = 1.0019531f;
                break;
            // VillagerRenderer.CUSTOM_HEAD_TRANSFORMS (ZombieVillagerRenderer
            // passes the same).
            case EntityTypeId::Villager: case EntityTypeId::ZombieVillager:
                t.yOffset = -0.1171875f;
                t.skullYOffset = -0.07421875f;
                break;
            // CopperGolemRenderer: Transforms.DEFAULT over CopperGolemModel,
            // whose translateToHead ends translate(0, 0.125, 0),
            // scale(1.0625).
            case EntityTypeId::CopperGolem:
                t.modelHeadYOffset = 0.125f;
                t.modelHeadScale = 1.0625f;
                break;
            default:
                break;
        }
        return t;
    }

    void MobRenderer::AppendCustomHead(const EntityModel& model, const glm::mat4& entityMatrix,
                                       const Game::ItemStack& head, const HeadTransforms& transforms,
                                       float wornHeadAnimationPos, const EmitFn& emit) {
        if (head.IsEmpty()) return;
        const bool skull = IsSkullBlockItem(head.itemId);
        // LivingEntityRenderer.extractRenderState: a mob head is wornHeadType;
        // anything else is headItem unless the armour layer draws it.
        if (!skull && DrawnAsHeadArmor(head)) return;
        // scale(horizontal, vertical, horizontal), root, translateToHead —
        // the chain to the "head" part (a root child on every HeadedModel
        // here).
        glm::mat4 headChain(1.0f);
        {
            glm::mat4 acc(1.0f);
            std::function<bool(const ModelPart&, const glm::mat4&)> find =
                [&](const ModelPart& part, const glm::mat4& parent) -> bool {
                    const glm::mat4 here = parent * part.LocalMatrix();
                    if (part.name == "head") { headChain = here; return true; }
                    for (const auto& child : part.children) {
                        if (find(*child, here)) return true;
                    }
                    return false;
                };
            if (!find(model.Root(), acc)) return;
        }
        glm::mat4 m = glm::scale(entityMatrix, glm::vec3(transforms.horizontalScale, transforms.verticalScale,
                                                         transforms.horizontalScale));
        m = m * headChain;
        m = glm::scale(m, glm::vec3(16.0f));   // pixels → blocks
        // translateToHead's model-specific tail (CopperGolemModel).
        m = glm::translate(m, glm::vec3(0.0f, transforms.modelHeadYOffset, 0.0f));
        m = glm::scale(m, glm::vec3(transforms.modelHeadScale));

        if (skull) {
            const int kind = SkullKindOf(head.itemId);
            if (kind < 0) return;   // the dragon head: no DragonHeadModel here
            if (!m_skullModels[kind]) {
                // SkullModel.createMobHeadLayer / createHumanoidHeadLayer,
                // PiglinHeadModel (PiglinModel.addHead).
                auto root = std::make_unique<ModelPart>();
                ModelPart* h = root->AddChild("head", PartPose::Zero());
                if (kind == 5) {
                    AddCube(h, 0, 0, -5.0f, -8.0f, -4.0f, 10.0f, 8.0f, 8.0f);
                    AddCube(h, 31, 1, -2.0f, -4.0f, -5.0f, 4.0f, 4.0f, 1.0f);
                    AddCube(h, 2, 4, 2.0f, -2.0f, -5.0f, 1.0f, 2.0f, 1.0f);
                    AddCube(h, 2, 0, -3.0f, -2.0f, -5.0f, 1.0f, 2.0f, 1.0f);
                    ModelPart* leftEar = h->AddChild("left_ear",
                        PartPose::OffsetAndRotation(4.5f, -6.0f, 0.0f, 0.0f, 0.0f, -0.5235988f));
                    AddCube(leftEar, 51, 6, 0.0f, 0.0f, -2.0f, 1.0f, 5.0f, 4.0f);
                    ModelPart* rightEar = h->AddChild("right_ear",
                        PartPose::OffsetAndRotation(-4.5f, -6.0f, 0.0f, 0.0f, 0.0f, 0.5235988f));
                    AddCube(rightEar, 39, 6, -1.0f, 0.0f, -2.0f, 1.0f, 5.0f, 4.0f);
                } else {
                    AddCube(h, 0, 0, -4.0f, -8.0f, -4.0f, 8.0f, 8.0f, 8.0f);
                    if (kind == 2 || kind == 3) {
                        ModelPart* hat = h->AddChild("hat", PartPose::Zero());
                        AddCube(hat, 32, 0, -4.0f, -8.0f, -4.0f, 8.0f, 8.0f, 8.0f, 0.25f);
                    }
                }
                m_skullModels[kind] = std::move(root);
            }
            ModelPart& root = *m_skullModels[kind];
            root.ResetPose();
            if (kind == 5) {
                // PiglinHeadModel.setupAnim: the ears flap with the wearer's
                // walk (wornHeadAnimationPos).
                if (ModelPart* h = root.Find("head")) {
                    const float pos = wornHeadAnimationPos;
                    if (ModelPart* le = h->Find("left_ear")) {
                        le->zRot = static_cast<float>(-(std::cos(pos * 3.1415927f * 0.2f * 1.2f) + 2.5f)) * 0.2f;
                    }
                    if (ModelPart* re = h->Find("right_ear")) {
                        re->zRot = static_cast<float>(std::cos(pos * 3.1415927f * 0.2f) + 2.5f) * 0.2f;
                    }
                }
            }
            // translate(0, skullYOffset, 0), scale 1.1875, the skull model
            // in pixels.
            glm::mat4 sk = glm::translate(m, glm::vec3(0.0f, transforms.skullYOffset, 0.0f));
            sk = glm::scale(sk, glm::vec3(1.1875f / 16.0f));
            const TextureHandle tex = LoadTexture(kSkullInfo[kind].texture);
            const size_t first = m_indices.size();
            root.Build(sk, kSkullInfo[kind].texW, kSkullInfo[kind].texH, m_verts, m_indices);
            emit(tex, first, false);
            return;
        }
        // CustomHeadLayer.translateToHead: translate(0, -0.25 + yOffset, 0),
        // Y 180, scale(0.625, -0.625, -0.625); then the HEAD display.
        m = glm::translate(m, glm::vec3(0.0f, -0.25f + transforms.yOffset, 0.0f));
        m = glm::rotate(m, glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        m = glm::scale(m, glm::vec3(0.625f, -0.625f, -0.625f));
        AppendItem(m, head, Ctx::Head, ItemUseState{}, emit);
    }

    // ── HumanoidArmorLayer ─────────────────────────────────────────────────

    MobRenderer::ArmorFamily MobRenderer::ArmorFamilyFor(Game::EntityTypeId type) {
        switch (type) {
            // AbstractZombieRenderer, GiantMobRenderer, AbstractSkeletonRenderer.
            case EntityTypeId::Zombie: case EntityTypeId::Husk: case EntityTypeId::Drowned:
            case EntityTypeId::Giant:
            case EntityTypeId::Skeleton: case EntityTypeId::Stray: case EntityTypeId::Bogged:
            case EntityTypeId::Parched: case EntityTypeId::WitherSkeleton:
                return ArmorFamily::Humanoid;
            case EntityTypeId::ZombieVillager:
                return ArmorFamily::ZombieVillager;
            // PiglinRenderer (piglin, brute), ZombifiedPiglinRenderer.
            case EntityTypeId::Piglin: case EntityTypeId::PiglinBrute: case EntityTypeId::ZombifiedPiglin:
                return ArmorFamily::Piglin;
            default:
                return ArmorFamily::None;
        }
    }

    HumanoidArmorModel& MobRenderer::ArmorModel(int index) {
        using Mesh = HumanoidArmorModel::Mesh;
        std::unique_ptr<HumanoidArmorModel>& slot = m_armorModels[index];
        if (!slot) {
            switch (index) {
                // LayerDefinitions: OUTER 1.0, INNER 0.5; the piglin outer
                // 1.02; BABY_OUTER (-0.1, 0.5, 0.3), BABY_INNER (-0.1, 0.3,
                // 0.3); the piglin babies 0.7 both with
                // BABY_PIGLIN_ARMOR_ARM_OFFSET (0.5, -0.5, 0).
                case 0: slot = std::make_unique<HumanoidArmorModel>(Mesh::Humanoid, glm::vec3(1.0f)); break;
                case 1: slot = std::make_unique<HumanoidArmorModel>(Mesh::Humanoid, glm::vec3(0.5f)); break;
                case 2: slot = std::make_unique<HumanoidArmorModel>(Mesh::Humanoid, glm::vec3(1.02f)); break;
                case 3: slot = std::make_unique<HumanoidArmorModel>(Mesh::ZombieVillager, glm::vec3(1.0f)); break;
                case 4: slot = std::make_unique<HumanoidArmorModel>(Mesh::ZombieVillager, glm::vec3(0.5f)); break;
                case 5: slot = std::make_unique<HumanoidArmorModel>(Mesh::HumanoidBaby, glm::vec3(-0.1f, 0.5f, 0.3f)); break;
                case 6: slot = std::make_unique<HumanoidArmorModel>(Mesh::HumanoidBaby, glm::vec3(-0.1f, 0.3f, 0.3f)); break;
                default:
                    slot = std::make_unique<HumanoidArmorModel>(Mesh::HumanoidBaby, glm::vec3(0.7f),
                                                                glm::vec3(0.5f, -0.5f, 0.0f));
                    break;
            }
        }
        return *slot;
    }

    void MobRenderer::AppendHumanoidArmor(ArmorFamily family, bool babyMesh,
                                          const Game::ItemStack* equipment, EntityModel& wearer,
                                          const EntityRenderState& state, const glm::dvec3& renderPos,
                                          float bodyRot, const glm::vec3& cameraPos, const EmitFn& emit,
                                          const glm::mat4* rootOverride) {
        if (family == ArmorFamily::None || !equipment) return;
        // One armour mesh onto the batch: the entity chain AppendMob builds,
        // or the caller's own root (a skinned player's full pose stack).
        const auto appendArmor = [&](HumanoidArmorModel& armorModel) {
            if (!rootOverride) {
                AppendMob(armorModel, state, renderPos, bodyRot, cameraPos, m_verts, m_indices);
                return;
            }
            armorModel.SetupAnim(state);
            armorModel.Root().Build(*rootOverride, armorModel.TexWidth(), armorModel.TexHeight(),
                                    m_verts, m_indices, armorModel.CullBackFaces());
        };
        // HumanoidArmorLayer.submit's order: chest, legs, feet, head.
        static constexpr EquipmentSlot kOrder[4] = {
            EquipmentSlot::CHEST, EquipmentSlot::LEGS, EquipmentSlot::FEET, EquipmentSlot::HEAD };
        int posed = 0;   // bit per armour model index already posed this call
        for (const EquipmentSlot slot : kOrder) {
            const Game::ItemStack& piece = equipment[static_cast<int>(slot)];
            if (piece.IsEmpty()) continue;
            const auto equippable = piece.get(Game::DataComponents::EQUIPPABLE);
            if (!equippable || equippable->slot != slot) continue;
            const std::string asset = EquipmentAssetOf(piece);
            if (asset.empty()) continue;
            const bool inner = slot == EquipmentSlot::LEGS;   // usesInnerModel
            int index = 0;
            switch (family) {
                case ArmorFamily::Humanoid:
                    index = babyMesh ? (inner ? 6 : 5) : (inner ? 1 : 0);
                    break;
                case ArmorFamily::ZombieVillager:
                    index = babyMesh ? (inner ? 6 : 5) : (inner ? 4 : 3);
                    break;
                case ArmorFamily::Piglin:
                    index = babyMesh ? 7 : (inner ? 1 : 2);
                    break;
                case ArmorFamily::None:
                    return;
            }
            HumanoidArmorModel& armor = ArmorModel(index);
            if (!(posed & (1 << index))) {
                // The wearer's parts as posed this frame.
                if (!armor.CopyPoseFrom(wearer)) return;
                posed |= 1 << index;
            }
            // EquipmentClientInfo.LayerType: HUMANOID_BABY for a baby (every
            // slot), HUMANOID_LEGGINGS for the legs, HUMANOID otherwise.
            const std::string dir = babyMesh ? "assets/textures/entity/equipment/humanoid_baby/"
                                  : inner    ? "assets/textures/entity/equipment/humanoid_leggings/"
                                             : "assets/textures/entity/equipment/humanoid/";
            const TextureHandle sheet = LoadTexture(dir + asset + ".png");
            if (sheet == INVALID_TEXTURE) continue;
            const bool leather = asset == "leather";
            armor.ShowPartsForSlot(static_cast<int>(slot));

            const size_t first = m_indices.size();
            const size_t firstVert = m_verts.size();
            appendArmor(armor);
            if (leather) {
                // The dyeable layer: DyedItemColor, else colorWhenUndyed
                // (-6265536, 0xA06540).
                uint32_t rgb = 0xA06540u;
                if (const auto dyed = piece.get(Game::DataComponents::DYED_COLOR)) {
                    rgb = static_cast<uint32_t>(*dyed) & 0xFFFFFFu;
                }
                const uint8_t r = static_cast<uint8_t>((rgb >> 16) & 0xFF);
                const uint8_t g = static_cast<uint8_t>((rgb >> 8) & 0xFF);
                const uint8_t b = static_cast<uint8_t>(rgb & 0xFF);
                for (size_t i = firstVert; i < m_verts.size(); ++i) {
                    ModelVertex& v = m_verts[i];
                    v.r = static_cast<uint8_t>((v.r * r) / 255);
                    v.g = static_cast<uint8_t>((v.g * g) / 255);
                    v.b = static_cast<uint8_t>((v.b * b) / 255);
                }
            }
            emit(sheet, first, false);
            if (leather) {
                const TextureHandle overlay = LoadTexture(dir + "leather_overlay.png");
                if (overlay != INVALID_TEXTURE) {
                    const size_t overlayFirst = m_indices.size();
                    appendArmor(armor);
                    emit(overlay, overlayFirst, false);
                }
            }
            // EquipmentLayerRenderer: the TRIM on the piece's layer — not on
            // the baby layer (HUMANOID_BABY has no trims).
            if (!babyMesh) {
                const TextureHandle trimTex = TrimTexture(piece, asset, inner);
                if (trimTex != INVALID_TEXTURE) {
                    const size_t trimFirst = m_indices.size();
                    appendArmor(armor);
                    emit(trimTex, trimFirst, false);
                }
            }
        }
    }

    void MobRenderer::AppendMobArmorLayer(const Game::Mob& mob, EntityModel& model,
                                          const EntityRenderState& state,
                                          const glm::dvec3& renderPos, float bodyRot,
                                          const glm::vec3& cameraPos, const EmitFn& emit) {
        const ArmorFamily family = ArmorFamilyFor(mob.GetType());
        if (family == ArmorFamily::None) return;
        Game::ItemStack equipment[Game::Mob::kHumanoidEquipmentSlots];
        for (int i = 0; i < Game::Mob::kHumanoidEquipmentSlots; ++i) {
            equipment[i] = mob.GetEquipment(static_cast<EquipmentSlot>(i));
        }
        // The 26.x baby armour mesh set goes with the 26.x baby body; the
        // classic look's baby is the adult mesh through the baby transform,
        // which its poses carry onto the adult armour.
        const ModelEntry* entry = state.isBaby ? GetModelFor(mob.GetType()) : nullptr;
        const bool babyMesh = entry && GetBabyModelLook() == BabyModelLook::New &&
                              entry->babyModel && &model == entry->babyModel.get();
        AppendHumanoidArmor(family, babyMesh, equipment, model, state, renderPos, bodyRot, cameraPos, emit);
    }

    // ── The mob pass's item layers ─────────────────────────────────────────

    void MobRenderer::AppendMobItemLayers(const Game::Mob& mob, Game::EntityTypeId type, EntityModel& model,
                                          const EntityRenderState& state, const glm::mat4& entityMatrix,
                                          const EmitFn& emit) {
        const Game::ItemStack& mainHand = mob.GetEquipment(EquipmentSlot::MAINHAND);
        const Game::ItemStack& offHand  = mob.GetEquipment(EquipmentSlot::OFFHAND);
        // Mob.getMainArm: LEFT for a left-handed mob — the main hand's item
        // is then the LEFT arm's (getItemHeldByArm).
        const bool leftHanded = mob.IsLeftHanded();
        const Game::ItemStack& rightItem = leftHanded ? offHand : mainHand;
        const Game::ItemStack& leftItem  = leftHanded ? mainHand : offHand;
        // The block units MC's layers work in, after LivingEntityRenderer's
        // flip and model offset.
        const glm::mat4 blocks = glm::scale(entityMatrix, glm::vec3(16.0f));

        // CustomHeadLayer (the piglin head's ears read the walk).
        if (HasCustomHeadLayer(type)) {
            AppendCustomHead(model, entityMatrix, mob.GetEquipment(EquipmentSlot::HEAD),
                             HeadTransformsFor(type), state.walkAnimationPos, emit);
        }

        switch (type) {
            // VindicatorRenderer: the item only while aggressive.
            case EntityTypeId::Vindicator:
                if (!state.isAggressive) return;
                break;
            // EvokerRenderer: only while casting (SPELLCASTING arm pose).
            case EntityTypeId::Evoker:
                if (state.mobArmPose != 2.0f) return;
                break;
            // IllusionerRenderer: casting or aggressive.
            case EntityTypeId::Illusioner:
                if (state.mobArmPose != 2.0f && !state.isAggressive) return;
                break;
            // AllayRenderer: ItemInHandLayer over AllayModel.translateToHand
            // — root, body, translate (0, 1/16, 3/16), the right arm's xRot,
            // scale 0.7, translate (1/16, 0, 0). Its held stack is its own
            // (Allay::GetMainHandItem).
            case EntityTypeId::Allay: {
                const auto* allay = dynamic_cast<const Game::Allay*>(&mob);
                const Game::ItemStack held = allay ? allay->GetMainHandItem() : Game::ItemStack{};
                if (held.IsEmpty()) return;
                glm::mat4 body(1.0f);
                const ModelPart* rightArm = nullptr;
                std::function<bool(const ModelPart&, const glm::mat4&)> walk =
                    [&](const ModelPart& part, const glm::mat4& parent) -> bool {
                        const glm::mat4 here = parent * part.LocalMatrix();
                        if (part.name == "body") body = here;
                        if (part.name == "right_arm") rightArm = &part;
                        for (const auto& child : part.children) walk(*child, here);
                        return true;
                    };
                walk(model.Root(), glm::mat4(1.0f));
                glm::mat4 hand = body;
                hand = glm::translate(hand, glm::vec3(0.0f, 1.0f, 3.0f));
                hand = glm::rotate(hand, rightArm ? rightArm->xRot : 0.0f, glm::vec3(1.0f, 0.0f, 0.0f));
                hand = glm::scale(hand, glm::vec3(0.7f));
                hand = glm::translate(hand, glm::vec3(1.0f, 0.0f, 0.0f));
                glm::mat4 pose;
                if (ArmItemPose(model, type, entityMatrix, false, state.isBaby, pose, &hand)) {
                    AppendItem(pose, held, Ctx::ThirdPersonRightHand, ItemUseState{}, emit);
                }
                return;
            }
            // CopperGolemRenderer: ItemInHandLayer over CopperGolemModel.
            // translateToHand — root, body, the arm; then, IDLE, Y ∓90 and
            // (0, 0, 0.125), else scale 0.55 and (-0.125, 0.3125, -0.1875)
            // (blocks; ×16 in the pixel chain here) — and BlockDecorationLayer:
            // the antenna's block (EQUIPMENT_SLOT_ANTENNA) through
            // applyBlockOnAntennaTransform — root, body, head, then
            // (0, -1.75, 0) — and UNIT_CUBE_BOTTOM_CENTER_TO_ANTENNA_CENTER,
            // NO_OVERLAY. (The CustomHeadLayer is above.)
            case EntityTypeId::CopperGolem: {
                const auto* golem = dynamic_cast<const Game::CopperGolem*>(&mob);
                if (!golem) return;
                glm::mat4 body(1.0f), head(1.0f);
                const ModelPart* arms[2] = { nullptr, nullptr };   // right, left
                glm::mat4 armChain[2] = { glm::mat4(1.0f), glm::mat4(1.0f) };
                std::function<void(const ModelPart&, const glm::mat4&)> walk =
                    [&](const ModelPart& part, const glm::mat4& parent) {
                        const glm::mat4 here = parent * part.LocalMatrix();
                        if (part.name == "body") body = here;
                        if (part.name == "head") head = here;
                        if (part.name == "right_arm") { arms[0] = &part; armChain[0] = here; }
                        if (part.name == "left_arm")  { arms[1] = &part; armChain[1] = here; }
                        for (const auto& child : part.children) walk(*child, here);
                    };
                walk(model.Root(), glm::mat4(1.0f));
                const bool idle = golem->GetState() == Game::CopperGolem::State::Idle;
                // ItemInHandLayer: the right arm holds the main hand (the
                // golem's main arm), the left the off hand.
                for (int side = 0; side < 2; ++side) {
                    const Game::ItemStack& stack = side == 0 ? rightItem : leftItem;
                    if (stack.IsEmpty() || !arms[side]) continue;
                    const bool left = side == 1;
                    glm::mat4 hand = armChain[side];
                    if (idle) {
                        hand = glm::rotate(hand, glm::radians(left ? 90.0f : -90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
                        hand = glm::translate(hand, glm::vec3(0.0f, 0.0f, 0.125f * 16.0f));
                    } else {
                        hand = glm::scale(hand, glm::vec3(0.55f));
                        hand = glm::translate(hand, glm::vec3(-0.125f, 0.3125f, -0.1875f) * 16.0f);
                    }
                    glm::mat4 pose;
                    if (ArmItemPose(model, type, entityMatrix, left, false, pose, &hand)) {
                        AppendItem(pose, stack, left ? Ctx::ThirdPersonLeftHand : Ctx::ThirdPersonRightHand,
                                   ItemUseState{}, emit);
                    }
                }
                // BlockDecorationLayer: a BlockItem on the antenna, its
                // BLOCK_STATE component applied to the block's default state.
                const Game::ItemStack& antenna = mob.GetEquipment(Game::CopperGolem::kAntennaSlot);
                if (!antenna.IsEmpty() && Game::ItemRegistry::IsBlockItem(antenna.itemId) && g_atlasBuilder) {
                    const Game::BlockState blockState = Game::ApplyBlockItemStateProperties(
                        antenna, Game::BlockStates::Default(Game::ItemRegistry::ToBlock(antenna.itemId)));
                    std::vector<ItemCubeVert> bv;
                    std::vector<uint32_t> bi;
                    BlockCubeEntityRenderer::BuildStateMesh(blockState, bv, bi);
                    if (!bv.empty()) {
                        glm::mat4 m = glm::scale(entityMatrix * head, glm::vec3(16.0f));   // pixels -> blocks
                        m = glm::translate(m, glm::vec3(0.0f, -1.75f, 0.0f));
                        // UNIT_CUBE_BOTTOM_CENTER_TO_ANTENNA_CENTER:
                        // translation(-0.5, 0, -0.5).rotateAround(Z 180°,
                        // 0.5, 0.5, 0.5).
                        m = glm::translate(m, glm::vec3(-0.5f, 0.0f, -0.5f));
                        m = glm::translate(m, glm::vec3(0.5f));
                        m = glm::rotate(m, glm::radians(180.0f), glm::vec3(0.0f, 0.0f, 1.0f));
                        m = glm::translate(m, glm::vec3(-0.5f));
                        const size_t first = m_indices.size();
                        AppendTransformed(bv, bi, m, 0u, m_verts, m_indices);
                        emit(g_atlasBuilder->GetBackendTextureHandle(), first, false);
                    }
                }
                (void)body;
                return;
            }
            // WitchItemLayer (a CrossedArmsItemLayer): a potion goes to the
            // nose — root, head, nose, translate (1/16, 4/16, 0), Z 180,
            // X 140, Z 10, X 180 — anything else across the arms.
            case EntityTypeId::Witch: {
                if (mainHand.IsEmpty()) return;
                glm::mat4 chain(1.0f);
                const bool potion = mainHand.itemId == Game::Items::Potion;
                std::function<bool(const ModelPart&, const glm::mat4&, std::string_view)> find =
                    [&](const ModelPart& part, const glm::mat4& parent, std::string_view name) -> bool {
                        const glm::mat4 here = parent * part.LocalMatrix();
                        if (part.name == name) { chain = here; return true; }
                        for (const auto& child : part.children) {
                            if (find(*child, here, name)) return true;
                        }
                        return false;
                    };
                glm::mat4 m;
                if (potion) {
                    if (!find(model.Root(), glm::mat4(1.0f), "nose")) return;
                    m = glm::scale(entityMatrix * chain, glm::vec3(16.0f));
                    m = glm::translate(m, glm::vec3(0.0625f, 0.25f, 0.0f));
                    m = glm::rotate(m, glm::radians(180.0f), glm::vec3(0.0f, 0.0f, 1.0f));
                    m = glm::rotate(m, glm::radians(140.0f), glm::vec3(1.0f, 0.0f, 0.0f));
                    m = glm::rotate(m, glm::radians(10.0f), glm::vec3(0.0f, 0.0f, 1.0f));
                    m = glm::rotate(m, glm::radians(180.0f), glm::vec3(1.0f, 0.0f, 0.0f));
                } else {
                    if (!find(model.Root(), glm::mat4(1.0f), "arms")) return;
                    m = glm::scale(entityMatrix * chain, glm::vec3(16.0f));
                    m = glm::rotate(m, 0.75f, glm::vec3(1.0f, 0.0f, 0.0f));
                    m = glm::scale(m, glm::vec3(1.07f));
                    m = glm::translate(m, glm::vec3(0.0f, 0.13f, -0.34f));
                    m = glm::rotate(m, 3.1415927f, glm::vec3(1.0f, 0.0f, 0.0f));
                }
                AppendItem(m, mainHand, Ctx::Ground, ItemUseState{}, emit);
                return;
            }
            // CrossedArmsItemLayer (VillagerRenderer, WanderingTraderRenderer):
            // translateToArms (root, arms), X 0.75 rad, scale 1.07,
            // translate (0, 0.13, -0.34), X π; the GROUND display.
            case EntityTypeId::Villager:
            case EntityTypeId::WanderingTrader: {
                if (mainHand.IsEmpty()) return;
                glm::mat4 arms(1.0f);
                std::function<bool(const ModelPart&, const glm::mat4&)> find =
                    [&](const ModelPart& part, const glm::mat4& parent) -> bool {
                        const glm::mat4 here = parent * part.LocalMatrix();
                        if (part.name == "arms") { arms = here; return true; }
                        for (const auto& child : part.children) {
                            if (find(*child, here)) return true;
                        }
                        return false;
                    };
                if (!find(model.Root(), glm::mat4(1.0f))) return;
                glm::mat4 m = glm::scale(entityMatrix * arms, glm::vec3(16.0f));
                m = glm::rotate(m, 0.75f, glm::vec3(1.0f, 0.0f, 0.0f));
                m = glm::scale(m, glm::vec3(1.07f));
                m = glm::translate(m, glm::vec3(0.0f, 0.13f, -0.34f));
                m = glm::rotate(m, 3.1415927f, glm::vec3(1.0f, 0.0f, 0.0f));
                AppendItem(m, mainHand, Ctx::Ground, ItemUseState{}, emit);
                return;
            }
            // FoxHeldItemLayer: the mouth — the head part's offset, the
            // baby's 0.75, the head roll, the look, the sleeping / baby
            // seats, X 90 (and Z 90 asleep); GROUND display.
            case EntityTypeId::Fox: {
                if (mainHand.IsEmpty()) return;
                const ModelPart* head = model.Root().Find("head");
                if (!head) return;
                const bool sleeping = state.isSleeping;
                glm::mat4 m = glm::translate(blocks, glm::vec3(head->x, head->y, head->z) / 16.0f);
                if (state.isBaby) m = glm::scale(m, glm::vec3(0.75f));
                m = glm::rotate(m, state.headRollAngle, glm::vec3(0.0f, 0.0f, 1.0f));
                m = glm::rotate(m, glm::radians(state.yRot), glm::vec3(0.0f, 1.0f, 0.0f));
                m = glm::rotate(m, glm::radians(state.xRot), glm::vec3(1.0f, 0.0f, 0.0f));
                if (state.isBaby) {
                    m = glm::translate(m, sleeping ? glm::vec3(0.4f, 0.26f, 0.15f) : glm::vec3(0.06f, 0.26f, -0.5f));
                } else {
                    m = glm::translate(m, sleeping ? glm::vec3(0.46f, 0.26f, 0.22f) : glm::vec3(0.06f, 0.27f, -0.5f));
                }
                m = glm::rotate(m, glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
                if (sleeping) m = glm::rotate(m, glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
                AppendItem(m, mainHand, Ctx::Ground, ItemUseState{}, emit);
                return;
            }
            // DolphinCarryingItemLayer: ahead of the snout, raised or lowered
            // with the pitch; GROUND display.
            case EntityTypeId::Dolphin: {
                if (mainHand.IsEmpty()) return;
                const float angleXPercent = std::abs(state.xRot) / 60.0f;
                glm::mat4 m = blocks;
                if (state.xRot < 0.0f) {
                    m = glm::translate(m, glm::vec3(0.0f, 1.0f - angleXPercent * 0.5f, -1.0f + angleXPercent * 0.5f));
                } else {
                    m = glm::translate(m, glm::vec3(0.0f, 1.0f + angleXPercent * 0.8f, -1.0f + angleXPercent * 0.2f));
                }
                AppendItem(m, mainHand, Ctx::Ground, ItemUseState{}, emit);
                return;
            }
            // PandaHoldsItemLayer: only a sitting, unscared panda; the
            // eating bob; GROUND display.
            case EntityTypeId::Panda: {
                if (mainHand.IsEmpty() || !state.isSitting || state.isScared) return;
                float z = -0.6f;
                float y = 1.4f;
                if (state.isEating) {
                    z -= 0.2f * std::sin(state.ageInTicks * 0.6f) + 0.2f;
                    y -= 0.09f * std::sin(state.ageInTicks * 0.6f);
                }
                AppendItem(glm::translate(blocks, glm::vec3(0.1f, y, z)), mainHand, Ctx::Ground,
                           ItemUseState{}, emit);
                return;
            }
            default:
                break;
        }

        if (HasItemInHandLayer(type)) {
            // ItemInHandLayer.useBabyOffset — the 26.x baby body's grip. The
            // classic look's baby arm carries the baby transform's scale in
            // its pose, which takes the item with it, as pre-26.1 MC did.
            const bool babyGrip = state.isBaby && GetBabyModelLook() == BabyModelLook::New;
            ItemUseState rightUse, leftUse;
            ArmUseStates(state, rightUse, leftUse);
            AppendHeldItems(model, type, entityMatrix, rightItem, leftItem,
                            babyGrip, rightUse, leftUse, emit);
            return;
        }

        // The mod mobs' populateDefaultEquipmentSlots items with no synced
        // stack (ModMobRender.hpp HeldItem) — a handheld tool sprite in the
        // right hand.
        if (mainHand.IsEmpty()) {
            if (const char* sprite = ModMobs::HeldItem(mob, type, state)) {
                static constexpr DisplaySpec kHandheldSpec{ {0.0f, 4.0f, 0.5f}, {0.0f, -90.0f, 55.0f}, 0.85f };
                const size_t first = m_indices.size();
                const TextureHandle tex = AppendHeldSprite(model, entityMatrix, sprite, kHandheldSpec,
                                                           m_verts, m_indices);
                emit(tex, first, false);
            }
        }
    }

    // ── /morph ─────────────────────────────────────────────────────────────

    void MobRenderer::AppendMorphEquipment(const MorphPose& pose, Game::EntityTypeId type, bool playerModel,
                                           EntityModel& model, const EntityRenderState& state,
                                           const glm::mat4& entityMatrix, const glm::dvec3& renderPos,
                                           bool babyMesh, const Game::ItemStack& mainHand,
                                           const glm::vec3& cameraPos, const EmitFn& emit) {
        // A player is right-handed (Avatar.getMainArm): the main hand in the
        // right arm, the offhand in the left; the use clock goes with the
        // hand in use (AvatarRenderState.ticksUsingItem(arm)).
        if (playerModel || HasItemInHandLayer(type)) {
            ItemUseState rightUse, leftUse;
            ArmUseStates(state, rightUse, leftUse);
            ItemUseState& inUse = pose.useItemHand == 1 ? leftUse : rightUse;
            inUse.usingItem = pose.usingItem;
            inUse.useTicks  = pose.usingItem ? pose.ticksUsingItem : 0.0f;
            AppendHeldItems(model, type, entityMatrix, mainHand,
                            pose.equipment[static_cast<int>(EquipmentSlot::OFFHAND)],
                            /*babyGrip=*/state.isBaby && !playerModel &&
                                GetBabyModelLook() == BabyModelLook::New,
                            rightUse, leftUse, emit);
        }
        // The worn pieces only where the mob's renderer wears them — never on
        // the Herobrine body.
        if (playerModel) return;
        const ArmorFamily family = ArmorFamilyFor(type);
        if (family == ArmorFamily::None) return;
        AppendHumanoidArmor(family, babyMesh, pose.equipment, model, state, renderPos, pose.bodyYaw,
                            cameraPos, emit);
        AppendCustomHead(model, entityMatrix, pose.equipment[static_cast<int>(EquipmentSlot::HEAD)],
                         HeadTransformsFor(type), state.walkAnimationPos, emit);
    }

} // namespace Render
