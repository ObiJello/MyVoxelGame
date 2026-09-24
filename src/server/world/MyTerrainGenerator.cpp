#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
// File: src/server/world/MyTerrainGenerator.cpp
#include "MyTerrainGenerator.hpp"
#include "storage/SectionDataUnpacker.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/block/entity/BaseContainerBlockEntity.hpp"
#include "common/world/block/entity/SignBlockEntity.hpp"
#include "common/world/block/entity/LecternBlockEntity.hpp"
#include "common/world/block/entity/SpawnerBlockEntity.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "server/world/storage/NBTParser.hpp"
#include "server/world/storage/anvil/SpawnerNbt.hpp"
#include "common/world/block/entity/AurelithBlockEntities.hpp"
#include "server/world/storage/anvil/ItemStackNbt.hpp"   // ItemFromName
#include "common/world/biome/Biomes.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include <chrono>
#include <cmath>
#include <future>
#include <sstream>
#include <stdexcept>   // std::runtime_error — libc++ pulls it in transitively, MSVC does not

// Terrain library includes
#include "levelgen/ModTerrainSettings.h"
#include "levelgen/density/WorldgenRegistries.h"
#include "levelgen/density/terrain/TerrainSettings.h"
#include "levelgen/Heightmap.h"
#include "nbt/AllTags.h"
#include "nbt/NbtIo.h"
#include "world/biome/OverworldBiomeBuilder.h"
#include "world/biome/TheEndBiomeSource.h"
#include "world/biome/TwilightBiomeSource.h"
#include "data/worldgen/BiomeFeatureRegistry.h"
#include "levelgen/WorldGenTweaks.h"
#include <nlohmann/json.hpp>

using minecraft::world::level::block::Blocks;
using minecraft::world::BlockRegistry;
using minecraft::BlockState;

// MC DimensionType level heights (data/minecraft/dimension_type/*.json):
// overworld min_y -64 / height 384, the_nether and the_end min_y 0 /
// height 256.
//
// This is the LEVEL height — what the ServerChunkCache sizes its chunks to —
// and it is deliberately NOT the noise height. NETHER_NOISE_SETTINGS is
// (0, 128) inside a 256-tall nether and END_NOISE_SETTINGS is (0, 128) inside
// a 256-tall end (NoiseSettings.cpp:14-15); ChunkGenerator.cpp:715-721 clamps
// the noise range into the level range on purpose, which is what puts the
// nether's carve ceiling at y<=120. Do not "fix" the mismatch.
namespace {

    // ── Generated block entities ───────────────────────────────────────────
    //
    // MC has no hand-off here: a structure's chest is placed straight into
    // the level with its BlockEntity. The vendored library instead parks the
    // block-entity NBT on the ProtoChunk (ChunkAccess.setBlockEntityNbt) as
    // the CANONICAL single-line text its parity harness uses (nbt/
    // CanonicalNbt.h — NOT SNBT: keys sorted, floats as f0x…, strings with
    // only \\ and \" escaped). This reads that text back into the engine's
    // block entities so a template chest arrives with the Items the template
    // carried, or the LootTable key it will roll from when first opened
    // (BaseContainerBlockEntity::UnpackLootTable).
    struct CanonicalTag {
        // Number is every INTEGER scalar (its NBT width in `suffix`); Float /
        // Double carry the IEEE value decoded from the f0x… / d0x… bit
        // pattern. A typed array is a List whose `arrayType` is 'B', 'I' or
        // 'L'.
        enum class Kind { Compound, List, String, Number, Float, Double, Other };
        Kind kind = Kind::Other;
        std::map<std::string, CanonicalTag> compound;
        std::vector<CanonicalTag> list;
        std::string str;
        long long number = 0;
        double real = 0.0;
        char suffix = 0;       // Number: 'b', 's', 'l', or 0 for an int
        char arrayType = 0;    // List: 'B' / 'I' / 'L' for a typed array

        const CanonicalTag* Get(const char* key) const {
            auto it = compound.find(key);
            return it == compound.end() ? nullptr : &it->second;
        }
    };

    class CanonicalNbtReader {
    public:
        explicit CanonicalNbtReader(std::string_view text) : m_text(text) {}

        bool Read(CanonicalTag& out) {
            return Value(out) && m_pos == m_text.size();
        }

    private:
        bool Value(CanonicalTag& out) {
            if (m_pos >= m_text.size()) return false;
            const char c = m_text[m_pos];
            if (c == '{') return Compound(out);
            if (c == '[') return List(out);
            if (c == '"') { out.kind = CanonicalTag::Kind::String; return Quoted(out.str); }
            return Scalar(out);
        }

        bool Compound(CanonicalTag& out) {
            out.kind = CanonicalTag::Kind::Compound;
            ++m_pos;                                          // '{'
            if (Peek() == '}') { ++m_pos; return true; }
            for (;;) {
                std::string key;
                if (Peek() == '"') { if (!Quoted(key)) return false; }
                else {
                    const size_t start = m_pos;
                    while (m_pos < m_text.size() && m_text[m_pos] != ':') ++m_pos;
                    key.assign(m_text.substr(start, m_pos - start));
                }
                if (Peek() != ':') return false;
                ++m_pos;
                CanonicalTag value;
                if (!Value(value)) return false;
                out.compound[std::move(key)] = std::move(value);
                if (Peek() == ',') { ++m_pos; continue; }
                if (Peek() == '}') { ++m_pos; return true; }
                return false;
            }
        }

        bool List(CanonicalTag& out) {
            out.kind = CanonicalTag::Kind::List;
            ++m_pos;                                          // '['
            // Typed arrays: [B;…] [I;…] [L;…] — a type letter then ';'.
            if (m_pos + 1 < m_text.size() && m_text[m_pos + 1] == ';') {
                out.arrayType = m_text[m_pos];
                m_pos += 2;
            }
            if (Peek() == ']') { ++m_pos; return true; }
            for (;;) {
                CanonicalTag value;
                if (!Value(value)) return false;
                out.list.push_back(std::move(value));
                if (Peek() == ',') { ++m_pos; continue; }
                if (Peek() == ']') { ++m_pos; return true; }
                return false;
            }
        }

        bool Quoted(std::string& out) {
            ++m_pos;                                          // opening quote
            while (m_pos < m_text.size()) {
                const char c = m_text[m_pos++];
                if (c == '\\') {
                    if (m_pos >= m_text.size()) return false;
                    out += m_text[m_pos++];
                } else if (c == '"') {
                    return true;
                } else {
                    out += c;
                }
            }
            return false;
        }

        // <n>b <n>s <n> <n>l are integers; f0x… / d0x… are IEEE bit patterns
        // (kept as Other — nothing here needs them); anything else is Other.
        bool Scalar(CanonicalTag& out) {
            const size_t start = m_pos;
            while (m_pos < m_text.size()) {
                const char c = m_text[m_pos];
                if (c == ',' || c == '}' || c == ']') break;
                ++m_pos;
            }
            std::string_view token = m_text.substr(start, m_pos - start);
            if (token.empty()) return false;
            if ((token[0] == 'f' || token[0] == 'd') && token.size() > 3 && token[1] == '0' && token[2] == 'x') {
                // IEEE-754 bit patterns: f0x<8 hex> / d0x<16 hex>.
                const std::string hex(token.substr(3));
                char* hexEnd = nullptr;
                const unsigned long long bits = std::strtoull(hex.c_str(), &hexEnd, 16);
                if (hexEnd == hex.c_str() || *hexEnd != '\0') { out.kind = CanonicalTag::Kind::Other; return true; }
                if (token[0] == 'f') {
                    const uint32_t b32 = static_cast<uint32_t>(bits);
                    float f;
                    std::memcpy(&f, &b32, sizeof(f));
                    out.kind = CanonicalTag::Kind::Float;
                    out.real = f;
                } else {
                    double d;
                    std::memcpy(&d, &bits, sizeof(d));
                    out.kind = CanonicalTag::Kind::Double;
                    out.real = d;
                }
                return true;
            }
            const char suffix = token.back();
            if (suffix == 'b' || suffix == 's' || suffix == 'l') {
                out.suffix = suffix;
                token.remove_suffix(1);
            }
            char* end = nullptr;
            const std::string digits(token);
            const long long v = std::strtoll(digits.c_str(), &end, 10);
            if (end == digits.c_str() || *end != '\0') { out.kind = CanonicalTag::Kind::Other; return true; }
            out.kind = CanonicalTag::Kind::Number;
            out.number = v;
            return true;
        }

        char Peek() const { return m_pos < m_text.size() ? m_text[m_pos] : '\0'; }

        std::string_view m_text;
        size_t m_pos = 0;
    };

    // A canonical tag as the Anvil reader's NBT tree, so an item baked into
    // a template goes through the same ReadItemStack a saved chest does —
    // components (potions, enchantments, written books) included.
    ::World::NBTTagPtr CanonicalToNbt(const CanonicalTag& tag) {
        using namespace ::World;
        switch (tag.kind) {
            case CanonicalTag::Kind::Compound: {
                auto out = std::make_shared<NBTTagCompound>();
                for (const auto& [key, value] : tag.compound) {
                    if (auto child = CanonicalToNbt(value)) out->value[key] = std::move(child);
                }
                return out;
            }
            case CanonicalTag::Kind::List: {
                if (tag.arrayType == 'B') {
                    auto out = std::make_shared<NBTTagByteArray>();
                    for (const auto& e : tag.list) out->value.push_back(static_cast<int8_t>(e.number));
                    return out;
                }
                if (tag.arrayType == 'I') {
                    auto out = std::make_shared<NBTTagIntArray>();
                    for (const auto& e : tag.list) out->value.push_back(static_cast<int32_t>(e.number));
                    return out;
                }
                if (tag.arrayType == 'L') {
                    auto out = std::make_shared<NBTTagLongArray>();
                    for (const auto& e : tag.list) out->value.push_back(static_cast<int64_t>(e.number));
                    return out;
                }
                auto out = std::make_shared<NBTTagList>();
                for (const auto& e : tag.list) {
                    if (auto child = CanonicalToNbt(e)) {
                        if (out->value.empty()) out->listType = child->type;
                        out->value.push_back(std::move(child));
                    }
                }
                return out;
            }
            case CanonicalTag::Kind::String:
                return std::make_shared<NBTTagString>(tag.str);
            case CanonicalTag::Kind::Number:
                switch (tag.suffix) {
                    case 'b': return std::make_shared<NBTTagByte>(static_cast<int8_t>(tag.number));
                    case 's': return std::make_shared<NBTTagShort>(static_cast<int16_t>(tag.number));
                    case 'l': return std::make_shared<NBTTagLong>(static_cast<int64_t>(tag.number));
                    default:  return std::make_shared<NBTTagInt>(static_cast<int32_t>(tag.number));
                }
            case CanonicalTag::Kind::Float:
                return std::make_shared<NBTTagFloat>(static_cast<float>(tag.real));
            case CanonicalTag::Kind::Double:
                return std::make_shared<NBTTagDouble>(tag.real);
            case CanonicalTag::Kind::Other:
                break;
        }
        return nullptr;
    }

    // One item compound ({id, count, components?}) through the Anvil reader.
    Game::ItemStack ItemFromCanonical(const CanonicalTag& entry) {
        auto nbt = std::dynamic_pointer_cast<::World::NBTTagCompound>(CanonicalToNbt(entry));
        return nbt ? Game::Anvil::ReadItemStack(*nbt) : Game::ItemStack{};
    }

    // The Anvil loader's ReadContainerItems, over the canonical text instead
    // of a parsed tag: Slot plus the item (read by ReadItemStack, components
    // and all) per entry, unknown items and bad slots dropped exactly as
    // vanilla does.
    void FillContainerFromCanonical(const CanonicalTag& root, Game::BaseContainerBlockEntity& container) {
        // A structure chest normally carries only {LootTable, LootTableSeed}
        // (RandomizableContainer.setBlockEntityLootTable); the roll happens on
        // first access, exactly as in MC.
        if (const CanonicalTag* lootTable = root.Get("LootTable");
            lootTable && lootTable->kind == CanonicalTag::Kind::String && !lootTable->str.empty()) {
            const CanonicalTag* seed = root.Get("LootTableSeed");
            container.SetLootTable(lootTable->str,
                                   (seed && seed->kind == CanonicalTag::Kind::Number) ? seed->number : 0);
            return;
        }
        const CanonicalTag* items = root.Get("Items");
        if (!items || items->kind != CanonicalTag::Kind::List) return;
        for (const CanonicalTag& entry : items->list) {
            if (entry.kind != CanonicalTag::Kind::Compound) continue;
            const CanonicalTag* slot  = entry.Get("Slot");
            if (!slot || slot->kind != CanonicalTag::Kind::Number) continue;
            if (slot->number < 0 || slot->number >= container.GetContainerSize()) continue;

            Game::ItemStack stack = ItemFromCanonical(entry);
            if (stack.IsEmpty()) continue;
            container.SetItem(static_cast<int>(slot->number), stack);
        }
    }

    // MC SignBlockEntity.loadAdditional over the canonical text: front_text /
    // back_text (SignText's codec — four `messages`, a dye `color`,
    // `has_glowing_text`) and is_waxed. Template signs (Aurelith's street
    // names and plaques) carry their lines as plain-string text components;
    // a JSON-object component keeps only its "text", as the Anvil loader does.
    void FillSignFromCanonical(const CanonicalTag& root, Game::SignBlockEntity& sign) {
        auto readText = [&](const char* key, Game::SignTextSlot slot) {
            const CanonicalTag* text = root.Get(key);
            if (!text || text->kind != CanonicalTag::Kind::Compound) return;
            Game::SignText out;
            if (const CanonicalTag* messages = text->Get("messages");
                messages && messages->kind == CanonicalTag::Kind::List) {
                for (size_t i = 0; i < out.lines.size() && i < messages->list.size(); ++i) {
                    const CanonicalTag& line = messages->list[i];
                    if (line.kind == CanonicalTag::Kind::String) {
                        out.lines[i] = line.str;
                    } else if (line.kind == CanonicalTag::Kind::Compound) {
                        if (const CanonicalTag* t = line.Get("text");
                            t && t->kind == CanonicalTag::Kind::String) {
                            out.lines[i] = t->str;
                        }
                    }
                }
            }
            if (const CanonicalTag* colour = text->Get("color");
                colour && colour->kind == CanonicalTag::Kind::String) {
                Game::DyeColor dye = Game::DyeColor::Black;
                if (Game::DyeColorFromName(colour->str, dye)) out.color = dye;
            }
            if (const CanonicalTag* glow = text->Get("has_glowing_text");
                glow && glow->kind == CanonicalTag::Kind::Number) {
                out.glowing = glow->number != 0;
            }
            sign.SetText(slot, out);
        };
        readText("front_text", Game::SignTextSlot::Front);
        readText("back_text", Game::SignTextSlot::Back);
        if (const CanonicalTag* waxed = root.Get("is_waxed");
            waxed && waxed->kind == CanonicalTag::Kind::Number) {
            sign.SetWaxed(waxed->number != 0);
        }
    }

    // The canonical text as binary NBT, so the Anvil block-entity readers
    // (which speak NBTParser trees) can load a generated block entity the
    // same way they load a saved one. Kinds map one to one; an integer's
    // width is its suffix, a typed array its letter, a list's element type
    // its first element's (an empty list is TAG_End, as vanilla writes it).
    void WriteCanonicalValue(Game::Nbt::Writer& w, std::string_view name, const CanonicalTag& tag);

    Game::Nbt::TagType CanonicalElementType(const CanonicalTag& tag) {
        using T = Game::Nbt::TagType;
        switch (tag.kind) {
            case CanonicalTag::Kind::Compound: return T::Compound;
            case CanonicalTag::Kind::String:   return T::String;
            case CanonicalTag::Kind::Float:    return T::Float;
            case CanonicalTag::Kind::Double:   return T::Double;
            case CanonicalTag::Kind::List:
                return tag.arrayType == 'B' ? T::ByteArray
                     : tag.arrayType == 'I' ? T::IntArray
                     : tag.arrayType == 'L' ? T::LongArray : T::List;
            case CanonicalTag::Kind::Number:
                return tag.suffix == 'b' ? T::Byte : tag.suffix == 's' ? T::Short
                     : tag.suffix == 'l' ? T::Long : T::Int;
            default: return T::End;
        }
    }

    template <typename V>
    std::vector<V> CanonicalArray(const CanonicalTag& tag) {
        std::vector<V> out;
        out.reserve(tag.list.size());
        for (const CanonicalTag& e : tag.list) out.push_back(static_cast<V>(e.number));
        return out;
    }

    void WriteCanonicalElement(Game::Nbt::Writer& w, Game::Nbt::Writer::ListScope& list,
                               const CanonicalTag& tag) {
        using T = Game::Nbt::TagType;
        switch (CanonicalElementType(tag)) {
            case T::Compound:
                w.ListCompoundBegin(list);
                for (const auto& [key, child] : tag.compound) WriteCanonicalValue(w, key, child);
                w.ListCompoundEnd(list);
                break;
            case T::String: w.ListString(list, tag.str); break;
            case T::Float:  w.ListFloat(list, static_cast<float>(tag.real)); break;
            case T::Double: w.ListDouble(list, tag.real); break;
            case T::Byte:   w.ListByte(list, static_cast<int8_t>(tag.number)); break;
            case T::Short:  w.ListShort(list, static_cast<int16_t>(tag.number)); break;
            case T::Int:    w.ListInt(list, static_cast<int32_t>(tag.number)); break;
            case T::Long:   w.ListLong(list, static_cast<int64_t>(tag.number)); break;
            case T::ByteArray: { auto v = CanonicalArray<int8_t>(tag);  w.ListByteArray(list, v.data(), v.size()); break; }
            case T::IntArray:  { auto v = CanonicalArray<int32_t>(tag); w.ListIntArray(list, v.data(), v.size()); break; }
            case T::LongArray: { auto v = CanonicalArray<int64_t>(tag); w.ListLongArray(list, v.data(), v.size()); break; }
            case T::List: {
                auto nested = w.ListListBegin(list, tag.list.empty() ? T::End : CanonicalElementType(tag.list.front()));
                for (const CanonicalTag& e : tag.list) WriteCanonicalElement(w, nested, e);
                w.EndList(nested);
                break;
            }
            default: break;
        }
    }

    void WriteCanonicalValue(Game::Nbt::Writer& w, std::string_view name, const CanonicalTag& tag) {
        using T = Game::Nbt::TagType;
        switch (CanonicalElementType(tag)) {
            case T::Compound:
                w.BeginCompound(name);
                for (const auto& [key, child] : tag.compound) WriteCanonicalValue(w, key, child);
                w.EndCompound();
                break;
            case T::String: w.String(name, tag.str); break;
            case T::Float:  w.Float(name, static_cast<float>(tag.real)); break;
            case T::Double: w.Double(name, tag.real); break;
            case T::Byte:   w.Byte(name, static_cast<int8_t>(tag.number)); break;
            case T::Short:  w.Short(name, static_cast<int16_t>(tag.number)); break;
            case T::Int:    w.Int(name, static_cast<int32_t>(tag.number)); break;
            case T::Long:   w.Long(name, static_cast<int64_t>(tag.number)); break;
            case T::ByteArray: { auto v = CanonicalArray<int8_t>(tag);  w.ByteArray(name, v.data(), v.size()); break; }
            case T::IntArray:  { auto v = CanonicalArray<int32_t>(tag); w.IntArray(name, v.data(), v.size()); break; }
            case T::LongArray: { auto v = CanonicalArray<int64_t>(tag); w.LongArray(name, v.data(), v.size()); break; }
            case T::List: {
                auto list = w.BeginList(name, tag.list.empty() ? T::End : CanonicalElementType(tag.list.front()));
                for (const CanonicalTag& e : tag.list) WriteCanonicalElement(w, list, e);
                w.EndList(list);
                break;
            }
            default: break;
        }
    }

    std::shared_ptr<::World::NBTTagCompound> CanonicalToNbtTree(const CanonicalTag& root) {
        Game::Nbt::Writer w;
        w.BeginRootCompound();
        for (const auto& [key, child] : root.compound) WriteCanonicalValue(w, key, child);
        w.EndRootCompound();
        if (!w.ok()) return nullptr;
        try {
            return std::dynamic_pointer_cast<::World::NBTTagCompound>(
                ::World::NBTParser::Parse(w.TakeBytes()));
        } catch (const std::exception&) {
            return nullptr;
        }
    }

    void AttachGeneratedBlockEntities(Game::Chunk& gameChunk, const minecraft::world::IChunk& libChunk,
                                      Game::Math::ChunkPos position) {
        const auto* pending = libChunk.getBlockEntityNbts();
        if (!pending || pending->empty()) return;

        for (const auto& [key, text] : *pending) {
            const int worldY = std::get<0>(key);
            const int worldZ = std::get<1>(key);
            const int worldX = std::get<2>(key);
            if ((worldX >> 4) != position.x || (worldZ >> 4) != position.z) continue;

            CanonicalTag root;
            if (!CanonicalNbtReader(text).Read(root) || root.kind != CanonicalTag::Kind::Compound) {
                Log::Warning("[MyTerrainGenerator] unreadable block-entity text at (%d,%d,%d): %s",
                             worldX, worldY, worldZ, text.c_str());
                continue;
            }
            const CanonicalTag* idTag = root.Get("id");
            if (!idTag || idTag->kind != CanonicalTag::Kind::String) continue;
            std::string id = idTag->str;
            if (id.rfind("minecraft:", 0) == 0) id.erase(0, 10);

            const int localX = worldX & 15;
            const int localZ = worldZ & 15;
            const Game::BlockID blockAt = gameChunk.GetBlock(localX, worldY, localZ);

            // "DUMMY" marks a block placed during generation that owns a block
            // entity but carries no data (WorldGenRegion.setBlock ->
            // ProtoChunk.setBlockEntityNbt): MC's LevelChunk.
            // promotePendingBlockEntity creates the block's own entity.
            const Game::BlockEntityType* type = id == "DUMMY"
                ? Game::BlockEntityTypes::ForBlock(blockAt)
                : Game::BlockEntityTypes::ByStringId(id);
            if (!type) continue;                              // a block entity this build lacks
            if (!type->IsValidFor(blockAt)) continue;         // the template's block did not survive placement

            auto entity = type->Create(glm::ivec3(worldX, worldY, worldZ), blockAt);
            if (!entity) continue;
            if (auto* container = dynamic_cast<Game::BaseContainerBlockEntity*>(entity.get())) {
                FillContainerFromCanonical(root, *container);
            }
            if (auto* sign = dynamic_cast<Game::SignBlockEntity*>(entity.get())) {
                FillSignFromCanonical(root, *sign);
            }
            if (auto* lectern = dynamic_cast<Game::LecternBlockEntity*>(entity.get())) {
                // MC LecternBlockEntity.loadAdditional: Book (an ItemStack,
                // components and all) and Page, clamped by the entity.
                const CanonicalTag* book = root.Get("Book");
                const CanonicalTag* page = root.Get("Page");
                if (book && book->kind == CanonicalTag::Kind::Compound) {
                    lectern->LoadFromNbt(ItemFromCanonical(*book),
                                         (page && page->kind == CanonicalTag::Kind::Number)
                                             ? static_cast<int>(page->number) : 0);
                }
            }
            // Aurelith's quest block entities (AurelithBlockEntities.hpp):
            // the engine's rotation, a pedestal's or socket's Item, the
            // cabinet's contents and song.
            if (auto* engine = dynamic_cast<Game::ResonanceEngineBlockEntity*>(entity.get())) {
                const CanonicalTag* rotation = root.Get("Rotation");
                if (rotation && rotation->kind == CanonicalTag::Kind::Number) {
                    engine->SetRotation(static_cast<int>(rotation->number));
                }
            }
            const CanonicalTag* item = root.Get("Item");
            const bool hasItem = item && item->kind == CanonicalTag::Kind::Compound;
            if (auto* pedestal = dynamic_cast<Game::VoicePedestalBlockEntity*>(entity.get())) {
                if (hasItem) pedestal->LoadFromNbt(ItemFromCanonical(*item));
            }
            if (auto* socket = dynamic_cast<Game::ChordSocketBlockEntity*>(entity.get())) {
                if (hasItem) socket->LoadFromNbt(ItemFromCanonical(*item), 0, false);
            }
            if (auto* cabinet = dynamic_cast<Game::ChoirCabinetBlockEntity*>(entity.get())) {
                std::vector<Game::ItemStack> contents;
                if (const CanonicalTag* items = root.Get("Items");
                    items && items->kind == CanonicalTag::Kind::List) {
                    for (const CanonicalTag& entry : items->list) {
                        if (entry.kind != CanonicalTag::Kind::Compound) continue;
                        Game::ItemStack stack = ItemFromCanonical(entry);
                        if (!stack.IsEmpty()) contents.push_back(std::move(stack));
                    }
                }
                std::vector<std::string> melody;
                if (const CanonicalTag* notes = root.Get("Melody");
                    notes && notes->kind == CanonicalTag::Kind::List) {
                    for (const CanonicalTag& note : notes->list) {
                        if (note.kind == CanonicalTag::Kind::String) melody.push_back(note.str);
                    }
                }
                cabinet->LoadFromNbt(std::move(contents), std::move(melody), 0, false);
            }
            if (auto* spawner = dynamic_cast<Game::SpawnerBlockEntity*>(entity.get())) {
                // MC BaseSpawner.load over the generated tag — the dungeon's,
                // mineshaft's, stronghold's, fortress's or a template's
                // SpawnData (and any SpawnPotentials / limits it carries).
                if (auto tree = CanonicalToNbtTree(root)) Game::Anvil::ReadSpawner(*tree, *spawner);
            }
            gameChunk.SetBlockEntity(localX, worldY, localZ, std::move(entity));
        }
    }

    // MC ProtoChunk.getEntities, handed over for ServerLevel.
    // addWorldGenChunkEntities. WorldGenRegion.addFreshEntity already filed
    // each entity under the chunk its position falls in; the position test
    // here only guards against a region write straying into a neighbour.
    void AttachGeneratedEntities(Game::Chunk& gameChunk, const minecraft::world::IChunk& libChunk,
                                 Game::Math::ChunkPos position) {
        const auto* pending = libChunk.getEntities();
        if (!pending || pending->empty()) return;

        gameChunk.worldgenEntities.reserve(pending->size());
        for (const auto& entity : *pending) {
            if (!entity.tag) continue;
            const minecraft::nbt::ListTag* pos = entity.tag->getListPtr("Pos");
            if (!pos || pos->size() != 3) continue;
            const int blockX = static_cast<int>(std::floor(pos->getDouble(0)));
            const int blockZ = static_cast<int>(std::floor(pos->getDouble(2)));
            if ((blockX >> 4) != position.x || (blockZ >> 4) != position.z) {
                Log::Warning("[MyTerrainGenerator] worldgen entity %s at (%d,%d) filed under chunk (%d,%d)",
                             entity.tag->getStringOr("id", "?").c_str(), blockX, blockZ,
                             position.x, position.z);
                continue;
            }
            std::ostringstream bytes(std::ios::binary);
            minecraft::nbt::NbtIo::write(*entity.tag, bytes);
            const std::string buffer = bytes.str();
            Game::WorldgenEntity out;
            out.nbt.assign(buffer.begin(), buffer.end());
            out.finalizeSpawn = entity.finalizeSpawn;
            gameChunk.worldgenEntities.push_back(std::move(out));
        }
    }

    struct DimensionHeight {
        int minY;
        int height;
    };
    constexpr DimensionHeight OVERWORLD_LEVEL{-64, 384};
    constexpr DimensionHeight NETHER_LEVEL{0, 256};
    constexpr DimensionHeight END_LEVEL{0, 256};
    // The Hush: an engine-only surface dimension (DimensionId::Hush) on the
    // Overworld's noise settings, so it shares the Overworld's level height.
    constexpr DimensionHeight HUSH_LEVEL{-64, 384};
    // The Aether (dimension_type/the_aether.json): min_y 0, height 256 —
    // twice its 128-tall skylands noise, like the nether/end mismatch above.
    constexpr DimensionHeight AETHER_LEVEL{0, 256};
    // The Twilight Forest (dimension_type/twilight_forest_type.json): min_y
    // -32, height 288 — its twilight_noise_gen noise is (-32, 256), so the
    // top 32 blocks are always above the terrain.
    constexpr DimensionHeight TWILIGHT_LEVEL{-32, 288};
}

// Epoch for the thread_local MapBlockType caches. Bumped every generator
// Initialize() because Blocks::bootstrap() may recreate Block objects on
// world reload — a worker's cached pointers from the previous world would
// otherwise alias freshly allocated blocks at reused addresses.
static std::atomic<uint32_t> s_blockMapEpoch{1};

// Run BOTH conversion paths and compare, logging any disagreement.
//
// This is the gate for the palette remap. tools/terrain_parity cannot serve as
// one: CLAUDE.md is explicit that it "links terrain_library alone (no game
// code)", so it is unchanged by anything in this file by construction and would
// pass trivially. Checking against the per-voxel original instead covers every
// section of every chunk a real session generates, which is strictly more.
//
// Costs roughly double conversion when on. Off in every normal build.
static constexpr bool kVerifyPaletteConvert = false;

namespace Game {

    // Two dedicated threads for the library's serial schedulers (dispatcher
    // mailbox + worldgen lane) — see ChunkMap's constructor for why they must
    // not share the FIFO worldgen pool.
    BackgroundExecutor& SharedLaneExecutor() {
        static BackgroundExecutor lane(2);
        return lane;
    }

    BackgroundExecutor& SharedBackgroundExecutor() {
        // Leaked on purpose — see the declaration for why it must outlive
        // static destruction rather than race it.
        static BackgroundExecutor* pool = [] {
            auto* p = new BackgroundExecutor();
            Log::Info("[MyTerrainGenerator] Shared worldgen pool started (%zu threads, "
                      "shared by every dimension)",
                      BackgroundExecutor::DefaultThreadCount());
            return p;
        }();
        return *pool;
    }

    MyTerrainGenerator::MyTerrainGenerator(const GenerationConfig& config)
        : m_config(config) {
        Log::Info("[MyTerrainGenerator] Created for dimension '%s' with seed: %lld",
                  config.dimension.c_str(), static_cast<long long>(config.seed));
    }

    MyTerrainGenerator::~MyTerrainGenerator() {
        Shutdown();
    }

    bool MyTerrainGenerator::Initialize() {
        if (m_initialized) {
            Log::Warning("[MyTerrainGenerator] Already initialized");
            return true;
        }

        try {
            int64_t seed = static_cast<int64_t>(m_config.seed);
            Log::Info("[MyTerrainGenerator] Initializing with seed: %lld", seed);

            // Invalidate every worker thread's MapBlockType cache — the
            // bootstrap below may recreate Block objects, and stale cached
            // pointers from a previous world could alias reused addresses.
            s_blockMapEpoch.fetch_add(1, std::memory_order_release);

            // ================================================================
            // Step 1: Bootstrap registries (once per program)
            // ================================================================
            // The worldgen registries (noise, density_function,
            // noise_settings, material_rule) are datapack JSON read on first
            // use through density::WorldgenRegistries; only the block table
            // needs bootstrapping up front.
            Blocks::bootstrap();
            Log::Info("[MyTerrainGenerator] Registries bootstrapped");

            // ================================================================
            // Step 2: Cache block states
            // ================================================================
            m_airBlock = Blocks::AIR->defaultBlockState();
            m_stoneBlock = Blocks::STONE->defaultBlockState();

            // ================================================================
            // Step 3: Create block registry
            // ================================================================
            m_blockRegistry = new BlockRegistry();
            m_blockRegistry->registerBlock(m_airBlock);
            m_blockRegistry->registerBlock(m_stoneBlock);
            m_blockRegistry->registerBlock(Blocks::WATER->defaultBlockState());
            m_blockRegistry->registerBlock(Blocks::LAVA->defaultBlockState());
            m_blockRegistry->registerBlock(Blocks::DEEPSLATE->defaultBlockState());
            m_blockRegistry->registerBlock(Blocks::BEDROCK->defaultBlockState());
            m_blockRegistry->registerBlock(Blocks::GRASS_BLOCK->defaultBlockState());
            m_blockRegistry->registerBlock(Blocks::DIRT->defaultBlockState());
            m_blockRegistry->registerBlock(Blocks::SAND->defaultBlockState());
            m_blockRegistry->registerBlock(Blocks::GRAVEL->defaultBlockState());
            m_blockRegistry->registerBlock(Blocks::TUFF->defaultBlockState());
            Log::Info("[MyTerrainGenerator] BlockRegistry initialized");

            // ================================================================
            // Step 4: Create world generation components
            //
            // Two INDEPENDENT axes:
            //
            //   dimension  - "overworld" / "nether" / "end" (MC LevelStem).
            //                Selects the noise router, noise settings, biome
            //                source, surface rules, fluid picker, default
            //                block/fluid, sea level, random algorithm and
            //                level height.
            //
            //   world type - MC WorldPresets: "default", "large_biomes",
            //                "amplified", "single_biome_surface", "flat".
            //                Large/amplified differ from default ONLY in the
            //                noise router (NoiseGeneratorSettings.overworld(
            //                ctx, amplified, large)); single-biome swaps the
            //                biome source for a FixedBiomeSource; flat swaps
            //                the whole generator for FlatLevelSource.
            //
            // The presets are all defined against the overworld router/biome
            // source, so they are OVERWORLD ONLY — the parity harness rejects
            // the combination outright ("--world-type is overworld-only",
            // CppChunkGeneratorTest.cpp:963-966). Here the nether/end are
            // constructed by the engine rather than typed by a user, so an
            // accidental combination is forced back to "default" and logged
            // instead of failing world creation.
            // ================================================================
            const bool isNether = (m_config.dimension == "nether");
            const bool isEnd    = (m_config.dimension == "end");
            // The Hush (DimensionGeneratorKey(DimensionId::Hush) == "hush"):
            // engine-only dimension on the Overworld's router and noise
            // settings with its own biome source, surface rules, default
            // block and features. It is NOT the overworld — isOverworld
            // excludes it so the WorldGenTweaks writer guard, the world-type
            // presets and the spawn target all stay Overworld-only.
            const bool isHush   = (m_config.dimension == "hush");
            // The Aether (DimensionGeneratorKey(DimensionId::Aether) ==
            // "aether"): skylands.json — its own router, noise settings,
            // biome source, surface rules and features. Not the overworld.
            const bool isAether = (m_config.dimension == "aether");
            // The Twilight Forest (DimensionGeneratorKey(DimensionId::
            // TwilightForest) == "twilight_forest"): twilight_noise_gen.json —
            // its own router, noise settings, layer-stack biome source,
            // surface rules, carvers and features. Its default block is
            // vanilla stone, so the library tells it apart by the dimension
            // tag set on its NoiseGeneratorSettings below. Not the overworld.
            const bool isTwilight = (m_config.dimension == "twilight_forest");
            const bool isOverworld = !isNether && !isEnd && !isHush && !isAether && !isTwilight;
            if (isOverworld && m_config.dimension != "overworld") {
                Log::Warning("[MyTerrainGenerator] Unknown dimension '%s' — generating"
                             " overworld", m_config.dimension.c_str());
            }
            const char* dimensionName =
                isNether ? "nether"
                         : (isEnd ? "end"
                                  : (isHush ? "hush"
                                            : (isAether ? "aether"
                                                        : (isTwilight ? "twilight_forest" : "overworld"))));
            Log::Info("[MyTerrainGenerator] Dimension: %s", dimensionName);

            if (!isOverworld && !m_config.worldType.empty()
                && m_config.worldType != "default") {
                Log::Warning("[MyTerrainGenerator] World type '%s' is overworld-only —"
                             " ignoring it for the %s", m_config.worldType.c_str(),
                             dimensionName);
            }
            const std::string worldType = isOverworld ? m_config.worldType : "default";
            const bool isFlat = (worldType == "flat" || worldType == "superflat");
            const bool isAmplified = (worldType == "amplified");
            const bool isLargeBiomes = (worldType == "large_biomes");
            const bool isSingleBiome =
                (worldType == "single_biome" || worldType == "single_biome_surface");
            Log::Info("[MyTerrainGenerator] World type: %s", worldType.c_str());

            // MC DimensionType level height for this dimension. Read the
            // DimensionHeight comment at the top of this file before touching
            // it — the nether/end level height is NOT their noise height.
            const DimensionHeight levelHeight =
                isNether ? NETHER_LEVEL
                         : (isEnd ? END_LEVEL
                                  : (isHush ? HUSH_LEVEL
                                            : (isAether ? AETHER_LEVEL
                                                        : (isTwilight ? TWILIGHT_LEVEL : OVERWORLD_LEVEL))));

            // World Properties sandbox tweaks: reset to pure vanilla, then
            // apply the world's JSON BEFORE any biome source / generator
            // construction (the MultiNoise biome filter reads them in its
            // constructor). Empty JSON = untouched vanilla generation.
            //
            // OVERWORLD ONLY, and that is a correctness guard, not a feature
            // decision: WorldGenTweaks::get() is one PROCESS-GLOBAL struct
            // (WorldGenTweaks.h) and reset() overwrites it wholesale. With a
            // generator per dimension, letting the nether initialize second
            // would wipe the overworld's tweaks — silently, with no crash, and
            // presenting to the player as "my amplified/no-caves world stopped
            // being amplified" long after world creation. Making the overworld
            // generator the single writer removes the hazard entirely and does
            // not depend on which dimension initializes first (a std::once_flag
            // would, and would hand the tweaks to whichever dimension happened
            // to come up first). The tweaks are a whole-world property and
            // still apply to every dimension: this only controls who WRITES
            // them.
            if (isOverworld) {
                minecraft::levelgen::WorldGenTweaks::reset();
                if (!m_config.worldgenTweaks.empty()) {
                    try {
                        auto& tweaks = minecraft::levelgen::WorldGenTweaks::get();
                        nlohmann::json tj = nlohmann::json::parse(m_config.worldgenTweaks);
                        tweaks.carversEnabled = tj.value("caves", true);
                        if (tj.contains("steps") && tj["steps"].is_array()) {
                            for (size_t i = 0; i < tweaks.featureStepEnabled.size()
                                               && i < tj["steps"].size(); ++i) {
                                tweaks.featureStepEnabled[i] = tj["steps"][i].get<bool>();
                            }
                        }
                        tweaks.featureDensity     = tj.value("featureDensity", 1.0f);
                        tweaks.oreDensity         = tj.value("oreDensity", 1.0f);
                        tweaks.vegetationDensity  = tj.value("vegetationDensity", 1.0f);
                        tweaks.structureFrequency = tj.value("structureFrequency", 1.0f);
                        for (const auto& b : tj.value("disabledBiomes",
                                                      nlohmann::json::array())) {
                            tweaks.disabledBiomes.insert(b.get<std::string>());
                        }
                        if (!tweaks.isDefault()) {
                            Log::Info("[MyTerrainGenerator] World Properties tweaks ACTIVE"
                                      " (non-vanilla generation)");
                        }
                    } catch (const std::exception& e) {
                        Log::Warning("[MyTerrainGenerator] Bad worldgenTweaks JSON (%s)"
                                     " - using vanilla generation", e.what());
                        minecraft::levelgen::WorldGenTweaks::reset();
                    }
                }
            }

            // ---- Noise settings + biome source, per dimension. The vanilla
            // dimensions read their worldgen/noise_settings entry exactly as a
            // world load does (router, aquifers, material rule, spawn target,
            // sea level, default block/fluid, random algorithm); the engine's
            // own dimensions build theirs in code (ModTerrainSettings).
            //
            // THE DEFAULT BLOCK IS LOAD-BEARING BEYOND THE FILL COLOUR. The
            // library has no dimension enum; ChunkStatusTasks picks the
            // nether/end/Hush/Aether featuresPerStep and NoiseBasedChunk-
            // Generator the carvers from it ("minecraft:netherrack", ...), and
            // featuresPerStep drives setFeatureSeed, so the wrong one silently
            // reseeds every feature. The Twilight Forest's default block is
            // vanilla stone, so it is told apart by its dimension tag.
            std::shared_ptr<const minecraft::levelgen::density::TerrainSettings> terrain;
            std::string dimensionTag;
            if (isNether) {
                terrain = minecraft::levelgen::density::TerrainSettings::load(
                    "minecraft:nether", minecraft::levelgen::density::WorldgenRegistries::get());
                m_biomeSource = minecraft::world::biome::MultiNoiseBiomeSource::createNether();
            } else if (isEnd) {
                terrain = minecraft::levelgen::density::TerrainSettings::load(
                    "minecraft:end", minecraft::levelgen::density::WorldgenRegistries::get());
                // TheEndBiomeSource is the one biome source that takes the
                // seed directly — the End's island layout is simplex noise
                // over the world seed, not a climate lookup.
                m_biomeSource =
                    std::make_unique<minecraft::world::biome::TheEndBiomeSource>(seed);
            } else if (isHush) {
                // The Overworld's router and aquifers under hushstone, sea
                // level 50 and the Hush material rules: its climate bands need
                // the continents / erosion / depth functions the nether and
                // end routers drop.
                terrain = minecraft::levelgen::ModTerrainSettings::hush();
                m_biomeSource = minecraft::world::biome::MultiNoiseBiomeSource::createHush();
            } else if (isAether) {
                // skylands.json (AetherNoiseBuilders): 3D blended noise
                // squeezed between two Y gradients into islands between y 8
                // and 128; aether:* shifted noises drive the_aether.json's
                // multi-noise source.
                terrain = minecraft::levelgen::ModTerrainSettings::aether();
                m_biomeSource = minecraft::world::biome::MultiNoiseBiomeSource::createAether();
            } else if (isTwilight) {
                // dimension/twilight_forest.json: biome source
                // twilightforest:twilight_biomes over the legacy layer stack,
                // settings twilight_noise_gen. The router's biome_driven_terrain
                // / biome_driven_noise read the same layout, so the source is
                // built first and hands its layout to the settings.
                auto twilightSource =
                    std::make_unique<minecraft::world::biome::TwilightBiomeSource>(seed);
                terrain = minecraft::levelgen::ModTerrainSettings::twilight(twilightSource->layout());
                m_biomeSource = std::move(twilightSource);
                dimensionTag = "twilight_forest";
            } else {
                // WorldPresets: large_biomes / amplified select their own
                // noise_settings; default, single_biome_surface and flat use
                // minecraft:overworld (flat only for the RandomState, which
                // nothing in a flat world samples - Java uses dummy()).
                const char* key = isLargeBiomes ? "minecraft:large_biomes"
                                                : (isAmplified ? "minecraft:amplified" : "minecraft:overworld");
                terrain = minecraft::levelgen::density::TerrainSettings::load(
                    key, minecraft::levelgen::density::WorldgenRegistries::get());
                if (isSingleBiome) {
                    // Reference: WorldPresets SINGLE_BIOME_SURFACE -
                    // FixedBiomeSource + normal overworld noise settings.
                    std::string biome = m_config.singleBiome.empty()
                        ? "minecraft:plains" : m_config.singleBiome;
                    if (biome.find(':') == std::string::npos) biome = "minecraft:" + biome;
                    if (!minecraft::data::worldgen::BiomeFeatureRegistry::isKnownBiomeKey(biome)) {
                        Log::Warning("[MyTerrainGenerator] Unknown single biome '%s', using plains",
                                     biome.c_str());
                        biome = "minecraft:plains";
                    }
                    Log::Info("[MyTerrainGenerator] Single biome: %s", biome.c_str());
                    m_biomeSource =
                        std::make_unique<minecraft::world::biome::FixedBiomeSource>(biome);
                } else if (!isFlat) {
                    m_biomeSource =
                        minecraft::world::biome::MultiNoiseBiomeSource::createOverworld();
                }
                // flat: FlatLevelSource owns its own FixedBiomeSource, so
                // m_biomeSource stays null.
            }
            m_settings = std::make_shared<minecraft::levelgen::NoiseGeneratorSettings>(terrain, dimensionTag);
            m_randomState = minecraft::levelgen::RandomState::create(m_settings, seed);

            if (isFlat) {
                // Reference: WorldPresets FLAT - FlatLevelSource with the
                // selected preset (default = FlatLevelGeneratorSettings.
                // getDefault()), optionally overridden by a custom
                // "<layers>;<biome>" string (PresetFlatWorldScreen format).
                minecraft::levelgen::flat::FlatLevelGeneratorSettings flatSettings =
                    m_config.flatPreset.empty()
                        ? minecraft::levelgen::flat::FlatLevelGeneratorSettings::getDefault()
                        : minecraft::levelgen::flat::FlatLevelGeneratorSettings::preset(
                              m_config.flatPreset);
                if (!m_config.flatLayers.empty()) {
                    flatSettings = minecraft::levelgen::flat::FlatLevelGeneratorSettings::
                        fromString(m_config.flatLayers, flatSettings);
                }
                Log::Info("[MyTerrainGenerator] Flat settings: %s",
                          flatSettings.toString().c_str());
                auto* flatGenerator =
                    new minecraft::levelgen::FlatLevelSource(std::move(flatSettings));
                flatGenerator->setLevelHeightRange(levelHeight.minY, levelHeight.height);
                m_generator = flatGenerator;
            } else {
                auto* noiseGenerator = new minecraft::levelgen::NoiseBasedChunkGenerator(m_settings);
                noiseGenerator->setBiomeSource(m_biomeSource.get());
                m_generator = noiseGenerator;
            }
            Log::Info("[MyTerrainGenerator] World generation components created");

            // ================================================================
            // Step 5: Create executors (lease on the shared pool + main thread
            // queue). The pool itself is process-wide and shared by every
            // dimension — see SharedBackgroundExecutor in the header.
            // ================================================================
            m_backgroundLease = std::make_unique<SharedExecutorLease>();
            if (const char* deco = std::getenv("OBEY_DECO_THREADS")) {
                const int n = std::atoi(deco);
                if (n > 0) m_decorationPool = std::make_unique<BackgroundExecutor>(static_cast<size_t>(n), /*elevated=*/true);
            }
            m_mainThreadExecutor = std::make_unique<MainThreadExecutor>();
            {
                MainThreadExecutor* exec = m_mainThreadExecutor.get();
                std::lock_guard<std::mutex> lock(m_sink->mutex);
                m_sink->closed = false;
                m_sink->wake = [exec]() { exec->wakeAll(); };
            }
            Log::Info("[MyTerrainGenerator] Executors created (leased the shared %zu-thread"
                      " worldgen pool)", BackgroundExecutor::DefaultThreadCount());

            // ================================================================
            // Step 6: Create ServerChunkCache (the full async pipeline)
            //
            // This is the SAME pipeline as async_chunk_test and Minecraft's
            // DedicatedServer. Chunks flow through:
            //   ServerChunkCache -> ChunkMap -> DistanceManager ->
            //   ChunkGenerationTask -> Worker Threads
            // ================================================================
            m_chunkCache = std::make_unique<minecraft::server::level::ServerChunkCache>(
                m_generator,
                m_randomState,
                seed,
                m_backgroundLease->getExecutor(),
                m_mainThreadExecutor->getExecutor(),
                nullptr,   // lane executor: share the pool (a dedicated one measured no gain and complicated shutdown)
                m_blockRegistry,
                m_airBlock,
                m_stoneBlock,
                // LEVEL height for this dimension (harness:1105-1106 with
                // --dimension, harness:922-933). 256-tall nether/end even
                // though their noise settings are 128 tall.
                levelHeight.minY,
                levelHeight.height,
                m_config.storagePath);

            if (m_decorationPool) m_chunkCache->getChunkMap().worldGenContextMutable().decorationExecutor = m_decorationPool->getExecutor();
            // processUnloads (run by tick(), TickLibrary) must not free a
            // holder whose chunk is being converted: the completion hands
            // the game a raw pointer into it.
            m_chunkCache->setUnloadVeto([this](int64_t key) {
                const minecraft::world::ChunkPos pos(key);
                return !IsConversionPinned(Math::ChunkPos{pos.x(), pos.z()});
            });
            // MC ChunkMap.onChunkReadyToSend: a chunk whose 3x3 is FULL goes
            // to whoever is waiting for it, whichever ticket brought it
            // there. Pinned here, on the main thread and before any
            // processUnloads can see it; ServiceGenerationQueues takes it or
            // unpins it.
            {
                std::shared_ptr<CompletionSink> sink = m_sink;
                m_chunkCache->getChunkMap().setChunkReadyListener(
                    [this, sink](int64_t key, minecraft::world::IChunk* chunk) {
                        const minecraft::world::ChunkPos pos(key);
                        const Math::ChunkPos position{pos.x(), pos.z()};
                        std::lock_guard<std::mutex> lock(sink->mutex);
                        if (sink->closed) return;
                        PinReady(position);
                        sink->ready.push_back(Completion{position, chunk});
                    });
            }
            m_chunkCache->setTaskPoller([this]() {
                if (m_mainThreadExecutor->hasPendingTasks()) {
                    m_mainThreadExecutor->runPendingTasks();
                }
            });
            if (m_libraryStorage) {
                // MC ChunkMap's storage: unfinished chunks are saved on unload
                // and read back (processUnloads -> save), in the world's own
                // region files.
                m_chunkCache->getChunkMap().setChunkStorage(m_libraryStorage, m_libraryDataVersion);
                Log::Info("[MyTerrainGenerator] Library chunk storage: world region files (DataVersion %d)",
                          m_libraryDataVersion);
            }
            Log::Info("[MyTerrainGenerator] ServerChunkCache created");
            {   // Diagnostics: accumulated radii of both pyramids for a FULL target.
                using minecraft::world::chunk::status::ChunkPyramid;
                using minecraft::world::chunk::status::ChunkStatus;
                std::string gen, load;
                for (const auto* st : ChunkStatus::getStatusList()) {
                    gen  += st->getName() + "=" + std::to_string(ChunkPyramid::getGenerationPyramid().getStepTo(ChunkStatus::FULL).getAccumulatedRadiusOf(*st)) + " ";
                    load += st->getName() + "=" + std::to_string(ChunkPyramid::getLoadingPyramid().getStepTo(ChunkStatus::FULL).getAccumulatedRadiusOf(*st)) + " ";
                }
                Log::Info("[LibDiag] generation pyramid radii (FULL): %s", gen.c_str());
                Log::Info("[LibDiag] loading pyramid radii (FULL):    %s", load.c_str());
            }

            // ================================================================
            // Step 6.5: Structure generation (MC WorldOptions.generateStructures)
            //
            // Mirrors MC ChunkStatusTasks.generateStructureStarts: when the
            // world option is off, no structure state exists and the starts
            // task is a no-op — references and the decoration structure pass
            // then naturally do nothing. When on, build the placement state
            // (structure_set/structure JSONs from data/) and inject it into
            // the pipeline's WorldGenContext, exactly like the parity harness.
            // ================================================================
            if (m_config.generateStructures) {
                if (auto* flatGenerator =
                        dynamic_cast<minecraft::levelgen::FlatLevelSource*>(m_generator)) {
                    // Reference: FlatLevelSource.createState - the flat
                    // settings' structure_overrides (or ALL sets when none),
                    // via createForFlat (concentricRingsSeed = 0).
                    std::vector<const minecraft::levelgen::structure::StructureSet*> sets;
                    const auto& overrides = flatGenerator->settings().structureOverrides();
                    if (overrides.has_value()) {
                        for (const std::string& setName : *overrides) {
                            sets.push_back(&minecraft::levelgen::structure::StructureSets::byName(setName));
                        }
                    } else {
                        sets = minecraft::levelgen::structure::StructureSets::all();
                    }
                    m_structureState = std::make_unique<
                        minecraft::levelgen::structure::ChunkGeneratorStructureState>(
                        minecraft::levelgen::structure::ChunkGeneratorStructureState::createForFlat(
                            m_randomState->sampler(), seed, flatGenerator->biomeSource(), sets));
                } else {
                    m_structureState = std::make_unique<
                        minecraft::levelgen::structure::ChunkGeneratorStructureState>(
                        minecraft::levelgen::structure::ChunkGeneratorStructureState::createForNormal(
                            m_randomState->sampler(), seed, m_biomeSource.get(),
                            minecraft::levelgen::structure::StructureSets::all()));
                }
                // Stronghold ring positions: start now, on the terrain pool,
                // exactly as Java's supplyAsync tasks do — measured 2.2 s of
                // serial lane time at the first structure step otherwise.
                m_structureState->startRingGeneration(m_backgroundLease->getExecutor());
                m_chunkCache->getChunkMap().worldGenContextMutable().structureState =
                    m_structureState.get();
                // Gates the structure pass in applyBiomeDecoration (Java:
                // structureManager.shouldGenerateStructures()).
                m_generator->setGenerateStructures(true);
                Log::Info("[MyTerrainGenerator] Structures ENABLED (%zu structure sets)",
                          m_structureState->possibleStructureSets().size());
            } else {
                Log::Info("[MyTerrainGenerator] Structures DISABLED (world option)");
            }

            // ================================================================
            // Step 7: Set target chunk status
            // Full generation: EMPTY -> FULL (phases 0-11)
            // ================================================================
            m_targetStatus = &minecraft::world::chunk::status::ChunkStatus::FULL;
            Log::Info("[MyTerrainGenerator] Target status: %s", m_targetStatus->getName().c_str());

            m_initialized = true;
            Log::Info("[MyTerrainGenerator] Initialization complete!");
            return true;

        } catch (const std::exception& e) {
            Log::Error("[MyTerrainGenerator] Initialization failed: %s", e.what());
            Shutdown();
            return false;
        }
    }

    bool MyTerrainGenerator::IsAbortRequested() const {
        return m_chunkCache && m_chunkCache->isAbortRequested();
    }

    void MyTerrainGenerator::RequestAbort() {
        if (m_chunkCache) {
            m_chunkCache->requestAbort();
        }
        // The server thread may be parked in waitForTasks until the next tick
        // deadline. Wake it so shutdown does not wait out the remainder of the
        // idle window.
        if (m_mainThreadExecutor) {
            m_mainThreadExecutor->wakeAll();
        }
    }

    void MyTerrainGenerator::SetLibraryGameTime(int64_t gameTime) {
        if (m_chunkCache) m_chunkCache->getChunkMap().setGameTime(gameTime);
    }

    void MyTerrainGenerator::Shutdown() {
        // Orphan the completion sink first: futures may still complete on
        // pool threads while the executors below are torn down.
        {
            std::lock_guard<std::mutex> lock(m_sink->mutex);
            m_sink->closed = true;
            m_sink->wake = nullptr;
            m_sink->completions.clear();
        }
        if (!m_initialized) return;

        Log::Info("[MyTerrainGenerator] Shutting down...");

        // Fence this generator's background work FIRST so no task references
        // destroyed objects. The shared pool is NOT stopped — the other
        // dimensions are still running on it; closeAndWait blocks until every
        // task submitted through THIS lease has finished and drops anything
        // submitted afterwards, which is exactly what the old
        // per-generator `m_backgroundExecutor.reset()` did.
        if (m_backgroundLease) {
            m_backgroundLease->closeAndWait();
        }
        // MC stops a level with saveAllChunks(true): every chunk still being
        // generated is written, so the next session carries on where this one
        // stopped. Nothing runs any more (the lease is closed), so every
        // holder's chunk is quiet whatever tasks still claim it.
        if (m_chunkCache && m_libraryStorage) {
            m_chunkCache->getChunkMap().saveAllChunks(
                /*flush=*/true, [this](int64_t key) {
                    const minecraft::world::ChunkPos pos(key);
                    return !IsConversionPinned(Math::ChunkPos{pos.x(), pos.z()});
                },
                /*quiesced=*/true);
            Log::Info("[MyTerrainGenerator] Library chunks saved");
        }
        m_mainThreadExecutor.reset();
        m_chunkCache.reset();
        // After m_chunkCache: its executor closure holds the lease pointer.
        m_backgroundLease.reset();
        // After m_chunkCache: the pipeline's WorldGenContext pointed at this.
        m_structureState.reset();

        delete m_generator;   m_generator = nullptr;
        m_biomeSource.reset();
        delete m_randomState;  m_randomState = nullptr;
        m_settings.reset();
        delete m_blockRegistry; m_blockRegistry = nullptr;

        m_initialized = false;
        Log::Info("[MyTerrainGenerator] Shutdown complete");
    }

    glm::ivec3 MyTerrainGenerator::FindSpawnPosition() {
        // Overworld-only algorithm, and NOTHING SHOULD CALL IT on the other
        // dimensions: MC's setInitialSpawn runs against the overworld LevelStem
        // alone, and arrival elsewhere is by portal (nether) or onto the fixed
        // obsidian platform (end), neither of which asks the generator. Both
        // steps below would be wrong there anyway — the climate search needs a
        // spawn target these dimensions do not carry (Initialize gives them an
        // empty list, so it would just return the origin), and "worldgen
        // surface above sea level" means nothing in a nether whose sea is lava
        // at y=32 or an End whose sea level is 0.
        //
        // Answered before the lazy Initialize below: there is no reason to
        // stand a whole pipeline up to return a constant.
        if (m_config.dimension == "nether") {
            Log::Warning("[MyTerrainGenerator] FindSpawnPosition called on the nether —"
                         " world spawn is an overworld concept; returning a placeholder");
            // No MC counterpart to copy (the nether is entered by portal);
            // mid-height above the lava sea is the least harmful constant.
            return glm::ivec3(0, 64, 0);
        }
        if (m_config.dimension == "end") {
            Log::Warning("[MyTerrainGenerator] FindSpawnPosition called on the end —"
                         " world spawn is an overworld concept; returning the"
                         " obsidian-platform point");
            // ServerLevel.java:188 — END_SPAWN_POINT = BlockPos(100, 50, 0).
            return glm::ivec3(100, 50, 0);
        }
        if (m_config.dimension == "hush") {
            Log::Warning("[MyTerrainGenerator] FindSpawnPosition called on the hush —"
                         " world spawn is an overworld concept; returning a placeholder");
            // Entered by portal only (the ancient-city frame); like the
            // nether there is no MC counterpart. A point above the Hush sea
            // level (50) is the least harmful constant — the portal code
            // resolves the real surface height on arrival.
            return glm::ivec3(0, 80, 0);
        }
        if (m_config.dimension == "aether") {
            Log::Warning("[MyTerrainGenerator] FindSpawnPosition called on the aether —"
                         " world spawn is an overworld concept; returning a placeholder");
            // Entered by portal only (AetherPortalForcer finds or builds the
            // far portal); skylands.json has an empty spawn_target. Mid-island
            // height (islands sit between y 8 and 128) is the least harmful
            // constant.
            return glm::ivec3(0, 80, 0);
        }

        if (m_config.dimension == "twilight_forest") {
            Log::Warning("[MyTerrainGenerator] FindSpawnPosition called on the twilight"
                         " forest — world spawn is an overworld concept; returning a"
                         " placeholder");
            // Entered by the pool portal only (TFTeleporter finds or builds
            // the far portal); twilight_noise_gen.json has an empty
            // spawn_target. Just above the forest floor (sea level 0, most
            // terrain between y 0 and 30) is the least harmful constant.
            return glm::ivec3(0, 32, 0);
        }

        // Called once per world on the server thread; the generator may not
        // have lazily initialized yet.
        if (!m_initialized && !Initialize()) {
            Log::Warning("[MyTerrainGenerator] FindSpawnPosition: init failed, using legacy spawn");
            return glm::ivec3(0, 67, 0);
        }

        // ── Step 1: the generator's origin (MC ServerLevel.setInitialSpawn ->
        // generator.getOrigin(randomState)). The noise generator searches the
        // settings' spawn_target (NoiseSpawnFinder: a radial climate-fit
        // search biased toward the world origin); a flat world has none and
        // starts at chunk (0, 0).
        auto* noiseGenerator = dynamic_cast<minecraft::levelgen::NoiseBasedChunkGenerator*>(m_generator);
        const minecraft::world::ChunkPos originChunk = noiseGenerator != nullptr
            ? noiseGenerator->getOrigin(m_randomState)
            : minecraft::world::ChunkPos(0, 0);
        const int spawnChunkX = originChunk.x();
        const int spawnChunkZ = originChunk.z();

        const int32_t seaLevel = m_generator->getSeaLevel();
        auto surfaceAt = [&](int blockX, int blockZ) {
            return m_generator->getBaseHeight(blockX, blockZ,
                minecraft::levelgen::Heightmap::Types::WORLD_SURFACE_WG,
                m_randomState);
        };

        // ── Step 2: chunk spiral (MC setInitialSpawn) ──────────────────────
        // MC walks an 11×11 chunk spiral around the climate chunk and takes
        // the first chunk with a valid spawn block. Full block validation
        // (PlayerSpawnFinder) needs generated chunk data, which doesn't exist
        // yet at world init — the dry-land test (worldgen surface above sea
        // level at the chunk centre) stands in for it, which is also what
        // rules out ocean columns in practice.
        const glm::ivec3 fallback(spawnChunkX * 16 + 8,
                                  std::max(surfaceAt(spawnChunkX * 16 + 8, spawnChunkZ * 16 + 8),
                                           seaLevel + 1),
                                  spawnChunkZ * 16 + 8);

        int xOff = 0, zOff = 0;
        int dx = 0, dz = -1;
        for (int i = 0; i < 11 * 11; ++i) {
            if (xOff >= -5 && xOff <= 5 && zOff >= -5 && zOff <= 5) {
                const int blockX = (spawnChunkX + xOff) * 16 + 8;
                const int blockZ = (spawnChunkZ + zOff) * 16 + 8;
                const int32_t surfaceY = surfaceAt(blockX, blockZ);
                if (surfaceY > seaLevel) {
                    Log::Info("[MyTerrainGenerator] Spawn selected at (%d, %d, %d) "
                              "(origin chunk %d,%d; %d chunk probes)",
                              blockX, surfaceY, blockZ, spawnChunkX, spawnChunkZ, i + 1);
                    return glm::ivec3(blockX, surfaceY, blockZ);
                }
            }
            // Square-spiral turn rule (matches MC's iteration order).
            if (xOff == zOff || (xOff < 0 && xOff == -zOff) ||
                (xOff > 0 && xOff == 1 - zOff)) {
                const int t = dx;
                dx = -dz;
                dz = t;
            }
            xOff += dx;
            zOff += dz;
        }

        Log::Info("[MyTerrainGenerator] Spawn fallback at (%d, %d, %d) — no dry land "
                  "within 5 chunks of climate pos", fallback.x, fallback.y, fallback.z);
        return fallback;
    }

    ChunkGenerationResult MyTerrainGenerator::GenerateChunk(Math::ChunkPos position) {
        ChunkGenerationResult result;
        result.success = false;

        if (!m_initialized) {
            result.errorMessage = "Generator not initialized";
            return result;
        }

        auto startTime = std::chrono::high_resolution_clock::now();

        try {
            // ================================================================
            // Generate chunk through the FULL ServerChunkCache pipeline
            //
            // ServerChunkCache.getChunk() goes through:
            //   1. Cache check
            //   2. getChunkFutureMainThread() -> adds ticket
            //   3. runDistanceManagerUpdates()
            //   4. ChunkHolder.scheduleChunkGenerationTask()
            //   5. ChunkMap.scheduleGenerationTask()
            //   6. ChunkTaskDispatcher.submit() -> ConsecutiveExecutor
            //   7. ChunkGenerationTask runs through all statuses
            //      (BIOMES -> TERRAIN -> FEATURES -> ...)
            //   8. managedBlock() pumps tasks until complete
            //
            // This provides multi-chunk neighbor access via WorldGenRegion,
            // so features like trees can span chunk boundaries correctly.
            // ================================================================
            world::IChunk* chunk = nullptr;
            // The holder owns `chunk`, and processUnloads may destroy it once
            // its ticket goes; pinned until the conversion below has read it
            // (the same guard the asynchronous path's results carry).
            PinConversion(position);
            struct Unpin {
                MyTerrainGenerator* self; Math::ChunkPos pos;
                ~Unpin() { self->UnpinConversion(pos); }
            } unpin{this, position};
            {
                // Time the MC generation pipeline separately from our
                // conversion loop below — the next Tracy capture shows how
                // the per-chunk cost splits between the two.
                PROFILE_ZONE_N("TerrainLibGetChunk");
                chunk = m_chunkCache->getChunk(
                    position.x, position.z, *m_targetStatus, true
                );
            }

            if (!chunk) {
                result.errorMessage = "ServerChunkCache returned null chunk";
                return result;
            }

            // ================================================================
            // Convert from terrain library chunk to game chunk format
            // (section-wise, all-air sections skipped, lock-free block map)
            // ================================================================
            int blocksSet = 0;
            auto gameChunk = ConvertLibChunk(chunk, position, &blocksSet);
            if (!gameChunk) {
                result.errorMessage = "Library chunk could not be converted";
                return result;
            }

            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

            result.success = true;
            result.chunk = gameChunk;

            m_stats.chunksGenerated++;
            m_stats.totalGenerationTimeMs += duration.count();

            Log::Debug("[MyTerrainGenerator] Chunk (%d, %d) generated in %lldms (%d non-air blocks)",
                      position.x, position.z, duration.count(), blocksSet);

        } catch (const std::exception& e) {
            result.errorMessage = std::string("Exception: ") + e.what();
            Log::Error("[MyTerrainGenerator] Generation failed for chunk (%d, %d): %s",
                      position.x, position.z, e.what());
        }

        return result;
    }

    MyTerrainGenerator::MappedBlock
    MyTerrainGenerator::MapBlockType(minecraft::world::BlockState* blockState) const {
        if (!blockState) return { BlockID::Stone, 0 };

        // Keyed on the BlockState*, not the Block*. Library states are interned
        // per (block, property tuple) by StateDefinition and, like Block*, are
        // created once per bootstrap epoch and never moved — so pointer
        // equality still suffices, but now `leaf_litter{facing=west,
        // segment_amount=3}` and `leaf_litter{facing=north,segment_amount=1}`
        // no longer collide. Keying on the Block* is what made every generated
        // furnace, log and leaf litter clump come out in its default state.
        //
        // Lock-free per-thread cache + last-state memo. Terrain is dominated by
        // long runs of the identical state (air, stone, deepslate, water), so
        // the memo alone absorbs the vast majority of calls; the map handles the
        // rest. No mutex — the old shared cache took ~98k lock/unlock per
        // converted chunk with every worker contending on it. The map is now
        // bounded by distinct states rather than distinct blocks, which is a
        // few thousand for a real world instead of ~1150.
        struct ThreadCache {
            uint32_t epoch = 0;
            const void* lastState = nullptr;
            MappedBlock lastMapped{ BlockID::Stone, 0 };
            std::unordered_map<const void*, MappedBlock> map;
        };
        thread_local ThreadCache tc;

        const uint32_t epoch = s_blockMapEpoch.load(std::memory_order_acquire);
        if (tc.epoch != epoch) {
            tc.map.clear();
            tc.lastState = nullptr;
            tc.epoch = epoch;
        }

        if (blockState == tc.lastState) {
            return tc.lastMapped;
        }

        auto it = tc.map.find(blockState);
        if (it == tc.map.end()) {
            // First encounter on this thread — resolve via string lookup
            // (slow path, one hit per distinct state per worker thread).
            Game::BlockStateRegistry::Initialize();
            Game::NbtBlockState gameState = Game::BlockStateRegistry::CreateBlockState(
                blockState->getIdentifier(), blockState->getProperties());
            it = tc.map.emplace(blockState,
                                MappedBlock{ gameState.resolvedId, gameState.resolvedState }).first;
        }

        tc.lastState = blockState;
        tc.lastMapped = it->second;
        return it->second;
    }

    uint16_t MyTerrainGenerator::MapBiome(const void* libBiome, const std::string& name) const {
        if (!libBiome) return Game::BiomeRegistry::Fallback();

        struct ThreadCache {
            uint32_t epoch = 0;
            const void* last = nullptr;
            uint16_t lastId = 0;
            std::unordered_map<const void*, uint16_t> map;
        };
        thread_local ThreadCache tc;

        const uint32_t epoch = s_blockMapEpoch.load(std::memory_order_acquire);
        if (tc.epoch != epoch) {
            tc.map.clear();
            tc.last = nullptr;
            tc.epoch = epoch;
        }
        if (libBiome == tc.last) return tc.lastId;

        auto it = tc.map.find(libBiome);
        if (it == tc.map.end()) {
            it = tc.map.emplace(libBiome, Game::BiomeRegistry::FromName(name)).first;
        }
        tc.last = libBiome;
        tc.lastId = it->second;
        return it->second;
    }

    // Try the palette-to-palette path. Returns false when the shapes cannot be
    // lined up, leaving `outSection` untouched for the caller's fallback.
    bool MyTerrainGenerator::TryConvertSectionByPalette(
            const minecraft::world::LevelChunkSection& libSection,
            ChunkSection& outSection, int& outNonAir) const {

        const auto& libStates = libSection.getStates();
        const std::vector<minecraft::world::BlockState*> libPalette =
            libStates.getPaletteEntries();

        // Empty means the library fell back to a GLOBAL palette, where the
        // value is its own index in the library's id space — which is not ours.
        if (libPalette.empty()) return false;

        // Map each DISTINCT state once. Two library states can collapse onto
        // one game state (a property this port does not model); the resulting
        // duplicate palette entries are harmless — both indices resolve to the
        // same value.
        std::vector<uint32_t> values;
        values.reserve(libPalette.size());
        for (auto* st : libPalette) {
            const MappedBlock mapped = MapBlockType(st);
            values.push_back(Game::BlockStates::FromIndex(mapped.id, mapped.state).RawId());
        }

        // Unpack the library's indices: pure shift-and-mask over the packed
        // words. No palette lookups, no map lookups, no virtual calls.
        std::vector<uint32_t> indices(ChunkSection::TOTAL, 0);
        const int libBits = libStates.getBitsPerEntry();
        if (libBits > 0) {
            const std::vector<int64_t> raw = libStates.getRawData();
            const int perLong = 64 / libBits;
            const size_t needed =
                static_cast<size_t>((ChunkSection::TOTAL + perLong - 1) / perLong);
            if (raw.size() < needed) return false;

            const uint64_t mask = (1ULL << libBits) - 1ULL;
            for (int i = 0; i < ChunkSection::TOTAL; ++i) {
                const int cell = i / perLong;
                const int bit  = (i - cell * perLong) * libBits;
                indices[i] = static_cast<uint32_t>(
                    (static_cast<uint64_t>(raw[cell]) >> bit) & mask);
            }
        }
        // libBits == 0 is a single-value palette: every index is 0, which
        // `indices` already is.

        Game::PalettedContainer built(
            Game::PaletteStrategy::ForBlockStates(Game::kBlockStateBits),
            Game::BlockState{}.RawId());
        if (!built.BuildFrom(values, indices)) return false;

        int nonAir = 0;
        for (int i = 0; i < ChunkSection::TOTAL; ++i) {
            if (Game::BlockState::FromRawId(values[indices[i]]).Block() != BlockID::Air) ++nonAir;
        }

        outSection.AdoptStates(std::move(built));
        outNonAir = nonAir;
        return true;
    }

    // Per-voxel conversion. The original path, kept as the fallback AND as the
    // reference the palette path is checked against — see kVerifyPaletteConvert.
    int MyTerrainGenerator::ConvertSectionPerVoxel(
            const minecraft::world::LevelChunkSection& libSection,
            ChunkSection& outSection) const {
        int nonAir = 0;
        for (int ly = 0; ly < 16; ++ly) {
            for (int lz = 0; lz < 16; ++lz) {
                for (int lx = 0; lx < 16; ++lx) {
                    const MappedBlock mapped =
                        MapBlockType(libSection.getBlockState(lx, ly, lz));
                    if (mapped.id != BlockID::Air) {
                        outSection.SetBlockState(lx, ly, lz, mapped.id, mapped.state);
                        ++nonAir;
                    }
                }
            }
        }
        return nonAir;
    }

    // Translate ONE library section into a game section.
    //
    // The library stores sections in its own PalettedContainer — a port of the
    // same MC class ours now is — so the two agree on everything except which
    // ids the palette entries carry. That makes the conversion a mapping of the
    // PALETTE (a handful of entries) plus an unpack of the indices, instead of
    // 4096 lookups through MapBlockType and 4096 paletted writes.
    //
    // MC needs none of this: its generator writes into the container the world
    // keeps. This is as close to that as a vendored generator allows.
    int MyTerrainGenerator::ConvertSection(const minecraft::world::LevelChunkSection& libSection,
                                           ChunkSection& outSection) const {
        PROFILE_ZONE_N("ConvertSection");

        int nonAir = 0;
        if (TryConvertSectionByPalette(libSection, outSection, nonAir)) {
            // Cross-check against the path this replaced. terrain_parity cannot
            // cover this — it links the library alone, with no game code — so
            // the palette remap is verified against the per-voxel original
            // instead, over every section of every chunk actually generated.
            //
            // Compiled out entirely by default; flip to true, run a session,
            // and any disagreement is logged with its coordinates.
            if constexpr (kVerifyPaletteConvert) {
                ChunkSection reference;
                const int refNonAir = ConvertSectionPerVoxel(libSection, reference);
                if (refNonAir != nonAir) {
                    Log::Error("[ConvertSection] non-air count differs: palette=%d per-voxel=%d",
                               nonAir, refNonAir);
                }
                for (int ly = 0; ly < 16; ++ly) {
                    for (int lz = 0; lz < 16; ++lz) {
                        for (int lx = 0; lx < 16; ++lx) {
                            if (outSection.Get(lx, ly, lz) != reference.Get(lx, ly, lz) ||
                                outSection.GetState(lx, ly, lz) != reference.GetState(lx, ly, lz)) {
                                Log::Error("[ConvertSection] MISMATCH at (%d,%d,%d): "
                                           "palette=%u/%u per-voxel=%u/%u",
                                           lx, ly, lz,
                                           outSection.Get(lx, ly, lz), outSection.GetState(lx, ly, lz),
                                           reference.Get(lx, ly, lz), reference.GetState(lx, ly, lz));
                                return nonAir;   // one report per section is enough
                            }
                        }
                    }
                }
            }
            return nonAir;
        }

        return ConvertSectionPerVoxel(libSection, outSection);
    }

    std::shared_ptr<Chunk> MyTerrainGenerator::ConvertLibChunk(minecraft::world::IChunk* chunk,
                                                               Math::ChunkPos position,
                                                               int* outBlocksSet) const {
        PROFILE_ZONE_N("ConvertChunk");
        // A FULL chunk the game already took, whose blocks the library has
        // released (ChunkMap::releaseHandedOffChunks). The game loads such a
        // chunk from its own save, never from here; reaching this means that
        // save is missing, and converting would hand over an empty column.
        if (const auto* proto = dynamic_cast<const minecraft::world::ProtoChunk*>(chunk);
            proto && proto->isBlockDataReleased()) {
            Log::Error("[MyTerrainGenerator] chunk (%d, %d) was requested again after the game "
                       "took it and its library copy was released — the game's own save of it "
                       "is missing", position.x, position.z);
            return nullptr;
        }
        auto gameChunk = std::make_shared<Chunk>();
        gameChunk->pos = position;

        // Section-wise, and PALETTE-WISE — see ConvertSection. All-air sections
        // are skipped entirely (most of a 384-block column is sky).
        //
        // MC has no conversion step at all: its generator writes into the very
        // container the world keeps, so there is nothing to translate. Ours
        // exists only because generation lives in a vendored library with its
        // own palette. Translating palette-to-palette rather than voxel-by-voxel
        // is as close to MC's absence of a conversion as this shape allows.
        const int libMinY = chunk->getMinBuildHeight();
        const int sectionsCount = chunk->getSectionsCount();
        int blocksSet = 0;

        for (int si = 0; si < sectionsCount; ++si) {
            auto& libSection = chunk->getSection(si);
            if (libSection.hasOnlyAir()) continue;

            const int baseY = libMinY + si * 16;
            const int gameSectionIndex = Math::WorldCoordinates::WorldYToSectionIndex(baseY);
            if (gameSectionIndex < 0 || gameSectionIndex >= Math::SECTIONS_PER_CHUNK) continue;

            // Sections are always allocated now (MC replaceMissingSections),
            // so this is a plain fetch. It used to EnsureSection first — and
            // that call, plus the one in SetBiomeQuart below, is why a
            // generated chunk already carried all 24 sections before this
            // change.
            blocksSet += ConvertSection(libSection,
                                        *gameChunk->GetSection(gameSectionIndex));
        }

        // ── Biomes ──────────────────────────────────────────────────────────
        // One entry per 4x4x4 cell, matching MC's noise-biome resolution.
        // IChunk exposes getBiome(BlockPos), which is ChunkAccess's own
        // block -> quart conversion (QuartPos::fromBlock, i.e. >> 2) followed by
        // getNoiseBiome — so feeding it the BLOCK coordinate of each cell's
        // corner samples exactly the cell we want to store.
        {
            const int baseX = position.x * Math::CHUNK_SIZE_X;
            const int baseZ = position.z * Math::CHUNK_SIZE_Z;

            for (int qy = 0; qy < Chunk::BIOME_VERTICAL; ++qy) {
                const int blockY = Math::WorldCoordinates::MIN_WORLD_Y + qy * 4;
                for (int qz = 0; qz < Chunk::BIOME_HORIZONTAL; ++qz) {
                    for (int qx = 0; qx < Chunk::BIOME_HORIZONTAL; ++qx) {
                        const auto* biome = chunk->getBiome(minecraft::core::BlockPos(
                            baseX + qx * 4, blockY, baseZ + qz * 4));
                        gameChunk->SetBiomeQuart(
                            qx, qy, qz,
                            MapBiome(biome, biome ? biome->getName() : std::string{}));
                    }
                }
            }
        }

        // ── Heightmaps ──────────────────────────────────────────────────────
        //
        // COPIED from the library rather than recomputed. The library primes
        // MOTION_BLOCKING_NO_LEAVES and WORLD_SURFACE at its generateFeatures
        // stage exactly as MC does, so the values are already there and already
        // correct; a fresh 256-column scan here would cost real time on the
        // chunk pipeline — which CLAUDE.md is explicit is the most expensive
        // thing in the program — to arrive at the same answer.
        //
        // One consequence worth knowing: these heights were computed against
        // the LIBRARY's block predicates, and every later incremental update
        // uses the GAME's (Heightmap.cpp's table). Those can disagree for a
        // block whose type mapping is approximate. The drift is bounded and
        // self-correcting — a column only re-evaluates when something writes to
        // it, and from then on it is consistently the game's predicate — and
        // the library's answer is the more MC-faithful of the two to start from.
        {
            using LibTypes = minecraft::levelgen::Heightmap::Types;

            struct Mapping { HeightmapType game; LibTypes lib; };
            // NOTE the library's enum order differs from MC's, so these are
            // named rather than cast from an index.
            const Mapping mappings[] = {
                { HeightmapType::MotionBlockingNoLeaves, LibTypes::MOTION_BLOCKING_NO_LEAVES },
                { HeightmapType::WorldSurface,           LibTypes::WORLD_SURFACE },
                // Not consumed by the engine, but vanilla reads them out of a
                // saved chunk and primes only the ones that are ABSENT — so a
                // key we write zero-filled would be believed. The library
                // computes all four, so copying them is free.
                { HeightmapType::OceanFloor,             LibTypes::OCEAN_FLOOR },
                { HeightmapType::MotionBlocking,         LibTypes::MOTION_BLOCKING },
            };

            for (const Mapping& m : mappings) {
                Heightmap& out = gameChunk->GetHeightmap(m.game);
                for (int lx = 0; lx < Math::CHUNK_SIZE_X; ++lx) {
                    for (int lz = 0; lz < Math::CHUNK_SIZE_Z; ++lz) {
                        // getHeight is MC's getHighestTaken (the topmost
                        // matching block); the heightmap stores first-available,
                        // which is one higher.
                        const int height =
                            chunk->getHeight(static_cast<int>(m.lib), lx, lz);
                        out.SetHeight(lx, lz, height + 1);
                    }
                }
            }

            gameChunk->MarkHeightmapsPrimed();
        }

        // ── Block entities ─────────────────────────────────────────────────
        // Template chests and the like (see AttachGeneratedBlockEntities).
        AttachGeneratedBlockEntities(*gameChunk, *chunk, position);

        // ── Worldgen entities ──────────────────────────────────────────────
        // MC ProtoChunk.entities: template mobs (villagers, golems, cats,
        // bastion piglins, the igloo's pair) and piece-spawned ones (swamp hut
        // witch, monument elders, mansion illagers, end city shulkers). Carried
        // as binary NBT so every type survives exactly; the server adds them
        // to the level when it first claims this chunk's entities.
        AttachGeneratedEntities(*gameChunk, *chunk, position);

        // ── Structure spawn areas ──────────────────────────────────────────
        // What spawn_overrides and the fortress rule test at spawn time.
        if (const auto* areas = chunk->getStructureSpawnAreas()) {
            gameChunk->structureSpawnAreas.reserve(areas->size());
            for (const auto& area : *areas) {
                StructureSpawnArea out;
                out.structure = area.structure;
                out.startMin = glm::ivec3(area.startBox.minX, area.startBox.minY, area.startBox.minZ);
                out.startMax = glm::ivec3(area.startBox.maxX, area.startBox.maxY, area.startBox.maxZ);
                out.pieces.reserve(area.pieces.size());
                for (const auto& piece : area.pieces) {
                    out.pieces.push_back({glm::ivec3(piece.box.minX, piece.box.minY, piece.box.minZ),
                                          glm::ivec3(piece.box.maxX, piece.box.maxY, piece.box.maxZ),
                                          piece.templateId,
                                          static_cast<uint8_t>(piece.rotation & 3),
                                          piece.pieceType});
                }
                gameChunk->structureSpawnAreas.push_back(std::move(out));
            }
        }

        // ── Structure starts and references ────────────────────────────────
        // MC saves them with the full chunk; the library encoded them when
        // the chunk reached FULL (ChunkStatusTasks::full). Carried through so
        // a structure begun here finishes where it started after a reload.
        if (const auto* proto = dynamic_cast<const minecraft::world::ProtoChunk*>(chunk)) {
            gameChunk->structuresNbt = proto->getStructuresAtFull();

            // ── Post-processing ────────────────────────────────────────────
            // The cells worldgen marked (ProtoChunk.markPosForPostprocessing),
            // re-indexed from the library's sections (its level's min Y) to
            // the game's; applied when the chunk starts ticking.
            const auto& marked = proto->getPostProcessing();
            for (size_t si = 0; si < marked.size(); ++si) {
                if (marked[si].empty()) continue;
                const int baseY = libMinY + static_cast<int>(si) * 16;
                const int gameSectionIndex = Math::WorldCoordinates::WorldYToSectionIndex(baseY);
                if (gameSectionIndex < 0 || gameSectionIndex >= Chunk::SECTION_COUNT) continue;
                if (gameChunk->postProcessing.empty()) {
                    gameChunk->postProcessing.resize(Chunk::SECTION_COUNT);
                }
                auto& out = gameChunk->postProcessing[static_cast<size_t>(gameSectionIndex)];
                out.insert(out.end(), marked[si].begin(), marked[si].end());
            }
        }

        if (outBlocksSet) *outBlocksSet = blocksSet;
        return gameChunk;
    }

    // === Non-blocking async API ===

    bool MyTerrainGenerator::RequestChunkGeneration(Math::ChunkPos position) {
        if (!m_initialized || !m_chunkCache) return false;

        // MC's player loading ticket, per chunk: DistanceManager.
        // PlayerTicketTracker gives every chunk in a player's view a
        // PLAYER_LOADING ticket at PLAYER_TICKET_LEVEL — 31, ENTITY_TICKING —
        // so the 5x5 around it reaches FULL (levels 31-33) and is generated
        // together, the 25 chunks sharing one dependency pyramid. A ticket at
        // 33 made only the chunk itself FULL: every request built its own
        // pyramid (~529 structure-start and 25 terrain chunks for one FULL
        // chunk), and 50 players flying apart generated ~14 chunks a second
        // (stress test 2026-09-24).
        //
        // GENERATION_REQUEST rather than PLAYER_LOADING: the request lives
        // until the chunk leaves every view (IntegratedServer::
        // CancelLoadIfUnwanted), and ReleaseGenerationRequest removes it; with
        // it gone the holders' levels rise past MAX and processUnloads frees
        // them.
        //
        // ONLY the ticket, as MC's player tickets: the chunks they create are
        // picked up after the next runAllUpdates. This used to go through
        // getChunkFutureMainThread with loadOrGenerate, which runs a whole
        // distance-manager pass whenever the holder does not exist yet — one
        // pass per fresh chunk instead of one per batch. AttachPendingRequests
        // hooks the request onto its holder after the next pass.
        if (m_requestTickets.insert(position).second) {
            using minecraft::server::level::Ticket;
            using minecraft::server::level::TicketType;
            m_chunkCache->addTicket(Ticket(TicketType::GENERATION_REQUEST, RequestTicketLevel()),
                                    minecraft::world::ChunkPos(position.x, position.z));
        }
        // Once per position until it attaches: a request released and issued
        // again meanwhile (the stall watchdog) still produces one future.
        if (m_awaitingAttachSet.insert(position).second) m_awaitingAttach.push_back(position);
        return true;
    }

    int MyTerrainGenerator::RequestTicketLevel() {
        return minecraft::server::level::DistanceManager::getPlayerTicketLevel();
    }

    bool MyTerrainGenerator::AttachPendingRequests() {
        if (m_awaitingAttach.empty() || !m_chunkCache) return false;
        std::vector<Math::ChunkPos> batch;
        batch.swap(m_awaitingAttach);
        m_awaitingAttachSet.clear();

        std::shared_ptr<CompletionSink> sink = m_sink;
        // A request that cannot attach: failed completion and its release.
        const auto fail = [&sink](Math::ChunkPos position) {
            {
                std::lock_guard<std::mutex> lock(sink->mutex);
                if (sink->closed) return;
                sink->completions.push_back(Completion{position, nullptr});
                sink->releases.push_back(position);
            }
            if (sink->wake) sink->wake();
        };
        const auto release = [sink](Math::ChunkPos position) {
            {
                std::lock_guard<std::mutex> lock(sink->mutex);
                if (sink->closed) return;
                sink->releases.push_back(position);
            }
            if (sink->wake) sink->wake();
        };
        auto& chunkMap = m_chunkCache->getChunkMap();

        for (const Math::ChunkPos position : batch) {
            // Released before it attached (cancelled, or given up on): its
            // ticket is gone, so a future now would find the holder dropping.
            // The caller still counts it in flight — answer it as failed,
            // exactly as a request cancelled after attaching would be.
            if (m_requestTickets.count(position) == 0) {
                fail(position);
                continue;
            }
            // MC getChunkFutureMainThread(x, z, status, false): the ticket has
            // been propagated, so the holder exists at a level that allows
            // FULL; attach to its generation. A holder that is somehow absent
            // yields UNLOADED_CHUNK_FUTURE — a failed completion, retried by
            // the caller.
            auto future = m_chunkCache->getChunkFuture(position.x, position.z, *m_targetStatus,
                                                       /*loadOrGenerate=*/false);
            if (!future) {
                fail(position);
                continue;
            }
            // Pinned from attaching until the game has converted (or dropped)
            // the result, so processUnloads cannot destroy the holder while a
            // raw chunk pointer is on its way to us.
            PinConversion(position);
            future->thenAccept([sink, position](
                    const minecraft::server::level::ServerChunkCache::ChunkResultType& result) {
                minecraft::world::IChunk* chunk = result ? result->orElse(nullptr) : nullptr;
                {
                    std::lock_guard<std::mutex> lock(sink->mutex);
                    if (sink->closed) return;
                    sink->completions.push_back(Completion{position, chunk});
                }
                if (sink->wake) sink->wake();
            });
            // MC DistanceManager.runAllUpdates' ticketsToRelease: the
            // throttle slot is held until the chunk is ENTITY_TICKING — its
            // whole 5x5 FULL — and let go early if that future fails (the
            // ticket went: the chunk left every view). A holder whose level
            // never reached ENTITY_TICKING has the completed UNLOADED future.
            minecraft::server::level::ChunkHolder* holder =
                chunkMap.getVisibleChunkIfPresent(minecraft::world::ChunkPos::asLong(position.x, position.z));
            if (!holder) {
                release(position);
                continue;
            }
            auto entityTicking = holder->getEntityTickingChunkFuture();
            if (!entityTicking) {
                release(position);
                continue;
            }
            entityTicking->thenAccept([release, position](
                    const minecraft::server::level::ChunkHolder::LevelChunkResult&) {
                release(position);
            });
        }
        return true;
    }

    void MyTerrainGenerator::EnqueueGenerationRequest(Math::ChunkPos position) {
        {
            std::lock_guard<std::mutex> lock(m_requestMutex);
            m_requests.push_back(position);
        }
        // The server thread parks in WaitForPipelineWork between ticks;
        // give it a reason to service the queue now rather than next tick.
        if (m_mainThreadExecutor) m_mainThreadExecutor->wakeAll();
    }

    void MyTerrainGenerator::TakeRequests(std::vector<Math::ChunkPos>& out) {
        // No distance-manager pass here: nothing attaches to a holder until
        // AttachPendingRequests, which runs after one.
        std::lock_guard<std::mutex> lock(m_requestMutex);
        if (m_requests.empty()) return;
        out.insert(out.end(), m_requests.begin(), m_requests.end());
        m_requests.clear();
    }

    bool MyTerrainGenerator::HasQueuedWork() {
        {
            std::lock_guard<std::mutex> lock(m_requestMutex);
            if (!m_requests.empty()) return true;
        }
        std::lock_guard<std::mutex> lock(m_sink->mutex);
        return !m_sink->completions.empty() || !m_sink->releases.empty() || !m_sink->ready.empty();
    }

    void MyTerrainGenerator::TakeReady(std::vector<Completion>& out) {
        std::lock_guard<std::mutex> lock(m_sink->mutex);
        out.insert(out.end(), m_sink->ready.begin(), m_sink->ready.end());
        m_sink->ready.clear();
    }

    void MyTerrainGenerator::TakeCompletions(std::vector<Completion>& out) {
        std::lock_guard<std::mutex> lock(m_sink->mutex);
        out.insert(out.end(), m_sink->completions.begin(), m_sink->completions.end());
        m_sink->completions.clear();
    }

    void MyTerrainGenerator::TakeReleases(std::vector<Math::ChunkPos>& out) {
        std::lock_guard<std::mutex> lock(m_sink->mutex);
        out.insert(out.end(), m_sink->releases.begin(), m_sink->releases.end());
        m_sink->releases.clear();
    }

    std::shared_ptr<Chunk> MyTerrainGenerator::ConvertCompletedChunk(minecraft::world::IChunk* chunk,
                                                                     Math::ChunkPos position) {
        if (!chunk) return nullptr;
        PROFILE_ZONE_N("ConvertCompleted");
        auto gameChunk = ConvertLibChunk(chunk, position, nullptr);
        m_stats.chunksGenerated++;
        return gameChunk;
    }

    void MyTerrainGenerator::SetViewTicket(uint32_t viewerId, Math::ChunkPos center, int radius) {
        if (!m_initialized || !m_chunkCache) return;
        using minecraft::server::level::TicketType;
        auto it = m_viewTickets.find(viewerId);
        if (it != m_viewTickets.end()) {
            if (it->second.center == center && it->second.radius == radius) return;
            m_chunkCache->removeTicketWithRadius(TicketType::PLAYER_LOADING,
                minecraft::world::ChunkPos(it->second.center.x, it->second.center.z), it->second.radius);
        }
        m_chunkCache->addTicketWithRadius(TicketType::PLAYER_LOADING,
            minecraft::world::ChunkPos(center.x, center.z), radius);
        m_viewTickets[viewerId] = ViewTicket{center, radius};
    }

    void MyTerrainGenerator::ClearViewTicket(uint32_t viewerId) {
        if (!m_initialized || !m_chunkCache) return;
        auto it = m_viewTickets.find(viewerId);
        if (it == m_viewTickets.end()) return;
        m_chunkCache->removeTicketWithRadius(minecraft::server::level::TicketType::PLAYER_LOADING,
            minecraft::world::ChunkPos(it->second.center.x, it->second.center.z), it->second.radius);
        m_viewTickets.erase(it);
    }

    void MyTerrainGenerator::ReleaseGenerationRequest(Math::ChunkPos position) {
        if (!m_chunkCache || m_requestTickets.erase(position) == 0) return;
        using minecraft::server::level::Ticket;
        using minecraft::server::level::TicketType;
        m_chunkCache->removeTicket(
            Ticket(TicketType::GENERATION_REQUEST, RequestTicketLevel()),
            minecraft::world::ChunkPos(position.x, position.z));
    }

    size_t MyTerrainGenerator::SweepRequestTickets(const std::function<bool(Math::ChunkPos)>& stillWanted,
                                                   size_t maxChecks) {
        if (m_requestTickets.empty()) return 0;
        size_t released = 0;
        for (size_t checked = 0; checked < maxChecks; ++checked) {
            if (m_ticketSweepCursor >= m_ticketSweep.size()) {
                // Start the next pass over the tickets held right now.
                m_ticketSweep.assign(m_requestTickets.begin(), m_requestTickets.end());
                m_ticketSweepCursor = 0;
                if (checked > 0) break;   // at most one pass per call
            }
            const Math::ChunkPos pos = m_ticketSweep[m_ticketSweepCursor++];
            if (m_requestTickets.count(pos) == 0) continue;   // released since the snapshot
            if (!stillWanted(pos)) {
                ReleaseGenerationRequest(pos);
                ++released;
            }
        }
        return released;
    }

    void MyTerrainGenerator::PinConversion(Math::ChunkPos position) {
        std::lock_guard<std::mutex> lock(m_pinMutex);
        m_pinned.insert(position);
    }
    void MyTerrainGenerator::UnpinConversion(Math::ChunkPos position) {
        std::lock_guard<std::mutex> lock(m_pinMutex);
        m_pinned.erase(position);
    }
    bool MyTerrainGenerator::IsConversionPinned(Math::ChunkPos position) const {
        std::lock_guard<std::mutex> lock(m_pinMutex);
        return m_pinned.count(position) != 0 || m_readyPins.count(position) != 0;
    }
    void MyTerrainGenerator::PinReady(Math::ChunkPos position) {
        std::lock_guard<std::mutex> lock(m_pinMutex);
        ++m_readyPins[position];
    }
    void MyTerrainGenerator::UnpinReady(Math::ChunkPos position) {
        std::lock_guard<std::mutex> lock(m_pinMutex);
        auto it = m_readyPins.find(position);
        if (it != m_readyPins.end() && --it->second <= 0) m_readyPins.erase(it);
    }

    bool MyTerrainGenerator::PumpOneTask() {
        if (!m_initialized || !m_chunkCache) return false;

        // runDistanceManagerUpdates propagates ticket levels, promotes the
        // visible chunk map and dispatches generation tasks. One pass only —
        // the loop belongs to the caller, which owns the deadline. Every
        // request issued since the last pass attaches right after it.
        const bool updated = m_chunkCache->runDistanceManagerUpdates();
        const bool attached = AttachPendingRequests();
        if (updated || attached) {
            return true;
        }

        // Otherwise one generation callback. MC's pollTask falls through to
        // super.pollTask() here in exactly the same way.
        return m_mainThreadExecutor && m_mainThreadExecutor->runOnePendingTask();
    }

    // TickLibrary below is MC ServerChunkCache.tick: stale-ticket purge, the
    // distance-manager pass, then ChunkMap.processUnloads.
    size_t MyTerrainGenerator::TickLibrary(std::chrono::steady_clock::time_point deadline) {
        if (!m_initialized || !m_chunkCache) return 0;
        PROFILE_ZONE_N("Lib.Tick");
        auto haveTime = [deadline]() { return std::chrono::steady_clock::now() < deadline; };
        // Java runs purgeStaleTickets + runDistanceManagerUpdates every tick;
        // that is what turns a removed view ticket into unload candidates.
        m_chunkCache->tick(haveTime, /*tickChunks=*/false);
        // The pass above propagated this tick's requests (IntegratedServer
        // issues them in the watch-set phase): attach them now, so their
        // generation starts in the time after this tick.
        AttachPendingRequests();

        // Chunks the game took since the last tick become release
        // candidates; a slice of the candidates is checked every tick.
        std::vector<Math::ChunkPos> handedOff;
        {
            std::lock_guard<std::mutex> lock(m_handedOffMutex);
            handedOff.swap(m_handedOff);
        }
        auto& chunkMap = m_chunkCache->getChunkMap();
        for (const Math::ChunkPos& pos : handedOff) {
            chunkMap.markHandedOff(minecraft::world::ChunkPos::asLong(pos.x, pos.z));
        }
        // A few hundred a tick: a candidate waits only for its neighbours to
        // finish SPAWN, and the ring of view-edge chunks that wait longest is
        // a few thousand at most.
        constexpr size_t kReleaseChecksPerTick = 256;
        chunkMap.releaseHandedOffChunks(kReleaseChecksPerTick, [this](int64_t key) {
            const minecraft::world::ChunkPos pos(key);
            return !IsConversionPinned(Math::ChunkPos(pos.x(), pos.z()));
        });
        return m_chunkCache->lastUnloadCount();
    }

    void MyTerrainGenerator::NoteHandedOff(Math::ChunkPos position) {
        std::lock_guard<std::mutex> lock(m_handedOffMutex);
        m_handedOff.push_back(position);
    }

    MyTerrainGenerator::HolderStatusCounts MyTerrainGenerator::LibraryHolderStatusCounts() const {
        HolderStatusCounts counts;
        if (!m_initialized || !m_chunkCache) return counts;
        using minecraft::world::chunk::status::ChunkStatus;
        const int maxLevel = minecraft::server::level::ChunkLevel::getMaxLevel();
        auto& map = const_cast<minecraft::server::level::ChunkMap&>(m_chunkCache->getChunkMap());
        // Pointers under the map lock, statuses outside it: a status takes the
        // holder's futures mutex, which must not nest inside the map lock (see
        // ChunkMap::processUnloads). Holders are only destroyed on this thread.
        std::vector<minecraft::server::level::ChunkHolder*> holders;
        holders.reserve(map.size());
        map.forEachHolder([&](minecraft::server::level::ChunkHolder& h) { holders.push_back(&h); });
        for (auto* h : holders) {
            if (h->getTicketLevel() <= maxLevel) ++counts.wanted;
            const ChunkStatus* latest = h->getLatestStatus();
            if (latest == nullptr) ++counts.noChunk;
            else if (latest->isBefore(ChunkStatus::TERRAIN)) ++counts.beforeTerrain;
            else if (latest->isBefore(ChunkStatus::FULL)) ++counts.edgeBand;
            else ++counts.full;
        }
        return counts;
    }

    MyTerrainGenerator::UnloadDiag MyTerrainGenerator::GetUnloadDiag() const {
        UnloadDiag d;
        if (!m_initialized || !m_chunkCache) return d;
        const int maxLevel = minecraft::server::level::ChunkLevel::getMaxLevel();
        auto& map = const_cast<minecraft::server::level::ChunkMap&>(m_chunkCache->getChunkMap());
        map.forEachHolder([&](minecraft::server::level::ChunkHolder& h) {
            if (h.getTicketLevel() > maxLevel) ++d.aboveMax;
            if (h.generationRefCount() != 0) ++d.refHeld;
        });
        d.pendingUnload = map.pendingUnloadCount();
        std::vector<Math::ChunkPos> sample;
        {
            std::lock_guard<std::mutex> lock(m_pinMutex);
            d.pinned = m_pinned.size();
            for (const auto& p : m_pinned) { sample.push_back(p); if (sample.size() >= 4) break; }
        }
        for (const auto& p : sample) {
            auto* h = map.getUpdatingChunkIfPresent(minecraft::world::ChunkPos::asLong(p.x, p.z));
            Log::Info("[LibDiag] pinned (%d,%d): %s", p.x, p.z, h ? h->debugState().c_str() : "no holder");
            // Follow the wait chain up to 6 hops.
            for (int hop = 0; h && hop < 6; ++hop) {
                auto w = h->debugWaitingOn();
                if (w.second < 0) break;
                auto* next = map.getUpdatingChunkIfPresent(w.first);
                minecraft::world::ChunkPos wp(w.first);
                Log::Info("[LibDiag]   -> waits on (%d,%d)@%d: %s", wp.x(), wp.z(), w.second,
                          next ? next->debugState().c_str() : "NO HOLDER");
                if (next == h) break;
                h = next;
            }
        }
        {
            // Holder histogram by latest status, and by ticket level band.
            std::map<std::string, int> byStatus; int refHeldNoTask = 0, withTask = 0, taskNeverRan = 0;
            map.forEachHolder([&](minecraft::server::level::ChunkHolder& hh) {
                const auto* st = hh.getLatestStatus();
                byStatus[st ? st->getName() : "none"]++;
                const int runs = hh.debugTaskRuns();
                if (runs >= 0) { ++withTask; if (runs == 0) ++taskNeverRan; }
                else if (hh.generationRefCount() != 0) ++refHeldNoTask;
            });
            std::string hist;
            for (auto& [k, v] : byStatus) hist += k + "=" + std::to_string(v) + " ";
            Log::Info("[LibDiag] pool probe (spin ms / cpu:wall):%s", SharedBackgroundExecutor().DebugProbe().c_str());
            Log::Info("[LibDiag] holders by status: %s | withTask=%d taskNeverRan=%d refHeldNoTask=%d claimRetries=%zu claimWaiters=%zu",
                      hist.c_str(), withTask, taskNeverRan, refHeldNoTask,
                      map.featureClaimRetries(), map.featureClaimWaiters());
        }
        {
            auto st = map.worldgenDispatcherStats();
            Log::Info("[LibDiag] hot tasks:%s", map.debugHotTasks(4).c_str());
            Log::Info("[LibDiag] worldgen dispatcher: submitted=%zu popped=%zu executed=%zu polls=%zu hasWork=%d sleeping=%d stranded=%zu pendingGenTasks=%zu",
                      st.submitted, st.popped, st.executed, st.polls, (int)st.hasWork, (int)st.sleeping, st.stranded,
                      map.pendingGenerationTaskCount());
        }
        return d;
    }

    bool MyTerrainGenerator::IsChunkReady(Math::ChunkPos position) {
        if (!m_initialized || !m_chunkCache) return false;
        // getChunkNow returns non-null only if chunk is at FULL status
        return m_chunkCache->getChunkNow(position.x, position.z) != nullptr;
    }

    std::shared_ptr<Chunk> MyTerrainGenerator::GetCompletedChunk(Math::ChunkPos position) {
        if (!m_initialized || !m_chunkCache) return nullptr;

        auto* chunk = m_chunkCache->getChunkNow(position.x, position.z);
        if (!chunk) return nullptr;

        // Convert from terrain library chunk to game chunk format
        // (section-wise, all-air sections skipped, lock-free block map)
        auto gameChunk = ConvertLibChunk(chunk, position, nullptr);

        m_stats.chunksGenerated++;
        return gameChunk;
    }

    // === Configuration methods ===

    void MyTerrainGenerator::SetConfig(const GenerationConfig& config) {
        // Everything the dimension selects — router, biome source, settings,
        // surface rules, level height — is baked in at Initialize, so a late
        // change cannot take effect. Keep the field describing what this
        // generator ACTUALLY produces: FindSpawnPosition and the logs read it,
        // and letting it drift from the live pipeline is worse than ignoring
        // the write.
        const std::string activeDimension = m_config.dimension;
        m_config = config;
        if (m_initialized && m_config.dimension != activeDimension) {
            Log::Warning("[MyTerrainGenerator] Dimension changed after initialization"
                         " ('%s' -> '%s') - ignored, build a new generator instead",
                         activeDimension.c_str(), m_config.dimension.c_str());
            m_config.dimension = activeDimension;
        }
        if (m_initialized && config.seed != m_config.seed) {
            Log::Warning("[MyTerrainGenerator] Seed changed after initialization - requires restart");
        }
    }

    GenerationConfig MyTerrainGenerator::GetConfig() const { return m_config; }
    void MyTerrainGenerator::SetSeed(int64_t seed) { m_config.seed = seed; }
    int64_t MyTerrainGenerator::GetSeed() const { return m_config.seed; }
    void MyTerrainGenerator::SetWorldType(const std::string&) {}
    // NOT the dimension — IChunkGenerator's "world type" is the MC WorldPreset
    // (default/flat/amplified/...). The literal is historical and no caller
    // reads it (the only other implementation is ProceduralChunkGenerator);
    // m_config.worldType and m_config.dimension are the live values.
    std::string MyTerrainGenerator::GetWorldType() const { return "overworld"; }
    void MyTerrainGenerator::SetPassEnabled(GenerationPass, bool) {}
    bool MyTerrainGenerator::IsPassEnabled(GenerationPass) const { return true; }
    bool MyTerrainGenerator::IsReady() const { return m_initialized; }

    ChunkGenerationResult MyTerrainGenerator::GenerateWithPasses(
        Math::ChunkPos position, const std::vector<GenerationPass>&) {
        return GenerateChunk(position);
    }

    std::future<ChunkGenerationResult> MyTerrainGenerator::GenerateChunkAsync(Math::ChunkPos position) {
        return std::async(std::launch::async, [this, position]() {
            return GenerateChunk(position);
        });
    }

    std::vector<int> MyTerrainGenerator::GenerateHeightMap(Math::ChunkPos) {
        return std::vector<int>(16 * 16, 64);
    }

    int MyTerrainGenerator::SurfaceHeightAt(int blockX, int blockZ) const {
        if (!m_generator || !m_randomState) return INT_MIN;
        return m_generator->getBaseHeight(blockX, blockZ,
            minecraft::levelgen::Heightmap::Types::WORLD_SURFACE_WG, m_randomState);
    }

    std::string MyTerrainGenerator::GenerateBiome(Math::ChunkPos) { return "plains"; }

    IChunkGenerator::GeneratorStats MyTerrainGenerator::GetStats() const { return m_stats; }
    void MyTerrainGenerator::ResetStats() { m_stats = GeneratorStats{}; }
    void MyTerrainGenerator::SetMaxGenerationTime(float) {}
    float MyTerrainGenerator::GetMaxGenerationTime() const { return 0.0f; }
    void MyTerrainGenerator::RegisterTerrainFunction(const std::string&, TerrainFunction) {}
    void MyTerrainGenerator::RegisterFeatureFunction(const std::string&, FeatureFunction) {}
    void MyTerrainGenerator::SetTerrainFunction(const std::string&) {}
    void MyTerrainGenerator::AddFeatureFunction(const std::string&) {}

    IChunkGenerator::DebugInfo MyTerrainGenerator::GetDebugInfo(Math::ChunkPos) {
        DebugInfo info;
        info.biome = "plains";
        info.heightMap = std::vector<int>(16 * 16, 64);
        for (int i = 0; i < 7; ++i) info.generationTimePerPass[i] = 0.0f;
        return info;
    }

    void MyTerrainGenerator::SetDebugMode(bool) {}
    bool MyTerrainGenerator::IsDebugMode() const { return false; }
    std::string MyTerrainGenerator::GetLastError() const { return ""; }
    void MyTerrainGenerator::ClearErrors() {}

} // namespace Game
