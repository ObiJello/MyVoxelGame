// File: src/server/world/storage/anvil/NbtScan.cpp
#include "server/world/storage/anvil/NbtScan.hpp"

#include <cstring>

namespace Game::Anvil::NbtScan {

    namespace {
        struct Cursor {
            const std::vector<uint8_t>& nbt;
            size_t pos = 0;

            bool Have(size_t n) const { return pos + n <= nbt.size(); }
            uint16_t U16() {
                const uint16_t v = uint16_t(nbt[pos] << 8 | nbt[pos + 1]);
                pos += 2;
                return v;
            }
            int32_t I32() {
                const uint32_t v = uint32_t(nbt[pos]) << 24 | uint32_t(nbt[pos + 1]) << 16 |
                                   uint32_t(nbt[pos + 2]) << 8 | uint32_t(nbt[pos + 3]);
                pos += 4;
                return int32_t(v);
            }

            // Past one payload of `type`; false on bad or truncated data.
            bool Skip(uint8_t type, int depth) {
                if (depth > 512) return false;
                switch (type) {
                    case 1: return Advance(1);
                    case 2: return Advance(2);
                    case 3: case 5: return Advance(4);
                    case 4: case 6: return Advance(8);
                    case 7: case 11: case 12: {
                        if (!Have(4)) return false;
                        const int32_t n = I32();
                        const size_t width = type == 7 ? 1 : type == 11 ? 4 : 8;
                        return n >= 0 && Advance(size_t(n) * width);
                    }
                    case 8: {
                        if (!Have(2)) return false;
                        return Advance(U16());
                    }
                    case 9: {
                        if (!Have(5)) return false;
                        const uint8_t element = nbt[pos++];
                        const int32_t n = I32();
                        for (int32_t i = 0; i < n; ++i) {
                            if (!Skip(element, depth + 1)) return false;
                        }
                        return true;
                    }
                    case 10:
                        for (;;) {
                            if (!Have(1)) return false;
                            const uint8_t t = nbt[pos++];
                            if (t == 0) return true;
                            if (!Have(2)) return false;
                            if (!Advance(U16())) return false;
                            if (!Skip(t, depth + 1)) return false;
                        }
                    default:
                        return false;
                }
            }

            bool Advance(size_t n) {
                if (!Have(n)) return false;
                pos += n;
                return true;
            }
        };
    }

    bool FindRootTag(const std::vector<uint8_t>& nbt, std::string_view name, uint8_t type,
                     size_t& begin, size_t& end) {
        Cursor c{nbt};
        if (!c.Have(3) || nbt[0] != kCompound) return false;
        c.pos = 1;
        if (!c.Advance(c.U16())) return false;          // the root's (empty) name
        for (;;) {
            if (!c.Have(1)) return false;
            const uint8_t t = nbt[c.pos++];
            if (t == 0) return false;                     // end of root: absent
            if (!c.Have(2)) return false;
            const uint16_t length = c.U16();
            if (!c.Have(length)) return false;
            const bool match = t == type && length == name.size() &&
                               std::memcmp(&nbt[c.pos], name.data(), length) == 0;
            c.pos += length;
            const size_t payload = c.pos;
            if (!c.Skip(t, 0)) return false;
            if (match) {
                begin = payload;
                end = c.pos;
                return true;
            }
        }
    }

    bool ReadRootString(const std::vector<uint8_t>& nbt, std::string_view name, std::string& out) {
        size_t begin = 0, end = 0;
        if (!FindRootTag(nbt, name, kString, begin, end) || end - begin < 2) return false;
        out.assign(reinterpret_cast<const char*>(&nbt[begin + 2]), end - begin - 2);
        return true;
    }

} // namespace Game::Anvil::NbtScan
