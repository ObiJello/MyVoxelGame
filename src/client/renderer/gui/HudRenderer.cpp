// File: src/client/renderer/gui/HudRenderer.cpp
#include "HudRenderer.hpp"
#include "EffectsInInventory.hpp"
#include "BossBarState.hpp"
#include "common/entity/Inventory.hpp"
#include "common/entity/Item.hpp"
#include "common/data/DataComponents.hpp"
#include "common/network/packets/game/SetHealthS2CPacket.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/core/Log.hpp"
#include "../backend/RenderBackend.hpp"
#include "client/resource/ResourcePacks.hpp"
#include "stb_image.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>

// PlatformMain provides bundle-aware asset path resolution.
namespace PlatformMain { std::string GetAssetPath(const std::string&); }

namespace Render {

    // Sprite IDs matching MC's Gui.java constants
    static const char* HOTBAR_SPRITE = "hud/hotbar";
    static const char* HOTBAR_SELECTION_SPRITE = "hud/hotbar_selection";
    static const char* ARMOR_EMPTY_SPRITE = "hud/armor_empty";
    static const char* ARMOR_HALF_SPRITE = "hud/armor_half";
    static const char* ARMOR_FULL_SPRITE = "hud/armor_full";
    static const char* FOOD_EMPTY_SPRITE = "hud/food_empty";
    static const char* FOOD_HALF_SPRITE = "hud/food_half";
    static const char* FOOD_FULL_SPRITE = "hud/food_full";
    static const char* FOOD_EMPTY_HUNGER_SPRITE = "hud/food_empty_hunger";
    static const char* FOOD_HALF_HUNGER_SPRITE = "hud/food_half_hunger";
    static const char* FOOD_FULL_HUNGER_SPRITE = "hud/food_full_hunger";
    static const char* AIR_SPRITE = "hud/air";
    static const char* AIR_POPPING_SPRITE = "hud/air_bursting";
    static const char* AIR_EMPTY_SPRITE = "hud/air_empty";
    static const char* XP_BAR_BG_SPRITE = "hud/experience_bar_background";
    static const char* XP_BAR_PROGRESS_SPRITE = "hud/experience_bar_progress";

    void HudRenderer::Render(GuiGraphics& graphics, const Game::Inventory& inventory, float deltaTime) {
        // ── Detect selected-item changes — mirrors MC's tickSelectedItemName:
        //    • currently-held stack empty (air) → timer = 0 immediately
        //      (text vanishes the moment you switch to an empty slot, no
        //      lingering fade).
        //    • currently-held same as the displayed one → just count down.
        //    • currently-held differs (and is non-air) → restart timer at
        //      40 ticks × notificationDisplayTime (default 2) = 80 ticks
        //      → 4.0 s at 20 TPS.
        //    Suppress the very first observation so the overlay doesn't
        //    flash on first-spawn.
        const Game::ItemID currentSelected = inventory.GetSelectedItem();
        const auto& selectedStack = inventory.GetSlot(
            Game::Inventory::HotbarToIndex(inventory.GetSelectedSlot()));
        const std::string currentName = selectedStack.IsEmpty()
            ? std::string() : Game::GetItemStackHoverName(selectedStack);
        if (m_firstObserve) {
            m_lastSelected = currentSelected;
            m_lastSelectedName = currentName;
            m_displayedItem = currentSelected;
            m_firstObserve = false;
        } else if (currentSelected == Game::Items::Air) {
            // Empty hand → hide immediately, like MC.
            m_toolHighlightTimer = 0.0f;
            m_lastSelected       = currentSelected;
            m_lastSelectedName.clear();
        } else if (currentSelected != m_lastSelected || currentName != m_lastSelectedName) {
            m_lastSelected       = currentSelected;
            m_lastSelectedName   = currentName;
            m_displayedItem      = currentSelected;
            m_toolHighlightTimer = 4.0f;
        }

        // Update tooltip timer (only counts down when something IS held).
        if (m_toolHighlightTimer > 0.0f) {
            m_toolHighlightTimer -= deltaTime;
            if (m_toolHighlightTimer < 0.0f) m_toolHighlightTimer = 0.0f;
        }
        // MC Gui.tick: `if (overlayMessageTime > 0) --overlayMessageTime`.
        if (m_overlayMessageTime > 0.0f) {
            m_overlayMessageTime -= deltaTime;
            if (m_overlayMessageTime < 0.0f) m_overlayMessageTime = 0.0f;
        }

        // MC draws the screen effects (ScreenEffectRenderer: the water
        // overlay, the in-wall block, fire) in the world pass, under the
        // whole GUI. Same place in the stack here: first, under everything.
        RenderWaterOverlay(graphics);

        // MC render order: boss bar first (BossHealthOverlay renders before
        // the hotbar layer), then crosshair → hotbar → health/food/armor → XP.
        RenderBossBar(graphics);
        RenderAttackIndicator(graphics);
        RenderItemHotbar(graphics, inventory);

        graphics.NextStratum();

        // Status bars (MC: renderHotbarAndDecorations calls these).
        // Creative/spectator skip the whole survival stat block — MC's Gui
        // gates on gameMode.canHurtPlayer() / isSurvival().
        if (!m_statsHidden) {
            // Armor, hearts, food and air together: their rows are laid out
            // from one health-row count (MC Hud.extractPlayerHealth).
            RenderPlayerHealth(graphics);

            RenderExperienceBar(graphics);
            RenderExperienceLevel(graphics);
        }

        // Selected item name tooltip
        RenderSelectedItemName(graphics, inventory);

        // MC Hud.extractRenderState: extractHotbarAndDecorations, then
        // extractEffects.
        graphics.NextStratum();
        RenderEffects(graphics);

        // MC Gui's layer order: the hotbar layer above, then the sleep fade
        // over all of it, then the second layer — whose first visible member
        // here is the action bar — on top of the fade.
        graphics.NextStratum();
        RenderSleepOverlay(graphics);
        graphics.NextStratum();
        RenderOverlayMessage(graphics);
    }

    void HudRenderer::RenderEffects(GuiGraphics& graphics) {
        // MC Hud.extractEffects.
        if (m_activeEffects.empty()) return;
        if (InventoryShowsActiveEffects(graphics.GuiWidth())) return;

        // Ordering.natural().reverse().sortedCopy(activeEffects).
        std::vector<const Game::MobEffectInstance*> sorted;
        sorted.reserve(m_activeEffects.size());
        for (const auto& e : m_activeEffects) sorted.push_back(&e);
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const Game::MobEffectInstance* a, const Game::MobEffectInstance* b) {
                             return b->CompareTo(*a) < 0;
                         });

        int beneficialCount = 0;
        int harmfulCount = 0;
        for (const Game::MobEffectInstance* instance : sorted) {
            if (!instance->showIcon) continue;
            int x = graphics.GuiWidth();
            int y = 1;   // (+15 in the demo, which this port has not)
            if (Game::IsBeneficialEffect(instance->effect)) {
                ++beneficialCount;
                x -= 25 * beneficialCount;
            } else {
                ++harmfulCount;
                x -= 25 * harmfulCount;
                y += 26;
            }

            float alpha = 1.0f;
            if (instance->ambient) {
                graphics.BlitSprite("hud/effect_background_ambient", x, y, 24, 24);
            } else {
                graphics.BlitSprite("hud/effect_background", x, y, 24, 24);
                if (instance->EndsWithin(200)) {
                    // The last ten seconds' blink.
                    const int remainingDuration = instance->duration;
                    const int usedSeconds = 10 - remainingDuration / 20;
                    alpha = std::clamp(static_cast<float>(remainingDuration) / 10.0f / 5.0f * 0.5f, 0.0f, 0.5f) +
                            std::cos(static_cast<float>(remainingDuration) * 3.1415927f / 5.0f) *
                                std::clamp(static_cast<float>(usedSeconds) / 10.0f * 0.25f, 0.0f, 0.25f);
                    alpha = std::clamp(alpha, 0.0f, 1.0f);
                }
            }
            // ARGB.white(alpha).
            const uint32_t a = static_cast<uint32_t>(std::floor(alpha * 255.0f)) & 0xFFu;   // Mth.floor
            graphics.BlitSprite(EffectsInInventory::EffectSprite(instance->effect),
                                x + 3, y + 3, 18, 18, (a << 24) | 0x00FFFFFFu);
        }
    }

    namespace {
        // textures/misc/underwater.png, loaded once, wrap REPEAT so the
        // four-times tiling and the view-angle scroll wrap seamlessly.
        // Same lifetime rule as GuiGraphics' glint texture: dropped and
        // reloaded when the resource packs change.
        TextureHandle LoadUnderwaterTexture() {
            static TextureHandle s_tex = INVALID_TEXTURE;
            static bool          s_tried = false;
            static int           s_packGeneration = -1;
            if (Resources::CacheStale(s_packGeneration)) {
                if (s_tex != INVALID_TEXTURE && g_renderBackend) g_renderBackend->DestroyTexture(s_tex);
                s_tex = INVALID_TEXTURE;
                s_tried = false;
            }
            if (s_tried) return s_tex;
            s_tried = true;
            if (!g_renderBackend) return INVALID_TEXTURE;
            const std::string full = PlatformMain::GetAssetPath("assets/textures/misc/underwater.png");
            if (!std::filesystem::exists(full)) {
                Log::Warning("[HudRenderer] underwater texture missing at %s", full.c_str());
                return INVALID_TEXTURE;
            }
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) {
                Log::Warning("[HudRenderer] stbi_load underwater failed: %s", stbi_failure_reason());
                return INVALID_TEXTURE;
            }
            s_tex = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (s_tex != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(s_tex, TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap  (s_tex, TextureWrap::Repeat,  TextureWrap::Repeat);
            }
            return s_tex;
        }
    } // namespace

    void HudRenderer::RenderWaterOverlay(GuiGraphics& graphics) {
        // MC ScreenEffectRenderer.submitWater: a full-screen quad of
        // underwater.png, u from uOffset+4 down to uOffset across the screen
        // (four repeats), v likewise, coloured ARGB(0.1, b, b, b).
        if (!m_waterOverlay) return;
        const TextureHandle tex = LoadUnderwaterTexture();
        if (tex == INVALID_TEXTURE) return;
        const float b = glm::clamp(m_waterOverlayBrightness, 0.0f, 1.0f);
        const uint32_t gray  = static_cast<uint32_t>(b * 255.0f + 0.5f) & 0xFFu;
        const uint32_t alpha = static_cast<uint32_t>(0.1f * 255.0f + 0.5f) & 0xFFu;
        const uint32_t color = (alpha << 24) | (gray << 16) | (gray << 8) | gray;
        graphics.Blit(tex, 0, 0, graphics.GuiWidth(), graphics.GuiHeight(),
                      m_waterOverlayU + 4.0f, m_waterOverlayV + 4.0f,
                      m_waterOverlayU,        m_waterOverlayV, color);
    }

    void HudRenderer::RenderSleepOverlay(GuiGraphics& graphics) {
        // MC Gui.renderSleepOverlay (26.3 Hud.extractSleepOverlay):
        //   float amount = sleepTimer / 100.0F;
        //   if (amount > 1.0F) amount = 1.0F - (sleepTimer - 100.0F) / 10.0F;
        //   fill(0, 0, w, h, (int)(220.0F * amount) << 24 | 1052704);
        // 1052704 is 0x101020 — a near-black blue. Alpha climbs to 220 over
        // the 100 ticks in bed and drops back over the 10 after.
        if (m_sleepTimer <= 0) return;
        float amount = static_cast<float>(m_sleepTimer) / 100.0f;
        if (amount > 1.0f) amount = 1.0f - (static_cast<float>(m_sleepTimer) - 100.0f) / 10.0f;
        if (amount <= 0.0f) return;
        const uint32_t alpha = static_cast<uint32_t>(220.0f * amount) & 0xFFu;
        graphics.Fill(0, 0, graphics.GuiWidth(), graphics.GuiHeight(), (alpha << 24) | 0x101020u);
    }

    void HudRenderer::RenderOverlayMessage(GuiGraphics& graphics) {
        // MC Gui.renderOverlayMessage:
        //   int i = (int)(overlayMessageTime * 255.0F / 20.0F); if (i > 255) i = 255;
        //   if (i > 8) drawStringWithBackdrop(text, -width/2, -4, width, ARGB(i << 24, white))
        // about (width / 2, height - 68).
        if (m_overlayMessageTime <= 0.0f || m_overlayMessage.empty()) return;
        const float ticks = m_overlayMessageTime * 20.0f;
        int alpha = static_cast<int>(ticks * 255.0f / 20.0f);
        if (alpha > 255) alpha = 255;
        if (alpha <= 8) return;
        const int width = graphics.GetStringWidth(m_overlayMessage);
        const int x = graphics.GuiWidth() / 2 - width / 2;
        const int y = graphics.GuiHeight() - 68 - 4;
        graphics.DrawStringWithBackdrop(m_overlayMessage, x, y, width,
                                        (static_cast<uint32_t>(alpha) << 24) | 0x00FFFFFFu);
    }

    void HudRenderer::OnSelectedSlotChanged(Game::BlockID blockId) {
        // Legacy entrypoint — Render() already auto-detects ItemID changes.
        // Kept compiling for any caller that still passes a BlockID.
        if (blockId != Game::BlockID::Air) {
            m_displayedItem      = Game::ItemRegistry::FromBlock(blockId);
            m_toolHighlightTimer = 4.0f;
        }
    }

    // ========================================================================
    // Hotbar (MC: Gui.renderItemHotbar)
    // ========================================================================

    void HudRenderer::RenderBossBar(GuiGraphics& graphics) {
        // MC BossHealthOverlay.render: a 182x5 bar centred at y=12 per event,
        // the name centred 9px above it. One bar here (the dragon) — see
        // BossBarState.hpp. The sprites are vanilla's own
        // gui/sprites/boss_bar/*.png, auto-loaded by GuiAtlas.
        const Client::BossBarState& bar = Client::g_bossBarState;
        if (!bar.visible) return;

        static const char* kColorNames[] = {
            "pink", "blue", "red", "green", "yellow", "purple", "white",
        };
        const uint8_t colorIndex =
            bar.color < 7 ? bar.color : 0;

        const int x = graphics.GuiWidth() / 2 - 91;
        const int y = 12;

        const std::string background =
            std::string("boss_bar/") + kColorNames[colorIndex] + "_background";
        const std::string progress =
            std::string("boss_bar/") + kColorNames[colorIndex] + "_progress";

        graphics.BlitSprite(background, x, y, 182, 5);
        const int fill = static_cast<int>(bar.progress * 183.0f);
        if (fill > 0) {
            graphics.BlitSprite(progress, 182, 5, 0, 0, x, y,
                                std::min(fill, 182), 5);
        }
        if (bar.notches != 0) {
            const std::string notchBg = "boss_bar/notched_" +
                std::to_string(bar.notches) + "_background";
            const std::string notchFg = "boss_bar/notched_" +
                std::to_string(bar.notches) + "_progress";
            graphics.BlitSprite(notchBg, x, y, 182, 5);
            if (fill > 0) {
                graphics.BlitSprite(notchFg, 182, 5, 0, 0, x, y,
                                    std::min(fill, 182), 5);
            }
        }

        // The name, white with the usual shadow, 9 px above the bar.
        if (!bar.name.empty()) {
            graphics.DrawCenteredString(bar.name, graphics.GuiWidth() / 2, y - 9,
                                        0xFFFFFFFF);
        }
    }

    void HudRenderer::RenderAttackIndicator(GuiGraphics& graphics) {
        // MC Gui.renderCrosshair's AttackIndicatorStatus.CROSSHAIR branch,
        // verbatim including the offsets: the bar sits 16 px below the
        // crosshair's top edge, is 16x4, and the progress fill is scaled to
        // SEVENTEEN not sixteen — MC's `(int)(scale * 17.0F)` overshoots the
        // sprite by a pixel on purpose so a full bar has no gap at the end.
        const int cx = graphics.GuiWidth() / 2;
        const int cy = graphics.GuiHeight() / 2;
        const int x = cx - 8;
        const int y = cy - 7 + 16;

        // MC hides the bar entirely for a weapon that refills in 5 ticks or
        // less — a bare hand or a hoe — because it would be full essentially
        // always and just clutter the crosshair.
        const bool showFull = m_crosshairTarget
                           && m_attackStrengthScale >= 1.0f
                           && m_attackStrengthDelay > 5.0f;

        if (showFull) {
            graphics.BlitSprite("hud/crosshair_attack_indicator_full", x, y, 16, 16);
        } else if (m_attackStrengthScale < 1.0f) {
            const int progress = static_cast<int>(m_attackStrengthScale * 17.0f);
            graphics.BlitSprite("hud/crosshair_attack_indicator_background",
                                x, y, 16, 4);
            if (progress > 0) {
                graphics.BlitSprite("hud/crosshair_attack_indicator_progress",
                                    16, 4, 0, 0, x, y, progress, 4);
            }
        }
    }

    void HudRenderer::RenderItemHotbar(GuiGraphics& graphics, const Game::Inventory& inventory) {
        int screenCenter = graphics.GuiWidth() / 2;
        int bottomY = graphics.GuiHeight();

        // Hotbar background: 182x22, centered at bottom
        graphics.BlitSprite(HOTBAR_SPRITE, screenCenter - 91, bottomY - 22, 182, 22);

        // Selection highlight: 24x23, positioned at selected slot
        int selectedSlot = inventory.GetSelectedSlot();
        graphics.BlitSprite(HOTBAR_SELECTION_SPRITE,
                           screenCenter - 91 - 1 + selectedSlot * 20,
                           bottomY - 22 - 1, 24, 23);

        // Render items in 9 slots
        for (int i = 0; i < 9; i++) {
            int x = screenCenter - 90 + i * 20 + 2;
            int y = bottomY - 16 - 3;
            RenderSlot(graphics, x, y, inventory.GetSlot(Game::Inventory::HotbarToIndex(i)));
        }

        // Offhand slot box — MC Gui.renderItemHotbar draws it only when the
        // offhand holds something, attached to the hotbar's left edge for
        // right-handed players: sprite hud/hotbar_offhand_left 29×24 at
        // (cx - 91 - 29, h - 23), item at (cx - 91 - 26, h - 16 - 3).
        const auto& offhand = inventory.GetSlot(Game::Inventory::OFFHAND_BEGIN);
        if (!offhand.IsEmpty()) {
            graphics.BlitSprite("hud/hotbar_offhand_left",
                                screenCenter - 91 - 29, bottomY - 23, 29, 24);
            RenderSlot(graphics, screenCenter - 91 - 26, bottomY - 16 - 3, offhand);
        }
    }

    void HudRenderer::RenderSlot(GuiGraphics& graphics, int x, int y,
                                  const Game::InventorySlot& slot) {
        if (slot.IsEmpty()) return;

        // Render item icon first, then decorations (count text) on top
        graphics.RenderItem(slot, x, y);
        graphics.NextStratum(); // Text draws on top of item icon
        graphics.RenderItemDecorations(slot, x, y);
        graphics.NextStratum();
    }

    // ========================================================================
    // Selected Item Name (MC: Gui.renderSelectedItemName)
    // ========================================================================

    void HudRenderer::RenderSelectedItemName(GuiGraphics& graphics, const Game::Inventory& inventory) {
        if (m_toolHighlightTimer <= 0.0f) return;
        if (m_displayedItem == Game::Items::Air) return;

        // Name resolution mirrors ItemStack.getStyledHoverName (same chain as
        // the inventory tooltip): CUSTOM_NAME → ITEM_NAME → registry name,
        // colored by RARITY. Read from the LIVE selected stack so per-stack
        // renames show here too.
        const auto& stack = inventory.GetSlot(
            Game::Inventory::HotbarToIndex(inventory.GetSelectedSlot()));
        std::string name;
        uint32_t nameRGB = 0x00FFFFFFu;   // WHITE (rarity COMMON)
        if (!stack.IsEmpty() && stack.itemId == m_displayedItem) {
            // CUSTOM_NAME → Item.getName(stack), which for the potion items
            // is the POTION_CONTENTS name ("Splash Potion of Healing").
            name = Game::GetItemStackHoverName(stack);
            nameRGB = Game::RarityColorARGB(
                          stack.get(Game::DataComponents::RARITY)
                              .value_or(Game::Rarity::COMMON))
                      & 0x00FFFFFFu;
        }
        if (name.empty()) {
            // Registry display name — works for both block items (copied
            // from BlockRegistry at init) and pure items (GeneratedItemList).
            name = Game::ItemRegistry::Get(m_displayedItem).name;
        }
        if (name.empty()) return;

        // MC's Gui.renderSelectedItemName layout exactly:
        //   x = (guiWidth - font.width(component)) / 2
        //   y = guiHeight - 59         (+14 if !canHurtPlayer; we always
        //                               can hurt, so no shift)
        const int strWidth = graphics.GetStringWidth(name);
        const int x = (graphics.GuiWidth() - strWidth) / 2;
        const int y = graphics.GuiHeight() - 59;

        // Fade — MC: `int l = (int)(toolHighlightTimer * 256.0F / 10.0F);
        //              if (l > 255) l = 255;`
        // We tick in seconds at render rate; convert back to ticks (×20).
        const float ticks = m_toolHighlightTimer * 20.0f;
        int alpha = static_cast<int>(ticks * 256.0f / 10.0f);
        if (alpha > 255) alpha = 255;
        if (alpha <= 0) return;

        // MC: `guiGraphics.drawString(font, component, j, k, 16777215 + (l << 24));`
        //   White in MC because the name Component carries its own rarity
        //   style; we bake the rarity RGB directly. Alpha packs the fade.
        //   drawString defaults to dropShadow = true — same here. NO backdrop.
        const uint32_t color = (static_cast<uint32_t>(alpha) << 24) | nameRGB;
        graphics.DrawString(name, x, y, color, /*dropShadow=*/true);
    }


    // ── Status bars — MC Hud.extractPlayerHealth ───────────────────────
    //
    // One pass lays out armor, hearts, food and air from the same numbers:
    // the number of heart rows (max health plus absorption, ten per row)
    // decides how tightly the rows stack (healthRowHeight) and where the
    // armor row and the air row sit above them. The blink and jitter
    // bookkeeping is Gui's, verbatim: a heart lost while the hurt cooldown
    // runs blinks for 20 ticks (10 for a heart gained), the old health stays
    // drawn under the blink until a second passes, the hearts jitter at
    // four half-hearts or fewer, and a regenerating heart bobs up in turn.
    namespace {
        int64_t NowMillis() {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }
    }

    void HudRenderer::RenderPlayerHealth(GuiGraphics& graphics) {
        using Network::SetHealthS2CPacket;
        const int currentHealth = m_health;   // already Mth.ceil'ed by the handler
        const bool blink = m_healthBlinkTime > m_tickCount &&
                           ((m_healthBlinkTime - m_tickCount) / 3) % 2 == 1;
        const int64_t timeMillis = NowMillis();
        if (currentHealth < m_lastHealth && m_damageCooldownTime > 0) {
            m_lastHealthTime = timeMillis;
            m_healthBlinkTime = m_tickCount + 20;
        } else if (currentHealth > m_lastHealth && m_damageCooldownTime > 0) {
            m_lastHealthTime = timeMillis;
            m_healthBlinkTime = m_tickCount + 10;
        }
        if (timeMillis - m_lastHealthTime > 1000) {
            m_displayHealth = currentHealth;
            m_lastHealthTime = timeMillis;
        }
        m_lastHealth = currentHealth;
        const int oldHealth = m_displayHealth;

        // The jitter and the hunger shake draw from a per-tick seeded random
        // so every frame of a tick agrees.
        m_random.SetSeed(static_cast<int64_t>(m_tickCount) * 312871);

        const int xLeft     = graphics.GuiWidth() / 2 - 91;
        const int xRight    = graphics.GuiWidth() / 2 + 91;
        const int yLineBase = graphics.GuiHeight() - 39;
        const float maxHealth = std::max(static_cast<float>(m_maxHealth),
                                         static_cast<float>(std::max(oldHealth, currentHealth)));
        const int totalAbsorption = static_cast<int>(std::ceil(m_absorption));
        const int numHealthRows   = static_cast<int>(std::ceil((maxHealth + static_cast<float>(totalAbsorption)) / 2.0f / 10.0f));
        const int healthRowHeight = std::max(10 - (numHealthRows - 2), 3);
        int yLineAir = yLineBase - 10;
        int heartOffsetIndex = -1;
        if (m_hudFlags & SetHealthS2CPacket::kFlagRegeneration) {
            heartOffsetIndex = m_tickCount % static_cast<int>(std::ceil(maxHealth + 5.0f));
        }

        RenderArmor(graphics, yLineBase, numHealthRows, healthRowHeight, xLeft);
        RenderHearts(graphics, xLeft, yLineBase, healthRowHeight, heartOffsetIndex, maxHealth,
                     currentHealth, oldHealth, totalAbsorption, blink);
        // No ridden vehicle with hearts here, so the food row always draws
        // and the air row sits above it.
        RenderFood(graphics, yLineBase, xRight);
        yLineAir -= 10;
        RenderAir(graphics, yLineAir, xRight);
    }

    // MC Hud.extractArmor: the row above the hearts, a full/half/empty icon
    // per two armor points.
    void HudRenderer::RenderArmor(GuiGraphics& graphics, int yLineBase, int numHealthRows,
                                  int healthRowHeight, int xLeft) {
        const int armor = m_armor;
        if (armor <= 0) return;
        const int yLineArmor = yLineBase - (numHealthRows - 1) * healthRowHeight - 10;
        for (int i = 0; i < 10; ++i) {
            const int xo = xLeft + i * 8;
            if (i * 2 + 1 < armor) {
                graphics.BlitSprite(ARMOR_FULL_SPRITE, xo, yLineArmor, 9, 9);
            }
            if (i * 2 + 1 == armor) {
                graphics.BlitSprite(ARMOR_HALF_SPRITE, xo, yLineArmor, 9, 9);
            }
            if (i * 2 + 1 > armor) {
                graphics.BlitSprite(ARMOR_EMPTY_SPRITE, xo, yLineArmor, 9, 9);
            }
        }
    }

    std::string HudRenderer::HeartSprite(HeartType type, bool hardcore, bool half, bool blink) {
        // MC Hud.HeartType.getSprite over the hud/heart/ sheet names.
        std::string name = "hud/heart/";
        switch (type) {
            case HeartType::Container:
                name += hardcore ? "container_hardcore" : "container";
                if (blink) name += "_blinking";
                return name;
            case HeartType::Normal:    break;
            case HeartType::Poisoned:  name += "poisoned_";  break;
            case HeartType::Withered:  name += "withered_";  break;
            case HeartType::Absorbing: name += "absorbing_"; break;
            case HeartType::Frozen:    name += "frozen_";    break;
        }
        if (hardcore) name += "hardcore_";
        name += half ? "half" : "full";
        if (blink) name += "_blinking";
        return name;
    }

    // MC Hud.extractHearts, verbatim.
    void HudRenderer::RenderHearts(GuiGraphics& graphics, int xLeft, int yLineBase, int healthRowHeight,
                                   int heartOffsetIndex, float maxHealth, int currentHealth,
                                   int oldHealth, int absorption, bool blink) {
        using Network::SetHealthS2CPacket;
        // HeartType.forPlayer: poison, then wither, then frozen, else normal.
        HeartType type = HeartType::Normal;
        if      (m_hudFlags & SetHealthS2CPacket::kFlagPoison) type = HeartType::Poisoned;
        else if (m_hudFlags & SetHealthS2CPacket::kFlagWither) type = HeartType::Withered;
        else if (m_hudFlags & SetHealthS2CPacket::kFlagFrozen) type = HeartType::Frozen;
        const bool isHardcore = (m_hudFlags & SetHealthS2CPacket::kFlagHardcore) != 0;

        const int healthContainerCount     = static_cast<int>(std::ceil(maxHealth / 2.0));
        const int absorptionContainerCount = static_cast<int>(std::ceil(absorption / 2.0));
        const int maxHealthHalvesCount     = healthContainerCount * 2;
        for (int containerIndex = healthContainerCount + absorptionContainerCount - 1;
             containerIndex >= 0; --containerIndex) {
            const int row    = containerIndex / 10;
            const int column = containerIndex % 10;
            const int xo = xLeft + column * 8;
            int yo = yLineBase - row * healthRowHeight;
            // Two hearts or fewer left: the whole row jitters.
            if (currentHealth + absorption <= 4) yo += m_random.NextInt(2);
            // Regeneration: one heart at a time bobs up.
            if (containerIndex < healthContainerCount && containerIndex == heartOffsetIndex) yo -= 2;

            graphics.BlitSprite(HeartSprite(HeartType::Container, isHardcore, false, blink), xo, yo, 9, 9);
            const int halves = containerIndex * 2;
            const bool isAbsorptionHeart = containerIndex >= healthContainerCount;
            if (isAbsorptionHeart) {
                const int absorptionHalves = halves - maxHealthHalvesCount;
                if (absorptionHalves < absorption) {
                    const bool halfHeart = absorptionHalves + 1 == absorption;
                    graphics.BlitSprite(HeartSprite(type == HeartType::Withered ? type : HeartType::Absorbing,
                                                    isHardcore, halfHeart, false), xo, yo, 9, 9);
                }
            }
            // The blink draws the OLD health underneath, so a lost heart
            // flashes where it was …
            if (blink && halves < oldHealth) {
                const bool halfHeart = halves + 1 == oldHealth;
                graphics.BlitSprite(HeartSprite(type, isHardcore, halfHeart, true), xo, yo, 9, 9);
            }
            // … and the current health on top.
            if (halves < currentHealth) {
                const bool halfHeart = halves + 1 == currentHealth;
                graphics.BlitSprite(HeartSprite(type, isHardcore, halfHeart, false), xo, yo, 9, 9);
            }
        }
    }

    // MC Hud.extractFood: the hunger effect's sheet, and the shake that
    // starts once saturation is gone — every (food*3+1)th tick a shank moves
    // a pixel, so the emptier the bar the more it trembles.
    void HudRenderer::RenderFood(GuiGraphics& graphics, int yLineBase, int xRight) {
        using Network::SetHealthS2CPacket;
        const int food = m_food;
        const bool hunger = (m_hudFlags & SetHealthS2CPacket::kFlagHunger) != 0;
        const char* empty = hunger ? FOOD_EMPTY_HUNGER_SPRITE : FOOD_EMPTY_SPRITE;
        const char* half  = hunger ? FOOD_HALF_HUNGER_SPRITE  : FOOD_HALF_SPRITE;
        const char* full  = hunger ? FOOD_FULL_HUNGER_SPRITE  : FOOD_FULL_SPRITE;
        for (int i = 0; i < 10; ++i) {
            int yo = yLineBase;
            if (m_saturation <= 0.0f && m_tickCount % (food * 3 + 1) == 0) {
                yo += m_random.NextInt(3) - 1;
            }
            const int xo = xRight - i * 8 - 9;
            graphics.BlitSprite(empty, xo, yo, 9, 9);
            if (i * 2 + 1 < food)  graphics.BlitSprite(full, xo, yo, 9, 9);
            if (i * 2 + 1 == food) graphics.BlitSprite(half, xo, yo, 9, 9);
        }
    }

    // MC Hud.extractAirBubbles: full bubbles, the one bursting at the
    // water line, and the empty ones — which wobble once every bubble is
    // gone and the drowning damage is landing. The pop sound waits on the
    // sound system.
    void HudRenderer::RenderAir(GuiGraphics& graphics, int yLineAir, int xRight) {
        const int maxAir = std::max(m_maxAir, 1);
        const int curAir = std::clamp(m_air, 0, maxAir);
        const bool isUnderWater = m_isUnderWater;
        if (!isUnderWater && curAir >= maxAir) return;
        const auto bubble = [&](int tickOffset) {
            return static_cast<int>(std::ceil(static_cast<float>((curAir + tickOffset) * 10) /
                                              static_cast<float>(maxAir)));
        };
        const int emptyDelay = (curAir != 0 && isUnderWater) ? 1 : 0;   // getEmptyBubbleDelayDuration
        const int fullAirBubbles = bubble(-2);
        const int poppingAirBubblePosition = bubble(0);
        const int emptyAirBubbles = 10 - bubble(emptyDelay);
        const bool isPoppingBubble = fullAirBubbles != poppingAirBubblePosition;
        for (int airBubble = 1; airBubble <= 10; ++airBubble) {
            const int xo = xRight - (airBubble - 1) * 8 - 9;
            if (airBubble <= fullAirBubbles) {
                graphics.BlitSprite(AIR_SPRITE, xo, yLineAir, 9, 9);
            } else if (isPoppingBubble && airBubble == poppingAirBubblePosition && isUnderWater) {
                graphics.BlitSprite(AIR_POPPING_SPRITE, xo, yLineAir, 9, 9);
            } else if (airBubble > 10 - emptyAirBubbles) {
                const int wobble = (emptyAirBubbles == 10 && m_tickCount % 2 == 0) ? m_random.NextInt(2) : 0;
                graphics.BlitSprite(AIR_EMPTY_SPRITE, xo, yLineAir + wobble, 9, 9);
            }
        }
    }

    void HudRenderer::RenderExperienceBar(GuiGraphics& graphics) {
        int screenCenter = graphics.GuiWidth() / 2;
        int y = graphics.GuiHeight() - 32 + 3; // MC positions XP bar here

        // Background (full width)
        graphics.BlitSprite(XP_BAR_BG_SPRITE, screenCenter - 91, y, 182, 5);

        // Progress fill
        if (m_experience > 0.0f) {
            int progressWidth = static_cast<int>(m_experience * 182.0f);
            if (progressWidth > 0) {
                graphics.BlitSprite(XP_BAR_PROGRESS_SPRITE, 182, 5,
                                   0, 0, screenCenter - 91, y, progressWidth, 5);
            }
        }
    }

    // ========================================================================
    // Experience Level Number (MC: centered above XP bar)
    // ========================================================================

    void HudRenderer::RenderExperienceLevel(GuiGraphics& graphics) {
        if (m_experienceLevel <= 0) return;

        std::string levelStr = std::to_string(m_experienceLevel);
        int x = graphics.GuiWidth() / 2;
        int y = graphics.GuiHeight() - 31 - 4;

        // MC renders XP level in green (0xFF80FF20) with black shadow
        graphics.DrawCenteredString(levelStr, x, y, 0xFF80FF20);
    }

} // namespace Render
