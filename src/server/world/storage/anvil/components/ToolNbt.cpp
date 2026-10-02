// File: src/server/world/storage/anvil/components/ToolNbt.cpp
//
// NBT codecs (MC DataComponentType.codec() over NbtOps) for the tool &
// equipment components, registered with ComponentNbt: tool, equippable,
// glider. (repairable, enchantable and damage_resistant stay with the
// durability set in ItemStackNbt.cpp.)
#include "server/world/storage/anvil/ComponentNbt.hpp"
#include "common/data/components/ToolComponents.hpp"

#include <cmath>

namespace Game::Anvil::ComponentNbt {

    namespace {

        constexpr const char* kSlotNames[] = {"mainhand", "offhand", "feet", "legs", "chest", "head", "body", "saddle"};

        bool SlotFromName(std::string_view name, EquipmentSlot& out) {
            for (int i = 0; i < 8; ++i) {
                if (name == kSlotNames[i]) { out = static_cast<EquipmentSlot>(i); return true; }
            }
            return false;
        }

        // SoundEvent.CODEC (Holder): the id, or the direct {sound_id, range?}.
        std::optional<std::string> ReadSound(const ::World::NBTTag* tag) {
            if (!tag) return std::nullopt;
            if (auto s = StringOf(*tag)) return std::string(StripMinecraft(*s));
            if (const auto* c = AsCompound(tag)) {
                const std::string id = c->GetValue<std::string>("sound_id", "");
                if (!id.empty()) return std::string(StripMinecraft(id));
            }
            return std::nullopt;
        }

        std::optional<bool> ReadBoolKey(const ::World::NBTTagCompound& c, const char* key) {
            auto tag = c.GetTag(key);
            return tag ? BoolOf(*tag) : std::nullopt;
        }

        // ── tool ──────────────────────────────────────────────────────────
        // Tool.CODEC: {rules: [Rule], default_mining_speed = 1.0,
        // damage_per_block = 1, can_destroy_blocks_in_creative = true};
        // Rule: {blocks: HolderSet<Block>, speed?: POSITIVE_FLOAT,
        // correct_for_drops?: bool}.
        void WriteTool(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto tool = stack.components.get(DataComponents::TOOL);
            if (!tool) return;
            w.BeginCompound(key);
            auto rules = w.BeginList("rules", Nbt::TagType::Compound);
            for (const Tool::Rule& rule : tool->rules) {
                w.ListCompoundBegin(rules);
                std::vector<std::string> blocks;
                for (const std::string& b : rule.blocks) blocks.push_back(WithNamespace(b));
                WriteHolderSet(w, "blocks", blocks);
                if (rule.speed) w.Float("speed", *rule.speed);
                if (rule.correctForDrops) w.Bool("correct_for_drops", *rule.correctForDrops);
                w.ListCompoundEnd(rules);
            }
            w.EndList(rules);
            if (tool->defaultMiningSpeed != 1.0f) w.Float("default_mining_speed", tool->defaultMiningSpeed);
            if (tool->damagePerBlock != 1) w.Int("damage_per_block", tool->damagePerBlock);
            if (!tool->canDestroyBlocksInCreative) w.Bool("can_destroy_blocks_in_creative", false);
            w.EndCompound();
        }

        bool ReadTool(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            Tool tool;
            const auto* rules = AsList(c->GetTag("rules").get());
            if (!rules) { ctx.Fail("No key rules in MapLike"); return false; }
            for (const auto& element : rules->value) {
                const auto* r = AsCompound(Unwrap(element.get()));
                if (!r) { ctx.Fail("Not a map"); return false; }
                Tool::Rule rule;
                rule.blocks = ReadHolderSet(r->GetTag("blocks").get());
                if (rule.blocks.empty()) { ctx.Fail("No key blocks in MapLike"); return false; }
                if (auto speed = r->GetTag("speed")) {
                    const std::optional<double> v = NumberOf(*speed);
                    if (!v || !(*v > 0.0)) { ctx.Fail("Value must be positive: " + std::to_string(v.value_or(0.0))); return false; }
                    rule.speed = static_cast<float>(*v);
                }
                if (auto drops = ReadBoolKey(*r, "correct_for_drops")) rule.correctForDrops = *drops;
                tool.rules.push_back(std::move(rule));
            }
            if (auto speed = c->GetTag("default_mining_speed")) {
                tool.defaultMiningSpeed = static_cast<float>(NumberOf(*speed).value_or(1.0));
            }
            if (auto dmg = c->GetTag("damage_per_block")) {
                const int v = static_cast<int>(NumberOf(*dmg).value_or(1.0));
                if (v < 0) { ctx.Fail("Value must be non-negative: " + std::to_string(v)); return false; }
                tool.damagePerBlock = v;
            }
            if (auto creative = ReadBoolKey(*c, "can_destroy_blocks_in_creative")) tool.canDestroyBlocksInCreative = *creative;
            stack.components.set(DataComponents::TOOL, std::move(tool));
            return true;
        }

        // ── equippable ────────────────────────────────────────────────────
        // Equippable.CODEC: slot (required), equip_sound (armor.equip_generic),
        // asset_id?, camera_overlay?, allowed_entities?, dispensable (true),
        // swappable (true), damage_on_hurt (true), equip_on_interact (false),
        // can_be_sheared (false), shearing_sound (item.shears.snip).
        void WriteEquippable(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto e = stack.components.get(DataComponents::EQUIPPABLE);
            if (!e) return;
            const Equippable defaults;
            w.BeginCompound(key);
            w.String("slot", kSlotNames[static_cast<int>(e->slot) & 7]);
            if (e->equipSound != defaults.equipSound) w.String("equip_sound", WithNamespace(e->equipSound));
            if (!e->assetId.empty()) w.String("asset_id", WithNamespace(e->assetId));
            if (!e->cameraOverlay.empty()) w.String("camera_overlay", WithNamespace(e->cameraOverlay));
            if (!e->allowedEntities.empty()) WriteHolderSet(w, "allowed_entities", e->allowedEntities);
            if (!e->dispensable) w.Bool("dispensable", false);
            if (!e->swappable) w.Bool("swappable", false);
            if (!e->damageOnHurt) w.Bool("damage_on_hurt", false);
            if (e->equipOnInteract) w.Bool("equip_on_interact", true);
            if (e->canBeSheared) w.Bool("can_be_sheared", true);
            if (e->shearingSound != defaults.shearingSound) w.String("shearing_sound", WithNamespace(e->shearingSound));
            w.EndCompound();
        }

        bool ReadEquippable(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            Equippable e;
            const std::string slot = c->GetValue<std::string>("slot", "");
            if (slot.empty()) { ctx.Fail("No key slot in MapLike"); return false; }
            if (!SlotFromName(slot, e.slot)) { ctx.Fail("Unknown element name:" + slot); return false; }
            if (auto sound = ReadSound(c->GetTag("equip_sound").get())) e.equipSound = *sound;
            if (auto asset = c->GetTag("asset_id")) {
                if (auto s = StringOf(*asset)) e.assetId = WithNamespace(*s);
            }
            if (auto overlay = c->GetTag("camera_overlay")) {
                if (auto s = StringOf(*overlay)) e.cameraOverlay = WithNamespace(*s);
            }
            e.allowedEntities = ReadHolderSet(c->GetTag("allowed_entities").get());
            for (std::string& entity : e.allowedEntities) entity = WithNamespace(entity);
            if (auto v = ReadBoolKey(*c, "dispensable"))       e.dispensable = *v;
            if (auto v = ReadBoolKey(*c, "swappable"))         e.swappable = *v;
            if (auto v = ReadBoolKey(*c, "damage_on_hurt"))    e.damageOnHurt = *v;
            if (auto v = ReadBoolKey(*c, "equip_on_interact")) e.equipOnInteract = *v;
            if (auto v = ReadBoolKey(*c, "can_be_sheared"))    e.canBeSheared = *v;
            if (auto sound = ReadSound(c->GetTag("shearing_sound").get())) e.shearingSound = *sound;
            stack.components.set(DataComponents::EQUIPPABLE, std::move(e));
            return true;
        }

        // ── glider (Unit.CODEC: {}) ────────────────────────────────────────
        void WriteGlider(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (!stack.components.get(DataComponents::GLIDER)) return;
            w.BeginCompound(key);
            w.EndCompound();
        }

        bool ReadGlider(const ::World::NBTTag&, ItemStack& stack, const ReadContext&) {
            stack.components.set(DataComponents::GLIDER, true);
            return true;
        }

        const Registrar kTool{DataComponents::TOOL, &WriteTool, &ReadTool};
        const Registrar kEquippable{DataComponents::EQUIPPABLE, &WriteEquippable, &ReadEquippable};
        const Registrar kGlider{DataComponents::GLIDER, &WriteGlider, &ReadGlider};

    } // namespace

} // namespace Game::Anvil::ComponentNbt
