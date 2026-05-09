// File: src/common/entity/Item.cpp
#include "Item.hpp"
#include "ItemModelLoader.hpp"
#include "../world/block/BlockRegistry.hpp"
#include "../core/Log.hpp"
#include <unordered_map>
#include <vector>
#include <cmath>
#include <cstdio>

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
        float ComputeCompassTargetRevolutions(const ItemRenderContext& ctx) {
            const float dx = ctx.compassTargetX - ctx.playerX;
            const float dz = ctx.compassTargetZ - ctx.playerZ;
            // World angle from player to target. MC's convention: yaw=0 faces +Z (south).
            const float kPi = 3.14159265358979323846f;
            float worldAngleRad = std::atan2(-dx, dz);              // -π..π
            float worldAngleRev = worldAngleRad / (2.0f * kPi);     // -0.5..0.5
            // Subtract player yaw (degrees → revolutions) so the needle compensates camera rot.
            float playerYawRev = ctx.playerYaw / 360.0f;
            return WrapToUnit(worldAngleRev - playerYawRev);
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

        // Block items — one per BlockID, sharing numeric IDs.
        const int total = static_cast<int>(BlockID::Count);
        g_blockItems.resize(total);
        for (int i = 0; i < total; ++i) {
            BlockID bid = static_cast<BlockID>(i);
            const auto& block = BlockRegistry::Get(bid);
            Item& item = g_blockItems[i];
            item.name        = block.name;
            item.renderType  = ItemRenderType::Block;
            item.blockId     = bid;
            item.spriteName  = "";
            item.maxStackSize = 64;
        }
        // Slot 0 (Air) is special — render type doesn't matter, but mark it as a sprite
        // with no texture so accidental rendering is a no-op.
        g_blockItems[0].name        = "Air";
        g_blockItems[0].renderType  = ItemRenderType::Sprite;
        g_blockItems[0].maxStackSize = 0;

        // Pure items — defined explicitly so the wire format is stable across versions.
        // To add an item: bump PURE_ITEM_BASE+N in Item.hpp Items namespace, register the
        // ItemID + slug here, drop the matching `assets/models/item/{slug}.json` on disk.
        // Textures (single layer0 OR override variants) are pulled from the JSON by
        // ItemModelLoader — exactly mirrors MC's `assets/.../models/item/{name}.json`.
        //
        // The compass needs a frame selector to pick which override variant to render
        // based on player→target angle. Other animated items would supply their own.
        {
            Item compass;
            compass.name         = "Compass";
            compass.renderType   = ItemRenderType::Sprite;
            compass.maxStackSize = 64;
            compass.selectFrame  = &CompassFrameSelector;
            // Loader fills spriteName + spriteFrames from `assets/models/item/compass.json`.
            // If that file is missing, we leave both empty and the renderer skips the item.
            if (!ItemModelLoader::LoadInto(compass, "compass")) {
                Log::Warning("[ItemRegistry] compass model JSON missing; compass icon disabled");
            }
            g_pureItems[Items::Compass] = std::move(compass);
        }

        g_initialized = true;
        Log::Info("[ItemRegistry] Initialized: %zu block items, %zu pure items",
                  g_blockItems.size(), g_pureItems.size());
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
