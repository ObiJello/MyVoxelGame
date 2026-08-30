// Fixture test for the canonical E-line NBT serializer.
// Reference: tests/parity/FORMAT.md; expected strings are REAL Java harness
// output lines (java_be_* dumps, 2026-08-13) so both implementations of the
// spec are pinned to each other.

#include "nbt/CanonicalNbt.h"

#include <iostream>
#include <memory>

using namespace minecraft::nbt;

namespace {

int failures = 0;

void check(const std::string& name, const CompoundTag& tag,
           const std::string& expected) {
    std::string got = canonical::serializeBlockEntity(tag);
    if (got != expected) {
        ++failures;
        std::cerr << "FAIL " << name << "\n  expected: " << expected
                  << "\n  got:      " << got << "\n";
    } else {
        std::cout << "ok " << name << "\n";
    }
}

std::unique_ptr<CompoundTag> compound() { return std::make_unique<CompoundTag>(); }

} // namespace

int main() {
    // Java: E,1,59,4,{LootTable:"minecraft:chests/igloo_chest",
    //   LootTableSeed:-17627126690112351l,components:{},id:"minecraft:chest"}
    {
        CompoundTag tag;
        tag.put("x", std::make_unique<IntTag>(1));
        tag.put("y", std::make_unique<IntTag>(59));
        tag.put("z", std::make_unique<IntTag>(4));
        tag.put("id", std::make_unique<StringTag>("minecraft:chest"));
        tag.put("LootTable", std::make_unique<StringTag>("minecraft:chests/igloo_chest"));
        tag.put("LootTableSeed", std::make_unique<LongTag>(-17627126690112351LL));
        tag.put("components", compound());
        check("chest", tag,
              "{LootTable:\"minecraft:chests/igloo_chest\","
              "LootTableSeed:-17627126690112351l,components:{},"
              "id:\"minecraft:chest\"}");
    }

    // Java: {id:"DUMMY"}
    {
        CompoundTag tag;
        tag.put("id", std::make_unique<StringTag>("DUMMY"));
        check("dummy", tag, "{id:\"DUMMY\"}");
    }

    // Java sign line: nested compounds, empty-string list, byte flags.
    // E,3,60,1,{back_text:{color:"black",has_glowing_text:0b,
    //   messages:["","","",""]},components:{},front_text:{color:"black",
    //   has_glowing_text:0b,messages:["","<----","---->",""]},
    //   id:"minecraft:sign",is_waxed:0b}
    {
        auto makeText = [](std::initializer_list<const char*> messages) {
            auto text = std::make_unique<CompoundTag>();
            text->put("color", std::make_unique<StringTag>("black"));
            text->put("has_glowing_text", std::make_unique<ByteTag>(0));
            auto list = std::make_unique<ListTag>();
            for (const char* m : messages) {
                list->add(std::make_unique<StringTag>(m));
            }
            text->put("messages", std::move(list));
            return text;
        };
        CompoundTag tag;
        tag.put("id", std::make_unique<StringTag>("minecraft:sign"));
        tag.put("is_waxed", std::make_unique<ByteTag>(0));
        tag.put("components", compound());
        tag.put("back_text", makeText({"", "", "", ""}));
        tag.put("front_text", makeText({"", "<----", "---->", ""}));
        check("sign", tag,
              "{back_text:{color:\"black\",has_glowing_text:0b,"
              "messages:[\"\",\"\",\"\",\"\"]},components:{},"
              "front_text:{color:\"black\",has_glowing_text:0b,"
              "messages:[\"\",\"<----\",\"---->\",\"\"]},"
              "id:\"minecraft:sign\",is_waxed:0b}");
    }

    // Java brewing stand: list of item compounds, short field, quoted key.
    // E,5,60,4,{BrewTime:0s,Fuel:0b,Items:[{Slot:1b,components:
    //   {"minecraft:potion_contents":{potion:"minecraft:weakness"}},count:1,
    //   id:"minecraft:splash_potion"}],components:{},id:"minecraft:brewing_stand"}
    {
        auto item = std::make_unique<CompoundTag>();
        item->put("Slot", std::make_unique<ByteTag>(1));
        item->put("count", std::make_unique<IntTag>(1));
        item->put("id", std::make_unique<StringTag>("minecraft:splash_potion"));
        auto potion = std::make_unique<CompoundTag>();
        potion->put("potion", std::make_unique<StringTag>("minecraft:weakness"));
        auto itemComponents = std::make_unique<CompoundTag>();
        itemComponents->put("minecraft:potion_contents", std::move(potion));
        item->put("components", std::move(itemComponents));
        auto items = std::make_unique<ListTag>();
        items->add(std::move(item));

        CompoundTag tag;
        tag.put("BrewTime", std::make_unique<ShortTag>(0));
        tag.put("Fuel", std::make_unique<ByteTag>(0));
        tag.put("Items", std::move(items));
        tag.put("components", compound());
        tag.put("id", std::make_unique<StringTag>("minecraft:brewing_stand"));
        check("brewing_stand", tag,
              "{BrewTime:0s,Fuel:0b,Items:[{Slot:1b,components:"
              "{\"minecraft:potion_contents\":{potion:\"minecraft:weakness\"}},"
              "count:1,id:\"minecraft:splash_potion\"}],components:{},"
              "id:\"minecraft:brewing_stand\"}");
    }

    // Float/double bit patterns + arrays (synthetic; spec cases).
    {
        CompoundTag tag;
        tag.put("f", std::make_unique<FloatTag>(1.0f));       // 0x3f800000
        tag.put("d", std::make_unique<DoubleTag>(-0.5));      // 0xbfe0000000000000
        tag.put("ba", std::make_unique<ByteArrayTag>(std::vector<int8_t>{1, -2}));
        tag.put("ia", std::make_unique<IntArrayTag>(std::vector<int32_t>{}));
        tag.put("la", std::make_unique<LongArrayTag>(std::vector<int64_t>{7}));
        check("numeric", tag,
              "{ba:[B;1b,-2b],d:d0xbfe0000000000000,f:f0x3f800000,"
              "ia:[I;],la:[L;7l]}");
    }

    if (failures) {
        std::cerr << failures << " fixture(s) FAILED\n";
        return 1;
    }
    std::cout << "all canonical NBT fixtures passed\n";
    return 0;
}
