// File: src/server/world/storage/anvil/ChunkSerializer.cpp
#include "server/world/storage/anvil/ChunkSerializer.hpp"

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

#include <string>

namespace Game::Anvil {

    namespace {

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
            auto nameTag = std::dynamic_pointer_cast<NBTTagString>(entry.GetTag("Name"));
            if (!nameTag) return BlockState{}.RawId();

            std::unordered_map<std::string, std::string> props;
            if (auto p = AsCompound(entry.GetTag("Properties"))) {
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
        w.Long("InhabitedTime", 0);
        // The ONE hard requirement: SerializableChunkData.parse returns null on
        // an empty Status, and the chunk then counts as absent entirely.
        w.String("Status", kStatusFull);

        {
            auto sections = w.BeginList("sections", Nbt::TagType::Compound);
            for (int i = 0; i < Chunk::SECTION_COUNT; ++i) {
                const ChunkSection* section = chunk.GetSection(i);
                if (!section) continue;             // never null in practice

                w.ListCompoundBegin(sections);
                WriteStateContainer(w, section->States());
                WriteBiomeContainer(w, section->Biomes());
                // Y is written LAST and as a signed byte, matching vanilla.
                w.Byte("Y", static_cast<int8_t>(i + kMinSectionY));
                w.ListCompoundEnd(sections);
            }
            w.EndList(sections);
        }

        // isLightOn is deliberately ABSENT. Vanilla writes it only when true,
        // and its absence tells Minecraft to relight the chunk on load — which
        // is correct, because we do not compute vanilla-identical light.

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
        {
            auto l = w.BeginList("block_ticks", Nbt::TagType::Compound);
            for (const SavedTick& t : chunk.BlockTicks().Pack(gameTime)) {
                w.ListCompoundBegin(l);
                w.String("i", BlockName(t.type));
                w.Int("x", t.pos.x);
                w.Int("y", t.pos.y);
                w.Int("z", t.pos.z);
                w.Int("t", t.delay);
                w.Int("p", static_cast<int32_t>(t.priority));
                w.ListCompoundEnd(l);
            }
            w.EndList(l);
        }
        // No fluid simulation yet, so this list is genuinely always empty —
        // unlike block_ticks, which was empty only because nothing produced it.
        { auto l = w.BeginList("fluid_ticks",    Nbt::TagType::Compound); w.EndList(l); }
        { auto l = w.BeginList("PostProcessing", Nbt::TagType::List);     w.EndList(l); }

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

        // Empty is legal, and this is the shape vanilla emits — MCA Selector
        // and Amulet both expect the two sub-compounds to exist.
        w.BeginCompound("structures");
        w.BeginCompound("starts");     w.EndCompound();
        w.BeginCompound("References"); w.EndCompound();
        w.EndCompound();

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

        auto sections = std::dynamic_pointer_cast<NBTTagList>(rootC->GetTag("sections"));
        if (sections) {
            for (const auto& sTag : sections->value) {
                auto sec = AsCompound(sTag);
                if (!sec) continue;

                const int sectionY = sec->GetValue<int8_t>("Y", 0);
                const int index    = sectionY - kMinSectionY;
                if (index < 0 || index >= Chunk::SECTION_COUNT) {
                    // Vanilla emits light-only sections one below and one above
                    // the build range; they carry no block_states and are not
                    // ours to store.
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

        // Scheduled block ticks. Read AFTER the sections, like the block
        // entities above, so an appointment naming a block this build does not
        // model can be spotted and dropped rather than resurrected as air.
        if (auto list = std::dynamic_pointer_cast<NBTTagList>(rootC->GetTag("block_ticks"))) {
            std::vector<SavedTick> saved;
            saved.reserve(list->value.size());
            for (const auto& element : list->value) {
                auto entry = AsCompound(element);
                if (!entry) continue;
                auto nameTag = std::dynamic_pointer_cast<NBTTagString>(entry->GetTag("i"));
                if (!nameTag) continue;

                // Only the BLOCK matters here — a scheduled tick names a block,
                // not a state (MC keys ScheduledTick on Block). Resolving with
                // an empty property map gives the default state, whose Block()
                // is exactly what we want.
                const NbtBlockState resolved =
                    BlockStateRegistry::CreateBlockState(nameTag->value, {});
                const BlockID type = resolved.resolvedId;
                // An unknown name resolves to air. Dropping it is the same rule
                // the palette reader applies, and keeping it would schedule a
                // tick that can never match its cell.
                if (type == BlockID::Air) continue;

                saved.push_back(SavedTick{
                    type,
                    glm::ivec3{entry->GetValue<int32_t>("x", 0),
                               entry->GetValue<int32_t>("y", 0),
                               entry->GetValue<int32_t>("z", 0)},
                    entry->GetValue<int32_t>("t", 0),
                    TickPriorityByValue(entry->GetValue<int32_t>("p", 0))});
            }
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
