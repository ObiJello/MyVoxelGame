// File: src/common/data/DataComponents.hpp
//
// Mirrors net/minecraft/core/component/DataComponents.java — the registry of
// every DataComponentType. Components are added here as features need them;
// the eventual MC-parity list is ~50 entries.
//
// Source line numbers cited per declaration so the C++ side can be diffed
// against MC's source.
// ── Network id table ────────────────────────────────────────────────────────
// Every component that crosses the wire gets a stable id here, passed as the
// second constructor argument in DataComponents.cpp. These are OUR bespoke
// protocol ids — deliberately NOT MC's registry ordinals (both endpoints ship
// in one binary, so the only requirement is stability within a build).
// 0 is reserved for "never serialized".
//
//    1  ENCHANTMENT_GLINT_OVERRIDE     8  CONSUMABLE
//    2  STORED_ENCHANTMENTS            9  FOOD
//    3  TOOL                          10  USE_REMAINDER
//    4  CUSTOM_NAME                   11  EQUIPPABLE
//    5  ITEM_NAME                     12  BLOCKS_ATTACKS
//    6  LORE                          13  BUNDLE_CONTENTS
//    7  RARITY                        14  SULFUR_CUBE_BUCKET
//   15  POTION_CONTENTS               16  POTION_DURATION_SCALE
//   17  SUSPICIOUS_STEW_EFFECTS       18  WRITTEN_BOOK_CONTENT
//   19  WRITABLE_BOOK_CONTENT         20  DYED_COLOR
//   21  DAMAGE                        22  MAX_DAMAGE
//   23  UNBREAKABLE                   24  REPAIR_COST
//   25  ENCHANTMENTS                  26  REPAIRABLE
//   27  ENCHANTABLE                   28  WEAPON
//   29  BREAK_SOUND                   30  DAMAGE_RESISTANT
//   40  PAINTING_VARIANT  (40, clear of the ids after 30 parallel work claims)
//   64  BUCKET_ENTITY_DATA            65  AXOLOTL_VARIANT
//   66  SALMON_SIZE                   67  TROPICAL_FISH_PATTERN
//   68  TROPICAL_FISH_BASE_COLOR      69  TROPICAL_FISH_PATTERN_COLOR
//   50  MAP_ID                        51  MAP_DECORATIONS
//   52  MAP_POST_PROCESSING           53  MAP_COLOR
//   76  FIREWORKS                     77  FIREWORK_EXPLOSION
//   78  CHARGED_PROJECTILES
//   84  OMINOUS_BOTTLE_AMPLIFIER      88  BANNER_PATTERNS
//   89  INSTRUMENT                    90  POT_DECORATIONS
//   91  CONTAINER
//  100  PORTAL_GUN_NEXT_COLOR        101  PORTAL_GUN_INSTANCE_ID
#pragma once

#include "DataComponentType.hpp"
#include "../world/enchantment/ItemEnchantments.hpp"
#include "../entity/MiningTier.hpp"
#include "../entity/Item.hpp"              // ItemStack (UseRemainder), ItemUseAnimation
#include "../entity/EquipmentSlot.hpp"
#include "../entity/alchemy/Potions.hpp"   // PotionContents, SuspiciousStewEffects
#include "BookContent.hpp"                 // WrittenBookContent, WritableBookContent
#include "../world/map/MapTypes.hpp"       // Maps::MapDecorations, Maps::MapPostProcessing
#include "FireworkExplosion.hpp"            // FireworkExplosion (FIREWORKS / FIREWORK_EXPLOSION)
#include "../core/Features.hpp"
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Game {

    // Mirrors net.minecraft.world.item.component.Tool (a record carrying the
    // mining-speed table + correct-tool tier). We collapse MC's "rules list"
    // (per-block-tag overrides) into a single (toolType, tier, miningSpeed)
    // tuple — sufficient for vanilla tools where the speed is uniform across
    // the entire mineable/<tag> set.
    struct Tool {
        ToolType   type        = ToolType::None;
        MiningTier tier        = MiningTier::Wood;
        float      miningSpeed = 1.0f;
        // MC Tool.damagePerBlock (codec default 1): the wear Item.mineBlock
        // deals per block with a non-zero destroy time. Swords, the mace and
        // the trident carry 2 (ToolMaterial.applySwordProperties, their
        // createToolProperties).
        int        damagePerBlock = 1;
    };

    // Mirrors the Weapon record — world/item/component/Weapon.java:
    // (itemDamagePerAttack, disableBlockingForSeconds). The wear a landed
    // melee hit deals the held item (ItemStack.postHurtEnemy); 1 for swords,
    // spears, the mace and the trident, 2 for every other tool
    // (ToolMaterial.applyToolProperties), the axe's 5 s shield disable.
    struct Weapon {
        int   itemDamagePerAttack       = 1;
        float disableBlockingForSeconds = 0.0f;
    };

    // Mirrors the Repairable record — world/item/enchantment/Repairable.java:
    // the HolderSet<Item> an anvil / crafting-grid repair accepts. Entries are
    // the HolderSet's raw form: "#ns:tag" or "ns:item" (a material's repair
    // tag, the elytra's phantom membrane, the mace's breeze rod). Membership
    // is resolved per item through DataTags at query time
    // (IsValidRepairItem).
    struct Repairable {
        std::vector<std::string> items;
    };

    // Mirrors world/item/consume_effects/ConsumeEffect.java — a sealed
    // interface with 5 record implementations. Data-only here: the appliers
    // are log-stubbed until the status-effect / teleport systems exist, but
    // the effect LIST is carried faithfully so food definitions are complete.
    struct ConsumeEffect {
        enum class Type : uint8_t {
            ApplyStatusEffects   = 0,  // ApplyStatusEffectsConsumeEffect.java
            RemoveStatusEffects  = 1,  // RemoveStatusEffectsConsumeEffect.java
            ClearAllStatusEffects= 2,  // ClearAllStatusEffectsConsumeEffect.java
            TeleportRandomly     = 3,  // TeleportRandomlyConsumeEffect.java
            PlaySound            = 4,  // PlaySoundConsumeEffect.java
        };
        Type        type = Type::PlaySound;
        // Freeform payload until the target systems exist: effect slugs +
        // durations for the status types, sound event name for PlaySound,
        // diameter for TeleportRandomly. Logged on consume.
        std::string payload;
    };

    // Mirrors the Consumable record — Consumable.java:32:
    //   (consumeSeconds, animation, sound, hasConsumeParticles, onConsumeEffects)
    struct Consumable {
        float                      consumeSeconds      = 1.6f;  // DEFAULT_CONSUME_SECONDS (:33)
        ItemUseAnimation           animation           = ItemUseAnimation::EAT;
        std::string                sound               = "entity.generic.eat"; // Holder<SoundEvent> → name (log-stub)
        bool                       hasConsumeParticles = true;
        std::vector<ConsumeEffect> onConsumeEffects;

        // Mirrors Consumable.consumeTicks (:81-83).
        int consumeTicks() const { return static_cast<int>(consumeSeconds * 20.0f); }
    };

    // Mirrors the FoodProperties record — FoodProperties.java:22:
    //   (nutrition, saturation, canAlwaysEat)
    // NOTE: `saturation` is the FINAL saturation value (MC's Builder converts
    // saturationModifier via FoodConstants.saturationByModifier at build time
    // — FoodProperties.java:60-62); our FoodDefs table does the same.
    // MC DataComponents.SULFUR_CUBE_CONTENT (the swallowed block, an
    // ItemStackTemplate) + the slice of BUCKET_ENTITY_DATA a sulfur cube
    // writes (Bucketable.saveDefaultDataToBucketTag + SulfurCube.
    // saveToBucketTag: age, age_locked; NoAI). One component here — the
    // bucket of sulfur cube is the only item that carries either.
    struct SulfurCubeBucketData {
        std::string bodyItem;      // item slug of the swallowed block, "" = none
        int         age = 0;
        bool        ageLocked = false;
        bool        noAi = false;
    };

    // MC DataComponents.BUCKET_ENTITY_DATA (a CustomData compound) as the
    // mob buckets write it: Bucketable.saveDefaultDataToBucketTag's keys —
    // each boolean only written when true, so false here IS "absent" — plus
    // the per-type extras (Axolotl / Tadpole: Age, AgeLocked; Axolotl:
    // HuntingCooldown). The sulfur cube keeps its own SULFUR_CUBE_BUCKET.
    struct BucketEntityData {
        bool noAi = false;                     // "NoAI"
        bool silent = false;                   // "Silent"
        bool noGravity = false;                // "NoGravity"
        bool glowing = false;                  // "Glowing" (carried; the engine has no entity glowing tag)
        bool invulnerable = false;             // "Invulnerable"
        bool persistenceRequired = false;      // "PersistenceRequired"
        std::optional<float>   health;         // "Health"
        std::optional<int32_t> age;            // "Age"
        std::optional<bool>    ageLocked;      // "AgeLocked"
        std::optional<int64_t> huntingCooldown;// "HuntingCooldown"
    };

    struct FoodProperties {
        int   nutrition    = 0;
        float saturation   = 0.0f;
        bool  canAlwaysEat = false;
    };

    // Mirrors the UseRemainder record — UseRemainder.java:8. What the stack
    // converts into when fully used up (stew → bowl, honey bottle → glass
    // bottle, milk bucket → bucket).
    struct UseRemainder {
        ItemStack convertInto{};
    };

    // Mirrors the Rarity enum — Rarity.java:13-16 (id, name, ChatFormatting
    // color). Ids are wire-stable (Rarity.STREAM_CODEC id-mapper).
    enum class Rarity : uint8_t {
        COMMON   = 0,   // WHITE
        UNCOMMON = 1,   // YELLOW
        RARE     = 2,   // AQUA
        EPIC     = 3,   // LIGHT_PURPLE
    };

    // The tooltip name-line color per rarity — MC ChatFormatting ARGB values.
    inline uint32_t RarityColorARGB(Rarity rarity) {
        switch (rarity) {
            case Rarity::UNCOMMON: return 0xFFFFFF55;  // YELLOW
            case Rarity::RARE:     return 0xFF55FFFF;  // AQUA
            case Rarity::EPIC:     return 0xFFFF55FF;  // LIGHT_PURPLE
            case Rarity::COMMON:
            default:               return 0xFFFFFFFF;  // WHITE
        }
    }

    // Mirrors the ItemLore record — ItemLore.java (list of Components; plain
    // strings here — no rich-text). MAX_LINES = 256 (ItemLore.java:22),
    // enforced at the wire decode.
    struct ItemLore {
        std::vector<std::string> lines;
    };

    // Mirrors the BundleContents record — BundleContents.java:21-29. `items`
    // is newest-first (Mutable.tryInsert adds at index 0). `selectedItem` is
    // the client-side scroll selection and is NOT serialized (matches MC's
    // STREAM_CODEC, which carries only the item list). Weight math lives in
    // BundleBehavior.cpp (needs ItemRegistry + recursion for nested bundles).
    struct BundleContents {
        std::vector<ItemStack> items;
        int                    selectedItem = -1;
    };

    // Mirrors the Equippable record — Equippable.java:32, every field. The
    // first four keep their historical order so the aggregate initialisers
    // ({slot, sound, swappable}) stay valid; the rest follow with the
    // Builder's defaults (Equippable.Builder:188-199).
    struct Equippable {
        EquipmentSlot slot       = EquipmentSlot::HEAD;
        std::string   equipSound = "item.armor.equip_generic"; // Holder<SoundEvent> → its id
        bool          swappable  = true;   // right-click auto-equip allowed
        // MC damageOnHurt (default true): the piece wears when its wearer is
        // hurt (LivingEntity.doHurtEquipment). False on the elytra.
        bool          damageOnHurt = true;
        // MC assetId — the EquipmentAsset key ("saddle", "iron",
        // "white_carpet", "red_harness"...) the render layers look up; ""
        // is Optional.empty().
        std::string   assetId;
        // MC cameraOverlay — the screen overlay while worn ("misc/pumpkinblur");
        // "" is Optional.empty().
        std::string   cameraOverlay;
        // MC allowedEntities (Optional<HolderSet<EntityType>>): empty is
        // Optional.empty() — anyone may wear it. Otherwise holder-set
        // entries, "#minecraft:can_equip_saddle" for a tag, a bare
        // "minecraft:llama" for a type (canBeEquippedBy).
        std::vector<std::string> allowedEntities;
        bool          dispensable     = true;    // EquipmentDispenseItemBehavior may put it on
        bool          equipOnInteract = false;   // ItemStack.interactLivingEntity → equipOnTarget
        bool          canBeSheared    = false;   // Mob.attemptToShearEquipment takes it off
        std::string   shearingSound   = "item.shears.snip";   // SoundEvents.SHEARS_SNIP

        // MC Equippable.canBeEquippedBy(type): no list, or `entityTypeId`
        // ("minecraft:horse" or a bare "horse") is in it — directly or
        // through a listed entity-type tag (data/<ns>/tags/entity_type).
        bool CanBeEquippedBy(std::string_view entityTypeId) const;
    };

    // Mirrors the BlocksAttacks record — BlocksAttacks.java:30. The full data
    // shape is carried (so shield definitions match Items.java verbatim) but
    // the damage math is unused — no combat system. What IS consumed:
    // blockDelayTicks() gates ServerPlayer::isBlocking(), and the sound names
    // are log-stub fodder.
    struct BlocksAttacks {
        float blockDelaySeconds    = 0.0f;
        float disableCooldownScale = 1.0f;
        // DamageReduction record — BlocksAttacks.java:89 (type filter omitted
        // — no damage-type registry).
        struct DamageReduction {
            float horizontalBlockingAngle = 90.0f;
            float base   = 0.0f;
            float factor = 1.0f;
        };
        std::vector<DamageReduction> damageReductions{DamageReduction{}};
        // ItemDamageFunction record — BlocksAttacks.java:106; DEFAULT {1,0,1} (:117).
        struct ItemDamageFunction {
            float threshold = 1.0f;
            float base      = 0.0f;
            float factor    = 1.0f;
        };
        ItemDamageFunction itemDamage{};
        std::string blockSound;     // Optional<Holder<SoundEvent>> → name ("" = none)
        std::string disableSound;

        // BlocksAttacks.blockDelayTicks — :71-73.
        int blockDelayTicks() const {
            return static_cast<int>(std::round(blockDelaySeconds * 20.0f));
        }
    };


    // Mirrors the Fireworks record — world/item/component/Fireworks.java:
    // (flightDuration, explosions). The rocket's FIREWORKS component: the
    // fuel count the rocket recipe put in (FireworkRocketEntity's lifetime is
    // 10 * (flightDuration + 1) + random) and the stars it throws.
    // ExtraCodecs.UNSIGNED_BYTE on disk, VAR_INT on the wire; at most
    // MAX_EXPLOSIONS stars.
    struct Fireworks {
        static constexpr size_t kMaxExplosions = 256;   // Fireworks.MAX_EXPLOSIONS
        int                            flightDuration = 0;
        std::vector<FireworkExplosion> explosions;

        bool operator==(const Fireworks& o) const {
            return flightDuration == o.flightDuration && explosions == o.explosions;
        }
    };

    // Mirrors the ChargedProjectiles record — world/item/component/
    // ChargedProjectiles.java: the item-stack templates a loaded crossbow
    // holds (one, or three with Multishot). EMPTY = not charged
    // (CrossbowItem.isCharged). At most MAX_SIZE entries.
    struct ChargedProjectiles {
        static constexpr size_t kMaxSize = 1024;   // ChargedProjectiles.MAX_SIZE
        std::vector<ItemStack> items;

        bool IsEmpty() const { return items.empty(); }
        // MC ChargedProjectiles.contains(item).
        bool Contains(ItemID item) const {
            for (const ItemStack& s : items) if (s.itemId == item) return true;
            return false;
        }
    };

    // Mirrors the BannerPatternLayers record — world/level/block/entity/
    // BannerPatternLayers.java: the pattern layers painted over a banner's
    // (or shield's) base colour, bottom to top. A layer is a banner_pattern
    // registry id ("minecraft:rhombus") and a DyeColor ordinal (white 0 …
    // black 15). The saved form is a list of {pattern, color} compounds;
    // the wire carries the id string and the colour's id.
    struct BannerPatternLayer {
        std::string pattern;
        uint8_t     color = 0;
    };
    struct BannerPatternLayers {
        std::vector<BannerPatternLayer> layers;
        bool IsEmpty() const { return layers.empty(); }
    };

    // MC PotDecorations (world/level/block/entity/PotDecorations.java): the
    // item on each of a decorated pot's four sides — a pottery sherd or a
    // brick — in the record's order back, left, right, front. Items::Air is
    // Optional.empty() (the blank side). EMPTY is all four empty.
    struct PotDecorations {
        std::array<ItemID, 4> sides{};   // back, left, right, front
        bool IsEmpty() const {
            for (ItemID id : sides) if (id != 0) return false;
            return true;
        }
        bool operator==(const PotDecorations& o) const { return sides == o.sides; }
    };

    // MC ItemContainerContents: a container item's stacks by slot (a
    // shulker box's inventory), EMPTY holes kept, trailing empties dropped
    // (ItemContainerContents.fromItems). At most 256 slots.
    struct ItemContainerContents {
        std::vector<ItemStack> items;
        bool IsEmpty() const {
            for (const ItemStack& s : items) if (!s.IsEmpty()) return false;
            return true;
        }
    };
}

namespace Game::DataComponents {

    // Explicit override for whether an item shows the enchantment "foil" (purple
    // glint) regardless of whether it has stored enchantments. Mirrors MC
    // DataComponents.java:125. Used by `enchanted_book` (Items.java:2854) so the
    // book glints even when its STORED_ENCHANTMENTS is still EMPTY.
    extern const DataComponentType<bool> ENCHANTMENT_GLINT_OVERRIDE;

    // Enchantments stored on an enchanted_book (NOT the same as ENCHANTMENTS,
    // which lives on the actual tool/armor — DataComponents.java:117). MC
    // DataComponents.java:146. Default for enchanted_book is ItemEnchantments::EMPTY
    // per Items.java:2854.
    extern const DataComponentType<ItemEnchantments> STORED_ENCHANTMENTS;

    // The TOOL component carries the mining-speed table + correct-tool tier.
    // Mirrors MC DataComponents.java:108 + world/item/component/Tool.java.
    // Present on every tool item (pickaxe/axe/shovel/hoe/sword/shears); absent
    // on non-tool items (Item.getDestroySpeed returns 1.0 in that case).
    extern const DataComponentType<Tool> TOOL;

    // Everything edible/drinkable — the eat-timer + animation + sound +
    // on-consume effects. Mirrors DataComponents.CONSUMABLE (the modern
    // eating system; drives the base Item.use dispatch step 1).
    extern const DataComponentType<Consumable> CONSUMABLE;

    // Nutrition/saturation restored when a CONSUMABLE with this component
    // finishes. Mirrors DataComponents.FOOD. Applied by the FoodProperties
    // ConsumableListener (FoodProperties.java:26-34) → ServerPlayer's FoodData.
    extern const DataComponentType<FoodProperties> FOOD;

    // What the stack converts into when used up. Mirrors
    // DataComponents.USE_REMAINDER (applied in
    // ItemStack.applyAfterUseComponentSideEffects, ItemStack.java:332-348).
    extern const DataComponentType<UseRemainder> USE_REMAINDER;

    // Anvil-renamed display name. Mirrors DataComponents.CUSTOM_NAME (a
    // Component in MC; plain string here — MC renders it italic, our font
    // has no italics). Wins over ITEM_NAME in the tooltip name line.
    extern const DataComponentType<std::string> CUSTOM_NAME;
    extern const DataComponentType<SulfurCubeBucketData> SULFUR_CUBE_BUCKET;
    // MC DataComponents.BUCKET_ENTITY_DATA — what a fish / axolotl / tadpole
    // bucket carries of its mob (Bucketable.saveToBucketTag).
    extern const DataComponentType<BucketEntityData> BUCKET_ENTITY_DATA;
    // MC DataComponents.AXOLOTL_VARIANT — the bucketed axolotl's colour
    // (Axolotl.Variant id), applied to the axolotl the bucket releases.
    extern const DataComponentType<int32_t> AXOLOTL_VARIANT;
    // MC DataComponents.SALMON_SIZE ("salmon/size") — the bucketed salmon's
    // Salmon.Variant id (small 0, medium 1, large 2).
    extern const DataComponentType<int32_t> SALMON_SIZE;
    // MC DataComponents.TROPICAL_FISH_PATTERN / _BASE_COLOR / _PATTERN_COLOR
    // ("tropical_fish/pattern", "tropical_fish/base_color",
    // "tropical_fish/pattern_color") — the bucketed tropical fish's variant:
    // the TropicalFish.Pattern ORDINAL (TropicalFishVariants::Pattern) and
    // two DyeColor ids. The pattern is also a TooltipProvider (the
    // "Clownfish" / "Kob" + colours lines).
    extern const DataComponentType<int32_t> TROPICAL_FISH_PATTERN;
    extern const DataComponentType<int32_t> TROPICAL_FISH_BASE_COLOR;
    extern const DataComponentType<int32_t> TROPICAL_FISH_PATTERN_COLOR;

    // Data-driven base name override (potion variants etc.). Mirrors
    // DataComponents.ITEM_NAME. Falls between CUSTOM_NAME and the registry
    // display name.
    extern const DataComponentType<std::string> ITEM_NAME;

    // Tooltip lore lines (dark purple). Mirrors DataComponents.LORE.
    extern const DataComponentType<ItemLore> LORE;

    // Name-line color tier. Mirrors DataComponents.RARITY.
    extern const DataComponentType<Rarity> RARITY;

    // Which slot the item is worn in + right-click auto-equip. Mirrors
    // DataComponents.EQUIPPABLE (base Item.use dispatch step 2, Equippable
    // swap logic Equippable.java:54-83). Also drives the click-handler's
    // per-armor-slot mayPlace.
    extern const DataComponentType<Equippable> EQUIPPABLE;

    // Shield blocking config. Mirrors DataComponents.BLOCKS_ATTACKS (base
    // Item.use dispatch step 3 → startUsingItem; BLOCK use animation; the
    // 72000-tick "infinite" use duration).
    extern const DataComponentType<BlocksAttacks> BLOCKS_ATTACKS;

    // Bundle contents (nested stacks + client-side selection). Mirrors
    // DataComponents.BUNDLE_CONTENTS; behaviour in BundleBehavior.cpp.
    extern const DataComponentType<BundleContents> BUNDLE_CONTENTS;

    // What a potion / splash / lingering potion / tipped arrow holds — MC
    // DataComponents.POTION_CONTENTS (PotionContents.java). Default EMPTY on
    // all four items (Items.java); the creative tab and brewing set a
    // potion. Also a ConsumableListener: drinking applies it
    // (ConsumableBehavior::OnConsume).
    extern const DataComponentType<PotionContents> POTION_CONTENTS;

    // MC DataComponents.POTION_DURATION_SCALE — the lingering potion's 0.25
    // and the tipped arrow's 0.125 (Items.java); 1.0 when absent.
    extern const DataComponentType<float> POTION_DURATION_SCALE;

    // MC DataComponents.SUSPICIOUS_STEW_EFFECTS — the stew's effect list
    // (default EMPTY); a ConsumableListener like POTION_CONTENTS.
    extern const DataComponentType<SuspiciousStewEffects> SUSPICIOUS_STEW_EFFECTS;

    // MC DataComponents.WRITTEN_BOOK_CONTENT — a signed book's title,
    // author, generation and pages (text components). Set by signing a book
    // and quill, by the set_written_book_pages / set_book_cover loot
    // functions, and read from disk. A written_book with none opens nothing.
    extern const DataComponentType<WrittenBookContent> WRITTEN_BOOK_CONTENT;

    // MC DataComponents.WRITABLE_BOOK_CONTENT — a book and quill's plain
    // pages. Default EMPTY on writable_book (Items.java).
    extern const DataComponentType<WritableBookContent> WRITABLE_BOOK_CONTENT;

    // MC DataComponents.DYED_COLOR (DyedItemColor.rgb) — the dye on a piece
    // of leather armour. Written by the set_random_dyes loot function (the
    // leatherworker's dyed armour trades) and read by the "dye" item tint
    // (Game::ResolveItemLayerTint); absent = the tint's LEATHER_COLOR default.
    extern const DataComponentType<int32_t> DYED_COLOR;

    // MC DataComponents.PAINTING_VARIANT ("minecraft:painting/variant") — the
    // canvas a painting item hangs, as the variant's id ("minecraft:kebab";
    // Game::PaintingVariants resolves it). Absent = a random fitting
    // #placeable canvas (Painting::Create). Set on the creative tab's preset
    // paintings; the painting's own drop never carries it (MC dropItem).
    extern const DataComponentType<std::string> PAINTING_VARIANT;

    // ── Maps (MC DataComponents.MAP_ID / MAP_DECORATIONS / MAP_COLOR /
    // MAP_POST_PROCESSING) ────────────────────────────────────────────────
    // Which MapItemSavedData a filled map shows (MapId.id — data/map_<id>.dat).
    // Presence is what makes a stack a map in hand, in a frame and in the
    // tooltip (MC tests `has(MAP_ID)`), for filled_map and 26.3's
    // per-structure map items alike.
    extern const DataComponentType<int32_t> MAP_ID;

    // Markers pinned to the stack in world coordinates — the exploration
    // map's target "+" (MapItemSavedData.addTargetDecoration). Copied onto
    // the map's own decorations by tickCarriedBy.
    extern const DataComponentType<Maps::MapDecorations> MAP_DECORATIONS;

    // What a crafted map still has to become once taken (MapItem.
    // onCraftedPostProcess): LOCK (cartography glass pane), SCALE (paper).
    extern const DataComponentType<Maps::MapPostProcessing> MAP_POST_PROCESSING;

    // The RGB of the item icon's markings layer (MapItemColor, the
    // "minecraft:map_color" item tint). Absent = the tint's default.
    extern const DataComponentType<int32_t> MAP_COLOR;

    // ── Durability (MC DataComponents.java:122-128) ─────────────────────────
    // Item.Properties.durability(n) sets all three on the prototype: MAX_DAMAGE
    // n, DAMAGE 0, MAX_STACK_SIZE 1 (Item.java). The free functions in
    // Item.hpp (IsDamageableItem, GetDamageValue, HurtAndBreak, ...) are the
    // only readers; nothing else should poke DAMAGE directly.

    // Wear taken so far (MC DAMAGE, ExtraCodecs.NON_NEGATIVE_INT). The item's
    // default 0 lives in defaultComponents; SetDamageValue drops the stack's
    // override again once it matches it, as MC's patch map does.
    extern const DataComponentType<int32_t> DAMAGE;

    // Durability cap (MC MAX_DAMAGE, ExtraCodecs.POSITIVE_INT).
    extern const DataComponentType<int32_t> MAX_DAMAGE;

    // MC UNBREAKABLE — a Unit: presence is the whole value. The bool carried
    // is always true; an unbreakable stack never takes wear
    // (IsDamageableItem) and the tooltip says "Unbreakable".
    extern const DataComponentType<bool> UNBREAKABLE;

    // Prior-work penalty the anvil adds and doubles (MC REPAIR_COST; every
    // item's default is 0 — DataComponents.COMMON_ITEM_COMPONENTS — so an
    // absent value reads as 0).
    extern const DataComponentType<int32_t> REPAIR_COST;

    // Enchantments on the item itself — tools, weapons, armour (MC
    // DataComponents.java:155). Every MC item defaults to EMPTY
    // (COMMON_ITEM_COMPONENTS), so an absent component reads as EMPTY here.
    // Enchanted books keep theirs in STORED_ENCHANTMENTS instead; go through
    // EnchantmentHelper::SetEnchantments, which picks the right one.
    extern const DataComponentType<ItemEnchantments> ENCHANTMENTS;

    // What repairs the item in an anvil / grindstone combine (MC REPAIRABLE).
    extern const DataComponentType<Repairable> REPAIRABLE;

    // MC ENCHANTABLE (Enchantable.value) — the enchantability the table and
    // enchant_with_levels roll against. Absent = not enchantable.
    extern const DataComponentType<int32_t> ENCHANTABLE;

    // MC WEAPON — the wear a landed melee hit deals the item.
    extern const DataComponentType<Weapon> WEAPON;

    // MC BREAK_SOUND (a sound event holder; the id here). Absent reads as
    // the COMMON_ITEM_COMPONENTS default, entity.item.break — only the shield
    // and wolf armour carry their own.
    extern const DataComponentType<std::string> BREAK_SOUND;

    // MC DAMAGE_RESISTANT (DamageResistant.types, a damage-type tag; the tag
    // id here). Item.Properties.fireResistant() puts #minecraft:is_fire on
    // the netherite gear: a resistant piece takes no wear from the sources
    // it resists (ItemStack.canBeHurtBy). Only the durable items carry it so
    // far — the item entities' own fire immunity is not modelled.
    extern const DataComponentType<std::string> DAMAGE_RESISTANT;

    // MC DataComponents.POT_DECORATIONS — a decorated pot item's sides
    // (DecoratedPotBlockEntity.collectImplicitComponents / the pot's
    // copy_components drop, the crafting recipe); placing the pot hands them
    // to its block entity.
    extern const DataComponentType<PotDecorations> POT_DECORATIONS;

    // MC DataComponents.CONTAINER — a container item's contents: what a
    // shulker box keeps when broken (its loot table copies it from the block
    // entity) and puts back when placed (BaseContainerBlockEntity.
    // applyImplicitComponents).
    extern const DataComponentType<ItemContainerContents> CONTAINER;

    // MC DataComponents.INSTRUMENT (InstrumentComponent) — the instrument a
    // goat horn plays, as its registry id ("minecraft:ponder_goat_horn"; the
    // definitions are data/<ns>/instrument/*.json, common/entity/
    // Instruments.hpp). Default ponder on goat_horn (Items.java); the
    // pillager outpost's set_instrument loot function picks one of
    // #regular_goat_horns.
    extern const DataComponentType<std::string> INSTRUMENT;

    // MC DataComponents.FIREWORKS — a firework rocket's flight duration and
    // explosions (Fireworks.java). Default on firework_rocket is
    // Fireworks(1, []) (Items.java); the rocket recipe writes the fuel count
    // and the stars. A TooltipProvider ("Flight Duration: N", the stars).
    extern const DataComponentType<Fireworks> FIREWORKS;

    // MC DataComponents.FIREWORK_EXPLOSION — a firework star's explosion
    // (FireworkExplosion.java). Written by the star recipe; the fade recipe
    // adds fade colours. Its colours also tint the star's icon (the
    // "minecraft:firework" ItemTintSource).
    extern const DataComponentType<FireworkExplosion> FIREWORK_EXPLOSION;

    // MC DataComponents.CHARGED_PROJECTILES — what a loaded crossbow holds
    // (ChargedProjectiles.java). Default EMPTY on the crossbow (Items.java);
    // CrossbowItem loads it at full draw and shoots (and clears) it on use.
    extern const DataComponentType<ChargedProjectiles> CHARGED_PROJECTILES;

    // MC DataComponents.OMINOUS_BOTTLE_AMPLIFIER (OminousBottleAmplifier:
    // 0..4) — the Bad Omen level an ominous bottle gives: drinking it adds
    // BAD_OMEN at this amplifier for 120000 ticks (ConsumableBehavior), and
    // the tooltip shows that effect. The ominous bottle's default is 0
    // (Items.java); the set_ominous_bottle_amplifier loot function and the
    // creative tab's five bottles set the rest.
    inline constexpr int kOminousBottleEffectDuration = 120000;   // EFFECT_DURATION
    inline constexpr int kOminousBottleMaxAmplifier   = 4;        // MAX_AMPLIFIER
    extern const DataComponentType<int32_t> OMINOUS_BOTTLE_AMPLIFIER;

    // MC DataComponents.BANNER_PATTERNS (BannerPatternLayers) — the painted
    // layers of a banner item. The ominous banner (Raid.
    // getOminousBannerInstance, common/entity/raid/OminousBanner.hpp) is a
    // white banner carrying eight of them; a patrol leader wears it and the
    // raid captain test (Raider.isCaptain) compares against it.
    extern const DataComponentType<BannerPatternLayers> BANNER_PATTERNS;

    // ── TODO: future component types to register, in MC parity order ────────
    // Each one unlocks a chunk of behaviour by populating Item.use() base
    // dispatch (see Item.hpp ItemUseFn doc comment) and other systems.
    //
    //   (KINETIC_WEAPON / PIERCING_WEAPON / ATTACK_RANGE / ATTACK_ANIMATION
    //    are fixed per spear in vanilla and live as the spears' item
    //    properties — common/entity/SpearItem.hpp.)
    //
    // None are registered yet because we have no consumers (no food eating,
    // no armor equipping, no anvil renaming). When the consumer lands, add
    // the type AND its struct definition (mirror MC's record verbatim) here
    // and wire defaults onto the right items in ItemRegistry::Initialize.

#if ENABLE_PORTAL_GUN
    // ── Portal-gun per-stack state (custom non-MC components) ───────────────
    // These don't mirror anything in MC's DataComponents.java — they're
    // bespoke to our portal-gun feature. Toggled out via the central
    // feature flag so a "no portal gun" build sees no portal-related
    // component types at all.

    // Which color the gun fires NEXT. 0 = blue, 1 = orange.
    // Right-clicking toggles this on the held stack — see
    // src/common/entity/PortalGunBehavior.cpp::OnGunUseOn. Default 0 (blue
    // first) so a freshly-spawned gun matches Portal-game's "always-blue
    // first" muscle memory.
    extern const DataComponentType<uint8_t>  PORTAL_GUN_NEXT_COLOR;

    // Stable per-stack instance id. Lazily assigned by the server on the
    // gun's first shot via PortalRegistry::AllocId(); zero = unassigned.
    // Used as the key into the server's PortalRegistry map of (gunId →
    // PortalPair) so the gun's blue+orange pair persists across inventory
    // moves, dropping/picking-up, trades. Stack size 1 means we never have
    // to handle splitting (if we did, a child stack would inherit the same
    // id, sharing the pair — probably not what you'd want; so we don't).
    extern const DataComponentType<uint64_t> PORTAL_GUN_INSTANCE_ID;
#endif

} // namespace Game::DataComponents
