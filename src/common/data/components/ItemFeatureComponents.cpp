// File: src/common/data/components/ItemFeatureComponents.cpp
#include "ItemFeatureComponents.hpp"

#include "ComponentTooltips.hpp"
#include "common/entity/DyeColorUtil.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/JukeboxSongs.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/text/Language.hpp"
#include "common/world/banner/BannerPatterns.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>

namespace Game {

    namespace {

        std::string_view StripMinecraft(std::string_view id) {
            return id.rfind("minecraft:", 0) == 0 ? id.substr(10) : id;
        }

        std::string WithMinecraft(std::string_view id) {
            if (id.find(':') != std::string_view::npos) return std::string(id);
            return "minecraft:" + std::string(id);
        }

        // ── The trim registries, read from the data pack once ─────────────

        std::filesystem::path DataRoot() {
            if (const char* env = std::getenv("MC_DATA_ROOT")) return env;
            return "data";
        }

        struct TrimEntry {
            bool                                         exists = false;
            std::optional<Text::Component>               description;
            std::string                                  asset;        // material asset_name / pattern asset_id path
            std::unordered_map<std::string, std::string> overrides;    // equipment asset -> material asset
        };

        std::mutex g_trimMutex;
        std::unordered_map<std::string, TrimEntry> g_materials;
        std::unordered_map<std::string, TrimEntry> g_patterns;

        const TrimEntry& LoadTrim(std::unordered_map<std::string, TrimEntry>& cache, const char* folder,
                                  std::string_view rawId) {
            const std::string id = WithMinecraft(rawId);
            std::lock_guard<std::mutex> lock(g_trimMutex);
            auto it = cache.find(id);
            if (it != cache.end()) return it->second;
            TrimEntry entry;
            const size_t colon = id.find(':');
            const std::filesystem::path file =
                DataRoot() / id.substr(0, colon) / folder / (id.substr(colon + 1) + ".json");
            std::ifstream in(file);
            if (in) {
                nlohmann::json j;
                try { in >> j; } catch (const std::exception&) { j = nullptr; }
                if (j.is_object()) {
                    entry.exists = true;
                    if (j.contains("description")) entry.description = Text::FromJson(j["description"]);
                    if (j.contains("asset_name") && j["asset_name"].is_string()) {
                        entry.asset = j["asset_name"].get<std::string>();
                    } else if (j.contains("asset_id") && j["asset_id"].is_string()) {
                        entry.asset = std::string(StripMinecraft(j["asset_id"].get<std::string>()));
                    }
                    if (j.contains("override_armor_assets") && j["override_armor_assets"].is_object()) {
                        for (auto& [k, v] : j["override_armor_assets"].items()) {
                            if (v.is_string()) entry.overrides[std::string(StripMinecraft(k))] = v.get<std::string>();
                        }
                    }
                }
            }
            return cache.emplace(id, std::move(entry)).first->second;
        }

    } // namespace

    std::optional<Text::Component> TrimMaterialDescription(std::string_view id) {
        return LoadTrim(g_materials, "trim_material", id).description;
    }

    std::optional<Text::Component> TrimPatternDescription(std::string_view id) {
        return LoadTrim(g_patterns, "trim_pattern", id).description;
    }

    bool TrimMaterialExists(std::string_view id) { return LoadTrim(g_materials, "trim_material", id).exists; }
    bool TrimPatternExists(std::string_view id)  { return LoadTrim(g_patterns, "trim_pattern", id).exists; }

    std::string TrimMaterialAssetFor(std::string_view material, std::string_view equipmentAsset) {
        const TrimEntry& e = LoadTrim(g_materials, "trim_material", material);
        auto it = e.overrides.find(std::string(StripMinecraft(equipmentAsset)));
        return it != e.overrides.end() ? it->second : e.asset;
    }

    std::string TrimPatternAsset(std::string_view pattern) {
        const TrimEntry& e = LoadTrim(g_patterns, "trim_pattern", pattern);
        return e.asset.empty() ? std::string(StripMinecraft(pattern)) : e.asset;
    }

    // ── Helpers ────────────────────────────────────────────────────────────

    int DyeColorOf(const ItemStack& stack) {
        if (stack.IsEmpty()) return -1;
        const auto dye = stack.get(DataComponents::DYE);
        return dye && *dye >= 0 && *dye < 16 ? *dye : -1;
    }

    std::string PotteryPatternStem(ItemID item) {
        if (item == Items::Air) return {};
        const auto pattern = ItemRegistry::Get(item).defaultComponents.get(DataComponents::PROVIDES_POTTERY_PATTERN);
        if (!pattern || pattern->empty()) return {};
        // DecoratedPotPatterns.registerWithDefaultAsset: <id>_pottery_pattern.
        return std::string(StripMinecraft(*pattern)) + "_pottery_pattern";
    }

    std::vector<std::string> ProvidedBannerPatterns(const ItemStack& stack) {
        if (stack.IsEmpty()) return {};
        return stack.get(DataComponents::PROVIDES_BANNER_PATTERNS).value_or(std::vector<std::string>{});
    }

    UseResult CompassUseOn(const UseOnContext& ctx, ItemStack& stack) {
        // MC CompassItem.useOn.
        if (!ctx.world || !ctx.player) return UseResult::Pass;
        const glm::ivec3 pos = ctx.hitResult.blockPos;
        if (ctx.world->GetBlock(pos.x, pos.y, pos.z) != BlockID::Lodestone) return UseResult::Pass;
        // level.playSound(null, pos, LODESTONE_COMPASS_LOCK, PLAYERS) — a
        // null-player sound from the server, which the client's run skips.
        if (!ctx.world->IsClientSide()) {
            ctx.world->PlaySound(nullptr, pos, SoundEvents::LODESTONE_COMPASS_LOCK, SoundSource::Players, 1.0f, 1.0f);
        }
        const bool replaceExistingStack = !ctx.player->isCreative() && stack.count == 1;
        LodestoneTracker target;
        target.target = GlobalPos{std::string(DimensionRegistryName(ctx.world->GetDimension())), pos};
        target.tracked = true;
        if (replaceExistingStack) {
            stack.components.set(DataComponents::LODESTONE_TRACKER, target);
        } else {
            // transmuteCopy(COMPASS, 1): the stack's components on a fresh
            // compass, which takes the tracker.
            ItemStack lodestoneCompass = stack;
            lodestoneCompass.itemId = Items::Compass;
            lodestoneCompass.count = 1;
            lodestoneCompass.components.set(DataComponents::LODESTONE_TRACKER, target);
            if (!ctx.player->isCreative() && --stack.count <= 0) stack.Clear();
            if (!ctx.world->IsClientSide()) ctx.player->AddItemOrDrop(lodestoneCompass);
        }
        return UseResult::Success;
    }

    bool TickLodestoneTracker(ItemStack& stack, DimensionId dimension, World& world) {
        // LodestoneTracker.tick: tracked, with a target in this dimension
        // whose lodestone is gone (outside the world, or no lodestone POI
        // there) -> the target is dropped (the needle spins).
        const auto tracker = stack.get(DataComponents::LODESTONE_TRACKER);
        if (!tracker || !tracker->tracked || !tracker->target) return false;
        if (tracker->target->dimension != DimensionRegistryName(dimension)) return false;
        const glm::ivec3 p = tracker->target->pos;
        const bool inBounds = p.y >= DimensionMinY(dimension) &&
                              p.y < DimensionMinY(dimension) + DimensionLogicalHeight(dimension) + 64 &&
                              std::abs(p.x) < 30000000 && std::abs(p.z) < 30000000;
        // The POI answer needs the chunk; an unloaded one keeps its target.
        if (inBounds && !world.IsPositionLoaded(p.x, p.y, p.z)) return false;
        if (inBounds && world.GetBlock(p.x, p.y, p.z) == BlockID::Lodestone) return false;
        stack.components.set(DataComponents::LODESTONE_TRACKER, LodestoneTracker{std::nullopt, true});
        return true;
    }

    UseResult KnowledgeBookUse(ILevelWrite* world, IUsePlayer* player, uint32_t /*hand*/, ItemStack& stack) {
        // MC KnowledgeBookItem.use: the book is spent either way; with no
        // recipes it fails. awardRecipes has nothing to unlock here — the
        // engine's recipe book shows every recipe from the start.
        if (!player) return UseResult::Pass;
        const std::vector<std::string> recipes = stack.get(DataComponents::RECIPES).value_or(std::vector<std::string>{});
        if (!player->isCreative() && --stack.count <= 0) stack.Clear();
        if (recipes.empty()) return UseResult::Fail;
        (void)world;
        return UseResult::Success;
    }

    // ── Defaults ───────────────────────────────────────────────────────────

    void ItemRegistry_RegisterItemFeatureDefaults(std::unordered_map<ItemID, Item>& pureItems) {
        auto find = [&](ItemID id) -> Item* {
            auto it = pureItems.find(id);
            return it == pureItems.end() ? nullptr : &it->second;
        };

        // .jukeboxPlayable(JukeboxSongs.X) on every music disc.
        for (int i = 0; i < JukeboxSongs::Count(); ++i) {
            const JukeboxSong* song = JukeboxSongs::Get(i);
            if (!song) continue;
            if (Item* item = find(song->discItem)) {
                item->defaultComponents.set(DataComponents::JUKEBOX_PLAYABLE, "minecraft:" + std::string(song->key));
            }
        }

        // The dyes' DYE (Items.java DYE collection, DyeColor order).
        for (int c = 0; c < 16; ++c) {
            if (Item* item = find(DyeItemOfColor(static_cast<uint8_t>(c)))) {
                item->defaultComponents.set(DataComponents::DYE, static_cast<int32_t>(c));
            }
        }

        // Items.java .trimMaterial(TrimMaterials.X).
        static constexpr std::pair<ItemID, const char*> kTrimMaterials[] = {
            {Items::Redstone, "minecraft:redstone"},      {Items::Diamond, "minecraft:diamond"},
            {Items::Emerald, "minecraft:emerald"},        {Items::LapisLazuli, "minecraft:lapis"},
            {Items::Quartz, "minecraft:quartz"},          {Items::AmethystShard, "minecraft:amethyst"},
            {Items::IronIngot, "minecraft:iron"},         {Items::CopperIngot, "minecraft:copper"},
            {Items::GoldIngot, "minecraft:gold"},         {Items::NetheriteIngot, "minecraft:netherite"},
            {Items::ResinBrick, "minecraft:resin"},
        };
        for (const auto& [id, material] : kTrimMaterials) {
            if (Item* item = find(id)) item->defaultComponents.set(DataComponents::PROVIDES_TRIM_MATERIAL, std::string(material));
        }

        for (auto& [id, item] : pureItems) {
            const std::string_view slug = ItemRegistry::Slug(id);
            // .potPattern(DecoratedPotPatterns.X) on every <x>_pottery_sherd.
            constexpr std::string_view kSherd = "_pottery_sherd";
            if (slug.size() > kSherd.size() && slug.substr(slug.size() - kSherd.size()) == kSherd) {
                item.defaultComponents.set(DataComponents::PROVIDES_POTTERY_PATTERN,
                                           "minecraft:" + std::string(slug.substr(0, slug.size() - kSherd.size())));
            }
            // The banner pattern items' PROVIDES_BANNER_PATTERNS (their
            // #pattern_item/<name> tag).
            const std::string tag = BannerPatterns::ProvidedTagOf(std::string(slug));
            if (!tag.empty()) {
                item.defaultComponents.set(DataComponents::PROVIDES_BANNER_PATTERNS, std::vector<std::string>{tag});
            }
        }

        // KNOWLEDGE_BOOK: .component(RECIPES, List.of()) and its use.
        if (Item* book = find(Items::KnowledgeBook)) {
            book->defaultComponents.set(DataComponents::RECIPES, std::vector<std::string>{});
            book->use = &KnowledgeBookUse;
        }
        // CompassItem.useOn.
        if (Item* compass = find(Items::Compass)) compass->useOn = &CompassUseOn;
    }

} // namespace Game

// ── Tooltips ────────────────────────────────────────────────────────────────

namespace Game {

    namespace {

        using ComponentTooltips::Line;
        using ComponentTooltips::Slot;

        Line RichLine(Text::Component c, uint32_t argb) {
            std::string text = Text::GetString(c);
            return Line{std::move(text), argb, std::move(c)};
        }

        // JukeboxPlayable.addToTooltip: the song's description, grey.
        void JukeboxTooltip(const ItemStack& stack, const ComponentTooltips::Context&, std::vector<Line>& out) {
            const auto song = stack.get(DataComponents::JUKEBOX_PLAYABLE);
            if (!song) return;
            const JukeboxSong* s = JukeboxSongs::Get(JukeboxSongs::IdFromKey(StripMinecraft(*song)));
            if (!s) return;
            out.push_back({s->Description(), ComponentTooltips::kGray, std::nullopt});
        }

        // ArmorTrim.addToTooltip: "Upgrade:" grey, then " " + the pattern's
        // description in the material's style, then " " + the material's.
        void TrimTooltip(const ItemStack& stack, const ComponentTooltips::Context&, std::vector<Line>& out) {
            const auto trim = stack.get(DataComponents::TRIM);
            if (!trim) return;
            out.push_back({Language::Get("item.minecraft.smithing_template.upgrade"), ComponentTooltips::kGray, std::nullopt});
            const std::optional<Text::Component> material = TrimMaterialDescription(trim->material);
            const std::optional<Text::Component> pattern = TrimPatternDescription(trim->pattern);
            Text::Component patternLine = Text::Component::Literal(" ");
            Text::Component patternText = pattern ? *pattern : Text::Component::Literal(trim->pattern);
            if (material) patternText.style = material->style;   // copyWithStyle(material)
            patternLine.Append(std::move(patternText));
            out.push_back(RichLine(std::move(patternLine), ComponentTooltips::kWhite));
            Text::Component materialLine = Text::Component::Literal(" ");
            materialLine.Append(material ? *material : Text::Component::Literal(trim->material));
            out.push_back(RichLine(std::move(materialLine), ComponentTooltips::kWhite));
        }

        const ComponentTooltips::Registrar kJukeboxTooltip{Slot::JukeboxPlayable, &DataComponents::JUKEBOX_PLAYABLE, &JukeboxTooltip};
        const ComponentTooltips::Registrar kTrimTooltip{Slot::Trim, &DataComponents::TRIM, &TrimTooltip};

    } // namespace

} // namespace Game

// ── Wire codecs + registrations ─────────────────────────────────────────────

namespace Game::DataComponents {

    namespace {

        void SerTrim(Network::PacketBuffer& b, const ArmorTrim& v) {
            b.WriteString(v.material);
            b.WriteString(v.pattern);
        }
        ArmorTrim DeTrim(Network::PacketReader& r) {
            ArmorTrim v;
            v.material = r.ReadString();
            v.pattern = r.ReadString();
            return v;
        }

        void SerString(Network::PacketBuffer& b, const std::string& v) { b.WriteString(v); }
        std::string DeString(Network::PacketReader& r) { return r.ReadString(); }

        void SerStrings(Network::PacketBuffer& b, const std::vector<std::string>& v) {
            b.WriteVarInt(static_cast<uint32_t>(v.size()));
            for (const std::string& s : v) b.WriteString(s);
        }
        std::vector<std::string> DeStrings(Network::PacketReader& r) {
            const uint32_t n = r.ReadVarInt();
            if (n > 65536) throw std::runtime_error("string list too long");
            std::vector<std::string> v;
            v.reserve(n);
            for (uint32_t i = 0; i < n; ++i) v.push_back(r.ReadString());
            return v;
        }

        // LodestoneTracker.STREAM_CODEC: optional GlobalPos, then tracked.
        void SerLodestone(Network::PacketBuffer& b, const LodestoneTracker& v) {
            b.WriteByte(v.target ? 1 : 0);
            if (v.target) {
                b.WriteString(v.target->dimension);
                b.WriteInt(static_cast<uint32_t>(v.target->pos.x));
                b.WriteInt(static_cast<uint32_t>(v.target->pos.y));
                b.WriteInt(static_cast<uint32_t>(v.target->pos.z));
            }
            b.WriteByte(v.tracked ? 1 : 0);
        }
        LodestoneTracker DeLodestone(Network::PacketReader& r) {
            LodestoneTracker v;
            if (r.ReadByte() != 0) {
                GlobalPos p;
                p.dimension = r.ReadString();
                p.pos.x = static_cast<int32_t>(r.ReadInt());
                p.pos.y = static_cast<int32_t>(r.ReadInt());
                p.pos.z = static_cast<int32_t>(r.ReadInt());
                v.target = std::move(p);
            }
            v.tracked = r.ReadByte() != 0;
            return v;
        }

        void SerDye(Network::PacketBuffer& b, const int32_t& v) { b.WriteVarInt(static_cast<uint32_t>(v)); }
        int32_t DeDye(Network::PacketReader& r) {
            const uint32_t v = r.ReadVarInt();
            if (v > 15) throw std::runtime_error("dye: bad colour");
            return static_cast<int32_t>(v);
        }

    } // namespace

    const DataComponentType<ArmorTrim>                TRIM{"trim", 370, &SerTrim, &DeTrim};
    const DataComponentType<std::string>              PROVIDES_TRIM_MATERIAL{"provides_trim_material", 371, &SerString, &DeString};
    const DataComponentType<std::string>              JUKEBOX_PLAYABLE{"jukebox_playable", 372, &SerString, &DeString};
    const DataComponentType<std::vector<std::string>> PROVIDES_BANNER_PATTERNS{"provides_banner_patterns", 373, &SerStrings, &DeStrings};
    const DataComponentType<std::string>              PROVIDES_POTTERY_PATTERN{"provides_pottery_pattern", 374, &SerString, &DeString};
    const DataComponentType<std::vector<std::string>> RECIPES{"recipes", 375, &SerStrings, &DeStrings};
    const DataComponentType<LodestoneTracker>         LODESTONE_TRACKER{"lodestone_tracker", 376, &SerLodestone, &DeLodestone};
    const DataComponentType<int32_t>                  DYE{"dye", 377, &SerDye, &DeDye};

} // namespace Game::DataComponents
