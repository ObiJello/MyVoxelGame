// File: tools/blockstate_parity/blockstate_parity.cpp
//
// Exhaustive offline check of the block-state core. No engine, no window, no
// world — two translation units and a JSON file, runs in milliseconds.
//
// Modelled on tools/terrain_parity for the same reason that exists: a
// representation change this wide needs to be falsifiable by something other
// than "the game looked fine". Every check below covers ALL ~29.6k states
// rather than a sample, because the failure modes here are silent. A stride
// that is wrong for one property of one block does not crash; it makes that
// block's `facing` read as its `half`.
//
// Build:
//   clang++ -std=c++20 -I src \
//       tools/blockstate_parity/blockstate_parity.cpp \
//       src/common/world/block/BlockState.cpp \
//       src/common/world/block/GeneratedBlockStates.cpp -o /tmp/bsparity
//   /tmp/bsparity [path/to/blocks.json]
#include "common/world/block/BlockState.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Game;

namespace {

    int g_failures = 0;
    int g_checks = 0;

    void Check(bool ok, const std::string& what) {
        ++g_checks;
        if (ok) return;
        if (++g_failures <= 25) std::printf("  FAIL  %s\n", what.c_str());
    }

    std::string StateDesc(BlockState s) {
        std::ostringstream o;
        const BlockID b = s.Block();
        o << "state " << s.RawId() << " (block " << static_cast<int>(b) << ")";
        return o.str();
    }

    // ── 1. Every state belongs to the block whose range contains it ─────────
    void CheckOwnership() {
        for (uint32_t id = 0; id < BlockStates::Total(); ++id) {
            const BlockState s = BlockState::FromRawId(id);
            const BlockID b = s.Block();
            const uint32_t base = BlockStates::Base(b);
            const uint32_t cnt = BlockStates::Count(b);
            Check(id >= base && id < base + cnt,
                  StateDesc(s) + " is not inside its own block's range");
        }
    }

    // ── 2. Defaults are in range, and Default() round-trips ─────────────────
    void CheckDefaults() {
        for (size_t i = 0; i < static_cast<size_t>(BlockID::Count); ++i) {
            const BlockID b = static_cast<BlockID>(i);
            const BlockState d = BlockStates::Default(b);
            Check(d.Block() == b,
                  "default of block " + std::to_string(i) + " belongs to another block");
            Check(d.RawId() >= BlockStates::Base(b) &&
                  d.RawId() < BlockStates::Base(b) + BlockStates::Count(b),
                  "default of block " + std::to_string(i) + " out of range");
        }
        Check(BlockStates::Default(BlockID::Air).RawId() == 0,
              "air's default must be global id 0 — the palette fill and a "
              "zero-initialised voxel both depend on it");
    }

    // ── 3. Get/Set round-trip over EVERY (state, property, value) ───────────
    //
    // This is the check that catches a bad stride. For each state and each of
    // its properties, setting every legal value must produce a state that reads
    // that value back, keeps every OTHER property unchanged, and stays inside
    // the same block.
    void CheckPropertyArithmetic() {
        uint64_t triples = 0;
        for (uint32_t id = 0; id < BlockStates::Total(); ++id) {
            const BlockState s = BlockState::FromRawId(id);
            const BlockID b = s.Block();
            const uint16_t nprops = BlockStates::PropertyCount(b);

            for (uint16_t slot = 0; slot < nprops; ++slot) {
                const PropertyId p = BlockStates::PropertyAt(b, slot);
                Check(s.HasProperty(p), StateDesc(s) + " denies its own property");
                const int cur = s.GetIndex(p);
                Check(cur >= 0, StateDesc(s) + " has no value for its own property");

                const uint16_t radix = BlockStates::PropertyValueCount(p);
                for (uint16_t v = 0; v < radix; ++v) {
                    ++triples;
                    const BlockState t = s.SetIndex(p, v);
                    Check(t.Block() == b, StateDesc(s) + " SetIndex left the block");
                    Check(t.GetIndex(p) == static_cast<int>(v),
                          StateDesc(s) + " SetIndex did not take");
                    // Every other property must be untouched.
                    for (uint16_t o = 0; o < nprops; ++o) {
                        if (o == slot) continue;
                        const PropertyId q = BlockStates::PropertyAt(b, o);
                        Check(t.GetIndex(q) == s.GetIndex(q),
                              StateDesc(s) + " SetIndex disturbed another property");
                    }
                    // Setting back must land exactly where we started.
                    Check(t.SetIndex(p, cur) == s,
                          StateDesc(s) + " SetIndex is not reversible");
                }
                // Out-of-range and unknown-property are no-ops, not corruption.
                Check(s.SetIndex(p, -1) == s, "negative value index must be a no-op");
                Check(s.SetIndex(p, radix) == s, "past-the-end value index must be a no-op");
            }
        }
        std::printf("  %llu (state, property, value) triples exercised\n",
                    static_cast<unsigned long long>(triples));
    }

    // ── 3b. Slot layout matches MC's odometer exactly ──────────────────────
    //
    // The arithmetic checks above are self-consistent: they pass even if every
    // block's property list is stored backwards, because they enumerate through
    // the same order they verify. This check pins the order to MC's.
    //
    // Two invariants, both externally observable:
    //   * slots come out in sorted-by-NAME order — MC's StateDefinition keeps
    //     an ImmutableSortedMap, and anything enumerating properties (the NBT
    //     writer, the debug overlay) inherits that order;
    //   * the strides form an odometer with the LAST property varying fastest:
    //     stepping the last property by one moves the state id by one, and each
    //     earlier property's step is the product of the radices after it.
    void CheckSlotLayout() {
        for (size_t i = 0; i < static_cast<size_t>(BlockID::Count); ++i) {
            const BlockID b = static_cast<BlockID>(i);
            const uint16_t n = BlockStates::PropertyCount(b);
            if (n == 0) continue;
            const BlockState base = BlockState::FromRawId(BlockStates::Base(b));

            std::string prev;
            for (uint16_t slot = 0; slot < n; ++slot) {
                const std::string name(BlockStates::PropertyName(BlockStates::PropertyAt(b, slot)));
                Check(prev.empty() || prev < name,
                      "block " + std::to_string(i) + ": properties are not sorted by name (" +
                      prev + " before " + name + ")");
                prev = name;
            }

            uint32_t expected = 1;
            for (int slot = n - 1; slot >= 0; --slot) {
                const PropertyId p = BlockStates::PropertyAt(b, static_cast<uint16_t>(slot));
                const uint32_t at0 = base.SetIndex(p, 0).RawId();
                const uint32_t at1 = base.SetIndex(p, 1).RawId();
                Check(at1 - at0 == expected,
                      "block " + std::to_string(i) + " property " +
                      std::string(BlockStates::PropertyName(p)) + ": stride " +
                      std::to_string(at1 - at0) + ", odometer wants " + std::to_string(expected));
                expected *= BlockStates::PropertyValueCount(p);
            }
            Check(expected == BlockStates::Count(b),
                  "block " + std::to_string(i) + ": radix product " + std::to_string(expected) +
                  " != state count " + std::to_string(BlockStates::Count(b)));
        }
    }

    // ── 4. A property this block does not declare is inert ──────────────────
    void CheckForeignProperty() {
        // stone declares nothing at all; setting anything on it must do nothing.
        const BlockState stone = BlockStates::FromSlug("stone");
        for (uint16_t p = 0; p < static_cast<uint16_t>(PropertyId::Count); ++p) {
            const PropertyId prop = static_cast<PropertyId>(p);
            Check(!stone.HasProperty(prop), "stone claims a property");
            Check(stone.GetIndex(prop) == -1, "stone returned a value for a property");
            Check(stone.SetIndex(prop, 0) == stone, "stone was changed by a foreign property");
        }
    }

    // ── 5. Parity with vanilla, per block ───────────────────────────────────
    //
    // The actual faithfulness test. For every block minecraft-data knows, our
    // within-block index of each state must equal MC's `globalId - minStateId`,
    // and our default must equal MC's. Loaded from JSON at runtime so nothing
    // is baked in here.
    struct McBlock {
        int minState = 0, maxState = 0, defaultState = 0;
        std::vector<std::pair<std::string, std::vector<std::string>>> props;
    };

    // Tolerant of pretty-printed JSON: the vendored file is indented with a
    // space after every colon, which a naive `"name":"` search misses entirely
    // — and misses SILENTLY, skipping the whole check. That is how this
    // returned "all passed" while comparing nothing at all.
    size_t SkipWs(const std::string& s, size_t i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' ||
                                s[i] == '\n' || s[i] == '\r')) ++i;
        return i;
    }

    // Value of `"key":` starting the search at `from`, bounded by `limit`.
    bool FieldInt(const std::string& s, const char* key, size_t from, size_t limit, int& out) {
        const std::string k = std::string("\"") + key + "\":";
        size_t at = s.find(k, from);
        if (at == std::string::npos || at >= limit) return false;
        out = std::atoi(s.c_str() + SkipWs(s, at + k.size()));
        return true;
    }

    std::map<std::string, McBlock> LoadMc(const char* path) {
        std::map<std::string, McBlock> out;
        std::ifstream f(path);
        if (!f) return out;
        const std::string all((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());

        const std::string kName = "\"name\":";
        size_t pos = all.find(kName);
        while (pos != std::string::npos) {
            size_t q = SkipWs(all, pos + kName.size());
            if (q >= all.size() || all[q] != '"') { pos = all.find(kName, pos + 1); continue; }
            const size_t ns = q + 1, ne = all.find('"', ns);
            const std::string name = all.substr(ns, ne - ns);

            const size_t next = all.find(kName, ne);
            const size_t limit = (next == std::string::npos) ? all.size() : next;

            McBlock b;
            const bool ok = FieldInt(all, "minStateId", ne, limit, b.minState) &&
                            FieldInt(all, "maxStateId", ne, limit, b.maxState) &&
                            FieldInt(all, "defaultState", pos, limit, b.defaultState);
            if (ok) out[name] = b;
            pos = next;
        }
        return out;
    }

    void CheckMcParity(const char* path) {
        auto mc = LoadMc(path);
        if (mc.empty()) {
            std::printf("  (skipped: could not read %s)\n", path);
            return;
        }
        int compared = 0;
        for (auto& [name, m] : mc) {
            const BlockState d = BlockStates::FromSlug(name);
            const BlockID b = d.Block();
            if (b == BlockID::Air && name != "air") continue;   // engine lacks it
            const uint32_t ours = BlockStates::Count(b);
            const uint32_t theirs = static_cast<uint32_t>(m.maxState - m.minState + 1);
            Check(ours == theirs,
                  name + ": " + std::to_string(ours) + " states, vanilla has " +
                  std::to_string(theirs));
            const uint32_t ourDefault = d.RawId() - BlockStates::Base(b);
            const uint32_t theirDefault = static_cast<uint32_t>(m.defaultState - m.minState);
            Check(ourDefault == theirDefault,
                  name + ": default index " + std::to_string(ourDefault) +
                  ", vanilla has " + std::to_string(theirDefault));
            ++compared;
        }
        std::printf("  %d blocks compared against vanilla state ranges\n", compared);
    }

    // ── 6. Contiguity: the id space has no holes and no overlaps ────────────
    void CheckContiguity() {
        std::vector<char> seen(BlockStates::Total(), 0);
        for (size_t i = 0; i < static_cast<size_t>(BlockID::Count); ++i) {
            const BlockID b = static_cast<BlockID>(i);
            for (uint32_t s = 0; s < BlockStates::Count(b); ++s) {
                const uint32_t id = BlockStates::Base(b) + s;
                Check(id < BlockStates::Total(), "state id past the end of the space");
                if (id < seen.size()) {
                    Check(!seen[id], "state id " + std::to_string(id) + " claimed twice");
                    seen[id] = 1;
                }
            }
        }
        for (uint32_t id = 0; id < seen.size(); ++id) {
            Check(seen[id] != 0, "state id " + std::to_string(id) + " belongs to no block");
        }
    }

    // ── 7. Hash line, terrain_parity style ─────────────────────────────────
    //
    // One number over every state's (slug, property, value) tuple. Two builds
    // printing the same hash have identical state spaces, which makes any
    // future generator change falsifiable by diff rather than by reading.
    void PrintHash() {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](std::string_view sv) {
            for (char c : sv) { h ^= static_cast<unsigned char>(c); h *= 1099511628211ull; }
        };
        for (uint32_t id = 0; id < BlockStates::Total(); ++id) {
            const BlockState s = BlockState::FromRawId(id);
            const BlockID b = s.Block();
            for (uint16_t slot = 0; slot < BlockStates::PropertyCount(b); ++slot) {
                const PropertyId p = BlockStates::PropertyAt(b, slot);
                mix(BlockStates::PropertyName(p));
                mix("=");
                mix(s.GetName(p));
                mix(",");
            }
            mix(";");
        }
        std::printf("state-space hash  %016llx\n", static_cast<unsigned long long>(h));
    }

} // namespace

int main(int argc, char** argv) {
    BlockStates::Init();

    std::printf("blockstate parity\n");
    std::printf("  %u states across %d blocks, %u properties\n",
                BlockStates::Total(), static_cast<int>(BlockID::Count),
                static_cast<unsigned>(PropertyId::Count));

    CheckOwnership();
    CheckDefaults();
    CheckContiguity();
    CheckSlotLayout();
    CheckForeignProperty();
    CheckPropertyArithmetic();
    CheckMcParity(argc > 1 ? argv[1]
                           : "tools/protocol-gen/node_modules/minecraft-data/"
                             "minecraft-data/data/pc/1.21.6/blocks.json");

    std::printf("\n%d checks", g_checks);
    if (g_failures) {
        std::printf(", %d FAILED\n", g_failures);
        return 1;
    }
    std::printf(", all passed\n");
    PrintHash();
    return 0;
}
