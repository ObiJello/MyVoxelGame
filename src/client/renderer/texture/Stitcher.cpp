// File: src/client/renderer/texture/Stitcher.cpp
#include "Stitcher.hpp"

#include "common/core/Log.hpp"

#include <algorithm>
#include <climits>

namespace Render {

    namespace {

        int LowestOneBit(int v) { return v & -v; }

        int Log2(int v) {
            int r = 0;
            while (v > 1) { v >>= 1; ++r; }
            return r;
        }

        // Mth.smallestEncompassingPowerOfTwo.
        int SmallestEncompassingPowerOfTwo(int v) {
            int p = 1;
            while (p < v) p <<= 1;
            return p;
        }

        // Stitcher.smallestFittingMinTexel: round up to a multiple of 1 << mip.
        int SmallestFittingMinTexel(int input, int mip) {
            return ((input >> mip) + ((input & ((1 << mip) - 1)) == 0 ? 0 : 1)) << mip;
        }

        // MC orders sprites by Identifier: path first, then namespace. Keys
        // here are "ns:path" or a bare path in the minecraft namespace.
        struct SpriteName {
            std::string ns, path;
            explicit SpriteName(const std::string& key) {
                const auto colon = key.find(':');
                if (colon == std::string::npos) { ns = "minecraft"; path = key; }
                else { ns = key.substr(0, colon); path = key.substr(colon + 1); }
            }
            bool operator<(const SpriteName& o) const {
                if (path != o.path) return path < o.path;
                return ns < o.ns;
            }
        };

        struct Holder {
            size_t entry;
            int width, height;
            SpriteName name;
        };

        // Stitcher.Region.
        struct Region {
            int originX, originY, width, height;
            std::vector<std::unique_ptr<Region>> subSlots;
            bool hasSubSlots = false;
            const Holder* holder = nullptr;

            Region(int x, int y, int w, int h) : originX(x), originY(y), width(w), height(h) {}

            bool Add(const Holder& h) {
                if (holder) return false;
                const int textureWidth = h.width;
                const int textureHeight = h.height;
                if (textureWidth > width || textureHeight > height) return false;
                if (textureWidth == width && textureHeight == height && !hasSubSlots) {
                    holder = &h;
                    return true;
                }
                if (!hasSubSlots) {
                    hasSubSlots = true;
                    subSlots.push_back(std::make_unique<Region>(originX, originY, textureWidth, textureHeight));
                    const int spareWidth = width - textureWidth;
                    const int spareHeight = height - textureHeight;
                    if (spareHeight > 0 && spareWidth > 0) {
                        const int right = std::max(height, spareWidth);
                        const int bottom = std::max(width, spareHeight);
                        if (right >= bottom) {
                            subSlots.push_back(std::make_unique<Region>(originX, originY + textureHeight, textureWidth, spareHeight));
                            subSlots.push_back(std::make_unique<Region>(originX + textureWidth, originY, spareWidth, height));
                        } else {
                            subSlots.push_back(std::make_unique<Region>(originX + textureWidth, originY, spareWidth, textureHeight));
                            subSlots.push_back(std::make_unique<Region>(originX, originY + textureHeight, width, spareHeight));
                        }
                    } else if (spareWidth == 0) {
                        subSlots.push_back(std::make_unique<Region>(originX, originY + textureHeight, textureWidth, spareHeight));
                    } else if (spareHeight == 0) {
                        subSlots.push_back(std::make_unique<Region>(originX + textureWidth, originY, spareWidth, textureHeight));
                    }
                }
                for (auto& sub : subSlots) {
                    if (sub->Add(h)) return true;
                }
                return false;
            }

            void Walk(std::vector<Stitcher::Placement>& out) const {
                if (holder) {
                    out.push_back({holder->entry, originX, originY});
                } else {
                    for (const auto& sub : subSlots) sub->Walk(out);
                }
            }
        };

    } // namespace

    int Stitcher::ChooseMipLevel(const std::vector<Entry>& entries, int maxMipLevels,
                                 const std::string& atlasName) {
        int minTexelSize = INT_MAX;
        int lowestOneBit = 1 << maxMipLevels;
        for (const Entry& e : entries) {
            minTexelSize = std::min(minTexelSize, std::min(e.width, e.height));
            const int bit = std::min(LowestOneBit(e.width), LowestOneBit(e.height));
            if (bit < lowestOneBit) {
                Log::Warning("Texture %s with size %dx%d limits mip level from %d to %d",
                             e.name.c_str(), e.width, e.height, Log2(lowestOneBit), Log2(bit));
                lowestOneBit = bit;
            }
        }
        const int minSize = std::min(minTexelSize, lowestOneBit);
        const int minPowerOfTwo = Log2(minSize);
        if (minPowerOfTwo < maxMipLevels) {
            Log::Warning("%s: dropping miplevel from %d to %d, because of minimum power of two: %d",
                         atlasName.c_str(), maxMipLevels, minPowerOfTwo, minSize);
            return minPowerOfTwo;
        }
        return maxMipLevels;
    }

    Stitcher::Result Stitcher::Stitch(const std::vector<Entry>& entries, int mipLevel, int maxSize) {
        Result result;
        result.mipLevel = mipLevel;
        result.padding = 1 << mipLevel;

        std::vector<Holder> holders;
        holders.reserve(entries.size());
        for (size_t i = 0; i < entries.size(); ++i) {
            const Entry& e = entries[i];
            holders.push_back({i,
                               SmallestFittingMinTexel(e.width + result.padding * 2, mipLevel),
                               SmallestFittingMinTexel(e.height + result.padding * 2, mipLevel),
                               SpriteName(e.name)});
        }
        // HOLDER_COMPARATOR: taller first, then wider, then by name.
        std::sort(holders.begin(), holders.end(), [](const Holder& a, const Holder& b) {
            if (a.height != b.height) return a.height > b.height;
            if (a.width != b.width) return a.width > b.width;
            return a.name < b.name;
        });

        std::vector<std::unique_ptr<Region>> storage;
        int storageX = 0, storageY = 0;
        for (const Holder& h : holders) {
            bool placed = false;
            for (auto& region : storage) {
                if (region->Add(h)) { placed = true; break; }
            }
            if (placed) continue;

            // Stitcher.expand.
            const int xCurrentSize = SmallestEncompassingPowerOfTwo(storageX);
            const int yCurrentSize = SmallestEncompassingPowerOfTwo(storageY);
            const int xNewSize = SmallestEncompassingPowerOfTwo(storageX + h.width);
            const int yNewSize = SmallestEncompassingPowerOfTwo(storageY + h.height);
            const bool xCanGrow = xNewSize <= maxSize;
            const bool yCanGrow = yNewSize <= maxSize;
            if (!xCanGrow && !yCanGrow) {
                result.failedEntry = entries[h.entry].name;
                return result;
            }
            const bool xWillGrow = xCanGrow && xCurrentSize != xNewSize;
            const bool yWillGrow = yCanGrow && yCurrentSize != yNewSize;
            const bool growOnX = (xWillGrow != yWillGrow) ? xWillGrow : (xCanGrow && xCurrentSize <= yCurrentSize);
            std::unique_ptr<Region> slot;
            if (growOnX) {
                if (storageY == 0) storageY = yNewSize;
                slot = std::make_unique<Region>(storageX, 0, xNewSize - storageX, storageY);
                storageX = xNewSize;
            } else {
                slot = std::make_unique<Region>(0, storageY, storageX, yNewSize - storageY);
                storageY = yNewSize;
            }
            slot->Add(h);
            storage.push_back(std::move(slot));
        }

        for (const auto& region : storage) region->Walk(result.placements);
        result.width = storageX;
        result.height = storageY;
        result.ok = true;
        return result;
    }

} // namespace Render
