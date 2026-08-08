#pragma once
// Minimal gzipped-NBT structure-template loader + placement for FossilFeature.
// Reference: StructureTemplate.java / StructurePlaceSettings.java, specialized
// to what fossil templates contain (single palette, bone/coal-ore states, no
// block entities). Draw order is parity-critical:
//   getRandomPalette -> nextInt(paletteCount) (ALWAYS draws, even for 1),
//   then BlockRotProcessor -> one nextFloat per template block (integrity<1),
//   then ProtectedBlocksProcessor (no draw) drops #features_cannot_replace.

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <zlib.h>

#include "core/BlockPos.h"

namespace minecraft {
namespace levelgen {
namespace structure {

struct FossilTemplate {
    struct BlockEntry {
        int x, y, z;
        int stateIndex;
    };
    struct PaletteState {
        std::string name;
        std::map<std::string, std::string> properties;  // sorted (NBT order irrelevant)
    };
    int sizeX = 0, sizeY = 0, sizeZ = 0;
    std::vector<PaletteState> palette;
    std::vector<BlockEntry> blocks;  // NBT file order
    int paletteCount = 1;
};

namespace nbt_detail {

class Reader {
public:
    explicit Reader(std::vector<uint8_t> data) : m_data(std::move(data)) {}
    uint8_t u8() { return m_data.at(m_pos++); }
    int16_t i16() { int16_t v = (i32From(2)); return v; }
    int32_t i32() { return static_cast<int32_t>(i32From(4)); }
    int64_t i64() { int64_t v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | u8(); return v; }
    std::string str() {
        int len = static_cast<uint16_t>(i16());
        std::string s;
        for (int i = 0; i < len; ++i) s.push_back(static_cast<char>(u8()));
        return s;
    }
    void skip(size_t n) { m_pos += n; }

private:
    int64_t i32From(int n) {
        int64_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 8) | u8();
        // sign-extend
        int shift = 64 - n * 8;
        return (v << shift) >> shift;
    }
    std::vector<uint8_t> m_data;
    size_t m_pos = 0;
};

inline std::vector<uint8_t> gunzipFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("fossil template missing: " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string raw = ss.str();

    std::vector<uint8_t> out;
    z_stream zs{};
    if (inflateInit2(&zs, 15 + 32) != Z_OK)  // gzip or zlib
        throw std::runtime_error("inflateInit failed");
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(raw.data()));
    zs.avail_in = static_cast<uInt>(raw.size());
    std::vector<uint8_t> buf(1 << 16);
    int rc;
    do {
        zs.next_out = buf.data();
        zs.avail_out = static_cast<uInt>(buf.size());
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END)
            { inflateEnd(&zs); throw std::runtime_error("inflate failed: " + path); }
        out.insert(out.end(), buf.data(), buf.data() + (buf.size() - zs.avail_out));
    } while (rc != Z_STREAM_END);
    inflateEnd(&zs);
    return out;
}

// Generic skip/parse of an NBT payload by tag id, capturing what we need.
struct Parser {
    Reader& r;
    FossilTemplate& out;

    void payload(int tag, const std::string& path) {
        switch (tag) {
            case 1: r.skip(1); break;
            case 2: r.skip(2); break;
            case 3: {
                int32_t v = r.i32();
                (void)v;
                break;
            }
            case 4: r.skip(8); break;
            case 5: r.skip(4); break;
            case 6: r.skip(8); break;
            case 7: { int32_t n = r.i32(); r.skip(static_cast<size_t>(n)); break; }
            case 8: { r.str(); break; }
            case 9: list(path); break;
            case 10: compound(path); break;
            case 11: { int32_t n = r.i32(); r.skip(static_cast<size_t>(n) * 4); break; }
            case 12: { int32_t n = r.i32(); r.skip(static_cast<size_t>(n) * 8); break; }
            default: throw std::runtime_error("bad NBT tag " + std::to_string(tag));
        }
    }

    void list(const std::string& path) {
        int elemTag = r.u8();
        int32_t n = r.i32();
        if (path == "/size") {
            out.sizeX = r.i32(); out.sizeY = r.i32(); out.sizeZ = r.i32();
            return;
        }
        for (int32_t i = 0; i < n; ++i) {
            if (path == "/palette") {
                out.palette.emplace_back();
                paletteEntry();
            } else if (path == "/blocks") {
                out.blocks.emplace_back();
                blockEntry();
            } else if (path == "/blocks/#/pos") {
                // handled inside blockEntry
                payload(elemTag, path + "/#");
            } else {
                payload(elemTag, path + "/#");
            }
        }
    }

    void paletteEntry() {
        auto& ps = out.palette.back();
        for (;;) {
            int tag = r.u8();
            if (tag == 0) break;
            std::string name = r.str();
            if (tag == 8 && name == "Name") {
                ps.name = r.str();
            } else if (tag == 10 && name == "Properties") {
                for (;;) {
                    int pt = r.u8();
                    if (pt == 0) break;
                    std::string key = r.str();
                    if (pt == 8) ps.properties[key] = r.str();
                    else payload(pt, "/palette/#/Properties/*");
                }
            } else {
                payload(tag, "/palette/#/*");
            }
        }
    }

    void blockEntry() {
        auto& b = out.blocks.back();
        for (;;) {
            int tag = r.u8();
            if (tag == 0) break;
            std::string name = r.str();
            if (tag == 9 && name == "pos") {
                r.u8();          // elem tag (int)
                r.i32();         // count (3)
                b.x = r.i32(); b.y = r.i32(); b.z = r.i32();
            } else if (tag == 3 && name == "state") {
                b.stateIndex = r.i32();
            } else {
                payload(tag, "/blocks/#/*");
            }
        }
    }

    void compound(const std::string& path) {
        for (;;) {
            int tag = r.u8();
            if (tag == 0) break;
            std::string name = r.str();
            payload(tag, path + "/" + name);
        }
    }
};

}  // namespace nbt_detail

// Loads (and caches) a fossil template by id like "minecraft:fossil/spine_1".
// Root discovery mirrors the block-tag loader: walk up for data/, or MC_DATA_ROOT.
const FossilTemplate& getFossilTemplate(const std::string& id);

}  // namespace structure
}  // namespace levelgen
}  // namespace minecraft
