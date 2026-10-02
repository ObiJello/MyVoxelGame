// File: src/server/world/storage/anvil/components/ItemPredicateNbt.cpp
//
// MC ItemPredicate (advancements/predicates/ItemPredicate) over its NBT form:
// items (HolderSet<Item>), count (MinMaxBounds.Ints), components
// (DataComponentExactPredicate — every listed component present with an
// equal value) and predicates (DataComponentPredicates' partial checks:
// damage, enchantments, stored_enchantments, potion_contents, custom_data,
// container, bundle_contents, firework_explosion, fireworks,
// writable_book_content, written_book_content, attribute_modifiers, trim,
// jukebox_playable, villager/variant). What a container's `lock` tests the
// key against (LockCode.unlocksWith).
#include "server/world/storage/anvil/ComponentNbt.hpp"
#include "server/world/storage/anvil/ItemStackNbt.hpp"
#include "server/commands/SnbtParser.hpp"

#include "common/data/DataComponents.hpp"
#include "common/data/NbtCompoundValue.hpp"
#include "common/text/TextComponent.hpp"
#include "common/world/enchantment/Enchantment.hpp"
#include "common/world/enchantment/ItemEnchantments.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace Game::Anvil::ComponentNbt {

    namespace {

        using Tag = ::World::NBTTag;
        using Compound = ::World::NBTTagCompound;

        const Tag* Field(const Compound& c, const char* key) {
            auto it = c.value.find(key);
            return it == c.value.end() ? nullptr : Unwrap(it->second.get());
        }

        // MinMaxBounds over NbtOps: a number, or {min?, max?}.
        bool Bounds(const Tag* t, double value) {
            if (!t) return true;
            if (auto n = NumberOf(*t)) return value == *n;
            if (const Compound* c = AsCompound(t)) {
                if (auto lo = c->GetTag("min")) { if (auto v = NumberOf(*lo); v && value < *v) return false; }
                if (auto hi = c->GetTag("max")) { if (auto v = NumberOf(*hi); v && value > *v) return false; }
                return true;
            }
            return false;
        }

        // A HolderSet test against one id (namespaced or bare), tags through
        // `inTag`.
        bool HolderSetHas(const Tag* t, std::string_view id, const std::function<bool(const std::string&)>& inTag) {
            if (!t) return true;
            const std::string_view bare = StripMinecraft(id);
            for (const std::string& entry : ReadHolderSet(t)) {
                if (!entry.empty() && entry[0] == '#') {
                    if (inTag && inTag(entry)) return true;
                } else if (StripMinecraft(entry) == bare) {
                    return true;
                }
            }
            return false;
        }

        bool ItemPredicateTest(const Tag* predicate, const ItemStack& stack);

        // CollectionPredicate {contains?, count?, size?} over `elements`.
        template<typename T>
        bool CollectionTest(const Tag* t, const std::vector<T>& elements,
                            const std::function<bool(const Tag*, const T&)>& test) {
            if (!t) return true;
            const Compound* c = AsCompound(t);
            if (!c) return false;
            // contains: every listed predicate matches a distinct-or-any
            // element (CollectionContentsPredicate: each test finds one).
            if (const Tag* contains = Field(*c, "contains")) {
                const auto* list = AsList(contains);
                if (!list) return false;
                for (const auto& p : list->value) {
                    const bool found = std::any_of(elements.begin(), elements.end(),
                                                   [&](const T& e) { return test(Unwrap(p.get()), e); });
                    if (!found) return false;
                }
            }
            // count: [{test, count}] — how many elements match each test.
            if (const Tag* counts = Field(*c, "count")) {
                const auto* list = AsList(counts);
                if (!list) return false;
                for (const auto& entry : list->value) {
                    const Compound* ec = AsCompound(Unwrap(entry.get()));
                    if (!ec) return false;
                    const Tag* testTag = Field(*ec, "test");
                    const int n = static_cast<int>(std::count_if(elements.begin(), elements.end(),
                                                                 [&](const T& e) { return test(testTag, e); }));
                    if (!Bounds(Field(*ec, "count"), n)) return false;
                }
            }
            if (const Tag* size = Field(*c, "size"); size && !Bounds(size, static_cast<double>(elements.size()))) {
                return false;
            }
            return true;
        }

        // EnchantmentsPredicate: a list of {enchantments?, levels?}, each
        // matching some entry.
        bool EnchantmentsTest(const Tag* t, const std::optional<ItemEnchantments>& enchantments) {
            const auto* list = AsList(t);
            if (!list) return false;
            for (const auto& p : list->value) {
                const Compound* c = AsCompound(Unwrap(p.get()));
                if (!c) return false;
                const Tag* ids = Field(*c, "enchantments");
                const Tag* levels = Field(*c, "levels");
                bool found = false;
                if (enchantments) {
                    for (const EnchantmentInstance& e : enchantments->entries) {
                        const std::string& slug = EnchantmentRegistry::Get(e.id).slug;
                        if (!HolderSetHas(ids, slug, nullptr)) continue;
                        if (!Bounds(levels, e.level)) continue;
                        found = true;
                        break;
                    }
                }
                // No id filter: any enchantment at all (with the levels).
                if (!found) return false;
            }
            return true;
        }

        bool SubPredicate(const std::string& type, const Tag* t, const ItemStack& stack) {
            if (type == "damage") {
                // ItemDamagePredicate {durability?, damage?}.
                const Compound* c = AsCompound(t);
                if (!c) return false;
                if (!stack.has(DataComponents::DAMAGE) || GetMaxDamage(stack) <= 0) return false;
                const int damage = GetDamageValue(stack);
                return Bounds(Field(*c, "durability"), GetMaxDamage(stack) - damage) && Bounds(Field(*c, "damage"), damage);
            }
            if (type == "enchantments") return EnchantmentsTest(t, stack.get(DataComponents::ENCHANTMENTS));
            if (type == "stored_enchantments") return EnchantmentsTest(t, stack.get(DataComponents::STORED_ENCHANTMENTS));
            if (type == "potion_contents") {
                const auto contents = stack.get(DataComponents::POTION_CONTENTS);
                if (!contents || !contents->potion) return false;
                const char* key = GetPotionKey(*contents->potion);
                return key && HolderSetHas(t, key, nullptr);
            }
            if (type == "custom_data") {
                // NbtPredicate: a compound or its SNBT, partially matching.
                const auto data = stack.get(DataComponents::CUSTOM_DATA);
                std::shared_ptr<Compound> expected;
                if (const Compound* c = AsCompound(t)) {
                    expected = std::dynamic_pointer_cast<Compound>(CloneNbtTag(*c));
                } else if (auto text = StringOf(*t)) {
                    std::string error;
                    expected = Server::Snbt::ParseCompound(*text, error);
                }
                if (!expected) return false;
                const Compound empty;
                return CompareNbt(expected.get(), data ? &data->Tag() : &empty, true);
            }
            if (type == "container" || type == "bundle_contents") {
                std::vector<ItemStack> items;
                if (type == "container") {
                    if (const auto c = stack.get(DataComponents::CONTAINER)) {
                        for (const ItemStack& s : c->items) if (!s.IsEmpty()) items.push_back(s);
                    }
                } else if (const auto b = stack.get(DataComponents::BUNDLE_CONTENTS)) {
                    items = b->items;
                }
                const Compound* c = AsCompound(t);
                if (!c) return false;
                return CollectionTest<ItemStack>(Field(*c, "items"), items,
                                                 [](const Tag* p, const ItemStack& s) { return ItemPredicateTest(p, s); });
            }
            const auto explosionTest = [](const Tag* p, const FireworkExplosion& e) {
                const Compound* c = AsCompound(p);
                if (!c) return false;
                if (auto shape = c->GetTag("shape")) {
                    const auto name = StringOf(*shape);
                    if (!name || FireworkExplosion::ShapeName(e.shape) != StripMinecraft(*name)) return false;
                }
                if (auto v = c->GetTag("has_twinkle")) { if (BoolOf(*v).value_or(false) != e.hasTwinkle) return false; }
                if (auto v = c->GetTag("has_trail"))   { if (BoolOf(*v).value_or(false) != e.hasTrail) return false; }
                return true;
            };
            if (type == "firework_explosion") {
                const auto e = stack.get(DataComponents::FIREWORK_EXPLOSION);
                return e && explosionTest(t, *e);
            }
            if (type == "fireworks") {
                const auto f = stack.get(DataComponents::FIREWORKS);
                const Compound* c = AsCompound(t);
                if (!f || !c) return false;
                if (!Bounds(Field(*c, "flight_duration"), f->flightDuration)) return false;
                return CollectionTest<FireworkExplosion>(Field(*c, "explosions"), f->explosions, explosionTest);
            }
            if (type == "writable_book_content") {
                const auto b = stack.get(DataComponents::WRITABLE_BOOK_CONTENT);
                const Compound* c = AsCompound(t);
                if (!b || !c) return false;
                const std::vector<std::string> pages = b->GetPages(false);
                return CollectionTest<std::string>(Field(*c, "pages"), pages, [](const Tag* p, const std::string& page) {
                    const Compound* pc = AsCompound(p);
                    if (!pc) return false;
                    const auto contents = pc->GetTag("contents");
                    return !contents || StringOf(*contents).value_or("") == page;
                });
            }
            if (type == "written_book_content") {
                const auto b = stack.get(DataComponents::WRITTEN_BOOK_CONTENT);
                const Compound* c = AsCompound(t);
                if (!b || !c) return false;
                if (auto author = c->GetTag("author"); author && StringOf(*author).value_or("") != b->author) return false;
                if (auto title = c->GetTag("title"); title && StringOf(*title).value_or("") != b->title.raw) return false;
                if (!Bounds(Field(*c, "generation"), b->generation)) return false;
                if (auto resolved = c->GetTag("resolved"); resolved && BoolOf(*resolved).value_or(false) != b->resolved) return false;
                std::vector<std::string> pages;
                for (const auto& p : b->pages) pages.push_back(Text::GetString(p.raw));
                return CollectionTest<std::string>(Field(*c, "pages"), pages, [](const Tag* p, const std::string& page) {
                    const Compound* pc = AsCompound(p);
                    if (!pc) return false;
                    const auto contents = pc->GetTag("contents");
                    if (!contents) return true;
                    if (auto s = StringOf(*contents)) return *s == page;
                    return false;
                });
            }
            if (type == "attribute_modifiers") {
                const auto mods = stack.get(DataComponents::ATTRIBUTE_MODIFIERS);
                const Compound* c = AsCompound(t);
                if (!c) return false;
                const std::vector<ItemAttributeModifiers::Entry> entries = mods ? mods->modifiers
                                                                               : std::vector<ItemAttributeModifiers::Entry>{};
                return CollectionTest<ItemAttributeModifiers::Entry>(Field(*c, "modifiers"), entries,
                    [](const Tag* p, const ItemAttributeModifiers::Entry& e) {
                        const Compound* pc = AsCompound(p);
                        if (!pc) return false;
                        if (!HolderSetHas(Field(*pc, "attribute"), AttributeName(e.attribute), nullptr)) return false;
                        if (auto id = pc->GetTag("id"); id && WithNamespace(StringOf(*id).value_or("")) != WithNamespace(e.id)) return false;
                        if (!Bounds(Field(*pc, "amount"), e.amount)) return false;
                        if (auto op = pc->GetTag("operation"); op && StringOf(*op).value_or("") != AttributeOperationName(e.operation)) return false;
                        if (auto slot = pc->GetTag("slot"); slot && StringOf(*slot).value_or("") != EquipmentSlotGroupName(e.slot)) return false;
                        return true;
                    });
            }
            if (type == "trim") {
                const auto trim = stack.get(DataComponents::TRIM);
                const Compound* c = AsCompound(t);
                if (!trim || !c) return false;
                return HolderSetHas(Field(*c, "material"), trim->material, nullptr) &&
                       HolderSetHas(Field(*c, "pattern"), trim->pattern, nullptr);
            }
            if (type == "jukebox_playable") {
                const auto song = stack.get(DataComponents::JUKEBOX_PLAYABLE);
                const Compound* c = AsCompound(t);
                if (!song || !c) return false;
                return HolderSetHas(Field(*c, "song"), *song, nullptr);
            }
            if (type == "villager/variant") {
                const auto variant = stack.get(DataComponents::VILLAGER_VARIANT);
                return variant && HolderSetHas(t, *variant, nullptr);
            }
            return false;   // an unknown predicate type fails closed
        }

        bool ItemPredicateTest(const Tag* predicate, const ItemStack& stack) {
            const Compound* p = AsCompound(predicate);
            if (!p) return false;
            if (p->value.empty()) return true;
            if (stack.IsEmpty()) return false;
            if (const Tag* items = Field(*p, "items")) {
                const std::string slug(ItemRegistry::Slug(stack.itemId));
                if (!HolderSetHas(items, slug, [&](const std::string& tag) {
                        return DataTags::HasTag(DataTags::Registry::Item, slug, tag);
                    })) {
                    return false;
                }
            }
            if (!Bounds(Field(*p, "count"), stack.count)) return false;
            if (const Compound* components = AsCompound(Field(*p, "components"))) {
                // DataComponentExactPredicate: the listed components, read
                // through the stack codec onto the same item, all present
                // with equal values.
                auto root = std::make_shared<Compound>();
                root->value["id"] = std::make_shared<::World::NBTTagString>(ItemName(stack.itemId));
                root->value["count"] = std::make_shared<::World::NBTTagInt>(1);
                root->value["components"] = CloneNbtTag(*components);
                const ItemStack expected = ReadItemStack(*root);
                if (expected.IsEmpty()) return false;
                if (!expected.components.IsExactSubsetOf(stack.components,
                                                         &ItemRegistry::Get(stack.itemId).defaultComponents)) {
                    return false;
                }
            }
            if (const Compound* predicates = AsCompound(Field(*p, "predicates"))) {
                for (const auto& [key, value] : predicates->value) {
                    const std::string type(StripMinecraft(key));
                    if (!SubPredicate(type, Unwrap(value.get()), stack)) return false;
                }
            }
            return true;
        }

    } // namespace

    bool ItemPredicateMatches(const ::World::NBTTagCompound& predicate, const ItemStack& stack) {
        return ItemPredicateTest(&predicate, stack);
    }

} // namespace Game::Anvil::ComponentNbt
