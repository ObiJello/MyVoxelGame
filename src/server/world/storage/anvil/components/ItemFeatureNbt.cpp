// File: src/server/world/storage/anvil/components/ItemFeatureNbt.cpp
//
// NBT codecs (MC DataComponentType.codec() over NbtOps) for the components
// of common/data/components/ItemFeatureComponents.hpp, registered with
// ComponentNbt: trim, provides_trim_material, jukebox_playable,
// provides_banner_patterns, provides_pottery_pattern, recipes,
// lodestone_tracker, dye.
#include "server/world/storage/anvil/ComponentNbt.hpp"
#include "common/data/components/ItemFeatureComponents.hpp"
#include "common/entity/JukeboxSongs.hpp"
#include "common/world/banner/BannerPatterns.hpp"

#include <algorithm>
#include <array>

namespace Game::Anvil::ComponentNbt {

    namespace {

        std::string Describe(const ::World::NBTTag& tag) { return NbtTagToSnbt(tag); }

        std::string UnknownKey(const char* registry, const std::string& key) {
            return std::string("Unknown registry key in ResourceKey[minecraft:root / minecraft:") + registry + "]: " + key;
        }

        // A Holder<T> field: the registry key (an inline, direct value is
        // not something the engine's fixed registries can hold).
        bool ReadHolderKey(const ::World::NBTTag* tag, const char* registry, std::string& out, const ReadContext& ctx) {
            if (!tag) { ctx.Fail("Missing value"); return false; }
            const auto s = StringOf(*tag);
            if (!s) {
                ctx.Fail(std::string("Inline ") + registry + " values are not supported; use a registry id");
                return false;
            }
            out = WithNamespace(*s);
            return true;
        }

        // DecoratedPotPatterns' registry.
        constexpr std::array<std::string_view, 23> kPotPatterns = {
            "angler", "archer", "arms_up", "blade", "brewer", "burn", "danger", "explorer", "flow",
            "friend", "guster", "heart", "heartbreak", "howl", "miner", "mourner", "plenty", "prize",
            "scrape", "sheaf", "shelter", "skull", "snort",
        };

        // ── trim {material, pattern} ───────────────────────────────────────
        void WriteTrim(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto v = stack.components.get(DataComponents::TRIM);
            if (!v) return;
            w.BeginCompound(key);
            w.String("material", WithNamespace(v->material));
            w.String("pattern", WithNamespace(v->pattern));
            w.EndCompound();
        }

        bool ReadTrim(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map: " + Describe(tag)); return false; }
            if (!c->GetTag("material")) { ctx.Fail("No key material in MapLike"); return false; }
            if (!c->GetTag("pattern")) { ctx.Fail("No key pattern in MapLike"); return false; }
            ArmorTrim v;
            if (!ReadHolderKey(c->GetTag("material").get(), "trim_material", v.material, ctx)) return false;
            if (!ReadHolderKey(c->GetTag("pattern").get(), "trim_pattern", v.pattern, ctx)) return false;
            if (!TrimMaterialExists(v.material)) { ctx.Fail(UnknownKey("trim_material", v.material)); return false; }
            if (!TrimPatternExists(v.pattern)) { ctx.Fail(UnknownKey("trim_pattern", v.pattern)); return false; }
            stack.components.set(DataComponents::TRIM, std::move(v));
            return true;
        }

        // ── provides_trim_material (Holder<TrimMaterial>) ──────────────────
        void WriteTrimMaterial(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (const auto v = stack.components.get(DataComponents::PROVIDES_TRIM_MATERIAL)) w.String(key, WithNamespace(*v));
        }

        bool ReadTrimMaterial(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            std::string id;
            if (!ReadHolderKey(&tag, "trim_material", id, ctx)) return false;
            if (!TrimMaterialExists(id)) { ctx.Fail(UnknownKey("trim_material", id)); return false; }
            stack.components.set(DataComponents::PROVIDES_TRIM_MATERIAL, id);
            return true;
        }

        // ── jukebox_playable (JukeboxSong holder) ──────────────────────────
        void WriteJukebox(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (const auto v = stack.components.get(DataComponents::JUKEBOX_PLAYABLE)) w.String(key, WithNamespace(*v));
        }

        bool ReadJukebox(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            std::string id;
            // A 1.21-era save wraps it: {song: "…", show_in_tooltip}.
            const ::World::NBTTag* value = &tag;
            if (const auto* c = AsCompound(&tag)) {
                auto song = c->GetTag("song");
                if (!song) { ctx.Fail("No key song in MapLike"); return false; }
                value = song.get();
                if (!ReadHolderKey(value, "jukebox_song", id, ctx)) return false;
            } else if (!ReadHolderKey(value, "jukebox_song", id, ctx)) {
                return false;
            }
            if (JukeboxSongs::IdFromKey(StripMinecraft(id)) < 0) { ctx.Fail(UnknownKey("jukebox_song", id)); return false; }
            stack.components.set(DataComponents::JUKEBOX_PLAYABLE, id);
            return true;
        }

        // ── provides_banner_patterns (HolderSet<BannerPattern>) ────────────
        void WriteBannerPatterns(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto v = stack.components.get(DataComponents::PROVIDES_BANNER_PATTERNS);
            if (!v) return;
            WriteHolderSet(w, key, *v);
        }

        bool ReadBannerPatterns(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            std::vector<std::string> entries = ReadHolderSet(&tag);
            if (entries.empty() && !AsList(&tag)) { ctx.Fail("Not a holder set: " + Describe(tag)); return false; }
            for (std::string& e : entries) {
                if (!e.empty() && e[0] == '#') {
                    e = "#" + WithNamespace(e.substr(1));
                } else {
                    e = WithNamespace(e);
                    if (!BannerPatterns::Exists(e)) { ctx.Fail(UnknownKey("banner_pattern", e)); return false; }
                }
            }
            stack.components.set(DataComponents::PROVIDES_BANNER_PATTERNS, std::move(entries));
            return true;
        }

        // ── provides_pottery_pattern (Holder<DecoratedPotPattern>) ─────────
        void WritePotteryPattern(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (const auto v = stack.components.get(DataComponents::PROVIDES_POTTERY_PATTERN)) w.String(key, WithNamespace(*v));
        }

        bool ReadPotteryPattern(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            std::string id;
            if (!ReadHolderKey(&tag, "decorated_pot_pattern", id, ctx)) return false;
            const std::string_view path = StripMinecraft(id);
            if (id.rfind("minecraft:", 0) != 0 ||
                std::find(kPotPatterns.begin(), kPotPatterns.end(), path) == kPotPatterns.end()) {
                ctx.Fail(UnknownKey("decorated_pot_pattern", id));
                return false;
            }
            stack.components.set(DataComponents::PROVIDES_POTTERY_PATTERN, id);
            return true;
        }

        // ── recipes (List<ResourceKey<Recipe>>) ────────────────────────────
        void WriteRecipes(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto v = stack.components.get(DataComponents::RECIPES);
            if (!v) return;
            auto list = w.BeginList(key, Nbt::TagType::String);
            for (const std::string& r : *v) w.ListString(list, WithNamespace(r));
            w.EndList(list);
        }

        bool ReadRecipes(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            if (!AsList(&tag)) { ctx.Fail("Not a list: " + Describe(tag)); return false; }
            std::vector<std::string> recipes = ReadStringList(&tag);
            for (std::string& r : recipes) r = WithNamespace(r);
            stack.components.set(DataComponents::RECIPES, std::move(recipes));
            return true;
        }

        // ── lodestone_tracker {target?: {dimension, pos}, tracked = true} ──
        void WriteLodestone(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto v = stack.components.get(DataComponents::LODESTONE_TRACKER);
            if (!v) return;
            w.BeginCompound(key);
            if (v->target) {
                w.BeginCompound("target");
                w.String("dimension", WithNamespace(v->target->dimension));
                const int32_t pos[3] = {v->target->pos.x, v->target->pos.y, v->target->pos.z};
                w.IntArray("pos", pos, 3);
                w.EndCompound();
            }
            if (!v->tracked) w.Bool("tracked", false);
            w.EndCompound();
        }

        bool ReadLodestone(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map: " + Describe(tag)); return false; }
            LodestoneTracker v;
            if (auto targetTag = c->GetTag("target")) {
                const auto* t = AsCompound(targetTag.get());
                if (!t) { ctx.Fail("Not a map: " + Describe(*targetTag)); return false; }
                auto dim = t->GetTag("dimension");
                auto pos = t->GetTag("pos");
                if (!dim) { ctx.Fail("No key dimension in MapLike"); return false; }
                if (!pos) { ctx.Fail("No key pos in MapLike"); return false; }
                const auto dimension = StringOf(*dim);
                if (!dimension) { ctx.Fail("Not a string: " + Describe(*dim)); return false; }
                const std::vector<int32_t> xyz = ReadIntList(pos.get());
                if (xyz.size() != 3) { ctx.Fail("Input is not a list of 3 ints"); return false; }
                v.target = GlobalPos{WithNamespace(*dimension), glm::ivec3(xyz[0], xyz[1], xyz[2])};
            }
            if (auto tracked = c->GetTag("tracked")) {
                if (auto b = BoolOf(*tracked)) v.tracked = *b;
            }
            stack.components.set(DataComponents::LODESTONE_TRACKER, std::move(v));
            return true;
        }

        // ── dye (DyeColor name) ────────────────────────────────────────────
        void WriteDye(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (const auto v = stack.components.get(DataComponents::DYE)) w.String(key, DyeName(*v));
        }

        bool ReadDye(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto s = StringOf(tag);
            if (!s) { ctx.Fail("Not a string: " + Describe(tag)); return false; }
            const auto dye = DyeFromName(*s);
            if (!dye) { ctx.Fail("Unknown element name:" + *s); return false; }
            stack.components.set(DataComponents::DYE, static_cast<int32_t>(*dye));
            return true;
        }

        const Registrar kTrim{DataComponents::TRIM, &WriteTrim, &ReadTrim};
        const Registrar kTrimMaterial{DataComponents::PROVIDES_TRIM_MATERIAL, &WriteTrimMaterial, &ReadTrimMaterial};
        const Registrar kJukebox{DataComponents::JUKEBOX_PLAYABLE, &WriteJukebox, &ReadJukebox};
        const Registrar kBannerPatterns{DataComponents::PROVIDES_BANNER_PATTERNS, &WriteBannerPatterns, &ReadBannerPatterns};
        const Registrar kPotteryPattern{DataComponents::PROVIDES_POTTERY_PATTERN, &WritePotteryPattern, &ReadPotteryPattern};
        const Registrar kRecipes{DataComponents::RECIPES, &WriteRecipes, &ReadRecipes};
        const Registrar kLodestone{DataComponents::LODESTONE_TRACKER, &WriteLodestone, &ReadLodestone};
        const Registrar kDye{DataComponents::DYE, &WriteDye, &ReadDye};

    } // namespace

} // namespace Game::Anvil::ComponentNbt
