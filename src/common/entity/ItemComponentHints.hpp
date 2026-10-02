// File: src/common/entity/ItemComponentHints.hpp
//
// The item components an item argument (`diamond_sword[enchantments=
// {sharpness:5}]`, MC ItemParser) may set: every persistent MC 26.3 item
// component (DataComponents.java's `.persistent(...)` ones — the transient
// creative_slot_lock, additional_trade_cost and map_post_processing are not
// settable, as in MC), each read by ItemStackNbt's ReadItemStack through its
// codec, with a fallback example or two each (the chat's value completion
// comes from each component's value shape and real registries —
// CommandSuggestions' ComponentSpec). The server's item argument accepts these
// names and refuses others; the chat completes them after the `[`. Ids
// without "minecraft:", as the rest of completion.
#pragma once

#include <array>
#include <string_view>

namespace Game::ItemComponentHints {

    struct Component {
        std::string_view name;
        std::array<std::string_view, 4> examples;   // empty entries unused
    };

    inline constexpr std::array<Component, 119> kComponents = {{
        {"attack_animation", {"{type:\"stab\",duration:6}", "{type:\"whack\"}"}},
        {"attack_range", {"{min_reach:0.0,max_reach:5.0}"}},
        {"attribute_modifiers", {"[{type:\"attack_damage\",id:\"my_damage\",amount:10,operation:\"add_value\",slot:\"mainhand\"}]", "[{type:\"movement_speed\",id:\"speed\",amount:0.5,operation:\"add_multiplied_base\",slot:\"feet\"}]"}},
        {"axolotl/variant", {"\"lucy\""}},
        {"banner_patterns", {"[{pattern:\"stripe_top\",color:\"red\"}]"}},
        {"base_color", {"\"white\"", "\"red\""}},
        {"bees", {"[{entity_data:{id:\"bee\"},ticks_in_hive:0,min_ticks_in_hive:600}]", "[]"}},
        {"block_entity_data", {"{id:\"chest\",Items:[]}"}},
        {"block_state", {"{lit:\"true\"}", "{facing:\"north\"}"}},
        {"block_transformer", {"\"axe\"", "\"hoe\"", "\"shovel\""}},
        {"blocks_attacks", {"{block_delay_seconds:0.25,damage_reductions:[{base:0.0,factor:1.0}]}"}},
        {"break_sound", {"\"entity.item.break\""}},
        {"brewing_fuel", {"{uses:\"brewing/uses_default\",speed_multiplier:\"brewing/speed_default\"}", "{uses:40,speed_multiplier:2.0}"}},
        {"bucket_entity_data", {"{NoAI:1b}", "{Health:10.0}"}},
        {"bundle_contents", {"[{id:\"diamond\",count:4}]"}},
        {"can_break", {"{blocks:\"stone\"}", "{blocks:\"#mineable/pickaxe\"}"}},
        {"can_place_on", {"{blocks:\"stone\"}", "[{blocks:\"#logs\"},{blocks:\"dirt\"}]"}},
        {"cat/collar", {"\"red\""}},
        {"cat/sound_variant", {"\"classic\""}},
        {"cat/variant", {"\"tabby\"", "\"black\""}},
        {"charged_projectiles", {"[{id:\"arrow\",count:1}]"}},
        {"chicken/sound_variant", {"\"classic\""}},
        {"chicken/variant", {"\"temperate\"", "\"cold\""}},
        {"compostable", {"{layers:\"compostable/medium\"}", "{layers:1}"}},
        {"consumable", {"{consume_seconds:1.6,animation:\"eat\"}", "{animation:\"drink\",sound:\"entity.generic.drink\"}"}},
        {"container", {"[{slot:0,item:{id:\"diamond\",count:1}}]"}},
        {"container_loot", {"{loot_table:\"chests/simple_dungeon\"}"}},
        {"cooking_fuel", {"{burn_time:\"cooking/time_coal\",speed_multiplier:\"cooking/speed_default\"}", "{burn_time:400,speed_multiplier:2.0}"}},
        {"cow/sound_variant", {"\"classic\""}},
        {"cow/variant", {"\"temperate\"", "\"warm\""}},
        {"cushion/color", {"\"red\""}},
        {"custom_data", {"{key:\"value\"}", "{}"}},
        {"custom_model_data", {"{floats:[1.0]}", "{strings:[\"variant\"]}"}},
        {"custom_name", {"\"Excalibur\"", "{text:\"Excalibur\",color:\"gold\"}"}},
        {"damage", {"0", "100"}},
        {"damage_resistant", {"{types:\"#is_fire\"}"}},
        {"damage_type", {"\"player_attack\"", "\"spear\""}},
        {"death_protection", {"{death_effects:[{type:\"clear_all_effects\"}]}"}},
        {"debug_stick_state", {"{}"}},
        {"dye", {"\"red\"", "\"blue\""}},
        {"dyed_color", {"16711680", "255"}},
        {"enchantable", {"{value:15}"}},
        {"enchantment_glint_override", {"true", "false"}},
        {"enchantments", {"{sharpness:5}", "{efficiency:5,unbreaking:3}", "{protection:4}"}},
        {"entity_data", {"{id:\"zombie\",NoAI:1b}"}},
        {"equippable", {"{slot:\"head\"}", "{slot:\"chest\",asset_id:\"diamond\"}"}},
        {"firework_explosion", {"{shape:\"large_ball\",colors:[I;16711680]}"}},
        {"fireworks", {"{flight_duration:1,explosions:[{shape:\"star\",colors:[I;255]}]}"}},
        {"food", {"{nutrition:4,saturation:2.4}", "{nutrition:1,saturation:0.1,can_always_eat:true}"}},
        {"fox/variant", {"\"red\"", "\"snow\""}},
        {"frog/variant", {"\"temperate\"", "\"warm\""}},
        {"glider", {"{}"}},
        {"horse/variant", {"\"white\"", "\"chestnut\""}},
        {"instrument", {"\"ponder_goat_horn\""}},
        {"intangible_projectile", {"{}"}},
        {"interact_animation", {"{type:\"none\"}", "{type:\"whack\"}"}},
        {"item_model", {"\"diamond\"", "\"stick\""}},
        {"item_name", {"\"Name\""}},
        {"jukebox_playable", {"\"cat\"", "\"pigstep\""}},
        {"kinetic_weapon", {"{delay_ticks:10,damage_multiplier:1.0}"}},
        {"llama/variant", {"\"creamy\"", "\"gray\""}},
        {"lock", {"{items:\"tripwire_hook\"}", "{components:{custom_name:\"Key\"}}"}},
        {"lodestone_tracker", {"{target:{dimension:\"overworld\",pos:[I;0,64,0]},tracked:true}", "{tracked:false}"}},
        {"lore", {"[\"Line one\",\"Line two\"]", "[{text:\"Lore\",color:\"gray\"}]"}},
        {"map_decorations", {"{}"}},
        {"map_id", {"0"}},
        {"max_damage", {"100", "2031"}},
        {"max_stack_size", {"1", "16", "64", "99"}},
        {"minimum_attack_charge", {"0.5", "1.0"}},
        {"mob_visibility", {"{targeting_entity_types:\"zombie\",visibility:0.5}"}},
        {"mooshroom/variant", {"\"red\"", "\"brown\""}},
        {"note_block_sound", {"\"entity.zombie.ambient\""}},
        {"ominous_bottle_amplifier", {"0", "4"}},
        {"painting/variant", {"\"kebab\""}},
        {"parrot/variant", {"\"red_blue\"", "\"green\""}},
        {"piercing_weapon", {"{deals_knockback:true,dismounts:false}"}},
        {"pig/sound_variant", {"\"classic\""}},
        {"pig/variant", {"\"temperate\"", "\"cold\""}},
        {"pot_decorations", {"[\"brick\",\"brick\",\"brick\",\"brick\"]"}},
        {"potion_contents", {"{potion:\"swiftness\"}", "{potion:\"strong_healing\"}"}},
        {"potion_duration_scale", {"1.0"}},
        {"profile", {"\"Notch\"", "{name:\"Notch\"}"}},
        {"provides_banner_patterns", {"\"#pattern_item/flower\""}},
        {"provides_pottery_pattern", {"\"angler\""}},
        {"provides_trim_material", {"\"iron\"", "\"gold\""}},
        {"rabbit/variant", {"\"brown\"", "\"evil\""}},
        {"rarity", {"\"common\"", "\"uncommon\"", "\"rare\"", "\"epic\""}},
        {"recipes", {"[\"diamond_sword\"]"}},
        {"repair_cost", {"0"}},
        {"repairable", {"{items:\"diamond\"}"}},
        {"salmon/size", {"\"small\"", "\"medium\"", "\"large\""}},
        {"sheep/color", {"\"white\"", "\"black\""}},
        {"shulker/color", {"\"purple\""}},
        {"sign_text_back", {"{messages:[\"Back\",\"\",\"\",\"\"]}"}},
        {"sign_text_front", {"{messages:[\"Hello\",\"\",\"\",\"\"]}"}},
        {"stored_enchantments", {"{mending:1}"}},
        {"sulfur_cube_content", {"\"slime_ball\"", "{id:\"slime_ball\",count:1}"}},
        {"suspicious_stew_effects", {"[{id:\"night_vision\",duration:160}]"}},
        {"tool", {"{rules:[{blocks:\"#mineable/pickaxe\",speed:8.0,correct_for_drops:true}]}"}},
        {"tooltip_display", {"{hide_tooltip:true}", "{hidden_components:[\"enchantments\"]}"}},
        {"tooltip_style", {"\"custom_style\""}},
        {"trim", {"{material:\"iron\",pattern:\"coast\"}", "{material:\"diamond\",pattern:\"silence\"}"}},
        {"tropical_fish/base_color", {"\"white\""}},
        {"tropical_fish/pattern", {"\"kob\""}},
        {"tropical_fish/pattern_color", {"\"orange\""}},
        {"unbreakable", {"{}"}},
        {"use_cooldown", {"{seconds:1.0}", "{seconds:5.0,cooldown_group:\"my_group\"}"}},
        {"use_effects", {"{can_sprint:true,speed_multiplier:1.0}"}},
        {"use_remainder", {"{id:\"bowl\"}"}},
        {"villager/variant", {"\"plains\"", "\"desert\""}},
        {"villager_food", {"{nutrition:4}"}},
        {"waxed", {"{}"}},
        {"weapon", {"{item_damage_per_attack:1}", "{disable_blocking_for_seconds:5.0}"}},
        {"wolf/collar", {"\"red\""}},
        {"wolf/sound_variant", {"\"classic\"", "\"angry\""}},
        {"wolf/variant", {"\"pale\"", "\"ashen\""}},
        {"writable_book_content", {"{pages:[\"Page one\"]}"}},
        {"written_book_content", {"{title:\"Title\",author:\"Author\",pages:[\"Page one\"]}"}},
        {"zombie_nautilus/variant", {"\"temperate\""}},
    }};

    inline bool Known(std::string_view name) {
        for (const Component& c : kComponents) if (c.name == name) return true;
        return false;
    }

} // namespace Game::ItemComponentHints
