// File: src/server/world/storage/anvil/ChunkSerializer.cpp
#include "server/world/storage/anvil/ChunkSerializer.hpp"
#include "server/world/storage/anvil/NbtScan.hpp"
#include "common/world/lighting/ChunkLight.hpp"
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>

#include "server/world/storage/anvil/BlockEntityNbt.hpp"
#include "server/world/storage/anvil/PaletteCodec.hpp"
#include "server/world/storage/NBTParser.hpp"
#include "server/world/storage/SectionDataUnpacker.hpp"

#include "common/core/Log.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/chunk/ChunkSection.hpp"
#include "common/world/fluid/FluidState.hpp"

#include <string>

namespace Game::Anvil {

    namespace {

        // Revision of the light engine that wrote a chunk's light. A chunk
        // stamped with an older one is relit on load rather than trusted.
        //   2 — air no longer dampens sky light (every open column's sky
        //       source used to sit at the top of its highest non-empty
        //       section, darkening everything below it).
        constexpr int32_t kObeyLightVersion = 2;

        // ── Palette entry naming ────────────────────────────────────────────

        // REQUIRED special case. BlockRegistry::Get(BlockID::Air).registrySlug
        // is the EMPTY STRING — air has no BlockDefs.inc row, and the X-macro
        // there is the only writer of registrySlug. A naive
        // "minecraft:" + slug therefore emits the literal "minecraft:" for
        // every air voxel, which is most of a world. Minecraft resolves that
        // to air by luck (an unknown name falls back to air), but MCA
        // Selector, Amulet and chunky all reject it.
        std::string BlockName(BlockID id) {
            if (id == BlockID::Air) return "minecraft:air";
            const std::string_view slug = BlockRegistry::Get(id).registrySlug;
            if (slug.empty()) return "minecraft:air";
            return "minecraft:" + std::string(slug);
        }

        std::string BiomeName(uint32_t biomeId) {
            const auto& info = BiomeRegistry::Get(static_cast<BiomeId>(biomeId));
            return "minecraft:" + std::string(info.name);
        }

        // {Name, Properties?} — NbtUtils.writeBlockState / BlockState.CODEC.
        void WriteBlockStateEntry(Nbt::Writer& w, Nbt::Writer::ListScope& palette,
                                  uint32_t rawStateId) {
            const BlockState state = BlockState::FromRawId(rawStateId);
            const BlockID    id    = state.Block();

            w.ListCompoundBegin(palette);
            w.String("Name", BlockName(id));

            const uint16_t propCount = BlockStates::PropertyCount(id);
            if (propCount > 0) {
                // Properties is OMITTED entirely for a block with none, and
                // every value is a TAG_String — "true"/"false" for booleans,
                // "3" for integers. Never a byte or an int.
                w.BeginCompound("Properties");
                for (uint16_t slot = 0; slot < propCount; ++slot) {
                    const PropertyId prop = BlockStates::PropertyAt(id, slot);
                    // PropertyAt, never a name lookup across blocks: PropertyId
                    // identity is (name, value-set), so `facing` is
                    // HORIZONTAL_FACING on one block and FACING on another.
                    w.String(BlockStates::PropertyName(prop), state.GetName(prop));
                }
                w.EndCompound();
            }
            w.ListCompoundEnd(palette);
        }

        void WriteStateContainer(Nbt::Writer& w, const PalettedContainer& states) {
            const DiskContainer packed = PackForDisk(states);

            w.BeginCompound("block_states");
            {
                auto palette = w.BeginList("palette", Nbt::TagType::Compound);
                for (uint32_t value : packed.palette) WriteBlockStateEntry(w, palette, value);
                w.EndList(palette);
            }
            // bits == 0 means a single-value palette, and vanilla omits `data`
            // entirely. A palette of >= 2 entries with no data is a hard error
            // on load ("Missing values for non-zero storage").
            if (packed.bits > 0) {
                w.LongArray("data", packed.data.data(), packed.data.size());
            }
            w.EndCompound();
        }

        void WriteBiomeContainer(Nbt::Writer& w, const PalettedContainer& biomes) {
            const DiskContainer packed = PackForDisk(biomes);

            w.BeginCompound("biomes");
            {
                // Biome palette entries are plain TAG_String ids, not compounds.
                auto palette = w.BeginList("palette", Nbt::TagType::String);
                for (uint32_t value : packed.palette) w.ListString(palette, BiomeName(value));
                w.EndList(palette);
            }
            if (packed.bits > 0) {
                w.LongArray("data", packed.data.data(), packed.data.size());
            }
            w.EndCompound();
        }

        // ── Read helpers ────────────────────────────────────────────────────

        // Fully qualified: inside namespace Game, a bare `World::` binds to
        // the forward-declared class Game::World (BlockRegistry.hpp:35), not
        // to the global namespace the NBT reader lives in.
        using ::World::NBTTagPtr;
        using ::World::NBTTagCompound;
        using ::World::NBTTagList;
        using ::World::NBTTagString;
        using ::World::NBTTagLongArray;

        std::shared_ptr<NBTTagCompound> AsCompound(const NBTTagPtr& t) {
            return std::dynamic_pointer_cast<NBTTagCompound>(t);
        }

        bool ReadLongArray(const NBTTagCompound& c, const char* key, std::vector<uint64_t>& out) {
            auto tag = std::dynamic_pointer_cast<NBTTagLongArray>(c.GetTag(key));
            if (!tag) { out.clear(); return false; }
            out.resize(tag->value.size());
            for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint64_t>(tag->value[i]);
            return true;
        }

        // A palette entry -> flat engine state id. Routes through the existing
        // BlockStateRegistry so the alias table (grass, cave_air, void_air,
        // snow) and the unimplemented-block tracker keep working, and so
        // property resolution stays NbtUtils.readBlockState-shaped: start from
        // the block's DEFAULT state and apply only the properties supplied,
        // silently skipping names and values this build does not model.
        uint32_t ResolveBlockStateEntry(const NBTTagCompound& entry) {
            // 26.3 renamed the block-state keys (BlockStateFieldNamesFix:
            // Name -> id, Properties -> properties); a world saved by 26.3
            // uses the new ones.
            auto nameTag = std::dynamic_pointer_cast<NBTTagString>(entry.GetTag("id"));
            if (!nameTag) nameTag = std::dynamic_pointer_cast<NBTTagString>(entry.GetTag("Name"));
            if (!nameTag) return BlockState{}.RawId();

            std::unordered_map<std::string, std::string> props;
            auto propsTag = entry.GetTag("properties");
            if (!propsTag) propsTag = entry.GetTag("Properties");
            if (auto p = AsCompound(propsTag)) {
                for (const auto& [k, v] : p->value) {
                    if (auto s = std::dynamic_pointer_cast<NBTTagString>(v)) props[k] = s->value;
                }
            }

            const NbtBlockState resolved =
                BlockStateRegistry::CreateBlockState(nameTag->value, props);
            return BlockStates::FromIndex(resolved.resolvedId, resolved.resolvedState).RawId();
        }

    } // namespace

    // ── Write ───────────────────────────────────────────────────────────────

    bool SerialiseChunk(const Chunk& chunk, int dataVersion, int64_t gameTime,
                        std::vector<uint8_t>& out, std::string& error) {
        // Shared against the writers (Chunk::SetBlock and the block-entity
        // mutators). Without this, a concurrent PalettedContainer::Grow
        // reallocates the palette vector while PackForDisk is walking it —
        // a use-after-free, not a torn value. See the design's R4.
        const auto guard = chunk.LockShared();

        Nbt::Writer w;
        w.BeginRootCompound();

        // Order is SerializableChunkData.write() verbatim.
        w.Int ("DataVersion",   dataVersion);
        w.Int ("xPos",          chunk.pos.x);
        w.Int ("yPos",          kMinSectionY);      // MIN SECTION index, not a block Y
        w.Int ("zPos",          chunk.pos.z);
        w.Long("LastUpdate",    0);
        w.Long("InhabitedTime", chunk.InhabitedTime());
        // The ONE hard requirement: SerializableChunkData.parse returns null on
        // an empty Status, and the chunk then counts as absent entirely.
        w.String("Status", kStatusFull);
        w.Long("ObeyModStamp", static_cast<int64_t>(chunk.ModStamp()));   // see Chunk::modStamp

        {
            // SerializableChunkData.copyOf: every LIGHT section (one past each
            // end of the world), with its block_states/biomes when it is a
            // world section and its BlockLight/SkyLight when that layer is not
            // empty (DataLayer.isEmpty: homogeneous zero). A section tag with
            // nothing in it is not written.
            const Lighting::ChunkLight& light = chunk.light;
            const bool writeLight = light.lightCorrect;
            std::array<int8_t, Lighting::DataLayer::kSize> bytes{};
            auto sections = w.BeginList("sections", Nbt::TagType::Compound);
            for (int li = 0; li < Lighting::kLightSectionCount; ++li) {
                const int i = li - 1;                               // block section index
                const ChunkSection* section =
                    (i >= 0 && i < Chunk::SECTION_COUNT) ? chunk.GetSection(i) : nullptr;
                const Lighting::DataLayer& blockLight = light.block[static_cast<size_t>(li)];
                const Lighting::DataLayer& skyLight = light.sky[static_cast<size_t>(li)];
                const bool hasBlockLight = writeLight && !blockLight.IsEmpty();
                const bool hasSkyLight = writeLight && !skyLight.IsEmpty();
                if (!section && !hasBlockLight && !hasSkyLight) continue;

                w.ListCompoundBegin(sections);
                if (section) {
                    WriteStateContainer(w, section->States());
                    WriteBiomeContainer(w, section->Biomes());
                }
                if (hasBlockLight) {
                    blockLight.CopyTo(reinterpret_cast<uint8_t*>(bytes.data()));
                    w.ByteArray("BlockLight", bytes.data(), bytes.size());
                }
                if (hasSkyLight) {
                    skyLight.CopyTo(reinterpret_cast<uint8_t*>(bytes.data()));
                    w.ByteArray("SkyLight", bytes.data(), bytes.size());
                }
                // Y is written LAST and as a signed byte, matching vanilla.
                w.Byte("Y", static_cast<int8_t>(Lighting::kMinLightSectionY + li));
                w.ListCompoundEnd(sections);
            }
            w.EndList(sections);
        }

        // MC writes isLightOn only when true: the light above is a finished
        // lighting of this chunk and Minecraft (and this engine) trust it on
        // load instead of relighting.
        if (chunk.light.lightCorrect) {
            w.Bool("isLightOn", true);
            // ObeyCraft extension: which revision of this engine's light
            // engine wrote the light (see kObeyLightVersion).
            w.Int("ObeyLightVersion", kObeyLightVersion);
        }

        {
            auto list = w.BeginList("block_entities", Nbt::TagType::Compound);
            for (const auto& [localPos, entity] : chunk.GetAllBlockEntities()) {
                if (entity) WriteBlockEntity(w, list, *entity, chunk.pos);
            }
            w.EndList(list);
        }
        // MC SavedTick.codec: { i: String, x/y/z: Int, t: Int, p: Int }.
        //
        // `t` is a DELAY relative to the current game time, not an absolute
        // trigger tick — that is what lets a save be reopened later without
        // every pending appointment firing at once. `p` is the SIGNED
        // TickPriority value (-3..3), because MC's codec is
        // Codec.INT.xmap(byValue, getValue) and not an ordinal.
        //
        // Water and lava appointments ride the same queue in this engine
        // (FlowingFluid.hpp) but are MC's LevelTicks<Fluid>, so they are
        // split out into `fluid_ticks` here and named by the FLUID the cell
        // holds right now — `minecraft:water` for a source, `flowing_water`
        // otherwise — because vanilla's tickFluid fires only when
        // `fluidState.is(type)` still holds.
        {
            const std::vector<SavedTick> packed = chunk.BlockTicks().Pack(gameTime);
            auto writeTick = [&](auto& l, const SavedTick& t, const std::string& name) {
                w.ListCompoundBegin(l);
                w.String("i", name);
                w.Int("x", t.pos.x);
                w.Int("y", t.pos.y);
                w.Int("z", t.pos.z);
                w.Int("t", t.delay);
                w.Int("p", static_cast<int32_t>(t.priority));
                w.ListCompoundEnd(l);
            };
            auto isFluidTick = [](const SavedTick& t) {
                return t.type == BlockID::Water || t.type == BlockID::Lava;
            };
            {
                auto l = w.BeginList("block_ticks", Nbt::TagType::Compound);
                for (const SavedTick& t : packed) {
                    if (isFluidTick(t)) continue;
                    writeTick(l, t, BlockName(t.type));
                }
                w.EndList(l);
            }
            {
                auto l = w.BeginList("fluid_ticks", Nbt::TagType::Compound);
                for (const SavedTick& t : packed) {
                    if (!isFluidTick(t)) continue;
                    const int localX = t.pos.x & 15;
                    const int localZ = t.pos.z & 15;
                    const FluidState fluid = FluidStateOf(BlockStates::FromIndex(
                        chunk.GetBlock(localX, t.pos.y, localZ),
                        chunk.GetBlockState(localX, t.pos.y, localZ)));
                    const bool lava = t.type == BlockID::Lava;
                    const std::string name = fluid.IsSource()
                        ? (lava ? "minecraft:lava" : "minecraft:water")
                        : (lava ? "minecraft:flowing_lava" : "minecraft:flowing_water");
                    writeTick(l, t, name);
                }
                w.EndList(l);
            }
        }
        {
            // SerializableChunkData.packOffsets: one short list per section
            // (Chunk::postProcessing), written only while cells are pending.
            auto l = w.BeginList("PostProcessing", Nbt::TagType::List);
            if (chunk.HasPostProcessing()) {
                for (int i = 0; i < Chunk::SECTION_COUNT; ++i) {
                    auto section = w.ListListBegin(l, Nbt::TagType::Short);
                    if (i < static_cast<int>(chunk.postProcessing.size())) {
                        for (int16_t packed : chunk.postProcessing[static_cast<size_t>(i)]) {
                            w.ListShort(section, packed);
                        }
                    }
                    w.EndList(section);
                }
            }
            w.EndList(l);
        }

        // Heightmaps: emit ONLY what we actually filled.
        //
        // SerializableChunkData.read primes the types that are ABSENT and
        // trusts the ones that are present. A key written zero-filled
        // therefore means "every column in this chunk is empty", and
        // Minecraft believes it — breaking mob spawning, snow and water
        // placement, and structure siting. Omitting the whole compound when
        // the chunk was never primed makes vanilla recompute all four.
        w.BeginCompound("Heightmaps");
        if (chunk.AreHeightmapsPrimed()) {
            for (size_t i = 0; i < static_cast<size_t>(HeightmapType::Count); ++i) {
                const auto type = static_cast<HeightmapType>(i);
                const std::vector<int64_t> packed = chunk.GetHeightmap(type).PackToLongs();
                w.LongArray(HeightmapSerializationKey(type), packed.data(), packed.size());
            }
        }
        w.EndCompound();

        // The terrain library's own compound when the chunk carries one
        // (references, and the starts begun here — Chunk::structuresNbt).
        // Otherwise empty, which is legal and the shape vanilla emits — MCA
        // Selector and Amulet both expect the two sub-compounds to exist.
        if (chunk.structuresNbt && !chunk.structuresNbt->empty()) {
            w.EmbedCompound("structures", *chunk.structuresNbt);
        } else {
            w.BeginCompound("structures");
            w.BeginCompound("starts");     w.EndCompound();
            w.BeginCompound("References"); w.EndCompound();
            w.EndCompound();
        }

        // ObeyCraft extension: the structure boxes the natural spawner's
        // spawn_overrides and fortress rule read (Chunk::structureSpawnAreas).
        // Vanilla keeps these as the "structures" starts above, which this
        // engine does not serialise; an unknown key is ignored by Minecraft.
        // Omitted when empty, which is almost every chunk.
        if (!chunk.structureSpawnAreas.empty()) {
            auto areas = w.BeginList("ObeyStructureSpawns", Nbt::TagType::Compound);
            for (const StructureSpawnArea& area : chunk.structureSpawnAreas) {
                w.ListCompoundBegin(areas);
                w.String("structure", area.structure);
                const int32_t box[6] = {area.startMin.x, area.startMin.y, area.startMin.z,
                                        area.startMax.x, area.startMax.y, area.startMax.z};
                w.IntArray("box", box, 6);
                auto pieces = w.BeginList("pieces", Nbt::TagType::Compound);
                for (const StructureSpawnArea::Piece& piece : area.pieces) {
                    w.ListCompoundBegin(pieces);
                    const int32_t pieceBox[6] = {piece.min.x, piece.min.y, piece.min.z,
                                                 piece.max.x, piece.max.y, piece.max.z};
                    w.IntArray("box", pieceBox, 6);
                    if (!piece.templateId.empty()) w.String("template", piece.templateId);
                    if (piece.rotation != 0) w.Byte("rot", static_cast<int8_t>(piece.rotation));
                    if (!piece.pieceType.empty()) w.String("type", piece.pieceType);
                    w.ListCompoundEnd(pieces);
                }
                w.EndList(pieces);
                w.ListCompoundEnd(areas);
            }
            w.EndList(areas);
        }

        w.EndRootCompound();

        if (!w.ok()) { error = "NBT writer refused the chunk"; return false; }
        out = w.TakeBytes();
        return true;
    }

    // ── Read ────────────────────────────────────────────────────────────────

    bool IsModernChunkLayout(const std::vector<uint8_t>& nbt) {
        ::World::NBTTagPtr root;
        try { root = ::World::NBTParser::Parse(nbt); } catch (...) { return false; }
        auto rootC = AsCompound(root);
        if (!rootC) return false;
        auto sections = std::dynamic_pointer_cast<NBTTagList>(rootC->GetTag("sections"));
        if (!sections || sections->value.empty()) return false;
        for (const auto& s : sections->value) {
            if (auto sec = AsCompound(s)) {
                if (sec->GetTag("block_states")) return true;
            }
        }
        return false;
    }

    bool DeserialiseChunk(const std::vector<uint8_t>& nbt, Math::ChunkPos expected,
                          Chunk& out, std::string& error, ReadMode mode,
                          int64_t gameTime) {
        ::World::NBTTagPtr root;
        try {
            root = ::World::NBTParser::Parse(nbt);
        } catch (const std::exception& e) {
            error = std::string("NBT parse failed: ") + e.what();
            return false;
        }
        auto rootC = AsCompound(root);
        if (!rootC) { error = "chunk root is not a compound"; return false; }

        // Status is the ONE hard requirement: SerializableChunkData.parse
        // returns null on an empty one and the chunk then counts as absent.
        const std::string status = rootC->GetValue<std::string>("Status");
        if (status.empty()) {
            error = "chunk has no Status";
            return false;
        }
        // A chunk that never finished generating is not terrain we can serve.
        // Loading one as if it were finished is how a world grows holes and
        // half-carved caves at its edges — vanilla re-runs the remaining
        // generation steps instead, which we cannot, so we regenerate.
        if (status != kStatusFull && status != "full") {
            error.clear();          // "not usable", not "broken"
            return false;
        }

        const int32_t xPos = rootC->GetValue<int32_t>("xPos", expected.x);
        const int32_t zPos = rootC->GetValue<int32_t>("zPos", expected.z);
        if (xPos != expected.x || zPos != expected.z) {
            if (mode == ReadMode::Strict) {
                error = "chunk is at " + std::to_string(xPos) + "," + std::to_string(zPos) +
                        " but was indexed at " + std::to_string(expected.x) + "," +
                        std::to_string(expected.z);
                return false;
            }
            // Vanilla's reportMisplacedChunk: warn and use the indexed position.
            Log::Warning("[Anvil] chunk file at (%d,%d) says it is (%d,%d) — relocating",
                         expected.x, expected.z, xPos, zPos);
        }
        out.pos = expected;
        // Kept verbatim for the next save and for the terrain library, which
        // reads its structure starts and references back from it.
        {
            size_t begin = 0, end = 0;
            if (NbtScan::FindRootTag(nbt, "structures", NbtScan::kCompound, begin, end)) {
                out.structuresNbt = std::make_shared<const std::vector<uint8_t>>(
                    nbt.begin() + static_cast<std::ptrdiff_t>(begin),
                    nbt.begin() + static_cast<std::ptrdiff_t>(end));
            }
        }
        out.modStamp.store(static_cast<uint64_t>(rootC->GetValue<int64_t>("ObeyModStamp", 0)), std::memory_order_relaxed);
        out.inhabitedTime.store(rootC->GetValue<int64_t>("InhabitedTime", 0), std::memory_order_relaxed);

        // MC SerializableChunkData: `isLightOn` says the saved light is a
        // finished lighting to be trusted; without it the chunk is relit
        // (ChunkProvider::CompleteChunkLoad).
        // A chunk this engine wrote (it carries ObeyModStamp) is trusted only
        // when its light came from the current light engine revision; one
        // Minecraft wrote carries no Obey tags and MC's own light is trusted.
        const bool writtenByEngine = rootC->GetTag("ObeyModStamp") != nullptr;
        const bool lightOn = rootC->GetValue<int8_t>("isLightOn", 0) != 0 &&
            (!writtenByEngine || rootC->GetValue<int32_t>("ObeyLightVersion", 0) >= kObeyLightVersion);
        std::array<bool, Lighting::kLightSectionCount> skyPresent{};
        out.light.Reset();

        auto sections = std::dynamic_pointer_cast<NBTTagList>(rootC->GetTag("sections"));
        if (sections) {
            for (const auto& sTag : sections->value) {
                auto sec = AsCompound(sTag);
                if (!sec) continue;

                const int sectionY = sec->GetValue<int8_t>("Y", 0);
                const int index    = sectionY - kMinSectionY;

                // Light first: it exists one section past each end of the
                // build range too (vanilla's light-only sections).
                const int li = sectionY - Lighting::kMinLightSectionY;
                if (lightOn && li >= 0 && li < Lighting::kLightSectionCount) {
                    auto readLayer = [&](const char* key, Lighting::DataLayer& dstLayer) {
                        auto arr = std::dynamic_pointer_cast<::World::NBTTagByteArray>(sec->GetTag(key));
                        if (!arr || arr->value.size() != static_cast<size_t>(Lighting::DataLayer::kSize)) return false;
                        dstLayer = Lighting::DataLayer::FromBytes(reinterpret_cast<const uint8_t*>(arr->value.data()));
                        dstLayer.Compact();
                        return true;
                    };
                    readLayer("BlockLight", out.light.block[static_cast<size_t>(li)]);
                    skyPresent[static_cast<size_t>(li)] = readLayer("SkyLight", out.light.sky[static_cast<size_t>(li)]);
                }

                if (index < 0 || index >= Chunk::SECTION_COUNT) {
                    // Light-only sections carry no block_states.
                    continue;
                }
                ChunkSection* dst = out.GetSection(index);
                if (!dst) continue;

                if (auto bs = AsCompound(sec->GetTag("block_states"))) {
                    auto pal = std::dynamic_pointer_cast<NBTTagList>(bs->GetTag("palette"));
                    if (pal && !pal->value.empty()) {
                        std::vector<uint32_t> values;
                        values.reserve(pal->value.size());
                        for (const auto& e : pal->value) {
                            auto entry = AsCompound(e);
                            values.push_back(entry ? ResolveBlockStateEntry(*entry) : BlockState{}.RawId());
                        }
                        std::vector<uint64_t> data;
                        ReadLongArray(*bs, "data", data);

                        PalettedContainer container;
                        std::string why;
                        if (!UnpackFromDisk(values, data, dst->States().Strategy(), container, why)) {
                            error = "section " + std::to_string(sectionY) + " block_states: " + why;
                            return false;
                        }
                        dst->AdoptStates(std::move(container));
                    }
                }

                if (auto bi = AsCompound(sec->GetTag("biomes"))) {
                    auto pal = std::dynamic_pointer_cast<NBTTagList>(bi->GetTag("palette"));
                    if (pal && !pal->value.empty()) {
                        std::vector<uint32_t> values;
                        values.reserve(pal->value.size());
                        for (const auto& e : pal->value) {
                            auto s = std::dynamic_pointer_cast<NBTTagString>(e);
                            values.push_back(s ? BiomeRegistry::FromName(s->value)
                                               : BiomeRegistry::Fallback());
                        }
                        std::vector<uint64_t> data;
                        ReadLongArray(*bi, "data", data);

                        PalettedContainer container;
                        std::string why;
                        if (!UnpackFromDisk(values, data, ChunkSection::BiomeStrategy(), container, why)) {
                            error = "section " + std::to_string(sectionY) + " biomes: " + why;
                            return false;
                        }
                        dst->AdoptBiomes(std::move(container));
                    }
                }
            }
        }

        // Sky layers the save left out: MC stores sky data only up to a
        // column's top non-empty section and derives the rest
        // (SkyLightSectionStorage.createDataLayer): nothing above -> all 15;
        // otherwise the lowest row of the nearest stored layer above,
        // repeated (repeatFirstLayer). Missing block layers are simply 0.
        if (lightOn) {
            for (int li = Lighting::kLightSectionCount - 1; li >= 0; --li) {
                if (skyPresent[static_cast<size_t>(li)]) continue;
                Lighting::DataLayer& layer = out.light.sky[static_cast<size_t>(li)];
                if (li == Lighting::kLightSectionCount - 1) { layer = Lighting::DataLayer(15); continue; }
                const Lighting::DataLayer& above = out.light.sky[static_cast<size_t>(li + 1)];
                if (above.IsDefinitelyHomogeneous()) { layer = Lighting::DataLayer(above.DefaultValue()); continue; }
                std::array<uint8_t, Lighting::DataLayer::kSize> repeated{};
                for (int y = 0; y < 16; ++y) {
                    std::memcpy(repeated.data() + y * Lighting::DataLayer::kLayerSize, above.RawData(),
                                Lighting::DataLayer::kLayerSize);
                }
                layer = Lighting::DataLayer::FromBytes(repeated.data());
                layer.Compact();
            }
            // The sky source heightmap is not saved (MC rebuilds it in the
            // LevelChunk constructor either): recompute from the blocks.
            out.light.skySources.FillFrom(out);
            out.light.lightCorrect = true;
        }

        // Block entities LAST: each is validated against the block actually
        // sitting under it, which is only known once the sections are decoded.
        if (auto list = std::dynamic_pointer_cast<NBTTagList>(rootC->GetTag("block_entities"))) {
            for (const auto& element : list->value) {
                auto entry = AsCompound(element);
                if (!entry) continue;
                const int worldY = entry->GetValue<int32_t>("y", 0);
                const int localX = entry->GetValue<int32_t>("x", 0) & 15;
                const int localZ = entry->GetValue<int32_t>("z", 0) & 15;
                const BlockID blockAt = out.GetBlock(localX, worldY, localZ);

                glm::ivec3 local{};
                auto entity = ReadBlockEntity(*entry, expected, blockAt, local);
                if (entity) out.SetBlockEntity(local.x, local.y, local.z, std::move(entity));
            }
        }

        // MC LevelChunk.postProcessing (see the writer): list i is section i.
        if (auto list = std::dynamic_pointer_cast<NBTTagList>(rootC->GetTag("PostProcessing"))) {
            for (size_t i = 0; i < list->value.size() && i < static_cast<size_t>(Chunk::SECTION_COUNT); ++i) {
                auto section = std::dynamic_pointer_cast<NBTTagList>(list->value[i]);
                if (!section || section->value.empty()) continue;
                if (out.postProcessing.empty()) out.postProcessing.resize(Chunk::SECTION_COUNT);
                for (const auto& element : section->value) {
                    if (auto packed = std::dynamic_pointer_cast<::World::NBTTagShort>(element)) {
                        out.postProcessing[i].push_back(packed->value);
                    }
                }
            }
        }

        // ObeyCraft extension: structure spawn areas (see the writer).
        if (auto list = std::dynamic_pointer_cast<NBTTagList>(rootC->GetTag("ObeyStructureSpawns"))) {
            auto readBox = [](const NBTTagCompound& c, glm::ivec3& lo, glm::ivec3& hi) {
                auto arr = std::dynamic_pointer_cast<::World::NBTTagIntArray>(c.GetTag("box"));
                if (!arr || arr->value.size() != 6) return false;
                lo = glm::ivec3(arr->value[0], arr->value[1], arr->value[2]);
                hi = glm::ivec3(arr->value[3], arr->value[4], arr->value[5]);
                return true;
            };
            for (const auto& element : list->value) {
                auto entry = AsCompound(element);
                if (!entry) continue;
                StructureSpawnArea area;
                area.structure = entry->GetValue<std::string>("structure", "");
                if (area.structure.empty() || !readBox(*entry, area.startMin, area.startMax)) continue;
                if (auto pieces = std::dynamic_pointer_cast<NBTTagList>(entry->GetTag("pieces"))) {
                    for (const auto& p : pieces->value) {
                        auto pc = AsCompound(p);
                        if (!pc) continue;
                        StructureSpawnArea::Piece piece;
                        if (!readBox(*pc, piece.min, piece.max)) continue;
                        piece.templateId = pc->GetValue<std::string>("template", "");
                        piece.rotation = static_cast<uint8_t>(pc->GetValue<int8_t>("rot", 0) & 3);
                        piece.pieceType = pc->GetValue<std::string>("type", "");
                        area.pieces.push_back(std::move(piece));
                    }
                }
                out.structureSpawnAreas.push_back(std::move(area));
            }
        }

        // Scheduled block ticks. Read AFTER the sections, like the block
        // entities above, so an appointment naming a block this build does not
        // model can be spotted and dropped rather than resurrected as air.
        {
            std::vector<SavedTick> saved;
            auto readTicks = [&](const char* key, bool fluidList) {
                auto list = std::dynamic_pointer_cast<NBTTagList>(rootC->GetTag(key));
                if (!list) return;
                saved.reserve(saved.size() + list->value.size());
                for (const auto& element : list->value) {
                    auto entry = AsCompound(element);
                    if (!entry) continue;
                    auto nameTag = std::dynamic_pointer_cast<NBTTagString>(entry->GetTag("i"));
                    if (!nameTag) continue;

                    BlockID type = BlockID::Air;
                    if (fluidList) {
                        // MC's fluid registry ids. Source and flowing collapse
                        // onto the one block that carries the fluid here; the
                        // tick runner re-derives which it is from the cell.
                        const std::string& n = nameTag->value;
                        if (n == "minecraft:water" || n == "minecraft:flowing_water") type = BlockID::Water;
                        else if (n == "minecraft:lava" || n == "minecraft:flowing_lava") type = BlockID::Lava;
                    } else {
                        // Only the BLOCK matters here — a scheduled tick names a
                        // block, not a state (MC keys ScheduledTick on Block).
                        // Resolving with an empty property map gives the default
                        // state, whose Block() is exactly what we want.
                        const NbtBlockState resolved =
                            BlockStateRegistry::CreateBlockState(nameTag->value, {});
                        type = resolved.resolvedId;
                    }
                    // An unknown name resolves to air. Dropping it is the same
                    // rule the palette reader applies, and keeping it would
                    // schedule a tick that can never match its cell.
                    if (type == BlockID::Air) continue;

                    saved.push_back(SavedTick{
                        type,
                        glm::ivec3{entry->GetValue<int32_t>("x", 0),
                                   entry->GetValue<int32_t>("y", 0),
                                   entry->GetValue<int32_t>("z", 0)},
                        entry->GetValue<int32_t>("t", 0),
                        TickPriorityByValue(entry->GetValue<int32_t>("p", 0))});
                }
            };
            readTicks("block_ticks", false);
            readTicks("fluid_ticks", true);
            if (!saved.empty()) {
                // Sub-tick order restarts from 0 for a freshly loaded chunk.
                // MC does the same (SavedTick.unpack takes the world's counter,
                // but the relative order within one chunk is all that survives
                // a save anyway) and the list order is the tiebreak.
                int64_t subTick = 0;
                out.BlockTicks().Unpack(saved, gameTime, subTick);
            }
        }

        // Heightmaps: adopt only the types actually present, and only if the
        // long array is the right length. Anything missing gets primed, which
        // is what vanilla does too.
        bool anyMissing = false;
        if (auto hm = AsCompound(rootC->GetTag("Heightmaps"))) {
            for (size_t i = 0; i < static_cast<size_t>(HeightmapType::Count); ++i) {
                const auto type = static_cast<HeightmapType>(i);
                auto tag = std::dynamic_pointer_cast<NBTTagLongArray>(
                    hm->GetTag(HeightmapSerializationKey(type)));
                if (!tag || !out.GetHeightmap(type).UnpackFromLongs(tag->value)) {
                    anyMissing = true;
                }
            }
        } else {
            anyMissing = true;
        }
        if (anyMissing) out.PrimeHeightmaps(); else out.MarkHeightmapsPrimed();

        return true;
    }

    // ── Debug round-trip guard ──────────────────────────────────────────────

    bool VerifyRoundTrip(const Chunk& source, const std::vector<uint8_t>& nbt,
                         std::string& mismatch) {
        Chunk reloaded;
        std::string error;
        if (!DeserialiseChunk(nbt, source.pos, reloaded, error)) {
            mismatch = "could not read back what we just wrote: " + error;
            return false;
        }

        // Scheduled ticks round-trip too. Only the COUNT is compared: the
        // delays were computed against the writer's game time and rebased
        // against 0 here, so the trigger ticks legitimately differ. A count
        // mismatch is the failure that matters — it means a tick was dropped by
        // the name lookup or the writer emitted a malformed entry.
        if (reloaded.BlockTicks().Size() != source.BlockTicks().Size()) {
            mismatch = "block_ticks count " +
                       std::to_string(reloaded.BlockTicks().Size()) + " != " +
                       std::to_string(source.BlockTicks().Size());
            return false;
        }

        for (int i = 0; i < Chunk::SECTION_COUNT; ++i) {
            const ChunkSection* a = source.GetSection(i);
            const ChunkSection* b = reloaded.GetSection(i);
            if (!a || !b) continue;

            for (size_t v = 0; v < static_cast<size_t>(a->States().Strategy().entryCount); ++v) {
                if (a->States().Get(v) != b->States().Get(v)) {
                    mismatch = "section " + std::to_string(i + kMinSectionY) + " voxel " +
                               std::to_string(v) + ": state " + std::to_string(a->States().Get(v)) +
                               " came back as " + std::to_string(b->States().Get(v));
                    return false;
                }
            }
            for (size_t v = 0; v < static_cast<size_t>(a->Biomes().Strategy().entryCount); ++v) {
                if (a->Biomes().Get(v) != b->Biomes().Get(v)) {
                    mismatch = "section " + std::to_string(i + kMinSectionY) + " biome cell " +
                               std::to_string(v) + ": " + std::to_string(a->Biomes().Get(v)) +
                               " came back as " + std::to_string(b->Biomes().Get(v));
                    return false;
                }
            }
        }
        return true;
    }

} // namespace Game::Anvil
