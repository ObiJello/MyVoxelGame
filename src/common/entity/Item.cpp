// File: src/common/entity/Item.cpp
#include "Item.hpp"
#include "GeneratedItemList.hpp"
#include "ItemModelLoader.hpp"
#include "ClientItemLoader.hpp"
#include "../world/block/BlockRegistry.hpp"
#include "../data/DataComponents.hpp"
#include "../core/Log.hpp"
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <filesystem>
#include <cctype>
#include <cmath>
#include <cstdio>

// Forward decl: lives in PlatformMain.cpp, not surfaced via a public header.
// (ItemModelLoader.cpp uses the same forward-declared pattern.)
namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Game {

    namespace {
        // Block items: dense vector indexed by BlockID's numeric value (0..BlockID::Count-1).
        std::vector<Item> g_blockItems;
        // Pure items: sparse map keyed by ItemID.
        std::unordered_map<ItemID, Item> g_pureItems;
        bool g_initialized = false;

        // Per-frame render context. Set by the renderer once per frame; read by per-item
        // frame selectors. Default is "no player, target at origin, t=0".
        ItemRenderContext g_renderContext{};

        // Compass needle state — global for now (MC stores this per-ItemStack via
        // DataComponents, which we don't have yet). The needle has angular momentum so it
        // swings toward the target instead of snapping. Matches CompassAngleState.wobble().
        float g_compassRotation = 0.0f;        // current displayed angle, REVOLUTIONS (0..1)
        float g_compassVelocity = 0.0f;        // angular velocity per tick, in revolutions
        float g_compassTickAccum = 0.0f;       // accumulator for fixed 20 TPS ticks
        constexpr float COMPASS_TICK_HZ = 20.0f;
        constexpr float COMPASS_TICK_DT = 1.0f / COMPASS_TICK_HZ;
        // MC's CompassAngleState constants
        constexpr float COMPASS_INERTIA  = 0.8f;   // velocity damping per tick
        constexpr float COMPASS_STIFFNESS = 0.41f; // pull-to-target factor per tick

        // Wrap a value to [0, 1) (handles negatives correctly).
        inline float WrapToUnit(float x) {
            x = std::fmod(x, 1.0f);
            if (x < 0.0f) x += 1.0f;
            return x;
        }

        // Compute the target angle the compass *should* point at, in revolutions [0,1).
        // 0 = needle pointing "up" on the texture (player facing target).
        //
        // Our yaw convention (per Player.cpp's `front.x = cos(yaw), front.z =
        // sin(yaw)`): yaw=0 → facing +X. yaw=90 → facing +Z. atan2(z, x) returns
        // the angle of the (x,z) vector measured from +X toward +Z — matching
        // our yaw convention exactly. So target_yaw_rev - player_yaw_rev gives
        // the needle's screen-rotation in revolutions, with 0 meaning "needle
        // points up the texture" = "target is straight ahead".
        float ComputeCompassTargetRevolutions(const ItemRenderContext& ctx) {
            const float dx = ctx.compassTargetX - ctx.playerX;
            const float dz = ctx.compassTargetZ - ctx.playerZ;
            const float kPi = 3.14159265358979323846f;
            float worldAngleRad = std::atan2(dz, dx);               // angle from +X to (dx,dz)
            float worldAngleRev = worldAngleRad / (2.0f * kPi);     // -0.5..0.5
            float playerYawRev  = ctx.playerYaw / 360.0f;
            // The texture's frame 0 points AWAY from the target with the bare
            // formula (verified by the pointing direction being 180° off). Add
            // 0.5 (half a revolution) to flip the needle so it points TO the
            // target instead.
            return WrapToUnit(worldAngleRev - playerYawRev + 0.5f);
        }

        // Advance the wobble simulation by one fixed tick (1/20 s). Mirrors MC's per-tick
        // CompassAngleState update — velocity damps, pulls toward target, integrates.
        void TickCompass(const ItemRenderContext& ctx) {
            float target = ComputeCompassTargetRevolutions(ctx);
            float delta  = target - g_compassRotation;
            // Wrap delta to [-0.5, 0.5) so we always take the short way around.
            if (delta >  0.5f) delta -= 1.0f;
            if (delta < -0.5f) delta += 1.0f;
            g_compassVelocity *= COMPASS_INERTIA;
            g_compassVelocity += delta * COMPASS_STIFFNESS;
            g_compassRotation = WrapToUnit(g_compassRotation + g_compassVelocity);
        }

        // Frame selector: pure read of the simulated rotation. The renderer ticks the
        // simulation separately (see ItemRegistry::TickAnimated below).
        int CompassFrameSelector(const ItemRenderContext& /*ctx*/) {
            int frame = static_cast<int>(std::floor(g_compassRotation * 32.0f + 0.5f)) % 32;
            if (frame < 0) frame += 32;
            return frame;
        }

        // Recovery compass — points at the player's last death position. We don't
        // track death yet, so it just behaves like a normal compass for now.
        int RecoveryCompassFrameSelector(const ItemRenderContext& ctx) {
            return CompassFrameSelector(ctx);
        }

        // Clock — frame from world time-of-day. MC's vanilla day is 24 000 ticks
        // = 20 minutes. The clock face has 64 frames covering one full revolution
        // per day. We use elapsed wall-clock seconds as a proxy for world time
        // until a real day/night cycle exists.
        int ClockFrameSelector(const ItemRenderContext& ctx) {
            constexpr float kDaySeconds = 20.0f * 60.0f;
            float dayFraction = std::fmod(ctx.timeSeconds, kDaySeconds) / kDaySeconds;
            int frame = static_cast<int>(std::floor(dayFraction * 64.0f + 0.5f)) % 64;
            if (frame < 0) frame += 64;
            return frame;
        }

        // Static-frame fallback for items whose JSON has overrides[] but whose
        // predicate (bow `pull`, fishing `cast`, shield `blocking`, etc.) we
        // don't simulate yet. Renders the resting frame.
        int StaticFrame0Selector(const ItemRenderContext& /*ctx*/) {
            return 0;
        }

        const Item& AirItem() {
            static Item air = []{
                Item a;
                a.name = "Air";
                a.renderType = ItemRenderType::Sprite;
                a.spriteName = "";
                a.maxStackSize = 0;
                a.blockId = BlockID::Air;
                return a;
            }();
            return air;
        }
    }

    void ItemRegistry::Initialize() {
        if (g_initialized) return;
        g_blockItems.clear();
        g_pureItems.clear();

        // Apply MC's `assets/items/{slug}.json` (the modern client-item-info
        // dispatch tree) onto an Item. Sets renderType + spriteName/spriteFrames
        // + blockModelOverride based on the resolved model kind. Caller has
        // already populated `name`, `blockId`, etc.
        auto applyClientItem = [](Item& item, const std::string& slug) {
            ClientItemDesc desc = ClientItemLoader::Load(slug);
            if (desc.kind == ClientItemKind::Missing) return false;
            switch (desc.kind) {
                case ClientItemKind::BlockModel:
                    item.renderType        = ItemRenderType::Block;
                    item.blockModelOverride = desc.restSlug; // e.g. "oak_trapdoor_bottom"
                    item.spriteName        = "";
                    break;
                case ClientItemKind::FlatSprite:
                case ClientItemKind::Special:
                    item.renderType = ItemRenderType::Sprite;
                    item.spriteName = desc.restSlug;
                    // The new client-item only gives us the MODEL identifier
                    // (e.g. "item/glass_pane"). The actual texture name lives in
                    // the legacy `models/item/{slug}.json` `textures.layer0` —
                    // for glass_pane that's "block/glass", not "glass_pane". Try
                    // to refine spriteName + multi-layer info from the legacy
                    // model. If the legacy file is missing, fall through.
                    {
                        Item probe;
                        try {
                            if (ItemModelLoader::LoadInto(probe, desc.restSlug)) {
                                if (!probe.spriteName.empty()) item.spriteName = probe.spriteName;
                                if (!probe.spriteLayers.empty()) item.spriteLayers = std::move(probe.spriteLayers);
                            }
                        } catch (...) {}
                    }
                    // Tints from the items/{slug}.json `tints` array — index N
                    // applies to layerN. 0 means untinted/white; non-zero is
                    // ARGB. Used by leather armor, spawn eggs, potions, etc.
                    if (!desc.layerTints.empty()) item.layerTints = desc.layerTints;

                    // ── Auto-attach the `_overlay.png` companion ONLY when
                    // the items.json carries tints. Mirrors MC's
                    // `ItemModelGenerators.generateItemWithTintedBaseLayer`
                    // (ItemModelGenerators.java:104), which generates a
                    // TWO_LAYERED_ITEM (layer0 tinted, layer1 untinted overlay)
                    // ONLY for items with a default tinted base. Items whose
                    // un-dyed branch declares no tint (e.g. wolf_armor's
                    // `condition on_false` branch) must stay single-layer —
                    // otherwise the overlay's white pixels render on top.
                    if (item.spriteLayers.size() == 1
                        && !item.layerTints.empty()
                        && !item.spriteName.empty()) {
                        const std::string overlayName = item.spriteName + "_overlay";
                        const std::string overlayPng = PlatformMain::GetAssetPath(
                            "assets/textures/item/" + overlayName + ".png");
                        if (std::filesystem::exists(overlayPng)) {
                            item.spriteLayers.push_back(overlayName);
                        }
                    }
                    // Carry the BEWLR hints onto the Item so per-kind renderers
                    // (chest, shulker_box, …) can pick the right texture variant.
                    item.specialKind    = desc.specialKind;
                    item.specialTexture = desc.specialTexture;
                    break;
                case ClientItemKind::Missing:
                    break;
            }
            // Animation frames + property → selector mapping.
            // The loader gives us MODEL slugs from the range_dispatch entries
            // (e.g. "brush_brushing_0"), but the renderer needs TEXTURE
            // names. For clock these happen to match (clock_00.json's layer0
            // is "item/clock_00"), but brush's brushing-frame models all
            // reference "item/brush" — different from their model name. So
            // resolve each frame's model JSON via ItemModelLoader and use
            // its layer0 as the actual frame texture.
            if (!desc.frameSlugs.empty()) {
                item.spriteFrames.clear();
                item.spriteFrames.reserve(desc.frameSlugs.size());
                for (const auto& slug : desc.frameSlugs) {
                    std::string textureName = slug; // fallback to slug
                    try {
                        Item frameProbe;
                        if (ItemModelLoader::LoadInto(frameProbe, slug)
                            && !frameProbe.spriteName.empty()) {
                            textureName = frameProbe.spriteName;
                        }
                    } catch (...) {}
                    item.spriteFrames.push_back(std::move(textureName));
                }
                item.predicateName = desc.property;
            }
            return true;
        };

        // Block items — one per BlockID, sharing numeric IDs.
        const int total = static_cast<int>(BlockID::Count);
        g_blockItems.resize(total);
        for (int i = 0; i < total; ++i) {
            BlockID bid = static_cast<BlockID>(i);
            const auto& block = BlockRegistry::Get(bid);
            Item& item = g_blockItems[i];
            item.name         = block.name;
            item.renderType   = ItemRenderType::Block;
            item.blockId      = bid;
            item.spriteName   = "";
            item.maxStackSize = 64;
            // Authoritative source: MC's assets/items/{slug}.json. Falls back
            // to block.modelName if no client-item file exists (vanilla full
            // cubes like dirt/oak_planks may rely on the block's own model).
            if (!block.modelName.empty()) {
                applyClientItem(item, block.modelName);
            }
        }
        // Slot 0 (Air) is special — render type doesn't matter, but mark it as a sprite
        // with no texture so accidental rendering is a no-op.
        g_blockItems[0].name        = "Air";
        g_blockItems[0].renderType  = ItemRenderType::Sprite;
        g_blockItems[0].maxStackSize = 0;

        // Pure items — auto-registered from the table in GeneratedItemList.cpp,
        // which mirrors MC's `Items.java` declaration order. Numeric IDs are
        // sequential from PURE_ITEM_BASE so they're stable across builds (the
        // table is APPEND-ONLY by convention; never reorder or delete).
        //
        // For each entry we:
        //   1. Look up its display name in en_us.json (fallback: title-case).
        //   2. Try to load `assets/models/item/{slug}.json` for animated items
        //      (compass, recovery_compass, clock, bow, etc.). If present, the
        //      loader populates spriteFrames + predicateName from the override
        //      array. If absent, we just use the slug as the static texture.
        //   3. Wire up the right ItemFrameSelector by predicate name —
        //      "angle" → CompassFrameSelector (or RecoveryCompassFrameSelector),
        //      "time"  → ClockFrameSelector, others → StaticFrame0Selector.
        std::unordered_map<std::string, std::string> lang;
        try {
            std::ifstream f(PlatformMain::GetAssetPath("assets/lang/en_us.json"));
            if (f.is_open()) {
                nlohmann::json j;
                f >> j;
                for (auto it = j.begin(); it != j.end(); ++it) {
                    if (it.value().is_string()) lang[it.key()] = it.value().get<std::string>();
                }
            }
        } catch (...) { /* missing or malformed → falls back to title-case below */ }

        auto titleCaseFromSlug = [](const std::string& slug) {
            std::string out;
            bool capitalize = true;
            for (char c : slug) {
                if (c == '_') { out.push_back(' '); capitalize = true; }
                else if (capitalize) { out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c)))); capitalize = false; }
                else { out.push_back(c); }
            }
            return out;
        };

        for (size_t i = 0; i < kPureItemTableSize; ++i) {
            const auto& entry = kPureItemTable[i];
            const std::string slug = entry.slug;
            Item it;
            const std::string langKey = "item.minecraft." + slug;
            auto langIt = lang.find(langKey);
            it.name         = (langIt != lang.end()) ? langIt->second : titleCaseFromSlug(slug);
            it.renderType   = ItemRenderType::Sprite;
            it.maxStackSize = 64;
            // Preferred source: MC's assets/items/{slug}.json (modern dispatch tree).
            // Fall back to the legacy assets/models/item/{slug}.json + overrides[]
            // for items whose new-style JSON is missing or unparseable.
            bool applied = applyClientItem(it, slug);
            if (!applied) {
                bool loaded = false;
                try {
                    loaded = ItemModelLoader::LoadInto(it, slug);
                } catch (const std::exception& e) {
                    Log::Warning("[ItemRegistry] '%s' legacy model parse failed: %s — using slug as texture",
                                 slug.c_str(), e.what());
                }
                if (!loaded) it.spriteName = slug;
            }
            // Wire animation selector. Honour explicit property names from the
            // new client-item schema first; fall back to the legacy override
            // predicate; finally to the generator's hint baked into the table.
            std::string pred = !it.predicateName.empty() ? it.predicateName
                                                         : std::string(entry.predicateHint);
            // Strip "minecraft:" namespace if present so matches below work for both schemas.
            if (pred.compare(0, 10, "minecraft:") == 0) pred = pred.substr(10);
            // Newer schema keys: compass/lodestone, compass/recovery, compass/spawn, time,
            // use_duration, damage, etc. Older overrides[]: angle, time, pull, damage…
            if (pred == "angle"
                || pred.compare(0, 8, "compass/") == 0) {
                it.selectFrame = (slug == "recovery_compass") ? &RecoveryCompassFrameSelector
                                                              : &CompassFrameSelector;
            } else if (pred == "time") {
                it.selectFrame = &ClockFrameSelector;
            } else if (!it.spriteFrames.empty()) {
                // Has frames but no selector matches (bow `use_duration`, fishing
                // `cast`, shield `blocking`, trident `charge`/`throwing`, …) → frame 0.
                it.selectFrame = &StaticFrame0Selector;
            }
            g_pureItems[PURE_ITEM_BASE + i] = std::move(it);
        }

        // Apply MC's per-item DataComponent defaults. Mirrors the
        // `Item.Properties.component(...)` calls in MC's Items.java. Future
        // items (tools with default DAMAGE/MAX_DAMAGE, named items, etc.)
        // extend this block as their components land in DataComponents.cpp.
        if (auto it = g_pureItems.find(Items::EnchantedBook); it != g_pureItems.end()) {
            // Verbatim from Items.java:2854:
            //   .component(DataComponents.STORED_ENCHANTMENTS, ItemEnchantments.EMPTY)
            //   .component(DataComponents.ENCHANTMENT_GLINT_OVERRIDE, true)
            it->second.defaultComponents.set(DataComponents::STORED_ENCHANTMENTS, ItemEnchantments::EMPTY);
            it->second.defaultComponents.set(DataComponents::ENCHANTMENT_GLINT_OVERRIDE, true);
        }
        // Items.java:2597 — ENCHANTED_GOLDEN_APPLE
        //   .component(DataComponents.ENCHANTMENT_GLINT_OVERRIDE, true)
        if (auto it = g_pureItems.find(Items::EnchantedGoldenApple); it != g_pureItems.end()) {
            it->second.defaultComponents.set(DataComponents::ENCHANTMENT_GLINT_OVERRIDE, true);
        }
        // Items.java:2827 — EXPERIENCE_BOTTLE ("bottle of enchanting")
        //   .component(DataComponents.ENCHANTMENT_GLINT_OVERRIDE, true)
        if (auto it = g_pureItems.find(Items::ExperienceBottle); it != g_pureItems.end()) {
            it->second.defaultComponents.set(DataComponents::ENCHANTMENT_GLINT_OVERRIDE, true);
        }

        g_initialized = true;
        Log::Info("[ItemRegistry] Initialized: %zu block items, %zu pure items",
                  g_blockItems.size(), g_pureItems.size());

        // One-shot DataComponent system sanity check. Verifies set/get round-trip
        // and that ItemStack::get<T> falls back to item defaults when the stack
        // itself has no override (mirrors MC's PatchedDataComponentMap behaviour).
        {
            ItemStack book(Items::EnchantedBook, 1);
            auto fallback = book.get(DataComponents::ENCHANTMENT_GLINT_OVERRIDE);
            ItemStack regular(Items::Stick, 1);
            auto noFallback = regular.get(DataComponents::ENCHANTMENT_GLINT_OVERRIDE);
            book.components.set(DataComponents::ENCHANTMENT_GLINT_OVERRIDE, false);
            auto override = book.get(DataComponents::ENCHANTMENT_GLINT_OVERRIDE);
            const bool ok = fallback.has_value() && *fallback == true
                         && !noFallback.has_value()
                         && override.has_value() && *override == false;
            Log::Info("[DataComponent] sanity check: %s", ok ? "OK" : "FAIL");
        }
    }

    void ItemRegistry::RegisterPureItem(ItemID id, const std::string& name,
                                        const std::string& spriteName, int maxStack) {
        Item item;
        item.name         = name;
        item.renderType   = ItemRenderType::Sprite;
        item.spriteName   = spriteName;
        item.maxStackSize = maxStack;
        item.blockId      = BlockID::Air;
        g_pureItems[id]   = std::move(item);
    }

    void ItemRegistry::RegisterAnimatedItem(ItemID id, const std::string& name,
                                            std::vector<std::string> frames,
                                            ItemFrameSelector selector, int maxStack) {
        Item item;
        item.name         = name;
        item.renderType   = ItemRenderType::Sprite;
        item.spriteFrames = std::move(frames);
        item.selectFrame  = selector;
        item.maxStackSize = maxStack;
        item.blockId      = BlockID::Air;
        // spriteName left empty — render path uses spriteFrames + selectFrame.
        g_pureItems[id]   = std::move(item);
    }

    bool ItemStack::HasFoil() const {
        // Verbatim from MC ItemStack.java:909-911.
        if (auto override = get(DataComponents::ENCHANTMENT_GLINT_OVERRIDE)) {
            return *override;
        }
        // Item.isFoil default behaviour: stack glints iff it has any
        // stored enchantments. ENCHANTMENTS (on tools) lands in a future PR;
        // for now only STORED_ENCHANTMENTS (on enchanted_book) counts.
        if (auto stored = get(DataComponents::STORED_ENCHANTMENTS)) {
            return !stored->entries.empty();
        }
        return false;
    }

    void ItemRegistry::SetRenderContext(const ItemRenderContext& ctx) {
        g_renderContext = ctx;
    }

    const ItemRenderContext& ItemRegistry::GetRenderContext() {
        return g_renderContext;
    }

    void ItemRegistry::TickAnimated(float dtSeconds) {
        // Drive the compass wobble at MC's fixed 20 TPS regardless of frame rate. We could
        // use a partial-tick remainder for smoother visuals (MC does this too) — for now
        // we just step in fixed increments and accept the per-tick granularity.
        g_compassTickAccum += dtSeconds;
        // Cap accumulated time so a long pause doesn't burst-tick the compass.
        if (g_compassTickAccum > 0.25f) g_compassTickAccum = 0.25f;
        while (g_compassTickAccum >= COMPASS_TICK_DT) {
            TickCompass(g_renderContext);
            g_compassTickAccum -= COMPASS_TICK_DT;
        }
    }

    const Item& ItemRegistry::Get(ItemID id) {
        if (id < g_blockItems.size()) {
            return g_blockItems[id];
        }
        auto it = g_pureItems.find(id);
        if (it != g_pureItems.end()) return it->second;
        return AirItem();
    }

    bool ItemRegistry::IsBlockItem(ItemID id) {
        return id != Items::Air && id < static_cast<ItemID>(BlockID::Count);
    }

    size_t ItemRegistry::Size() {
        return g_blockItems.size() + g_pureItems.size();
    }

} // namespace Game
