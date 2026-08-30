// File: tools/anvil_parity/anvil_parity.cpp
//
// Offline check of the Anvil container and the NBT writer. No engine, no
// window, no world — a handful of translation units and a real region file,
// runs in well under a second.
//
// Modelled on tools/blockstate_parity for the same reason that exists: this
// is bit-level code whose failure modes are silent. A sector allocator that
// is subtly wrong does not crash; it hands out a sector that still holds the
// only copy of another chunk, and the world loses terrain a week later.
// Checking it against a file vanilla actually wrote is the only honest gate.
//
// Build:
//   clang++ -std=c++20 -I src -g -fsanitize=address \
//       tools/anvil_parity/anvil_parity.cpp \
//       src/server/world/storage/anvil/AnvilRegion.cpp \
//       src/server/world/storage/NBTParser.cpp \
//       src/common/nbt/NbtWrite.cpp \
//       src/common/core/Log.cpp \
//       -lz -o /tmp/anvilparity
//   /tmp/anvilparity "<a real world folder>"
//
// With no argument it looks for a vanilla world under the user's Minecraft
// install, which is the ground truth we actually want to be measured against.
#include "server/world/storage/anvil/AnvilRegion.hpp"
#include "server/world/storage/anvil/PaletteCodec.hpp"
#include "server/world/storage/NBTParser.hpp"
#include "common/nbt/NbtWrite.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace Game;

namespace {

    int g_checks = 0;
    int g_failures = 0;
    int g_reported = 0;

    void Check(bool ok, const std::string& what) {
        ++g_checks;
        if (ok) return;
        ++g_failures;
        if (++g_reported <= 30) std::printf("  FAIL  %s\n", what.c_str());
    }

    void Section(const char* title) { std::printf("\n== %s ==\n", title); }

    // ── Semantic NBT comparison ─────────────────────────────────────────────
    //
    // NOT a byte comparison. NBTTagCompound is backed by std::unordered_map,
    // so the key order a parse produces is not the order the file had, and
    // re-emitting can never reproduce the original bytes. (Nor could it in
    // general: matching Mojang's exact deflate output is not a property worth
    // depending on.) What must hold is that the tag TREE survives a
    // write/read cycle unchanged.
    bool TreeEqual(const World::NBTTagPtr& a, const World::NBTTagPtr& b, std::string path, std::string& why);

    template <typename T>
    bool ScalarEqual(const World::NBTTagPtr& a, const World::NBTTagPtr& b) {
        auto x = std::dynamic_pointer_cast<T>(a);
        auto y = std::dynamic_pointer_cast<T>(b);
        return x && y && x->value == y->value;
    }

    bool TreeEqual(const World::NBTTagPtr& a, const World::NBTTagPtr& b, std::string path, std::string& why) {
        if (!a || !b) { why = path + ": one side is null"; return false; }
        if (a->type != b->type) { why = path + ": type differs"; return false; }

        using T = World::NBTTagType;
        switch (a->type) {
            case T::TAG_Byte:      if (!ScalarEqual<World::NBTTagByte>(a, b))   { why = path + ": byte differs";   return false; } return true;
            case T::TAG_Short:     if (!ScalarEqual<World::NBTTagShort>(a, b))  { why = path + ": short differs";  return false; } return true;
            case T::TAG_Int:       if (!ScalarEqual<World::NBTTagInt>(a, b))    { why = path + ": int differs";    return false; } return true;
            case T::TAG_Long:      if (!ScalarEqual<World::NBTTagLong>(a, b))   { why = path + ": long differs";   return false; } return true;
            case T::TAG_Float:     if (!ScalarEqual<World::NBTTagFloat>(a, b))  { why = path + ": float differs";  return false; } return true;
            case T::TAG_Double:    if (!ScalarEqual<World::NBTTagDouble>(a, b)) { why = path + ": double differs"; return false; } return true;
            case T::TAG_String:    if (!ScalarEqual<World::NBTTagString>(a, b)) { why = path + ": string differs"; return false; } return true;
            case T::TAG_Byte_Array:if (!ScalarEqual<World::NBTTagByteArray>(a,b)){ why = path + ": byte[] differs"; return false; } return true;
            case T::TAG_Int_Array: if (!ScalarEqual<World::NBTTagIntArray>(a,b)) { why = path + ": int[] differs";  return false; } return true;
            case T::TAG_Long_Array:if (!ScalarEqual<World::NBTTagLongArray>(a,b)){ why = path + ": long[] differs"; return false; } return true;

            case T::TAG_List: {
                auto x = std::dynamic_pointer_cast<World::NBTTagList>(a);
                auto y = std::dynamic_pointer_cast<World::NBTTagList>(b);
                if (!x || !y) { why = path + ": not a list"; return false; }
                if (x->value.size() != y->value.size()) { why = path + ": list size differs"; return false; }
                // An EMPTY list declares element type TAG_End in vanilla, so
                // only compare the element type when there are elements.
                if (!x->value.empty() && x->listType != y->listType) { why = path + ": list elem type differs"; return false; }
                for (size_t i = 0; i < x->value.size(); ++i) {
                    if (!TreeEqual(x->value[i], y->value[i], path + "[" + std::to_string(i) + "]", why)) return false;
                }
                return true;
            }
            case T::TAG_Compound: {
                auto x = std::dynamic_pointer_cast<World::NBTTagCompound>(a);
                auto y = std::dynamic_pointer_cast<World::NBTTagCompound>(b);
                if (!x || !y) { why = path + ": not a compound"; return false; }
                if (x->value.size() != y->value.size()) {
                    why = path + ": compound key count differs (" + std::to_string(x->value.size())
                        + " vs " + std::to_string(y->value.size()) + ")";
                    return false;
                }
                for (const auto& [k, v] : x->value) {
                    auto it = y->value.find(k);
                    if (it == y->value.end()) { why = path + "." + k + ": missing after round-trip"; return false; }
                    if (!TreeEqual(v, it->second, path + "." + k, why)) return false;
                }
                return true;
            }
            default: return true;
        }
    }

    // ── Re-emit a parsed tree through Game::Nbt::Writer ──────────────────────

    void EmitValue(Nbt::Writer& w, const std::string& name, const World::NBTTagPtr& t);

    void EmitCompoundBody(Nbt::Writer& w, const World::NBTTagCompound& c) {
        for (const auto& [k, v] : c.value) EmitValue(w, k, v);
    }

    Nbt::TagType MapType(World::NBTTagType t) { return static_cast<Nbt::TagType>(static_cast<uint8_t>(t)); }

    // Emit one list element of whatever kind the list declares. Every scalar
    // kind is covered: the first run of this harness found that entity Pos
    // and Motion are lists of double and Rotation is a list of float, which
    // the writer had no way to express.
    void EmitListElement(Nbt::Writer& w, Nbt::Writer::ListScope& scope,
                         World::NBTTagType kind, const World::NBTTagPtr& e) {
        using T = World::NBTTagType;
        switch (kind) {
            case T::TAG_Byte:   w.ListByte  (scope, std::dynamic_pointer_cast<World::NBTTagByte>(e)->value);   break;
            case T::TAG_Short:  w.ListShort (scope, std::dynamic_pointer_cast<World::NBTTagShort>(e)->value);  break;
            case T::TAG_Int:    w.ListInt   (scope, std::dynamic_pointer_cast<World::NBTTagInt>(e)->value);    break;
            case T::TAG_Long:   w.ListLong  (scope, std::dynamic_pointer_cast<World::NBTTagLong>(e)->value);   break;
            case T::TAG_Float:  w.ListFloat (scope, std::dynamic_pointer_cast<World::NBTTagFloat>(e)->value);  break;
            case T::TAG_Double: w.ListDouble(scope, std::dynamic_pointer_cast<World::NBTTagDouble>(e)->value); break;
            case T::TAG_String: w.ListString(scope, std::dynamic_pointer_cast<World::NBTTagString>(e)->value); break;
            case T::TAG_Compound: {
                w.ListCompoundBegin(scope);
                EmitCompoundBody(w, *std::dynamic_pointer_cast<World::NBTTagCompound>(e));
                w.ListCompoundEnd(scope);
                break;
            }
            case T::TAG_List: {
                auto inner = std::dynamic_pointer_cast<World::NBTTagList>(e);
                auto is = w.ListListBegin(scope, MapType(inner->listType));
                for (const auto& ie : inner->value) EmitListElement(w, is, inner->listType, ie);
                w.EndList(is);
                break;
            }
            case T::TAG_Byte_Array: {
                auto a = std::dynamic_pointer_cast<World::NBTTagByteArray>(e);
                w.ListByteArray(scope, a->value.data(), a->value.size());
                break;
            }
            case T::TAG_Int_Array: {
                auto a = std::dynamic_pointer_cast<World::NBTTagIntArray>(e);
                w.ListIntArray(scope, a->value.data(), a->value.size());
                break;
            }
            case T::TAG_Long_Array: {
                auto a = std::dynamic_pointer_cast<World::NBTTagLongArray>(e);
                w.ListLongArray(scope, a->value.data(), a->value.size());
                break;
            }
            default:
                std::printf("  NOTE  unhandled list element type %d\n", static_cast<int>(kind));
                break;
        }
    }

    void EmitList(Nbt::Writer& w, const std::string& name, const World::NBTTagList& l) {
        auto scope = w.BeginList(name, MapType(l.listType));
        for (const auto& e : l.value) EmitListElement(w, scope, l.listType, e);
        w.EndList(scope);
    }

    void EmitValue(Nbt::Writer& w, const std::string& name, const World::NBTTagPtr& t) {
        using T = World::NBTTagType;
        switch (t->type) {
            case T::TAG_Byte:   w.Byte  (name, std::dynamic_pointer_cast<World::NBTTagByte>(t)->value);   break;
            case T::TAG_Short:  w.Short (name, std::dynamic_pointer_cast<World::NBTTagShort>(t)->value);  break;
            case T::TAG_Int:    w.Int   (name, std::dynamic_pointer_cast<World::NBTTagInt>(t)->value);    break;
            case T::TAG_Long:   w.Long  (name, std::dynamic_pointer_cast<World::NBTTagLong>(t)->value);   break;
            case T::TAG_Float:  w.Float (name, std::dynamic_pointer_cast<World::NBTTagFloat>(t)->value);  break;
            case T::TAG_Double: w.Double(name, std::dynamic_pointer_cast<World::NBTTagDouble>(t)->value); break;
            case T::TAG_String: w.String(name, std::dynamic_pointer_cast<World::NBTTagString>(t)->value); break;
            case T::TAG_Byte_Array: {
                auto a = std::dynamic_pointer_cast<World::NBTTagByteArray>(t);
                w.ByteArray(name, a->value.data(), a->value.size());
                break;
            }
            case T::TAG_Int_Array: {
                auto a = std::dynamic_pointer_cast<World::NBTTagIntArray>(t);
                w.IntArray(name, a->value.data(), a->value.size());
                break;
            }
            case T::TAG_Long_Array: {
                auto a = std::dynamic_pointer_cast<World::NBTTagLongArray>(t);
                w.LongArray(name, a->value.data(), a->value.size());
                break;
            }
            case T::TAG_List:     EmitList(w, name, *std::dynamic_pointer_cast<World::NBTTagList>(t)); break;
            case T::TAG_Compound: {
                w.BeginCompound(name);
                EmitCompoundBody(w, *std::dynamic_pointer_cast<World::NBTTagCompound>(t));
                w.EndCompound();
                break;
            }
            default: break;
        }
    }

    std::vector<fs::path> RegionFiles(const fs::path& world) {
        std::vector<fs::path> out;
        for (const char* sub : {"region", "entities"}) {
            const fs::path dir = world / sub;
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) continue;
            for (const auto& e : fs::directory_iterator(dir, ec)) {
                if (e.path().extension() == ".mca") out.push_back(e.path());
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    fs::path FindDefaultWorld() {
        const char* home = std::getenv("HOME");
        if (!home) return {};
        const fs::path saves = fs::path(home) / "Library/Application Support/minecraft/saves";
        std::error_code ec;
        if (!fs::is_directory(saves, ec)) return {};
        fs::path best;
        size_t bestRegions = 0;
        for (const auto& e : fs::directory_iterator(saves, ec)) {
            if (!e.is_directory()) continue;
            const size_t n = RegionFiles(e.path()).size();
            if (n > bestRegions) { bestRegions = n; best = e.path(); }
        }
        return best;
    }

} // namespace

// ── Gate A: read every chunk vanilla wrote, re-emit it, read it back ────────
static void GateA_RoundTrip(const std::vector<fs::path>& regions) {
    Section("Gate A - semantic NBT round-trip against vanilla-written chunks");
    size_t chunks = 0, bytes = 0;

    for (const auto& path : regions) {
        std::string err;
        auto region = Anvil::AnvilRegion::Open(path, /*writable=*/false, err);
        if (!region) { Check(false, "open " + path.filename().string() + ": " + err); continue; }

        for (int lz = 0; lz < 32; ++lz) {
            for (int lx = 0; lx < 32; ++lx) {
                if (!region->Has(lx, lz)) continue;

                std::vector<uint8_t> raw;
                if (!region->Read(lx, lz, raw, err)) {
                    Check(false, path.filename().string() + " chunk " + std::to_string(lx) + "," +
                                 std::to_string(lz) + " read: " + err);
                    continue;
                }
                ++chunks; bytes += raw.size();

                World::NBTTagPtr original;
                try { original = World::NBTParser::Parse(raw); }
                catch (const std::exception& e) {
                    Check(false, std::string("parse of vanilla chunk threw: ") + e.what());
                    continue;
                }
                auto rootC = std::dynamic_pointer_cast<World::NBTTagCompound>(original);
                if (!rootC) { Check(false, "vanilla chunk root is not a compound"); continue; }

                Nbt::Writer w;
                w.BeginRootCompound();
                EmitCompoundBody(w, *rootC);
                w.EndRootCompound();
                Check(w.ok(), "writer refused a vanilla chunk in " + path.filename().string());
                if (!w.ok()) continue;

                World::NBTTagPtr again;
                try { again = World::NBTParser::Parse(w.Bytes()); }
                catch (const std::exception& e) {
                    Check(false, std::string("re-parse of our own bytes threw: ") + e.what());
                    continue;
                }

                std::string why;
                Check(TreeEqual(original, again, "root", why),
                      path.filename().string() + " chunk " + std::to_string(lx) + "," +
                      std::to_string(lz) + " differs after round-trip: " + why);
            }
        }
    }
    std::printf("  %zu chunks, %.1f MB of NBT round-tripped\n", chunks, bytes / (1024.0 * 1024.0));
    Check(chunks > 0, "found no chunks to test");
}

// ── Gate B: a region file past the old 32 MB / 8192-sector ceiling ──────────
static void GateB_LargeRegion(const fs::path& tmpDir) {
    Section("Gate B - region file beyond 32 MB (the bitset<8192> ceiling)");
    const fs::path path = tmpDir / "r.0.0.mca";
    std::error_code ec; fs::remove(path, ec);

    std::string err;
    auto region = Anvil::AnvilRegion::Open(path, /*writable=*/true, err);
    if (!region) { Check(false, "open for write: " + err); return; }

    // Payload that compresses poorly, so the file grows quickly.
    std::mt19937 rng(1234);
    std::vector<uint8_t> blob(300 * 1024);
    for (auto& b : blob) b = static_cast<uint8_t>(rng());

    std::vector<uint8_t> packed;
    Check(Nbt::ZlibCompress(blob, packed), "zlib compress");

    // 140 chunks x ~300 KB incompressible => comfortably past 8192 sectors.
    int written = 0;
    for (int i = 0; i < 140; ++i) {
        const int lx = i % 32, lz = i / 32;
        if (!region->Write(lx, lz, packed, Anvil::AnvilRegion::kCompressionZlib, err)) {
            Check(false, "write chunk " + std::to_string(i) + ": " + err);
            break;
        }
        ++written;
    }
    Check(region->Flush(err), "flush: " + err);
    region.reset();

    const auto size = fs::file_size(path, ec);
    std::printf("  wrote %d chunks, file is %.1f MB (%llu sectors)\n",
                written, size / (1024.0 * 1024.0),
                static_cast<unsigned long long>(size / 4096));
    Check(size > 32u * 1024u * 1024u, "file did not exceed the 32 MB ceiling");
    Check(size % 4096 == 0, "region file is not a whole number of sectors");

    // Reopen and verify every chunk survives, which is what the old
    // free-before-allocate ordering would have broken.
    auto reopened = Anvil::AnvilRegion::Open(path, /*writable=*/false, err);
    if (!reopened) { Check(false, "reopen: " + err); return; }
    int verified = 0;
    for (int i = 0; i < written; ++i) {
        const int lx = i % 32, lz = i / 32;
        std::vector<uint8_t> got;
        if (!reopened->Read(lx, lz, got, err)) { Check(false, "reread " + std::to_string(i) + ": " + err); continue; }
        if (got != blob) { Check(false, "chunk " + std::to_string(i) + " came back different"); continue; }
        ++verified;
    }
    std::printf("  %d/%d chunks read back byte-identical\n", verified, written);
    Check(verified == written, "not every chunk survived");
}

// ── Oversized chunks: the .mcc external path ───────────────────────────────
static void GateB2_External(const fs::path& tmpDir) {
    Section("External .mcc path (payload >= 256 sectors)");
    const fs::path path = tmpDir / "r.-1.2.mca";
    std::error_code ec; fs::remove(path, ec);

    std::string err;
    auto region = Anvil::AnvilRegion::Open(path, true, err);
    if (!region) { Check(false, "open: " + err); return; }

    std::mt19937 rng(99);
    std::vector<uint8_t> big(2 * 1024 * 1024);      // ~2 MB incompressible
    for (auto& b : big) b = static_cast<uint8_t>(rng());
    std::vector<uint8_t> packed;
    Check(Nbt::ZlibCompress(big, packed), "compress");
    Check(packed.size() >= 256 * 4096, "payload is not actually oversized");

    Check(region->Write(5, 7, packed, Anvil::AnvilRegion::kCompressionZlib, err), "write oversized: " + err);
    Check(region->Flush(err), "flush: " + err);
    region.reset();

    // Absolute chunk coords: region (-1,2) local (5,7) -> chunk (-27, 71).
    const fs::path mcc = tmpDir / "c.-27.71.mcc";
    Check(fs::exists(mcc, ec), "external file c.-27.71.mcc was not created");

    auto reopened = Anvil::AnvilRegion::Open(path, false, err);
    if (!reopened) { Check(false, "reopen: " + err); return; }
    std::vector<uint8_t> got;
    Check(reopened->Read(5, 7, got, err), "read external: " + err);
    Check(got == big, "external chunk came back different");

    // The in-region stub must occupy exactly one sector.
    const auto size = fs::file_size(path, ec);
    std::printf("  region stays %llu bytes; payload lives in %s\n",
                static_cast<unsigned long long>(size), mcc.filename().string().c_str());
    Check(size <= 3 * 4096, "oversized chunk did not stay out of the region file");
}

// ── Rewrite-in-place: the allocate-before-free ordering ────────────────────
static void GateB3_Rewrite(const fs::path& tmpDir) {
    Section("Rewrite churn (allocate-before-free)");
    const fs::path path = tmpDir / "r.3.3.mca";
    std::error_code ec; fs::remove(path, ec);

    std::string err;
    auto region = Anvil::AnvilRegion::Open(path, true, err);
    if (!region) { Check(false, "open: " + err); return; }

    std::mt19937 rng(7);
    std::map<int, std::vector<uint8_t>> expected;

    // Rewrite the same 12 slots repeatedly at randomly varying sizes. This is
    // where freeing the old extent before allocating the new one hands back
    // sectors that still hold the live copy.
    for (int round = 0; round < 12; ++round) {
        for (int slot = 0; slot < 12; ++slot) {
            std::vector<uint8_t> payload(4096 + (rng() % (48 * 1024)));
            for (auto& b : payload) b = static_cast<uint8_t>(rng());
            std::vector<uint8_t> packed;
            if (!Nbt::ZlibCompress(payload, packed)) { Check(false, "compress"); return; }
            if (!region->Write(slot % 32, slot / 32, packed, Anvil::AnvilRegion::kCompressionZlib, err)) {
                Check(false, "write: " + err); return;
            }
            expected[slot] = payload;

            // Every previously written slot must still read back correctly
            // right now, not just at the end.
            for (const auto& [s, want] : expected) {
                std::vector<uint8_t> got;
                if (!region->Read(s % 32, s / 32, got, err)) { Check(false, "read slot " + std::to_string(s) + ": " + err); return; }
                if (got != want) { Check(false, "slot " + std::to_string(s) + " corrupted during round " + std::to_string(round)); return; }
            }
        }
    }
    Check(true, "rewrite churn");
    std::printf("  144 rewrites, every live chunk verified after each write\n");

    // Clear must free sectors and leave everything else intact.
    Check(region->Clear(0, 0, err), "clear: " + err);
    Check(!region->Has(0, 0), "cleared chunk still present");
    std::vector<uint8_t> got;
    Check(region->Read(1, 0, got, err) && got == expected[1], "neighbour damaged by Clear");
}

// ── Gate C: truncation fuzz ────────────────────────────────────────────────
static void GateC_Truncation(const std::vector<fs::path>& regions, const fs::path& tmpDir) {
    Section("Gate C - truncated / corrupted region files");
    if (regions.empty()) { Check(false, "no source region to corrupt"); return; }

    std::ifstream in(regions.front(), std::ios::binary);
    std::vector<uint8_t> full((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (full.size() < 8192) { Check(false, "source region is too small to fuzz"); return; }

    const fs::path victim = tmpDir / "r.9.9.mca";
    std::mt19937 rng(20260825);
    int survived = 0;
    constexpr int kTrials = 400;

    for (int trial = 0; trial < kTrials; ++trial) {
        std::vector<uint8_t> bytes = full;
        if (trial % 2 == 0) {
            // Truncate at a random offset.
            bytes.resize(1 + (rng() % full.size()));
        } else {
            // Flip a run of bytes somewhere — usually inside the header, which
            // is what turns a slot into a nonsense extent.
            const size_t at = rng() % bytes.size();
            const size_t n  = 1 + (rng() % 64);
            for (size_t i = at; i < std::min(bytes.size(), at + n); ++i) bytes[i] = static_cast<uint8_t>(rng());
        }
        { std::ofstream out(victim, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }

        std::string err;
        auto region = Anvil::AnvilRegion::Open(victim, /*writable=*/false, err);
        if (!region) { ++survived; continue; }        // refusing to open is fine

        for (int lz = 0; lz < 32; ++lz) {
            for (int lx = 0; lx < 32; ++lx) {
                if (!region->Has(lx, lz)) continue;
                std::vector<uint8_t> out;
                std::string e;
                (void)region->Read(lx, lz, out, e);   // must not hang, crash or OOM
                Check(out.size() <= Anvil::AnvilRegion::kMaxDecompressedBytes, "read exceeded the inflate cap");
            }
        }
        ++survived;
    }
    std::printf("  %d/%d mutated files handled without a hang, crash or unbounded allocation\n", survived, kTrials);
    Check(survived == kTrials, "a mutated region file was not handled");
}


// ── Gate D: the palette packing, against bytes vanilla itself wrote ────────
//
// The strongest check available, and the one R1 is really about. It compares
// our bit packing to a real file WITHOUT any block-registry mapping in
// between: take vanilla's own long[], decode it at the width its palette size
// implies, re-encode those same indices with our packer, and require the
// bytes back. A self-consistent pack/unpack pair would happily agree with
// itself while both being wrong; this cannot.
static void GateD_PaletteGroundTruth(const std::vector<fs::path>& regions) {
    Section("Gate D - palette packing vs vanilla's own long arrays");

    size_t blockContainers = 0, biomeContainers = 0, singleValue = 0;
    size_t maxBlockPalette = 0, maxBiomePalette = 0;
    std::map<int, size_t> bitsHistogram;

    auto checkContainer = [&](const World::NBTTagCompound& c, size_t entryCount,
                              bool blockTiers, const std::string& where,
                              size_t& counter, size_t& maxPalette) {
        auto pal = std::dynamic_pointer_cast<World::NBTTagList>(c.GetTag("palette"));
        if (!pal) { Check(false, where + ": no palette"); return; }
        const size_t n = pal->value.size();
        maxPalette = std::max(maxPalette, n);
        ++counter;

        auto dataTag = std::dynamic_pointer_cast<World::NBTTagLongArray>(c.GetTag("data"));
        const int bits = Anvil::DiskBitsFor(n, blockTiers);
        bitsHistogram[bits]++;

        if (!dataTag) {
            // No data key: vanilla only omits it for a single-entry palette.
            Check(n == 1, where + ": data key absent but palette holds " + std::to_string(n));
            Check(bits == 0, where + ": we would have written " + std::to_string(bits) + " bits for a 1-entry palette");
            ++singleValue;
            return;
        }

        Check(bits > 0, where + ": data key present but we computed 0 bits");
        if (bits <= 0) return;

        // The word count our rule predicts must match what vanilla actually
        // wrote. This is the check that catches an off-by-one in the
        // non-straddling layout (37 longs for a heightmap, not 36).
        const size_t expect = Anvil::DiskWordCount(entryCount, bits);
        Check(dataTag->value.size() == expect,
              where + ": vanilla wrote " + std::to_string(dataTag->value.size()) +
              " longs, our rule says " + std::to_string(expect) +
              " (palette " + std::to_string(n) + ", " + std::to_string(bits) + " bits)");
        if (dataTag->value.size() != expect) return;

        std::vector<uint64_t> vanilla(dataTag->value.size());
        std::memcpy(vanilla.data(), dataTag->value.data(), vanilla.size() * sizeof(uint64_t));

        std::vector<uint32_t> indices;
        Check(Anvil::UnpackIndices(vanilla, entryCount, bits, indices), where + ": unpack refused");
        if (indices.size() != entryCount) return;

        // Every index must address a real palette entry, or vanilla would be
        // writing something it could not read back.
        size_t bad = 0;
        for (uint32_t i : indices) if (i >= n) ++bad;
        Check(bad == 0, where + ": " + std::to_string(bad) + " indices past the palette");

        std::vector<uint64_t> repacked;
        Anvil::PackIndices(indices, bits, repacked);
        Check(repacked == vanilla, where + ": repacked long array differs from vanilla's");
    };

    for (const auto& path : regions) {
        std::string err;
        auto region = Anvil::AnvilRegion::Open(path, false, err);
        if (!region) continue;

        for (int lz = 0; lz < 32; ++lz) {
            for (int lx = 0; lx < 32; ++lx) {
                if (!region->Has(lx, lz)) continue;
                std::vector<uint8_t> raw;
                if (!region->Read(lx, lz, raw, err)) continue;

                World::NBTTagPtr root;
                try { root = World::NBTParser::Parse(raw); } catch (...) { continue; }
                auto rootC = std::dynamic_pointer_cast<World::NBTTagCompound>(root);
                if (!rootC) continue;
                auto sections = std::dynamic_pointer_cast<World::NBTTagList>(rootC->GetTag("sections"));
                if (!sections) continue;

                for (const auto& sTag : sections->value) {
                    auto sec = std::dynamic_pointer_cast<World::NBTTagCompound>(sTag);
                    if (!sec) continue;
                    const std::string where = path.filename().string() + " " +
                                              std::to_string(lx) + "," + std::to_string(lz);
                    if (auto bs = std::dynamic_pointer_cast<World::NBTTagCompound>(sec->GetTag("block_states")))
                        checkContainer(*bs, 4096, /*blockTiers=*/true,  where + " block_states", blockContainers, maxBlockPalette);
                    if (auto bi = std::dynamic_pointer_cast<World::NBTTagCompound>(sec->GetTag("biomes")))
                        checkContainer(*bi, 64,  /*blockTiers=*/false, where + " biomes",       biomeContainers, maxBiomePalette);
                }
            }
        }
    }

    std::printf("  %zu block_states + %zu biomes containers (%zu single-value)\n",
                blockContainers, biomeContainers, singleValue);
    std::printf("  largest palette: blocks %zu, biomes %zu\n", maxBlockPalette, maxBiomePalette);
    std::printf("  widths seen:");
    for (const auto& [bits, n] : bitsHistogram) std::printf(" %db:%zu", bits, n);
    std::printf("\n");
    if (maxBlockPalette <= 256) {
        std::printf("  NOTE  no real section exceeded 256 entries, so the >8-bit\n"
                    "        local-palette path is covered only by Gate E below.\n");
    }
    Check(blockContainers > 0, "no block_states containers found");
}

// ── Gate E: the >256-entry palette the in-memory container cannot express ──
static void GateE_WidePalette() {
    Section("Gate E - palettes past 256 entries (9+ bit local indices)");

    // Every palette size that crosses a width boundary, plus the wide ones no
    // natural terrain produces.
    for (size_t n : {size_t(1), size_t(2), size_t(3), size_t(4), size_t(5), size_t(16),
                     size_t(17), size_t(32), size_t(33), size_t(64), size_t(65),
                     size_t(128), size_t(129), size_t(256), size_t(257), size_t(300),
                     size_t(512), size_t(513), size_t(1024), size_t(4096)}) {
        const int bits = Anvil::DiskBitsFor(n, true);
        std::vector<uint32_t> indices(4096);
        for (size_t i = 0; i < indices.size(); ++i) indices[i] = static_cast<uint32_t>(i % n);

        std::vector<uint64_t> packed;
        Anvil::PackIndices(indices, bits, packed);
        Check(packed.size() == Anvil::DiskWordCount(4096, bits),
              "palette " + std::to_string(n) + ": wrong word count");

        std::vector<uint32_t> back;
        Check(Anvil::UnpackIndices(packed, 4096, bits, back),
              "palette " + std::to_string(n) + ": unpack refused");
        Check(back == indices, "palette " + std::to_string(n) + ": indices did not survive");

        // Padding bits must be zero for any width that does not divide 64 —
        // that is what "entries never straddle a long" means.
        if (bits > 0 && (64 % bits) != 0) {
            const int used = (64 / bits) * bits;
            const uint64_t padMask = ~uint64_t{0} << used;
            size_t dirty = 0;
            for (uint64_t w : packed) if (w & padMask) ++dirty;
            Check(dirty == 0, "palette " + std::to_string(n) + ": " +
                  std::to_string(dirty) + " longs have non-zero padding bits");
        }
    }

    // Container level, including the GLOBAL case no vanilla file can produce.
    //
    // The engine's PalettedContainer switches to a global palette above 8 bits,
    // where RawWords() holds VALUES rather than indices and Palette() is empty.
    // The disk form has no such mode, so PackForDisk must rebuild a local
    // palette from the contents. Nothing in a real world exercises this — the
    // largest palette observed across a million sections was 63 — so it is
    // checked here or not at all.
    for (size_t distinct : {size_t(1), size_t(2), size_t(5), size_t(17), size_t(100),
                            size_t(257), size_t(300), size_t(1000), size_t(4096)}) {
        Game::PalettedContainer c(Game::PaletteStrategy::ForBlockStates(15), 0);
        for (size_t i = 0; i < 4096; ++i) {
            c.Set(i, static_cast<uint32_t>((i * 7919u) % distinct));
        }

        const auto packed = Anvil::PackForDisk(c);
        Check(packed.palette.size() == distinct,
              "distinct " + std::to_string(distinct) + ": disk palette holds " +
              std::to_string(packed.palette.size()));
        Check(packed.bits == Anvil::DiskBitsFor(distinct, true),
              "distinct " + std::to_string(distinct) + ": wrong disk width");
        // Above 256 entries the in-memory container is global while the disk
        // form still carries a palette — the exact divergence PackForDisk exists for.
        if (distinct > 256) {
            Check(c.IsGlobalPalette(), "distinct " + std::to_string(distinct) +
                  ": expected the in-memory container to be global");
            Check(!packed.palette.empty(), "distinct " + std::to_string(distinct) +
                  ": disk form must still carry a local palette");
        }

        Game::PalettedContainer back;
        std::string err;
        Check(Anvil::UnpackFromDisk(packed.palette, packed.data, c.Strategy(), back, err),
              "distinct " + std::to_string(distinct) + ": unpack refused: " + err);

        size_t wrong = 0;
        for (size_t i = 0; i < 4096; ++i) if (back.Get(i) != c.Get(i)) ++wrong;
        Check(wrong == 0, "distinct " + std::to_string(distinct) + ": " +
              std::to_string(wrong) + " voxels changed through the disk round-trip");
    }

    // Same for biomes, whose tiers differ (1,2,3 then global).
    for (size_t distinct : {size_t(1), size_t(2), size_t(4), size_t(9), size_t(64)}) {
        Game::PalettedContainer c(Game::PaletteStrategy::ForBiomes(16), 0);
        for (size_t i = 0; i < 64; ++i) c.Set(i, static_cast<uint32_t>(i % distinct));
        const auto packed = Anvil::PackForDisk(c);
        Check(packed.bits == Anvil::DiskBitsFor(distinct, false),
              "biomes distinct " + std::to_string(distinct) + ": wrong width");
        Game::PalettedContainer back;
        std::string err;
        Check(Anvil::UnpackFromDisk(packed.palette, packed.data, c.Strategy(), back, err),
              "biomes distinct " + std::to_string(distinct) + ": " + err);
        size_t wrong = 0;
        for (size_t i = 0; i < 64; ++i) if (back.Get(i) != c.Get(i)) ++wrong;
        Check(wrong == 0, "biomes distinct " + std::to_string(distinct) + ": round-trip lost cells");
    }

    // The documented widths, spelled out so a change to DiskBitsFor has to be
    // deliberate.
    struct { size_t n; int blocks; int biomes; } table[] = {
        {1,0,0}, {2,4,1}, {3,4,2}, {4,4,2}, {5,4,3}, {8,4,3}, {9,4,4}, {16,4,4},
        {17,5,5}, {32,5,5}, {33,6,6}, {64,6,6}, {65,7,7}, {128,7,7},
        {129,8,8}, {256,8,8}, {257,9,9}, {512,9,9}, {513,10,10},
    };
    for (const auto& r : table) {
        Check(Anvil::DiskBitsFor(r.n, true)  == r.blocks,
              "block bits for palette " + std::to_string(r.n));
        Check(Anvil::DiskBitsFor(r.n, false) == r.biomes,
              "biome bits for palette " + std::to_string(r.n));
    }

    // Word counts named in the design, checked rather than trusted.
    struct { int bits; size_t words; } blockWords[] = {
        {4,256},{5,342},{6,410},{7,456},{8,512},{9,586},{10,683}
    };
    for (const auto& r : blockWords)
        Check(Anvil::DiskWordCount(4096, r.bits) == r.words,
              "4096 entries at " + std::to_string(r.bits) + " bits");
    struct { int bits; size_t words; } biomeWords[] = { {1,1},{2,2},{3,4},{4,4},{5,6},{6,7} };
    for (const auto& r : biomeWords)
        Check(Anvil::DiskWordCount(64, r.bits) == r.words,
              "64 entries at " + std::to_string(r.bits) + " bits");
}


// ── Gate F: block_entities and biome palettes, as vanilla actually writes them
//
// Structural, not semantic — the block and item registries are not linked in
// here. What it pins down is the set of assumptions BlockEntityNbt and
// ChunkSerializer are written against, each of which is a coordinate or type
// mistake waiting to happen:
//   * block-entity x/z are ABSOLUTE world coords, y is already absolute
//   * every entry carries an `id`
//   * the biome palette is a list of plain strings, not compounds
//   * container Items entries carry a `Slot` byte and an `id`
static void GateF_ContentShapes(const std::vector<fs::path>& regions) {
    Section("Gate F - block entity / biome shapes in vanilla files");

    size_t blockEntities = 0, withItems = 0, biomePalettes = 0;
    std::map<std::string, size_t> byId;

    for (const auto& path : regions) {
        if (path.parent_path().filename() != "region") continue;   // not entities/
        std::string err;
        auto region = Anvil::AnvilRegion::Open(path, false, err);
        if (!region) continue;

        for (int lz = 0; lz < 32; ++lz) {
            for (int lx = 0; lx < 32; ++lx) {
                if (!region->Has(lx, lz)) continue;
                std::vector<uint8_t> raw;
                if (!region->Read(lx, lz, raw, err)) continue;
                World::NBTTagPtr root;
                try { root = World::NBTParser::Parse(raw); } catch (...) { continue; }
                auto rootC = std::dynamic_pointer_cast<World::NBTTagCompound>(root);
                if (!rootC) continue;

                const int chunkX = rootC->GetValue<int32_t>("xPos", 0);
                const int chunkZ = rootC->GetValue<int32_t>("zPos", 0);

                if (auto list = std::dynamic_pointer_cast<World::NBTTagList>(rootC->GetTag("block_entities"))) {
                    for (const auto& e : list->value) {
                        auto be = std::dynamic_pointer_cast<World::NBTTagCompound>(e);
                        if (!be) { Check(false, "block_entities element is not a compound"); continue; }
                        ++blockEntities;

                        const std::string id = be->GetValue<std::string>("id");
                        Check(!id.empty(), "block entity with no id");
                        byId[id]++;

                        Check(be->HasTag("x") && be->HasTag("y") && be->HasTag("z"),
                              id + ": missing a coordinate");
                        const int bx = be->GetValue<int32_t>("x", INT32_MIN);
                        const int bz = be->GetValue<int32_t>("z", INT32_MIN);
                        // The assumption BlockEntityNbt is built on: absolute
                        // world coordinates, so writing them needs the chunk
                        // origin added and reading them needs & 15.
                        Check((bx >> 4) == chunkX && (bz >> 4) == chunkZ,
                              id + ": x/z are not absolute world coords inside their chunk");

                        if (auto items = std::dynamic_pointer_cast<World::NBTTagList>(be->GetTag("Items"))) {
                            ++withItems;
                            for (const auto& ie : items->value) {
                                auto item = std::dynamic_pointer_cast<World::NBTTagCompound>(ie);
                                if (!item) { Check(false, id + ": Items element is not a compound"); continue; }
                                Check(item->HasTag("Slot"), id + ": item with no Slot");
                                Check(!item->GetValue<std::string>("id").empty(), id + ": item with no id");
                                // count is TAG_Int in the 1.20.5+ form.
                                if (item->HasTag("count")) {
                                    Check(std::dynamic_pointer_cast<World::NBTTagInt>(item->GetTag("count")) != nullptr,
                                          id + ": item count is not TAG_Int");
                                }
                            }
                        }
                    }
                }

                if (auto sections = std::dynamic_pointer_cast<World::NBTTagList>(rootC->GetTag("sections"))) {
                    for (const auto& st : sections->value) {
                        auto sec = std::dynamic_pointer_cast<World::NBTTagCompound>(st);
                        if (!sec) continue;
                        auto biomes = std::dynamic_pointer_cast<World::NBTTagCompound>(sec->GetTag("biomes"));
                        if (!biomes) continue;
                        auto pal = std::dynamic_pointer_cast<World::NBTTagList>(biomes->GetTag("palette"));
                        if (!pal) { Check(false, "biomes with no palette"); continue; }
                        ++biomePalettes;
                        for (const auto& e : pal->value) {
                            // Plain strings, NOT {Name, Properties} compounds
                            // the way block states are.
                            Check(std::dynamic_pointer_cast<World::NBTTagString>(e) != nullptr,
                                  "biome palette entry is not a plain string");
                        }
                    }
                }
            }
        }
    }

    std::printf("  %zu block entities (%zu with Items), %zu biome palettes\n",
                blockEntities, withItems, biomePalettes);
    if (!byId.empty()) {
        std::printf("  types:");
        size_t shown = 0;
        for (const auto& [id, n] : byId) {
            if (shown++ >= 8) { std::printf(" ..."); break; }
            std::printf(" %s:%zu", id.c_str(), n);
        }
        std::printf("\n");
    }
}


// ── Gate G: entities/*.mca, as vanilla actually writes it ──────────────────
//
// Structural, and aimed squarely at the assumptions an entity codec is built
// on: the root shape, Position being an int ARRAY of two rather than a
// compound, and which keys every entity really carries. Reports the key sets
// per type so a codec can be written against observed reality instead of a
// guess about what a pig stores.
static void GateG_EntityRegions(const fs::path& world) {
    Section("Gate G - entities/*.mca shape");

    const fs::path dir = world / "entities";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::printf("  (no entities/ folder in this world)\n");
        return;
    }

    size_t chunks = 0, entities = 0, emptyChunks = 0;
    std::map<std::string, size_t> byType;
    std::map<std::string, size_t> keyFrequency;

    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".mca") continue;
        std::string err;
        auto region = Anvil::AnvilRegion::Open(e.path(), false, err);
        if (!region) continue;

        for (int lz = 0; lz < 32; ++lz) {
            for (int lx = 0; lx < 32; ++lx) {
                if (!region->Has(lx, lz)) continue;
                std::vector<uint8_t> raw;
                if (!region->Read(lx, lz, raw, err)) continue;

                World::NBTTagPtr root;
                try { root = World::NBTParser::Parse(raw); } catch (...) { continue; }
                auto rootC = std::dynamic_pointer_cast<World::NBTTagCompound>(root);
                if (!rootC) { Check(false, "entity chunk root is not a compound"); continue; }
                ++chunks;

                // Exactly three keys, and Position is an INT ARRAY of two.
                Check(rootC->HasTag("DataVersion"), "entity chunk without DataVersion");
                auto position = std::dynamic_pointer_cast<World::NBTTagIntArray>(rootC->GetTag("Position"));
                Check(position != nullptr, "Position is not a TAG_Int_Array");
                if (position) {
                    Check(position->value.size() == 2,
                          "Position holds " + std::to_string(position->value.size()) + " ints, expected 2");
                }

                auto list = std::dynamic_pointer_cast<World::NBTTagList>(rootC->GetTag("Entities"));
                if (!list || list->value.empty()) {
                    // Vanilla DELETES an emptied entity chunk rather than
                    // writing an empty list, so finding one at all is worth
                    // noting.
                    ++emptyChunks;
                    continue;
                }

                for (const auto& element : list->value) {
                    auto entity = std::dynamic_pointer_cast<World::NBTTagCompound>(element);
                    if (!entity) { Check(false, "Entities element is not a compound"); continue; }
                    ++entities;

                    const std::string id = entity->GetValue<std::string>("id");
                    Check(!id.empty(), "entity with no id");
                    byType[id]++;
                    for (const auto& [key, _] : entity->value) keyFrequency[key]++;

                    auto pos = std::dynamic_pointer_cast<World::NBTTagList>(entity->GetTag("Pos"));
                    Check(pos && pos->value.size() == 3, id + ": Pos is not 3 doubles");
                    auto motion = std::dynamic_pointer_cast<World::NBTTagList>(entity->GetTag("Motion"));
                    Check(motion && motion->value.size() == 3, id + ": Motion is not 3 doubles");
                    auto rot = std::dynamic_pointer_cast<World::NBTTagList>(entity->GetTag("Rotation"));
                    Check(rot && rot->value.size() == 2, id + ": Rotation is not 2 floats");
                    auto uuid = std::dynamic_pointer_cast<World::NBTTagIntArray>(entity->GetTag("UUID"));
                    Check(uuid && uuid->value.size() == 4, id + ": UUID is not an int array of 4");

                    // A rider is nested, never stored as a root entity.
                    if (auto riders = std::dynamic_pointer_cast<World::NBTTagList>(entity->GetTag("Passengers"))) {
                        Check(!riders->value.empty(), id + ": empty Passengers list");
                    }
                }
            }
        }
    }

    std::printf("  %zu entity chunks, %zu entities (%zu chunks with an empty list)\n",
                chunks, entities, emptyChunks);
    if (!byType.empty()) {
        std::printf("  types:");
        size_t shown = 0;
        for (const auto& [id, n] : byType) {
            if (shown++ >= 10) { std::printf(" ... (%zu total)", byType.size()); break; }
            std::printf(" %s:%zu", id.c_str(), n);
        }
        std::printf("\n");
    }
    if (entities > 0) {
        // Keys present on EVERY entity are the ones a base codec must always
        // write; the rest are per-type or conditional.
        std::printf("  keys on every entity:");
        for (const auto& [key, n] : keyFrequency) {
            if (n == entities) std::printf(" %s", key.c_str());
        }
        std::printf("\n");
    }
}

// ── Writer unit checks that a real file cannot cover ────────────────────────
static void WriterUnits() {
    Section("Writer specifics");

    // An empty list declares element type TAG_End - vanilla's
    // ListTag.identifyRawElementType returns 0 when the list is empty.
    {
        Nbt::Writer w;
        w.BeginRootCompound();
        auto s = w.BeginList("block_entities", Nbt::TagType::Compound);
        w.EndList(s);
        w.EndRootCompound();
        const auto& b = w.Bytes();
        // 0A 0000 | 09 000E "block_entities" | elemType | count(4) | 00
        const size_t elemAt = 3 + 1 + 2 + 14;
        Check(b[elemAt] == 0x00, "empty list must declare element type TAG_End");
        Check(b[elemAt + 1] == 0 && b[elemAt + 4] == 0, "empty list count must be 0");
    }

    // A non-empty list keeps its declared type.
    {
        Nbt::Writer w;
        w.BeginRootCompound();
        auto s = w.BeginList("palette", Nbt::TagType::String);
        w.ListString(s, "minecraft:stone");
        w.EndList(s);
        w.EndRootCompound();
        const size_t elemAt = 3 + 1 + 2 + 7;
        Check(w.Bytes()[elemAt] == 0x08, "non-empty string list must declare TAG_String");
    }

    // Modified UTF-8: an emoji becomes a six-byte surrogate pair, and a
    // round-trip through the decoder returns the original UTF-8.
    {
        const std::string emoji = "pick\xF0\x9F\x98\x80";      // U+1F600
        std::vector<uint8_t> enc;
        Check(Nbt::EncodeModifiedUtf8(emoji, enc), "encode emoji");
        Check(enc.size() == 4 + 6, "supplementary scalar must encode as a 6-byte surrogate pair");
        std::string back;
        Check(Nbt::DecodeModifiedUtf8(enc.data(), enc.size(), back), "decode emoji");
        Check(back == emoji, "emoji did not survive the round-trip");
    }
    {
        const std::string withNul("a\0b", 3);
        std::vector<uint8_t> enc;
        Check(Nbt::EncodeModifiedUtf8(withNul, enc), "encode NUL");
        Check(enc.size() == 4 && enc[1] == 0xC0 && enc[2] == 0x80, "U+0000 must encode as C0 80");
        std::string back;
        Check(Nbt::DecodeModifiedUtf8(enc.data(), enc.size(), back), "decode NUL");
        Check(back == withNul, "NUL did not survive the round-trip");
    }

    // Determinism: identical calls produce identical bytes. Every later
    // "re-save and diff" gate depends on this.
    {
        auto build = [] {
            Nbt::Writer w;
            w.BeginRootCompound();
            w.Int("DataVersion", 4764);
            w.String("Status", "minecraft:full");
            const uint64_t words[] = {0x8000000000000001ull, 0x0123456789ABCDEFull};
            w.LongArray("data", words, 2);
            w.EndRootCompound();
            return w.Bytes();
        };
        Check(build() == build(), "writer is not deterministic");
    }

    // The top bit of a packed long must survive as a raw bit pattern: widths
    // that divide 64 leave no padding, so bit 63 is real data.
    {
        Nbt::Writer w;
        w.BeginRootCompound();
        const uint64_t words[] = {0xFFFFFFFFFFFFFFFFull};
        w.LongArray("data", words, 1);
        w.EndRootCompound();
        auto parsed = std::dynamic_pointer_cast<World::NBTTagCompound>(World::NBTParser::Parse(w.Bytes()));
        auto arr = std::dynamic_pointer_cast<World::NBTTagLongArray>(parsed->GetTag("data"));
        Check(arr && arr->value.size() == 1 && arr->value[0] == -1, "high bit of a packed long was not preserved");
    }

    // A refusal must latch, not corrupt.
    {
        Nbt::Writer w;
        w.BeginRootCompound();
        w.EndCompound();                       // unbalanced on purpose
        Check(!w.ok(), "writer should have refused an unbalanced EndCompound");
    }
}

// ── Gate H: the nested shapes the entity codec added ──────────────────────
//
// Every structure below is one the writer had never been asked for before the
// entity work: a LIST OF COMPOUNDS opened while already inside a list
// compound (Passengers), that same shape nested two deep (a jockey riding a
// jockey), a compound opened inside a list compound and recursing
// (hidden_effect), and a list of compounds beside a scalar (attributes).
//
// This is a writer test, not a vanilla-file test: no world on disk is
// guaranteed to contain a stacked jockey or a suspended potion effect, so the
// only way to know the emitted bytes parse is to emit them here. Getting the
// nesting wrong does not throw — it produces a compound whose remaining keys
// are read as garbage, which is exactly the failure a save format must not
// have.
static void GateH_NestedShapes() {
    Section("Gate H - nested writer shapes (Passengers / effects / attributes)");

    Nbt::Writer w;
    w.BeginRootCompound();
    auto entities = w.BeginList("Entities", Nbt::TagType::Compound);

    // A chicken carrying a zombie that is itself carrying a baby zombie, with
    // effects and attributes at every level, so a depth mistake anywhere
    // shows up as a parse difference rather than as silence.
    auto writeEffects = [&](int amplifier) {
        auto fx = w.BeginList("active_effects", Nbt::TagType::Compound);
        w.ListCompoundBegin(fx);
        w.String("id", "minecraft:speed");
        w.Byte  ("amplifier", static_cast<int8_t>(amplifier));
        w.Int   ("duration", 1200);
        w.Bool  ("ambient", false);
        w.Bool  ("show_particles", true);
        w.Bool  ("show_icon", true);
        w.BeginCompound("hidden_effect");           // compound inside a list compound
        w.String("id", "minecraft:speed");
        w.Byte  ("amplifier", 0);
        w.Int   ("duration", 40);
        w.EndCompound();
        w.ListCompoundEnd(fx);
        w.EndList(fx);
    };
    auto writeAttrs = [&](double maxHealth) {
        auto at = w.BeginList("attributes", Nbt::TagType::Compound);
        w.ListCompoundBegin(at);
        w.String("id", "minecraft:max_health");
        w.Double("base", maxHealth);
        w.ListCompoundEnd(at);
        w.EndList(at);
    };

    w.ListCompoundBegin(entities);
    w.String("id", "minecraft:chicken");
    w.Bool("IsChickenJockey", true);
    writeAttrs(4.0);
    writeEffects(1);
    {
        auto riders = w.BeginList("Passengers", Nbt::TagType::Compound);
        w.ListCompoundBegin(riders);
        w.String("id", "minecraft:zombie");
        w.Bool("IsBaby", true);
        writeAttrs(20.0);
        writeEffects(0);
        {
            auto riders2 = w.BeginList("Passengers", Nbt::TagType::Compound);
            w.ListCompoundBegin(riders2);
            w.String("id", "minecraft:silverfish");
            w.Int("obey_depth_marker", 3);
            w.ListCompoundEnd(riders2);
            w.EndList(riders2);
        }
        // A key written AFTER the nested list: if the list left the cursor at
        // the wrong depth this lands inside the rider instead of beside it.
        w.Long("anger_end_time", -1);
        w.ListCompoundEnd(riders);
        w.EndList(riders);
    }
    w.Int("PortalCooldown", 17);      // and again after the outer list
    w.ListCompoundEnd(entities);

    w.EndList(entities);
    w.EndRootCompound();
    Check(w.ok(), "writer latched a refusal while emitting nested shapes");

    World::NBTTagPtr root;
    try {
        root = World::NBTParser::Parse(w.Bytes());
    } catch (const std::exception& e) {
        Check(false, std::string("nested shapes failed to parse: ") + e.what());
        return;
    }
    auto rootC = std::dynamic_pointer_cast<World::NBTTagCompound>(root);
    if (!rootC) { Check(false, "nested-shape root is not a compound"); return; }

    auto list = std::dynamic_pointer_cast<World::NBTTagList>(rootC->GetTag("Entities"));
    if (!list || list->value.size() != 1) {
        Check(false, "Entities did not come back as a 1-element list");
        return;
    }
    auto chicken = std::dynamic_pointer_cast<World::NBTTagCompound>(list->value[0]);
    if (!chicken) { Check(false, "Entities[0] is not a compound"); return; }

    Check(chicken->GetValue<std::string>("id", "") == "minecraft:chicken",
          "vehicle id did not survive");
    // The key written after the nested Passengers list must be on the CHICKEN.
    Check(chicken->GetValue<int32_t>("PortalCooldown", -1) == 17,
          "a key after a nested list landed at the wrong depth");

    auto attrs = std::dynamic_pointer_cast<World::NBTTagList>(chicken->GetTag("attributes"));
    Check(attrs && attrs->value.size() == 1, "chicken attributes list is wrong");

    auto riders = std::dynamic_pointer_cast<World::NBTTagList>(chicken->GetTag("Passengers"));
    if (!riders || riders->value.size() != 1) {
        Check(false, "Passengers did not come back as a 1-element list");
        return;
    }
    auto zombie = std::dynamic_pointer_cast<World::NBTTagCompound>(riders->value[0]);
    if (!zombie) { Check(false, "Passengers[0] is not a compound"); return; }
    Check(zombie->GetValue<std::string>("id", "") == "minecraft:zombie",
          "rider id did not survive");
    Check(zombie->GetValue<int8_t>("IsBaby", 0) != 0, "rider per-type key did not survive");
    Check(zombie->GetValue<int64_t>("anger_end_time", 0) == -1,
          "a key after the INNER nested list landed at the wrong depth");

    auto fx = std::dynamic_pointer_cast<World::NBTTagList>(zombie->GetTag("active_effects"));
    if (!fx || fx->value.size() != 1) {
        Check(false, "rider active_effects is wrong");
    } else {
        auto inst = std::dynamic_pointer_cast<World::NBTTagCompound>(fx->value[0]);
        Check(inst != nullptr, "active_effects[0] is not a compound");
        if (inst) {
            Check(inst->GetValue<int32_t>("duration", 0) == 1200, "effect duration lost");
            auto hidden = std::dynamic_pointer_cast<World::NBTTagCompound>(inst->GetTag("hidden_effect"));
            Check(hidden != nullptr, "hidden_effect did not survive");
            if (hidden) {
                Check(hidden->GetValue<int32_t>("duration", 0) == 40,
                      "hidden_effect duration lost");
            }
        }
    }

    auto riders2 = std::dynamic_pointer_cast<World::NBTTagList>(zombie->GetTag("Passengers"));
    if (!riders2 || riders2->value.size() != 1) {
        Check(false, "second-level Passengers did not survive");
    } else {
        auto sf = std::dynamic_pointer_cast<World::NBTTagCompound>(riders2->value[0]);
        Check(sf && sf->GetValue<int32_t>("obey_depth_marker", 0) == 3,
              "second-level rider payload lost");
    }
}

int main(int argc, char** argv) {
    fs::path world = (argc > 1) ? fs::path(argv[1]) : FindDefaultWorld();
    if (world.empty() || !fs::is_directory(world)) {
        std::printf("usage: %s <world folder>\n(no vanilla world found automatically)\n", argv[0]);
        return 2;
    }
    const auto regions = RegionFiles(world);
    std::printf("world:   %s\nregions: %zu\n", world.string().c_str(), regions.size());

    const fs::path tmpDir = fs::temp_directory_path() / "anvil_parity";
    std::error_code ec;
    fs::remove_all(tmpDir, ec);
    fs::create_directories(tmpDir, ec);

    WriterUnits();
    GateH_NestedShapes();
    GateA_RoundTrip(regions);
    GateD_PaletteGroundTruth(regions);
    GateF_ContentShapes(regions);
    GateG_EntityRegions(world);
    GateE_WidePalette();
    GateB_LargeRegion(tmpDir);
    GateB2_External(tmpDir);
    GateB3_Rewrite(tmpDir);
    GateC_Truncation(regions, tmpDir);

    fs::remove_all(tmpDir, ec);

    std::printf("\n%s  %d checks, %d failures\n",
                g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
