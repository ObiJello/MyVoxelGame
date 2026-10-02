// File: src/launcher/updater/BinaryPatch.cpp
//
// The suffix sort, match search and diff loop are bsdiff 4.3's
// (Copyright 2003-2005 Colin Percival, BSD 2-clause), with 32-bit suffix
// indices (half the memory of off_t, and no release file nears 2 GB) and the
// target diffed in slices on several threads.
#include "launcher/updater/BinaryPatch.hpp"
#include "common/core/Deflate.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <thread>

namespace Launcher::BinaryPatch {

    namespace {

        using Index = int32_t;

        // ── Larsson-Sadakane suffix sort (bsdiff's qsufsort) ────────────────

        void Split(Index* I, Index* V, Index start, Index len, Index h) {
            Index i, j, k, x, tmp, jj, kk;

            if (len < 16) {
                for (k = start; k < start + len; k += j) {
                    j = 1;
                    x = V[I[k] + h];
                    for (i = 1; k + i < start + len; i++) {
                        if (V[I[k + i] + h] < x) {
                            x = V[I[k + i] + h];
                            j = 0;
                        }
                        if (V[I[k + i] + h] == x) {
                            tmp = I[k + j]; I[k + j] = I[k + i]; I[k + i] = tmp;
                            j++;
                        }
                    }
                    for (i = 0; i < j; i++) V[I[k + i]] = k + j - 1;
                    if (j == 1) I[k] = -1;
                }
                return;
            }

            x = V[I[start + len / 2] + h];
            jj = 0;
            kk = 0;
            for (i = start; i < start + len; i++) {
                if (V[I[i] + h] < x) jj++;
                if (V[I[i] + h] == x) kk++;
            }
            jj += start;
            kk += jj;

            i = start;
            j = 0;
            k = 0;
            while (i < jj) {
                if (V[I[i] + h] < x) {
                    i++;
                } else if (V[I[i] + h] == x) {
                    tmp = I[i]; I[i] = I[jj + j]; I[jj + j] = tmp;
                    j++;
                } else {
                    tmp = I[i]; I[i] = I[kk + k]; I[kk + k] = tmp;
                    k++;
                }
            }
            while (jj + j < kk) {
                if (V[I[jj + j] + h] == x) {
                    j++;
                } else {
                    tmp = I[jj + j]; I[jj + j] = I[kk + k]; I[kk + k] = tmp;
                    k++;
                }
            }

            if (jj > start) Split(I, V, start, jj - start, h);
            for (i = 0; i < kk - jj; i++) V[I[jj + i]] = kk - 1;
            if (jj == kk - 1) I[jj] = -1;
            if (start + len > kk) Split(I, V, kk, start + len - kk, h);
        }

        void SuffixSort(Index* I, Index* V, const uint8_t* old, Index oldSize) {
            Index buckets[256] = {};
            for (Index i = 0; i < oldSize; i++) buckets[old[i]]++;
            for (int i = 1; i < 256; i++) buckets[i] += buckets[i - 1];
            for (int i = 255; i > 0; i--) buckets[i] = buckets[i - 1];
            buckets[0] = 0;

            for (Index i = 0; i < oldSize; i++) I[++buckets[old[i]]] = i;
            I[0] = oldSize;
            for (Index i = 0; i < oldSize; i++) V[i] = buckets[old[i]];
            V[oldSize] = 0;
            for (int i = 1; i < 256; i++) {
                if (buckets[i] == buckets[i - 1] + 1) I[buckets[i]] = -1;
            }
            I[0] = -1;

            for (Index h = 1; I[0] != -(oldSize + 1); h += h) {
                Index len = 0;
                Index i = 0;
                while (i < oldSize + 1) {
                    if (I[i] < 0) {
                        len -= I[i];
                        i -= I[i];
                    } else {
                        if (len) I[i - len] = -len;
                        len = V[I[i]] + 1 - i;
                        Split(I, V, i, len, h);
                        i += len;
                        len = 0;
                    }
                }
                if (len) I[i - len] = -len;
            }

            for (Index i = 0; i < oldSize + 1; i++) I[V[i]] = i;
        }

        Index MatchLen(const uint8_t* a, Index aSize, const uint8_t* b, Index bSize) {
            Index i = 0;
            while (i < aSize && i < bSize && a[i] == b[i]) i++;
            return i;
        }

        Index Search(const Index* I, const uint8_t* old, Index oldSize, const uint8_t* cur, Index curSize,
                     Index st, Index en, Index* pos) {
            while (en - st >= 2) {
                const Index x = st + (en - st) / 2;
                const size_t n = static_cast<size_t>(std::min(oldSize - I[x], curSize));
                if (std::memcmp(old + I[x], cur, n) < 0) {
                    st = x;
                } else {
                    en = x;
                }
            }
            const Index x = MatchLen(old + I[st], oldSize - I[st], cur, curSize);
            const Index y = MatchLen(old + I[en], oldSize - I[en], cur, curSize);
            if (x > y) {
                *pos = I[st];
                return x;
            }
            *pos = I[en];
            return y;
        }

        // One slice [sliceStart, sliceEnd) of the target, as bsdiff's main loop
        // with matches clipped to the slice. The base position starts at 0 and
        // ends at `endPos`, which the caller rewinds between slices.
        struct Slice {
            std::vector<int64_t> control;
            std::vector<uint8_t> diff, extra;
            int64_t endPos = 0;
        };

        void DiffSlice(const Index* I, const uint8_t* old, Index oldSize, const uint8_t* cur,
                       Index sliceStart, Index sliceEnd, Slice& out) {
            Index scan = sliceStart, len = 0, pos = 0;
            Index lastScan = sliceStart, lastPos = 0, lastOffset = 0;

            while (scan < sliceEnd) {
                Index oldScore = 0;
                Index scsc;
                for (scsc = scan += len; scan < sliceEnd; scan++) {
                    len = Search(I, old, oldSize, cur + scan, sliceEnd - scan, 0, oldSize, &pos);
                    for (; scsc < scan + len; scsc++) {
                        if (scsc + lastOffset < oldSize && old[scsc + lastOffset] == cur[scsc]) oldScore++;
                    }
                    if ((len == oldScore && len != 0) || len > oldScore + 8) break;
                    if (scan + lastOffset < oldSize && old[scan + lastOffset] == cur[scan]) oldScore--;
                }

                if (len != oldScore || scan == sliceEnd) {
                    Index s = 0, sf = 0, lenF = 0;
                    for (Index i = 0; lastScan + i < scan && lastPos + i < oldSize;) {
                        if (old[lastPos + i] == cur[lastScan + i]) s++;
                        i++;
                        if (s * 2 - i > sf * 2 - lenF) {
                            sf = s;
                            lenF = i;
                        }
                    }

                    Index lenB = 0;
                    if (scan < sliceEnd) {
                        Index sb = 0;
                        s = 0;
                        for (Index i = 1; scan >= lastScan + i && pos >= i; i++) {
                            if (old[pos - i] == cur[scan - i]) s++;
                            if (s * 2 - i > sb * 2 - lenB) {
                                sb = s;
                                lenB = i;
                            }
                        }
                    }

                    if (lastScan + lenF > scan - lenB) {
                        const Index overlap = (lastScan + lenF) - (scan - lenB);
                        Index ss = 0, lenS = 0;
                        s = 0;
                        for (Index i = 0; i < overlap; i++) {
                            if (cur[lastScan + lenF - overlap + i] == old[lastPos + lenF - overlap + i]) s++;
                            if (cur[scan - lenB + i] == old[pos - lenB + i]) s--;
                            if (s > ss) {
                                ss = s;
                                lenS = i + 1;
                            }
                        }
                        lenF += lenS - overlap;
                        lenB -= lenS;
                    }

                    for (Index i = 0; i < lenF; i++) {
                        out.diff.push_back(static_cast<uint8_t>(cur[lastScan + i] - old[lastPos + i]));
                    }
                    const Index extraLen = (scan - lenB) - (lastScan + lenF);
                    out.extra.insert(out.extra.end(), cur + lastScan + lenF, cur + lastScan + lenF + extraLen);

                    out.control.push_back(lenF);
                    out.control.push_back(extraLen);
                    out.control.push_back(static_cast<int64_t>(pos - lenB) - (lastPos + lenF));

                    lastScan = scan - lenB;
                    lastPos = pos - lenB;
                    lastOffset = pos - scan;
                }
            }
            out.endPos = lastPos;
        }

        void PutU16(std::vector<uint8_t>& b, uint16_t v) {
            b.push_back(static_cast<uint8_t>(v));
            b.push_back(static_cast<uint8_t>(v >> 8));
        }
        void PutU32(std::vector<uint8_t>& b, uint32_t v) {
            for (int i = 0; i < 4; i++) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
        void PutU64(std::vector<uint8_t>& b, uint64_t v) {
            for (int i = 0; i < 8; i++) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
        uint64_t GetLE(const uint8_t* p, int n) {
            uint64_t v = 0;
            for (int i = 0; i < n; i++) v |= static_cast<uint64_t>(p[i]) << (8 * i);
            return v;
        }

        bool Inflate(const std::vector<uint8_t>& in, uint64_t size, std::vector<uint8_t>& out) {
            out.resize(static_cast<size_t>(size));
            if (size == 0) return true;
            return Core::Deflate::DecompressExact(in.data(), in.size(), out.data(), out.size(),
                                                  Core::Deflate::Format::Raw);
        }

        constexpr char kMagic[8] = {'O', 'B', 'P', 'A', 'T', 'C', 'H', '1'};

    } // namespace

    bool Create(const uint8_t* base, size_t baseSize, const uint8_t* target, size_t targetSize,
                Entry& out, int jobs, int level) {
        constexpr size_t kMax = static_cast<size_t>(std::numeric_limits<Index>::max()) - 1;
        if (baseSize == 0 || baseSize >= kMax || targetSize >= kMax) return false;
        const Index oldSize = static_cast<Index>(baseSize);
        const Index newSize = static_cast<Index>(targetSize);

        std::vector<Index> I(baseSize + 1);
        {
            std::vector<Index> V(baseSize + 1);
            SuffixSort(I.data(), V.data(), base, oldSize);
        }

        jobs = std::max(1, std::min(jobs, 64));
        const Index sliceLen = std::max<Index>(1, (newSize + jobs - 1) / jobs);
        std::vector<Slice> slices(static_cast<size_t>(jobs));
        std::vector<std::thread> threads;
        for (int j = 0; j < jobs; j++) {
            const Index s = std::min<Index>(newSize, static_cast<Index>(j) * sliceLen);
            const Index e = std::min<Index>(newSize, s + sliceLen);
            threads.emplace_back([&, j, s, e] { DiffSlice(I.data(), base, oldSize, target, s, e, slices[j]); });
        }
        for (auto& t : threads) t.join();
        I = {};

        std::vector<int64_t> control;
        std::vector<uint8_t> diff, extra;
        for (size_t j = 0; j < slices.size(); j++) {
            const Slice& s = slices[j];
            control.insert(control.end(), s.control.begin(), s.control.end());
            diff.insert(diff.end(), s.diff.begin(), s.diff.end());
            extra.insert(extra.end(), s.extra.begin(), s.extra.end());
            // Rewind the base position for the next slice, which starts at 0.
            if (j + 1 < slices.size() && s.endPos != 0) {
                control.push_back(0);
                control.push_back(0);
                control.push_back(-s.endPos);
            }
        }

        std::vector<uint8_t> controlBytes;
        controlBytes.reserve(control.size() * 8);
        for (int64_t v : control) PutU64(controlBytes, static_cast<uint64_t>(v));

        out.baseSize = baseSize;
        out.baseCrc = Core::Deflate::Crc32(base, baseSize);
        out.targetSize = targetSize;
        out.targetCrc = Core::Deflate::Crc32(target, targetSize);
        out.controlSize = controlBytes.size();
        out.diffSize = diff.size();
        out.extraSize = extra.size();

        // The three streams compress independently; do them side by side.
        bool ok[3] = {true, true, true};
        std::thread tc([&] { ok[0] = Core::Deflate::Compress(controlBytes.data(), controlBytes.size(), out.control, Core::Deflate::Format::Raw, level); });
        std::thread td([&] { ok[1] = Core::Deflate::Compress(diff.data(), diff.size(), out.diff, Core::Deflate::Format::Raw, level); });
        ok[2] = Core::Deflate::Compress(extra.data(), extra.size(), out.extra, Core::Deflate::Format::Raw, level);
        tc.join();
        td.join();
        return ok[0] && ok[1] && ok[2];
    }

    bool Apply(const Entry& e, const uint8_t* base, size_t baseSize, std::vector<uint8_t>& out) {
        if (baseSize != e.baseSize || e.controlSize % 24 != 0) return false;
        std::vector<uint8_t> control, diff, extra;
        if (!Inflate(e.control, e.controlSize, control) || !Inflate(e.diff, e.diffSize, diff) ||
            !Inflate(e.extra, e.extraSize, extra)) {
            return false;
        }

        out.assign(static_cast<size_t>(e.targetSize), 0);
        const int64_t newSize = static_cast<int64_t>(e.targetSize);
        const int64_t oldSize = static_cast<int64_t>(baseSize);
        int64_t newPos = 0, oldPos = 0;
        size_t diffAt = 0, extraAt = 0;
        for (size_t c = 0; c < control.size(); c += 24) {
            const int64_t addLen = static_cast<int64_t>(GetLE(&control[c], 8));
            const int64_t copyLen = static_cast<int64_t>(GetLE(&control[c + 8], 8));
            const int64_t seek = static_cast<int64_t>(GetLE(&control[c + 16], 8));
            if (addLen < 0 || copyLen < 0 || addLen > newSize - newPos) return false;
            if (static_cast<uint64_t>(addLen) > diff.size() - diffAt) return false;
            for (int64_t i = 0; i < addLen; i++) {
                uint8_t b = diff[diffAt + static_cast<size_t>(i)];
                const int64_t o = oldPos + i;
                if (o >= 0 && o < oldSize) b = static_cast<uint8_t>(b + base[o]);
                out[static_cast<size_t>(newPos + i)] = b;
            }
            diffAt += static_cast<size_t>(addLen);
            newPos += addLen;
            oldPos += addLen;

            if (copyLen > newSize - newPos || static_cast<uint64_t>(copyLen) > extra.size() - extraAt) return false;
            std::memcpy(out.data() + newPos, extra.data() + extraAt, static_cast<size_t>(copyLen));
            extraAt += static_cast<size_t>(copyLen);
            newPos += copyLen;
            oldPos += seek;
        }
        if (newPos != newSize) return false;
        return Core::Deflate::Crc32(out.data(), out.size()) == e.targetCrc;
    }

    void Write(const std::vector<Entry>& entries, std::vector<uint8_t>& file) {
        file.assign(kMagic, kMagic + sizeof kMagic);
        PutU32(file, static_cast<uint32_t>(entries.size()));
        for (const Entry& e : entries) {
            PutU16(file, static_cast<uint16_t>(e.name.size()));
            file.insert(file.end(), e.name.begin(), e.name.end());
            PutU32(file, e.baseCrc);
            PutU64(file, e.baseSize);
            PutU32(file, e.targetCrc);
            PutU64(file, e.targetSize);
            PutU64(file, e.controlSize);
            PutU64(file, e.control.size());
            PutU64(file, e.diffSize);
            PutU64(file, e.diff.size());
            PutU64(file, e.extraSize);
            PutU64(file, e.extra.size());
        }
        for (const Entry& e : entries) {
            file.insert(file.end(), e.control.begin(), e.control.end());
            file.insert(file.end(), e.diff.begin(), e.diff.end());
            file.insert(file.end(), e.extra.begin(), e.extra.end());
        }
    }

    bool Read(const uint8_t* data, size_t size, std::vector<Entry>& out) {
        out.clear();
        if (size < 12 || std::memcmp(data, kMagic, sizeof kMagic) != 0) return false;
        const uint32_t count = static_cast<uint32_t>(GetLE(data + 8, 4));
        size_t p = 12;
        constexpr size_t kFixed = 4 + 8 + 4 + 8 + 6 * 8;
        std::vector<uint64_t> stored;   // control/diff/extra stored sizes, in order
        for (uint32_t i = 0; i < count; i++) {
            if (size - p < 2) return false;
            const size_t nameLen = static_cast<size_t>(GetLE(data + p, 2));
            p += 2;
            if (size - p < nameLen + kFixed) return false;
            Entry e;
            e.name.assign(reinterpret_cast<const char*>(data + p), nameLen);
            p += nameLen;
            e.baseCrc = static_cast<uint32_t>(GetLE(data + p, 4));       p += 4;
            e.baseSize = GetLE(data + p, 8);                              p += 8;
            e.targetCrc = static_cast<uint32_t>(GetLE(data + p, 4));     p += 4;
            e.targetSize = GetLE(data + p, 8);                            p += 8;
            e.controlSize = GetLE(data + p, 8);                           p += 8;
            stored.push_back(GetLE(data + p, 8));                         p += 8;
            e.diffSize = GetLE(data + p, 8);                              p += 8;
            stored.push_back(GetLE(data + p, 8));                         p += 8;
            e.extraSize = GetLE(data + p, 8);                             p += 8;
            stored.push_back(GetLE(data + p, 8));                         p += 8;
            out.push_back(std::move(e));
        }
        size_t s = 0;
        for (Entry& e : out) {
            std::vector<uint8_t>* streams[3] = {&e.control, &e.diff, &e.extra};
            for (std::vector<uint8_t>* stream : streams) {
                const uint64_t n = stored[s++];
                if (n > size - p) return false;
                stream->assign(data + p, data + p + n);
                p += static_cast<size_t>(n);
            }
        }
        return true;
    }

} // namespace Launcher::BinaryPatch
