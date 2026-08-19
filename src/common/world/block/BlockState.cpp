// File: src/common/world/block/BlockState.cpp
#include "BlockState.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace Game {

    namespace {

        constexpr size_t kBlockCount = static_cast<size_t>(BlockID::Count);

        // One property of one block: which property it is, and the two numbers
        // that turn a state id into that property's digit.
        //
        // `stride` is the odometer place value. MC never materialises these
        // because it does not index states arithmetically; here they ARE the
        // whole mechanism, and they are derived rather than generated so the
        // invariant `stride[last] == 1` holds by construction.
        struct Slot {
            PropertyId prop;
            uint32_t   stride;
            uint16_t   radix;
        };

        struct Tables {
            std::array<uint32_t, kBlockCount> base{};      // first global id
            std::array<uint32_t, kBlockCount> count{};     // states, >= 1
            std::array<uint32_t, kBlockCount> deflt{};     // global id of the default
            std::array<uint32_t, kBlockCount> slotBegin{}; // window into `slots`
            std::array<uint16_t, kBlockCount> slotCount{};
            std::vector<Slot>     slots;
            std::vector<uint16_t> blockOfState;            // reverse map
            std::unordered_map<std::string_view, BlockID> bySlug;
            uint32_t total = 0;
            bool built = false;
        };

        Tables& T() {
            static Tables t;
            return t;
        }

        // Registry slug per BlockID, straight from column 2 of BlockDefs.inc —
        // the same string the generated table is keyed on.
        const std::array<const char*, kBlockCount>& Slugs() {
            static const std::array<const char*, kBlockCount> s = [] {
                std::array<const char*, kBlockCount> a{};
                a.fill("");
                size_t i = 1;   // 0 is Air, which BlockDefs.inc does not list
                #define BLOCK_DEF(e, m, d, r) a[static_cast<size_t>(BlockID::e)] = m; (void)i;
                #include "BlockDefs.inc"
                #undef BLOCK_DEF
                a[static_cast<size_t>(BlockID::Air)] = "air";
                return a;
            }();
            return s;
        }

    } // namespace

    void BlockStates::Init() {
        Tables& t = T();
        if (t.built) return;

        // Generated rows are keyed on slug; index them once.
        std::unordered_map<std::string_view, const GeneratedBlockStateRow*> rowBySlug;
        rowBySlug.reserve(kBlockStateRowCount);
        for (size_t i = 0; i < kBlockStateRowCount; ++i) {
            rowBySlug.emplace(kBlockStates[i].slug, &kBlockStates[i]);
        }

        const auto& slugs = Slugs();
        uint32_t next = 0;
        for (size_t b = 0; b < kBlockCount; ++b) {
            t.base[b] = next;
            t.slotBegin[b] = static_cast<uint32_t>(t.slots.size());

            auto it = slugs[b][0] ? rowBySlug.find(slugs[b]) : rowBySlug.end();
            if (it == rowBySlug.end()) {
                // No properties: exactly one state, and it is the default.
                t.count[b] = 1;
                t.slotCount[b] = 0;
                t.deflt[b] = next;
            } else {
                const GeneratedBlockStateRow& row = *it->second;
                t.count[b] = row.stateCount;
                t.slotCount[b] = row.propCount;
                t.deflt[b] = next + row.defaultIndex;

                // Strides run right-to-left: MC's cartesian product varies the
                // alphabetically-LAST property fastest, so the last slot has
                // stride 1 and each earlier one multiplies by the radix of the
                // slot after it.
                uint32_t stride = 1;
                for (int p = row.propCount - 1; p >= 0; --p) {
                    const PropertyId prop =
                        static_cast<PropertyId>(kBlockPropertyRefs[row.propBegin + p]);
                    const uint16_t radix = kProperties[static_cast<size_t>(prop)].valueCount;
                    t.slots.push_back(Slot{prop, stride, radix});
                    stride *= radix;
                }
                // Pushed back-to-front; put them in declared order so slot i is
                // property i of the block.
                std::reverse(t.slots.begin() + t.slotBegin[b], t.slots.end());
            }

            if (slugs[b][0]) t.bySlug.emplace(slugs[b], static_cast<BlockID>(b));
            next += t.count[b];
        }

        t.total = next;

        // The generated constant and the table built from it must agree. They
        // are derived from different things — the constant counts BlockDefs.inc
        // rows plus the synthetic enumerators in Blocks.hpp, this loop walks
        // the actual BlockID enum — so a block added to one and not the other
        // shows up here rather than as a palette sized for the wrong world.
        // The palette width is a static_assert over the constant, so a silent
        // disagreement means voxels quietly losing their high bits.
        if (t.total != kBlockStateCount) {
            std::fprintf(stderr,
                         "[BlockStates] FATAL: generated kBlockStateCount is %u but the "
                         "registry built %u states. BlockDefs.inc or Blocks.hpp changed "
                         "without re-running tools/gen_block_states.py.\n",
                         kBlockStateCount, t.total);
            std::abort();
        }

        t.blockOfState.resize(t.total);
        for (size_t b = 0; b < kBlockCount; ++b) {
            for (uint32_t s = 0; s < t.count[b]; ++s) {
                t.blockOfState[t.base[b] + s] = static_cast<uint16_t>(b);
            }
        }
        t.built = true;
    }

    BlockID BlockState::Block() const {
        const Tables& t = T();
        if (m_id >= t.total) return BlockID::Air;
        return static_cast<BlockID>(t.blockOfState[m_id]);
    }

    namespace {
        // The slot describing `prop` on this state's block, or null.
        //
        // A linear scan over at most seven uint16 comparisons — the same shape
        // as MC's identity scan over a Reference2ObjectArrayMap, and the reason
        // neither needs a hash. Most blocks have one or two.
        const Slot* FindSlot(uint32_t id, PropertyId prop, uint32_t& outBase) {
            const Tables& t = T();
            if (id >= t.total) return nullptr;
            const size_t b = t.blockOfState[id];
            outBase = t.base[b];
            const Slot* first = t.slots.data() + t.slotBegin[b];
            for (uint16_t i = 0; i < t.slotCount[b]; ++i) {
                if (first[i].prop == prop) return first + i;
            }
            return nullptr;
        }
    } // namespace

    bool BlockState::HasProperty(PropertyId prop) const {
        uint32_t base = 0;
        return FindSlot(m_id, prop, base) != nullptr;
    }

    int BlockState::GetIndex(PropertyId prop) const {
        uint32_t base = 0;
        const Slot* s = FindSlot(m_id, prop, base);
        if (!s) return -1;
        return static_cast<int>((m_id - base) / s->stride % s->radix);
    }

    std::string_view BlockState::GetName(PropertyId prop) const {
        const int v = GetIndex(prop);
        if (v < 0) return {};
        return BlockStates::PropertyValueName(prop, static_cast<uint16_t>(v));
    }

    std::string_view BlockState::GetValueByName(std::string_view propName) const {
        const BlockID b = Block();
        const uint16_t n = BlockStates::PropertyCount(b);
        for (uint16_t slot = 0; slot < n; ++slot) {
            const PropertyId p = BlockStates::PropertyAt(b, slot);
            if (BlockStates::PropertyName(p) == propName) return GetName(p);
        }
        return {};
    }

    BlockState BlockState::SetIndex(PropertyId prop, int valueIndex) const {
        uint32_t base = 0;
        const Slot* s = FindSlot(m_id, prop, base);
        if (!s) return *this;                              // MC trySetValue
        if (valueIndex < 0 || valueIndex >= s->radix) return *this;
        const int cur = static_cast<int>((m_id - base) / s->stride % s->radix);
        return FromRawId(static_cast<uint32_t>(
            static_cast<int64_t>(m_id) +
            static_cast<int64_t>(valueIndex - cur) * s->stride));
    }

    BlockState BlockState::SetName(PropertyId prop, std::string_view value) const {
        const auto& p = kProperties[static_cast<size_t>(prop)];
        for (uint16_t v = 0; v < p.valueCount; ++v) {
            if (kPropertyValues[p.valueBegin + v] == value) {
                return SetIndex(prop, static_cast<int>(v));
            }
        }
        return *this;
    }

    // ── BlockStates namespace ───────────────────────────────────────────────

    BlockState BlockStates::Default(BlockID id) {
        const Tables& t = T();
        const size_t b = static_cast<size_t>(id);
        if (b >= kBlockCount) return BlockState{};
        return BlockState::FromRawId(t.deflt[b]);
    }

    uint32_t BlockStates::Count(BlockID id) {
        const size_t b = static_cast<size_t>(id);
        return b < kBlockCount ? T().count[b] : 0;
    }

    uint32_t BlockStates::Base(BlockID id) {
        const size_t b = static_cast<size_t>(id);
        return b < kBlockCount ? T().base[b] : 0;
    }

    uint32_t BlockStates::Total() { return T().total; }

    BlockStateIndex BlockState::Index() const {
        return static_cast<BlockStateIndex>(m_id - BlockStates::Base(Block()));
    }

    BlockState BlockStates::FromIndex(BlockID id, BlockStateIndex stateIndex) {
        const uint32_t n = Count(id);
        if (n == 0) return BlockState{};                 // unregistered -> air
        const uint32_t st = (stateIndex < n) ? stateIndex : n - 1;
        return BlockState::FromRawId(Base(id) + st);
    }

    BlockState BlockStates::FromSlug(std::string_view slug) {
        const Tables& t = T();
        auto it = t.bySlug.find(slug);
        if (it == t.bySlug.end()) return BlockState{};     // MC: unknown name -> air
        return Default(it->second);
    }

    uint16_t BlockStates::PropertyCount(BlockID id) {
        const size_t b = static_cast<size_t>(id);
        return b < kBlockCount ? T().slotCount[b] : 0;
    }

    PropertyId BlockStates::PropertyAt(BlockID id, uint16_t slot) {
        const Tables& t = T();
        const size_t b = static_cast<size_t>(id);
        if (b >= kBlockCount || slot >= t.slotCount[b]) return PropertyId::Count;
        return t.slots[t.slotBegin[b] + slot].prop;
    }

    std::string_view BlockStates::PropertyName(PropertyId prop) {
        if (static_cast<size_t>(prop) >= kPropertyCount) return {};
        return kProperties[static_cast<size_t>(prop)].name;
    }

    uint16_t BlockStates::PropertyValueCount(PropertyId prop) {
        if (static_cast<size_t>(prop) >= kPropertyCount) return 0;
        return kProperties[static_cast<size_t>(prop)].valueCount;
    }

    std::string_view BlockStates::PropertyValueName(PropertyId prop, uint16_t valueIndex) {
        if (static_cast<size_t>(prop) >= kPropertyCount) return {};
        const auto& p = kProperties[static_cast<size_t>(prop)];
        if (valueIndex >= p.valueCount) return {};
        return kPropertyValues[p.valueBegin + valueIndex];
    }

} // namespace Game
