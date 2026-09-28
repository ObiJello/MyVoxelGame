// File: src/client/renderer/entity/MountEquipmentLayers.cpp
//
// MobRenderer's mount equipment layers — see the "Mount equipment layers"
// block in MobRenderer.hpp. MC 26.3 sources (client/…):
//   renderer/entity/layers/SimpleEquipmentLayer, LlamaDecorLayer, RopesLayer,
//   EquipmentLayerRenderer; the renderers that register them —
//   HorseRenderer, DonkeyRenderer (donkey, mule), UndeadHorseRenderer
//   (skeleton / zombie horse), LlamaRenderer (llama, trader llama),
//   PigRenderer, StriderRenderer, CamelRenderer, CamelHuskRenderer,
//   HappyGhastRenderer, NautilusRenderer, ZombieNautilusRenderer; the meshes
//   in model/geom/LayerDefinitions (HORSE_ARMOR, UNDEAD_HORSE_ARMOR,
//   *_SADDLE, LLAMA_DECOR, LLAMA_BABY_DECOR, HAPPY_GHAST_HARNESS,
//   HAPPY_GHAST_ROPES, NAUTILUS_ARMOR, NAUTILUS_SADDLE) and their model
//   classes (EquineSaddleModel, DonkeyModel's DONKEY_TRANSFORMER,
//   CamelSaddleModel, HappyGhastHarnessModel, HappyGhastModel,
//   NautilusArmorModel, NautilusSaddleModel); the asset side is
//   client/data/models/EquipmentAssetProvider (which layer types each
//   equipment asset carries, and which of those are dyeable).
//
// Every layer mesh here is its mob's body mesh plus the layer's own parts,
// exactly as MC builds them (createBodyMesh + addOrReplaceChild), so the
// body's own setupAnim program poses it from the same render state — the
// saddle rides the head, legs and tail the body is drawn with. What only
// the layer model adds (the reins, the goggles) is posed by its override.
#include "client/renderer/entity/MobRenderer.hpp"

#include "client/entity/ClientMobManager.hpp"
#include "client/renderer/entity/model/GeneratedEntityModels.hpp"
#include "client/renderer/entity/model/GeneratedSetupAnim.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/EntityType.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/MountInventory.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/world/tags/DataTags.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace Render {

    // Which layer mesh a MountModel entry is (the cache key's high half).
    enum class MobRenderer::MountMesh : uint8_t {
        HorseArmor,            // HORSE_ARMOR / UNDEAD_HORSE_ARMOR
        EquineSaddle,          // HORSE / DONKEY / MULE / SKELETON / ZOMBIE_HORSE_SADDLE
        PigSaddle,             // PIG_SADDLE
        StriderSaddle,         // STRIDER_SADDLE
        CamelSaddle,           // CAMEL_SADDLE / CAMEL_HUSK_SADDLE
        LlamaDecor,            // LLAMA_DECOR
        LlamaBabyDecor,        // LLAMA_BABY_DECOR
        HappyGhastHarness,     // HAPPY_GHAST_HARNESS
        HappyGhastBabyHarness, // HAPPY_GHAST_BABY_HARNESS
        HappyGhastRopes,       // HAPPY_GHAST_ROPES
        HappyGhastBabyRopes,   // HAPPY_GHAST_BABY_ROPES
        NautilusArmor,         // NAUTILUS_ARMOR
        NautilusSaddle,        // NAUTILUS_SADDLE
    };

    namespace {

        using Game::EntityTypeId;
        using Game::EquipmentSlot;

        // CubeListBuilder.texOffs(u, v).addBox(x, y, z, w, h, d, g) with
        // mirror() when `mirror`.
        void AddBox(ModelPart& part, float u, float v, float x, float y, float z,
                    float w, float h, float d, float grow = 0.0f, bool mirror = false) {
            CubeDefinition c{};
            c.originX = x; c.originY = y; c.originZ = z;
            c.sizeX = w;   c.sizeY = h;   c.sizeZ = d;
            c.texOffsX = u; c.texOffsY = v;
            c.growX = grow; c.growY = grow; c.growZ = grow;
            c.mirror = mirror;
            part.cubes.push_back(c);
        }

        // CubeDeformation added to every cube of `part` alone (not its
        // children).
        void InflatePart(ModelPart* part, float by) {
            if (!part) return;
            for (CubeDefinition& c : part->cubes) {
                c.growX += by; c.growY += by; c.growZ += by;
            }
        }

        // CubeDeformation added to every cube of the tree — a mesh built with
        // the same `g` on every box (LlamaModel, PigModel, HappyGhastModel).
        void InflateTree(ModelPart& part, float by) {
            InflatePart(&part, by);
            for (auto& child : part.children) InflateTree(*child, by);
        }

        // MC MeshTransformer.scaling(factor) on the root pose:
        // pose.scaled(factor).translated(0, 24.016 * (1 - factor), 0).
        PartPose ScaledMesh(PartPose pose, float factor) {
            if (factor != 1.0f) {
                pose.x *= factor; pose.y *= factor; pose.z *= factor;
                pose.xScale *= factor; pose.yScale *= factor; pose.zScale *= factor;
            }
            pose.y += 24.016f * (1.0f - factor);
            return pose;
        }

        std::string_view TypeSlug(EntityTypeId type) {
            return Game::GetEntityTypeInfo(type).slug;
        }

        // A generated mesh with its own compiled setupAnim when there is
        // one, else the program of `animSlug` (MobRenderer.cpp MakeRemodel:
        // the 26.x baby meshes keep the adult's part names).
        std::unique_ptr<GeneratedModel> MakeGenerated(std::string_view slug, std::string_view animSlug) {
            if (FindAnimProgram(slug)) return std::make_unique<GeneratedModel>(slug);
            return std::make_unique<GeneratedModel>(slug, animSlug);
        }

        // ── MC EquineSaddleModel.createSaddleLayer ─────────────────────────
        //
        // AbstractEquineModel.createBodyMesh(NONE) plus the saddle and the
        // bridle; the reins (left/right_saddle_line) show only while ridden.
        // Built over the body's own generated row, so DonkeyModel's
        // DONKEY_TRANSFORMER (the long ears, the chests) and the horse /
        // donkey / mule MeshTransformer.scaling come with it, exactly as
        // DonkeyModel.createSaddleLayer / HORSE_SADDLE apply them.
        class EquineSaddleModel final : public GeneratedModel {
        public:
            explicit EquineSaddleModel(std::string_view bodySlug) : GeneratedModel(bodySlug) {
                ModelPart* body = m_root.Find("body");
                ModelPart* headParts = m_root.Find("head_parts");
                if (body) {
                    ModelPart* saddle = body->AddChild("saddle", PartPose::Zero());
                    AddBox(*saddle, 26.0f, 0.0f, -5.0f, -8.0f, -9.0f, 10.0f, 9.0f, 9.0f, 0.5f);
                }
                if (headParts) {
                    AddBox(*headParts->AddChild("left_saddle_mouth", PartPose::Zero()),
                           29.0f, 5.0f, 2.0f, -9.0f, -6.0f, 1.0f, 2.0f, 2.0f);
                    AddBox(*headParts->AddChild("right_saddle_mouth", PartPose::Zero()),
                           29.0f, 5.0f, -3.0f, -9.0f, -6.0f, 1.0f, 2.0f, 2.0f);
                    const PartPose line = PartPose::OffsetAndRotation(0.0f, 0.0f, 0.0f, -0.5235988f, 0.0f, 0.0f);
                    m_leftLine = headParts->AddChild("left_saddle_line", line);
                    AddBox(*m_leftLine, 32.0f, 2.0f, 3.1f, -6.0f, -8.0f, 0.0f, 3.0f, 16.0f);
                    m_rightLine = headParts->AddChild("right_saddle_line", line);
                    AddBox(*m_rightLine, 32.0f, 2.0f, -3.1f, -6.0f, -8.0f, 0.0f, 3.0f, 16.0f);
                    AddBox(*headParts->AddChild("head_saddle", PartPose::Zero()),
                           1.0f, 1.0f, -3.0f, -11.0f, -1.9f, 6.0f, 5.0f, 6.0f, 0.22f);
                    AddBox(*headParts->AddChild("mouth_saddle_wrap", PartPose::Zero()),
                           19.0f, 0.0f, -2.0f, -11.0f, -4.0f, 4.0f, 5.0f, 2.0f, 0.2f);
                }
                m_root.ResetPose();
            }

            void SetupAnim(const EntityRenderState& state) override {
                GeneratedModel::SetupAnim(state);
                // EquineSaddleModel.setupAnim: ridingParts.visible = isRidden.
                if (m_leftLine)  m_leftLine->visible = state.isRidden;
                if (m_rightLine) m_rightLine->visible = state.isRidden;
            }

        private:
            ModelPart* m_leftLine = nullptr;
            ModelPart* m_rightLine = nullptr;
        };

        // ── MC CamelSaddleModel.createSaddleLayer ──────────────────────────
        //
        // AdultCamelModel's mesh plus the saddle (on the body), the bridle
        // and the reins (on the head); the reins show only while ridden.
        class CamelSaddleModel final : public GeneratedModel {
        public:
            explicit CamelSaddleModel(std::string_view bodySlug) : GeneratedModel(bodySlug) {
                constexpr float kInflate = 0.05f;
                if (ModelPart* body = m_root.Find("body")) {
                    ModelPart* saddle = body->AddChild("saddle", PartPose::Zero());
                    AddBox(*saddle, 74.0f, 64.0f, -4.5f, -17.0f, -15.5f, 9.0f, 5.0f, 11.0f, kInflate);
                    AddBox(*saddle, 92.0f, 114.0f, -3.5f, -20.0f, -15.5f, 7.0f, 3.0f, 11.0f, kInflate);
                    AddBox(*saddle, 0.0f, 89.0f, -7.5f, -12.0f, -23.5f, 15.0f, 12.0f, 27.0f, kInflate);
                }
                if (ModelPart* head = m_root.Find("head")) {
                    m_reins = head->AddChild("reins", PartPose::Zero());
                    AddBox(*m_reins, 98.0f, 42.0f, 3.51f, -18.0f, -17.0f, 0.0f, 7.0f, 15.0f);
                    AddBox(*m_reins, 84.0f, 57.0f, -3.5f, -18.0f, -2.0f, 7.0f, 7.0f, 0.0f);
                    AddBox(*m_reins, 98.0f, 42.0f, -3.51f, -18.0f, -17.0f, 0.0f, 7.0f, 15.0f);
                    ModelPart* bridle = head->AddChild("bridle", PartPose::Zero());
                    AddBox(*bridle, 60.0f, 87.0f, -3.5f, -7.0f, -15.0f, 7.0f, 8.0f, 19.0f, kInflate);
                    AddBox(*bridle, 21.0f, 64.0f, -3.5f, -21.0f, -15.0f, 7.0f, 14.0f, 7.0f, kInflate);
                    AddBox(*bridle, 50.0f, 64.0f, -2.5f, -21.0f, -21.0f, 5.0f, 5.0f, 6.0f, kInflate);
                    AddBox(*bridle, 74.0f, 70.0f, 2.5f, -19.0f, -18.0f, 1.0f, 2.0f, 2.0f);
                    AddBox(*bridle, 74.0f, 70.0f, -3.5f, -19.0f, -18.0f, 1.0f, 2.0f, 2.0f, 0.0f, /*mirror=*/true);
                }
                m_root.ResetPose();
            }

            void SetupAnim(const EntityRenderState& state) override {
                GeneratedModel::SetupAnim(state);
                if (m_reins) m_reins->visible = state.isRidden;   // CamelSaddleModel.setupAnim
            }

        private:
            ModelPart* m_reins = nullptr;
        };

        // ── MC HappyGhastHarnessModel ──────────────────────────────────────
        //
        // Its own mesh, not the ghast's: the 16³ harness box and the goggles
        // bar, the whole mesh scaled 4 (MeshTransformer.scaling(4)). The
        // baby row is createHarnessLayer(true) — which already applies
        // HappyGhastModel.BABY_TRANSFORMER — with LayerDefinitions applying
        // BABY_TRANSFORMER a second time on top (HAPPY_GHAST_BABY_HARNESS);
        // both are kept. setupAnim: the goggles sit down over the eyes
        // (y 14, level) while ridden and ride up on the forehead (y 9,
        // tilted -45°) otherwise.
        class HappyGhastHarnessModel final : public EntityModel {
        public:
            explicit HappyGhastHarnessModel(bool baby) {
                m_texWidth = 64.0f;
                m_texHeight = 64.0f;
                PartPose rootPose = ScaledMesh(PartPose::Zero(), 4.0f);
                if (baby) {
                    constexpr float kBabyScale = 0.2375f;   // HappyGhastModel.BABY_TRANSFORMER
                    rootPose = ScaledMesh(ScaledMesh(rootPose, kBabyScale), kBabyScale);
                }
                m_root.name = "root";
                m_root.pose = rootPose;
                ModelPart* harness = m_root.AddChild("harness", PartPose::Offset(0.0f, 24.0f, 0.0f));
                AddBox(*harness, 0.0f, 0.0f, -8.0f, -16.0f, -8.0f, 16.0f, 16.0f, 16.0f);
                m_goggles = m_root.AddChild("goggles", PartPose::Offset(0.0f, 14.0f, -5.5f));
                AddBox(*m_goggles, 0.0f, 32.0f, -8.0f, -2.5f, -2.5f, 16.0f, 5.0f, 5.0f, 0.15f);
                m_root.ResetPose();
            }

            void SetupAnim(const EntityRenderState& state) override {
                m_root.ResetPose();
                if (state.isRidden) {
                    m_goggles->xRot = 0.0f;
                    m_goggles->y = 14.0f;   // GOGGLES_Y_OFFSET
                } else {
                    m_goggles->xRot = -0.7854f;
                    m_goggles->y = 9.0f;
                }
            }

        private:
            ModelPart* m_goggles = nullptr;
        };

        // ── MC HappyGhastModel over the generated body mesh ────────────────
        //
        // The compiled setupAnim carries the tentacles; the body squeeze —
        // `if (!state.bodyItem.isEmpty()) body.scale = 0.9375` (BODY_SQUEEZE,
        // so the body sits inside a worn harness) — reads an ItemStack the
        // generator folds to "empty", so it is posed here from the render
        // state's body-item flag. Every happy ghast mesh goes through this:
        // the body (adult and baby) and the RopesLayer meshes, which MC also
        // builds as HappyGhastModel.
        class HappyGhastBodyModel final : public GeneratedModel {
        public:
            HappyGhastBodyModel(std::string_view slug, std::string_view animSlug)
                : GeneratedModel(slug, animSlug), m_body(m_root.Find("body")) {}

            void SetupAnim(const EntityRenderState& state) override {
                GeneratedModel::SetupAnim(state);
                if (state.hasBodyItem && m_body) {
                    m_body->xScale = 0.9375f;
                    m_body->yScale = 0.9375f;
                    m_body->zScale = 0.9375f;
                }
            }

        private:
            ModelPart* m_body = nullptr;
        };

        // ── NautilusArmorModel / NautilusSaddleModel.createSaddleLayer ─────
        //
        // NautilusModel.createBodyMesh with `root` re-added (its children
        // kept — PartDefinition.addOrReplaceChild) and `shell` re-added with
        // the layer's cubes: the body and mouths stay as the plain mesh.
        void ReplaceShell(EntityModel& model, bool saddle) {
            ModelPart* shell = model.Root().Find("shell");
            if (!shell) return;
            shell->cubes.clear();
            if (saddle) {
                AddBox(*shell, 0.0f, 0.0f, -7.0f, -10.0f, -7.0f, 14.0f, 10.0f, 16.0f, 0.2f);
            } else {
                AddBox(*shell, 0.0f, 0.0f, -7.0f, -10.0f, -7.0f, 14.0f, 10.0f, 16.0f, 0.01f);
                AddBox(*shell, 0.0f, 26.0f, -7.0f, 0.0f, -7.0f, 14.0f, 8.0f, 20.0f, 0.01f);
                AddBox(*shell, 48.0f, 26.0f, -7.0f, 0.0f, 6.0f, 14.0f, 8.0f, 0.0f, 0.0f);
            }
        }

        // ── Equipment assets (client/data/models/EquipmentAssetProvider) ───

        // One EquipmentClientInfo.Layer: its texture id and dyeable part.
        struct AssetLayer {
            std::string texture;
            bool     dyeable = false;          // Layer.dyeable present
            uint32_t colorWhenUndyed = 0;      // Dyeable.colorWhenUndyed, ARGB (0 = none)
        };
        // At most two layers per layer type in the vanilla assets.
        struct AssetLayers {
            std::array<AssetLayer, 2> layers;
            int count = 0;
            void Add(std::string texture, bool dyeable = false, uint32_t undyed = 0) {
                if (count < static_cast<int>(layers.size())) {
                    layers[static_cast<size_t>(count++)] = AssetLayer{ std::move(texture), dyeable, undyed };
                }
            }
        };

        bool EndsWith(std::string_view s, std::string_view suffix) {
            return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        // MC DyeColor serialized names — a carpet / harness asset's colour.
        bool IsDyeColorName(std::string_view name) {
            static constexpr std::string_view kDyeNames[16] = {
                "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black" };
            for (std::string_view dye : kDyeNames) {
                if (dye == name) return true;
            }
            return false;
        }

        // EquipmentAssetProvider.bootstrap: the layers `asset` defines for
        // `layerType` (empty when it defines none — a saddle on a llama's
        // body, horse armour on a nautilus is fine, a carpet on a horse
        // draws nothing).
        //   leather                    horse_body: leather (dyeable, undyed
        //                              #A06540), leather_overlay
        //   copper/iron/gold/diamond/  horse_body, nautilus_body: the asset
        //   netherite                  (leatherDyeable(name, false) — plain)
        //   saddle                     every *_saddle layer type: saddle
        //   <colour>_harness           happy_ghast_body: <colour>_harness
        //                              (onlyIfDyed(…, false) — plain)
        //   <colour>_carpet            llama_body: <colour>
        //   trader_llama(_baby)        llama_body: the asset
        AssetLayers EquipmentLayersFor(std::string_view asset, std::string_view layerType) {
            AssetLayers out;
            if (layerType == "horse_body") {
                if (asset == "leather") {
                    out.Add("leather", /*dyeable=*/true, 0xFFA06540u);   // ARGB.opaque(-6265536)
                    out.Add("leather_overlay");
                } else if (asset == "copper" || asset == "iron" || asset == "gold" ||
                           asset == "diamond" || asset == "netherite") {
                    out.Add(std::string(asset));
                }
            } else if (layerType == "nautilus_body") {
                if (asset == "copper" || asset == "iron" || asset == "gold" ||
                    asset == "diamond" || asset == "netherite") {
                    out.Add(std::string(asset));
                }
            } else if (layerType == "llama_body") {
                if (asset == "trader_llama" || asset == "trader_llama_baby") {
                    out.Add(std::string(asset));
                } else if (EndsWith(asset, "_carpet") &&
                           IsDyeColorName(asset.substr(0, asset.size() - 7))) {
                    out.Add(std::string(asset.substr(0, asset.size() - 7)));
                }
            } else if (layerType == "happy_ghast_body") {
                if (EndsWith(asset, "_harness") &&
                    IsDyeColorName(asset.substr(0, asset.size() - 8))) {
                    out.Add(std::string(asset));
                }
            } else if (EndsWith(layerType, "_saddle")) {
                if (asset == "saddle") out.Add("saddle");
            }
            return out;
        }

        // The equipment asset an item's EQUIPPABLE names (Items.java: the
        // saddle, the carpets, the harnesses, horseArmor / nautilusArmor /
        // humanoidArmor(ArmorMaterials.X) → the material's asset, the wolf
        // armour's armadillo_scute, the elytra), from its item id. Empty
        // when the item carries no asset.
        std::string EquipmentAssetFromItemId(Game::ItemID id) {
            const std::string slug(Game::ItemRegistry::Slug(id));
            if (slug.empty()) return {};
            if (slug == "saddle") return "saddle";
            if (EndsWith(slug, "_harness") &&
                IsDyeColorName(std::string_view(slug).substr(0, slug.size() - 8))) {
                return slug;
            }
            if (EndsWith(slug, "_carpet") &&
                IsDyeColorName(std::string_view(slug).substr(0, slug.size() - 7))) {
                return slug;
            }
            const auto material = [](std::string_view prefix) -> std::string {
                if (prefix == "golden") return "gold";
                return std::string(prefix);
            };
            if (EndsWith(slug, "_horse_armor")) {
                return material(std::string_view(slug).substr(0, slug.size() - 12));
            }
            if (EndsWith(slug, "_nautilus_armor")) {
                return material(std::string_view(slug).substr(0, slug.size() - 15));
            }
            if (id == Game::Items::WolfArmor) return "armadillo_scute";
            if (id == Game::Items::Elytra) return "elytra";
            // The humanoid sets (and the mods' — the table MobRenderer keeps).
            if (const char* armor = MobRenderer::EquipmentAssetFor(id)) return armor;
            return {};
        }

        // SimpleEquipmentLayer / LlamaDecorLayer's gate and asset: the
        // stack's EQUIPPABLE component and its assetId ("minecraft:" is the
        // only namespace the equipment sheets live under). A component
        // registered without an asset id falls back to the item's own
        // (EquipmentAssetFromItemId — the id → asset table Items.java's
        // Equippable builders define), which is empty for an item that has
        // none, so a pumpkin in the body slot still draws nothing.
        std::string MountEquipmentAsset(const Game::ItemStack& stack) {
            if (stack.IsEmpty()) return {};
            const auto equippable = stack.get(Game::DataComponents::EQUIPPABLE);
            if (!equippable) return {};
            std::string_view asset = equippable->assetId;
            if (asset.empty()) return EquipmentAssetFromItemId(stack.itemId);
            constexpr std::string_view kNamespace = "minecraft:";
            if (asset.substr(0, kNamespace.size()) == kNamespace) asset.remove_prefix(kNamespace.size());
            return std::string(asset);
        }

        // MC EquipmentLayerRenderer.getColorForLayer: -1 (untinted) for a
        // layer with no dyeable part; the dye, else colorWhenUndyed, else 0
        // (the layer is skipped) for a dyeable one.
        uint32_t ColorForLayer(const AssetLayer& layer, uint32_t dyeColor) {
            if (!layer.dyeable) return 0xFFFFFFFFu;
            return dyeColor != 0 ? dyeColor : layer.colorWhenUndyed;
        }

        // The client's MC Entity.isVehicle(): a mob passenger (the pointer
        // link), or anyone in the synched seat order (players are known to
        // a client only by SetPassengersS2C).
        bool IsVehicleOnClient(const Game::Mob& mob) {
            return mob.IsVehicle() || !mob.SyncedRiders().empty();
        }

        // MC AbstractChestedHorse.hasChest (DATA_ID_CHEST): the client
        // copy's synced chest flag on its mount inventory.
        bool CarriesChest(const Game::Mob& mob) {
            const Game::MountInventory* inventory = mob.GetMountInventory();
            return inventory && inventory->CanCarryChest() && inventory->HasChest();
        }

    } // namespace

    std::unique_ptr<EntityModel> CreateHappyGhastModel(std::string_view slug) {
        if (!FindGenModel(slug)) return nullptr;
        return std::make_unique<HappyGhastBodyModel>(
            slug, FindAnimProgram(slug) ? std::string_view{} : std::string_view("happy_ghast"));
    }

    // ── Render state (the mount renderers' extractRenderState) ─────────────

    void MobRenderer::FillMountRenderState(const Game::Mob& mob, Game::EntityTypeId type,
                                           EntityRenderState& state) {
        switch (type) {
            // AbstractHorseRenderer / StriderRenderer / CamelRenderer:
            // state.isRidden = entity.isVehicle() — the reins, the strider's
            // bristles.
            case EntityTypeId::Horse:
            case EntityTypeId::SkeletonHorse: case EntityTypeId::ZombieHorse:
            case EntityTypeId::Strider:
            case EntityTypeId::Camel: case EntityTypeId::CamelHusk:
                state.isRidden = IsVehicleOnClient(mob);
                break;
            // DonkeyRenderer: state.hasChest = entity.hasChest() — the
            // chests on the donkey / mule mesh (DonkeyModel.setupAnim).
            case EntityTypeId::Donkey: case EntityTypeId::Mule:
                state.isRidden = IsVehicleOnClient(mob);
                state.hasChest = CarriesChest(mob);
                break;
            // LlamaRenderer: state.hasChest = !isBaby() && hasChest().
            case EntityTypeId::Llama: case EntityTypeId::TraderLlama:
                state.hasChest = !mob.IsBaby() && CarriesChest(mob);
                break;
            // HappyGhastRenderer: isRidden (the goggles) and the body item
            // (HappyGhastModel's squeeze).
            case EntityTypeId::HappyGhast:
                if (const auto* ghast = dynamic_cast<const Game::HappyGhast*>(&mob)) {
                    state.isRidden = ghast->IsRidden();
                } else {
                    state.isRidden = IsVehicleOnClient(mob);
                }
                state.hasBodyItem = !mob.GetEquipment(EquipmentSlot::BODY).IsEmpty();
                break;
            default:
                break;
        }
    }

    // ── Mesh cache ─────────────────────────────────────────────────────────

    EntityModel* MobRenderer::MountModel(MountMesh mesh, Game::EntityTypeId type) {
        // The baby-look meshes (the llama cria's decor, the baby ghast's
        // ropes) follow World Settings → Baby Models like every other mesh.
        if (m_mountModelsLookGeneration != BabyModelLookGeneration()) {
            m_mountModelsLookGeneration = BabyModelLookGeneration();
            m_mountModels.clear();
        }
        const uint32_t key = (static_cast<uint32_t>(mesh) << 16) | static_cast<uint32_t>(type);
        if (const auto it = m_mountModels.find(key); it != m_mountModels.end()) return it->second.get();

        const std::string slug(TypeSlug(type));
        const bool newBabies = GetBabyModelLook() == BabyModelLook::New;
        std::unique_ptr<EntityModel> model;
        switch (mesh) {
            case MountMesh::HorseArmor:
                // LayerDefinitions HORSE_ARMOR (with the horse's 1.1 scaling)
                // / UNDEAD_HORSE_ARMOR: createBodyMesh(CubeDeformation(0.1)) —
                // `g` reaches the head, mane, mouth, legs and tail; the body
                // box keeps its own 0.05, the neck none, the ears -0.001.
                if (FindGenModel(slug)) {
                    auto armor = std::make_unique<GeneratedModel>(slug);
                    for (const char* part : { "head", "mane", "upper_mouth", "left_hind_leg",
                                              "right_hind_leg", "left_front_leg",
                                              "right_front_leg", "tail" }) {
                        InflatePart(armor->Root().Find(part), 0.1f);
                    }
                    model = std::move(armor);
                }
                break;
            case MountMesh::EquineSaddle:
                if (FindGenModel(slug)) model = std::make_unique<EquineSaddleModel>(slug);
                break;
            case MountMesh::PigSaddle: {
                // PIG_SADDLE: PigModel.createBodyLayer(CubeDeformation(0.5))
                // — the NORMAL pig mesh whatever the variant.
                auto saddle = std::make_unique<PigModel>(false);
                InflateTree(saddle->Root(), 0.5f);
                model = std::move(saddle);
                break;
            }
            case MountMesh::StriderSaddle:
                // STRIDER_SADDLE is the strider's own body layer.
                if (FindGenModel(slug)) model = std::make_unique<GeneratedModel>(slug);
                break;
            case MountMesh::CamelSaddle:
                if (FindGenModel(slug)) model = std::make_unique<CamelSaddleModel>(slug);
                break;
            case MountMesh::LlamaDecor:
                // LLAMA_DECOR: LlamaModel.createBodyLayer(CubeDeformation(0.5)).
                if (FindGenModel(slug)) {
                    auto decor = std::make_unique<GeneratedModel>(slug);
                    InflateTree(decor->Root(), 0.5f);
                    model = std::move(decor);
                }
                break;
            case MountMesh::LlamaBabyDecor: {
                // LLAMA_BABY_DECOR: BabyLlamaModel at CubeDeformation(0.2)
                // under the 26.x look; the classic baby transform of the
                // adult decor (0.5) otherwise — the mesh the body is drawn
                // with either way.
                const std::string remodel = slug + "_baby_new";
                const std::string classic = slug + "_baby";
                if (newBabies && FindGenModel(remodel)) {
                    auto decor = MakeGenerated(remodel, slug);
                    InflateTree(decor->Root(), 0.2f);
                    model = std::move(decor);
                } else if (FindGenModel(classic)) {
                    auto decor = std::make_unique<GeneratedModel>(classic);
                    InflateTree(decor->Root(), 0.5f);
                    model = std::move(decor);
                }
                break;
            }
            case MountMesh::HappyGhastHarness:
                model = std::make_unique<HappyGhastHarnessModel>(false);
                break;
            case MountMesh::HappyGhastBabyHarness:
                model = std::make_unique<HappyGhastHarnessModel>(true);
                break;
            case MountMesh::HappyGhastRopes:
                // HAPPY_GHAST_ROPES: HappyGhastModel.createBodyLayer(false,
                // CubeDeformation(0.2)).
                if ((model = CreateHappyGhastModel(slug))) InflateTree(model->Root(), 0.2f);
                break;
            case MountMesh::HappyGhastBabyRopes: {
                // HAPPY_GHAST_BABY_ROPES: the baby body layer at 0.2 (the
                // inner body's extend(-0.5) rides on top), on the baby mesh
                // the body is drawn with.
                const std::string remodel = slug + "_baby_new";
                const std::string classic = slug + "_baby";
                if (newBabies && FindGenModel(remodel)) model = CreateHappyGhastModel(remodel);
                else model = CreateHappyGhastModel(classic);
                if (model) InflateTree(model->Root(), 0.2f);
                break;
            }
            case MountMesh::NautilusArmor:
            case MountMesh::NautilusSaddle:
                // NautilusRenderer and ZombieNautilusRenderer alike bake the
                // NAUTILUS_ARMOR / NAUTILUS_SADDLE rows — the plain nautilus
                // mesh, animated by NautilusModel.setupAnim.
                if (FindGenModel("nautilus")) {
                    auto layer = std::make_unique<GeneratedModel>("nautilus");
                    ReplaceShell(*layer, mesh == MountMesh::NautilusSaddle);
                    model = std::move(layer);
                }
                break;
        }
        EntityModel* raw = model.get();
        m_mountModels.emplace(key, std::move(model));   // null cached too: tried once
        return raw;
    }

    // ── The layers ─────────────────────────────────────────────────────────

    void MobRenderer::AppendMountEquipmentLayers(const Game::Mob& mob, Game::EntityTypeId type,
                                                 const EntityRenderState& state,
                                                 const glm::dvec3& renderPos, float bodyRot,
                                                 const glm::vec3& cameraPos, const EmitFn& emit) {
        // EquipmentLayerRenderer.renderLayers: each of the asset's layers
        // for `layerType`, in order, on `model` — armorCutoutNoCull with
        // NO_OVERLAY (the caller's emit carries none), a dyeable layer
        // tinted by the stack's DYED_COLOR (else its undyed colour, else
        // skipped). The enchantment glint pass is not drawn (no glint pass
        // for entity layers here — the wolf and humanoid armour alike).
        const auto renderLayers = [&](std::string_view layerType, std::string_view asset,
                                      const Game::ItemStack& stack, EntityModel* model) {
            if (!model || asset.empty()) return;
            const AssetLayers defs = EquipmentLayersFor(asset, layerType);
            if (defs.count == 0) return;
            // DyedItemColor.getOrDefault(stack, 0): the opaque dye.
            uint32_t dyeColor = 0;
            if (!stack.IsEmpty()) {
                if (const auto dyed = stack.get(Game::DataComponents::DYED_COLOR)) {
                    dyeColor = 0xFF000000u | (static_cast<uint32_t>(*dyed) & 0xFFFFFFu);
                }
            }
            for (int i = 0; i < defs.count; ++i) {
                const AssetLayer& layer = defs.layers[static_cast<size_t>(i)];
                const uint32_t color = ColorForLayer(layer, dyeColor);
                if (color == 0) continue;
                // Layer.getTextureLocation(layerType).
                const TextureHandle tex = LoadTexture("assets/textures/entity/equipment/" +
                                                      std::string(layerType) + "/" + layer.texture + ".png");
                if (tex == INVALID_TEXTURE) continue;
                const size_t first = m_indices.size();
                const size_t firstVert = m_verts.size();
                AppendMob(*model, state, renderPos, bodyRot, cameraPos, m_verts, m_indices);
                if (m_indices.size() <= first) continue;
                if (color != 0xFFFFFFFFu) {
                    // The layer colour is the model's vertex colour, which
                    // the entity shader multiplies with the texture — the
                    // baked face shade survives under the dye.
                    const uint32_t r = (color >> 16) & 0xFFu;
                    const uint32_t g = (color >> 8) & 0xFFu;
                    const uint32_t b = color & 0xFFu;
                    for (size_t v = firstVert; v < m_verts.size(); ++v) {
                        ModelVertex& vert = m_verts[v];
                        vert.r = static_cast<uint8_t>((vert.r * r) / 255u);
                        vert.g = static_cast<uint8_t>((vert.g * g) / 255u);
                        vert.b = static_cast<uint8_t>((vert.b * b) / 255u);
                    }
                }
                emit(tex, first, false);
            }
        };

        // SimpleEquipmentLayer.submit: the slot's EQUIPPABLE asset, on the
        // adult model — none of the mount renderers but the happy ghast's
        // registers a baby model, so a foal draws nothing.
        const auto simpleLayer = [&](EquipmentSlot slot, std::string_view layerType,
                                     MountMesh adultMesh, bool hasBabyMesh, MountMesh babyMesh) {
            if (state.isBaby && !hasBabyMesh) return;
            const Game::ItemStack& stack = mob.GetEquipment(slot);
            const std::string asset = MountEquipmentAsset(stack);
            if (asset.empty()) return;
            renderLayers(layerType, asset, stack,
                         MountModel(state.isBaby ? babyMesh : adultMesh, type));
        };
        const auto adultLayer = [&](EquipmentSlot slot, std::string_view layerType, MountMesh mesh) {
            simpleLayer(slot, layerType, mesh, false, mesh);
        };

        switch (type) {
            // HorseRenderer: HorseMarkingLayer, then the HORSE_BODY layer on
            // HORSE_ARMOR and the HORSE_SADDLE layer (both order 2).
            case EntityTypeId::Horse:
                adultLayer(EquipmentSlot::BODY, "horse_body", MountMesh::HorseArmor);
                adultLayer(EquipmentSlot::SADDLE, "horse_saddle", MountMesh::EquineSaddle);
                break;
            // UndeadHorseRenderer: HORSE_BODY on UNDEAD_HORSE_ARMOR, then the
            // type's saddle layer on the unscaled HORSE_SADDLE mesh.
            case EntityTypeId::SkeletonHorse:
                adultLayer(EquipmentSlot::BODY, "horse_body", MountMesh::HorseArmor);
                adultLayer(EquipmentSlot::SADDLE, "skeleton_horse_saddle", MountMesh::EquineSaddle);
                break;
            case EntityTypeId::ZombieHorse:
                adultLayer(EquipmentSlot::BODY, "horse_body", MountMesh::HorseArmor);
                adultLayer(EquipmentSlot::SADDLE, "zombie_horse_saddle", MountMesh::EquineSaddle);
                break;
            // DonkeyRenderer: the saddle alone (DONKEY_SADDLE / MULE_SADDLE).
            case EntityTypeId::Donkey:
                adultLayer(EquipmentSlot::SADDLE, "donkey_saddle", MountMesh::EquineSaddle);
                break;
            case EntityTypeId::Mule:
                adultLayer(EquipmentSlot::SADDLE, "mule_saddle", MountMesh::EquineSaddle);
                break;
            case EntityTypeId::Pig:
                adultLayer(EquipmentSlot::SADDLE, "pig_saddle", MountMesh::PigSaddle);
                break;
            case EntityTypeId::Strider:
                adultLayer(EquipmentSlot::SADDLE, "strider_saddle", MountMesh::StriderSaddle);
                break;
            case EntityTypeId::Camel:
                adultLayer(EquipmentSlot::SADDLE, "camel_saddle", MountMesh::CamelSaddle);
                break;
            case EntityTypeId::CamelHusk:
                adultLayer(EquipmentSlot::SADDLE, "camel_husk_saddle", MountMesh::CamelSaddle);
                break;
            // NautilusRenderer / ZombieNautilusRenderer: NAUTILUS_BODY on
            // NAUTILUS_ARMOR, then NAUTILUS_SADDLE.
            case EntityTypeId::Nautilus:
            case EntityTypeId::ZombieNautilus:
                adultLayer(EquipmentSlot::BODY, "nautilus_body", MountMesh::NautilusArmor);
                adultLayer(EquipmentSlot::SADDLE, "nautilus_saddle", MountMesh::NautilusSaddle);
                break;
            // LlamaRenderer's LlamaDecorLayer: an adult's carpet (its BODY
            // EQUIPPABLE asset), else a trader llama's own decor
            // (TRADER_LLAMA, TRADER_LLAMA_BABY for a cria) — on LLAMA_DECOR
            // or LLAMA_BABY_DECOR, no invisibility gate. The classic baby
            // look draws the classic baby mesh, which keeps the adult sheet's
            // layout, so its trader decor is the adult sheet.
            case EntityTypeId::Llama:
            case EntityTypeId::TraderLlama: {
                const Game::ItemStack& body = mob.GetEquipment(EquipmentSlot::BODY);
                const std::string asset = MountEquipmentAsset(body);
                EntityModel* decor = MountModel(state.isBaby ? MountMesh::LlamaBabyDecor
                                                             : MountMesh::LlamaDecor, type);
                if (!asset.empty() && !state.isBaby) {
                    renderLayers("llama_body", asset, body, decor);
                } else if (type == EntityTypeId::TraderLlama) {
                    const bool babySheet = state.isBaby && GetBabyModelLook() == BabyModelLook::New;
                    renderLayers("llama_body", babySheet ? "trader_llama_baby" : "trader_llama",
                                 Game::ItemStack{}, decor);
                }
                break;
            }
            // HappyGhastRenderer: the HAPPY_GHAST_BODY layer (the harness, a
            // baby mesh too), then RopesLayer — the ropes sheet on the body
            // mesh at 0.2 while the ghast holds a quad leash and wears a
            // #harnesses item (submitModel, NO_OVERLAY).
            case EntityTypeId::HappyGhast: {
                simpleLayer(EquipmentSlot::BODY, "happy_ghast_body", MountMesh::HappyGhastHarness,
                            true, MountMesh::HappyGhastBabyHarness);
                const auto* ghast = dynamic_cast<const Game::HappyGhast*>(&mob);
                const Game::ItemStack& body = mob.GetEquipment(EquipmentSlot::BODY);
                if (ghast && ghast->IsLeashHolder() && !body.IsEmpty() &&
                    Game::DataTags::HasTag(Game::DataTags::Registry::Item,
                                           Game::ItemRegistry::Slug(body.itemId), "minecraft:harnesses")) {
                    EntityModel* ropes = MountModel(state.isBaby ? MountMesh::HappyGhastBabyRopes
                                                                 : MountMesh::HappyGhastRopes, type);
                    const TextureHandle tex = LoadTexture("assets/textures/entity/ghast/happy_ghast_ropes.png");
                    if (ropes && tex != INVALID_TEXTURE) {
                        const size_t first = m_indices.size();
                        AppendMob(*ropes, state, renderPos, bodyRot, cameraPos, m_verts, m_indices);
                        emit(tex, first, ropes->CullBackFaces());
                    }
                }
                break;
            }
            default:
                break;
        }
    }

} // namespace Render
