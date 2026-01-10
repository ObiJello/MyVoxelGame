#pragma once

#include "util/Palette.h"
#include "util/SimpleBitStorage.h"
#include <memory>
#include <mutex>

namespace minecraft {
namespace util {

/**
 * PalettedContainer - Stores values using a palette system for efficient storage
 * Reference: net/minecraft/world/level/chunk/PalettedContainer.java
 *
 * This is the core data structure for block and biome storage in chunks.
 * It uses a palette to map values to compact integer IDs, automatically
 * resizing the palette as needed.
 */
template<typename T>
class PalettedContainer : public PaletteResize<T> {
private:
    /**
     * Internal data structure holding storage and palette
     * Reference: PalettedContainer.java Data record
     */
    struct Data {
        std::unique_ptr<Configuration<T>> configuration;
        std::unique_ptr<BitStorage> storage;
        std::unique_ptr<Palette<T>> palette;

        Data(std::unique_ptr<Configuration<T>> config,
             std::unique_ptr<BitStorage> stor,
             std::unique_ptr<Palette<T>> pal)
            : configuration(std::move(config))
            , storage(std::move(stor))
            , palette(std::move(pal)) {}

        /**
         * Copy data from another palette/storage pair
         * Reference: PalettedContainer.java Data.copyFrom()
         */
        void copyFrom(Palette<T>& oldPalette, BitStorage& oldStorage) {
            auto& noResize = PaletteResize<T>::noResizeExpected();

            for (int32_t i = 0; i < oldStorage.getSize(); ++i) {
                T value = oldPalette.valueFor(oldStorage.get(i));
                storage->set(i, palette->idFor(value, noResize));
            }
        }

        /**
         * Create a copy of this data
         */
        std::unique_ptr<Data> copy() const {
            return std::make_unique<Data>(
                nullptr,  // Configuration doesn't need to be copied
                storage->copy(),
                palette->copy()
            );
        }
    };

    std::unique_ptr<Data> m_data;
    Strategy<T>& m_strategy;
    mutable std::mutex m_mutex;  // Thread safety

    /**
     * Create new Data with target bit count
     * Reference: PalettedContainer.java createOrReuseData()
     */
    std::unique_ptr<Data> createOrReuseData(Data* oldData, int32_t targetBits) {
        auto config = m_strategy.getConfigurationForBitCount(targetBits);
        int32_t bitsInMemory = config->bitsInMemory();

        std::unique_ptr<BitStorage> storage;
        if (bitsInMemory == 0) {
            storage = std::make_unique<ZeroBitStorage>(m_strategy.entryCount());
        } else {
            storage = std::make_unique<SimpleBitStorage>(bitsInMemory, m_strategy.entryCount());
        }

        auto palette = config->createPalette(m_strategy, {});

        return std::make_unique<Data>(std::move(config), std::move(storage), std::move(palette));
    }

public:
    /**
     * Constructor with initial value
     * Reference: PalettedContainer.java constructor line 64-68
     */
    PalettedContainer(T initialValue, Strategy<T>& strategy)
        : m_strategy(strategy)
    {
        m_data = createOrReuseData(nullptr, 0);
        m_data->palette->idFor(initialValue, *this);
    }

    /**
     * Copy constructor
     * Reference: PalettedContainer.java copy constructor line 59-62
     */
    PalettedContainer(const PalettedContainer& other)
        : m_strategy(other.m_strategy)
    {
        std::lock_guard<std::mutex> lock(other.m_mutex);
        m_data = other.m_data->copy();
    }

    /**
     * Handle palette resize
     * Reference: PalettedContainer.java onResize() lines 81-87
     */
    int32_t onResize(int32_t bits, T lastAddedValue) override {
        auto oldData = std::move(m_data);
        m_data = createOrReuseData(oldData.get(), bits);
        m_data->copyFrom(*oldData->palette, *oldData->storage);
        return m_data->palette->idFor(lastAddedValue, PaletteResize<T>::noResizeExpected());
    }

    /**
     * Get and set value at position (thread-safe)
     * Reference: PalettedContainer.java getAndSet() lines 89-100
     */
    T getAndSet(int32_t x, int32_t y, int32_t z, T value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return getAndSetUnchecked(x, y, z, value);
    }

    /**
     * Get and set value at position (not thread-safe, for internal use)
     * Reference: PalettedContainer.java getAndSetUnchecked() lines 102-104
     */
    T getAndSetUnchecked(int32_t x, int32_t y, int32_t z, T value) {
        return getAndSet(m_strategy.getIndex(x, y, z), value);
    }

private:
    /**
     * Get and set by index
     * Reference: PalettedContainer.java getAndSet(int, T) lines 106-110
     */
    T getAndSet(int32_t index, T value) {
        int32_t id = m_data->palette->idFor(value, *this);
        int32_t oldId = m_data->storage->getAndSet(index, id);
        return m_data->palette->valueFor(oldId);
    }

public:
    /**
     * Set value at position (thread-safe)
     * Reference: PalettedContainer.java set() lines 112-121
     */
    void set(int32_t x, int32_t y, int32_t z, T value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        setUnchecked(x, y, z, value);
    }

    /**
     * Set value at position (not thread-safe)
     */
    void setUnchecked(int32_t x, int32_t y, int32_t z, T value) {
        int32_t index = m_strategy.getIndex(x, y, z);
        int32_t id = m_data->palette->idFor(value, *this);
        m_data->storage->set(index, id);
    }

    /**
     * Get value at position
     * Reference: PalettedContainer.java get() lines 128-135
     */
    T get(int32_t x, int32_t y, int32_t z) const {
        return get(m_strategy.getIndex(x, y, z));
    }

private:
    /**
     * Get by index
     * Reference: PalettedContainer.java get(int) lines 132-135
     */
    T get(int32_t index) const {
        return m_data->palette->valueFor(m_data->storage->get(index));
    }

public:
    /**
     * Iterate over all unique values
     * Reference: PalettedContainer.java getAll() lines 137-144
     */
    void getAll(const std::function<void(T)>& consumer) const {
        std::set<int32_t> existingIds;
        m_data->storage->getAll([&existingIds](int32_t id) {
            existingIds.insert(id);
        });

        for (int32_t id : existingIds) {
            consumer(m_data->palette->valueFor(id));
        }
    }

    /**
     * Check if palette might contain a value matching predicate
     * Reference: PalettedContainer.java maybeHas() lines 271-273
     */
    bool maybeHas(const std::function<bool(T)>& predicate) const {
        return m_data->palette->maybeHas(predicate);
    }

    /**
     * Get bits per entry
     * Reference: PalettedContainer.java bitsPerEntry() lines 267-269
     */
    int32_t bitsPerEntry() const {
        return m_data->storage->getBits();
    }

    /**
     * Create a copy
     * Reference: PalettedContainer.java copy() lines 275-277
     */
    PalettedContainer copy() const {
        return PalettedContainer(*this);
    }

    /**
     * Create a new container with default value from palette
     * Reference: PalettedContainer.java recreate() lines 279-281
     */
    PalettedContainer recreate() const {
        return PalettedContainer(m_data->palette->valueFor(0), m_strategy);
    }

    /**
     * Count occurrences of each value
     * Reference: PalettedContainer.java count() lines 283-291
     */
    void count(const std::function<void(T, int32_t)>& output) const {
        if (m_data->palette->getSize() == 1) {
            output(m_data->palette->valueFor(0), m_data->storage->getSize());
        } else {
            std::unordered_map<int32_t, int32_t> counts;
            m_data->storage->getAll([&counts](int32_t id) {
                counts[id]++;
            });

            for (const auto& entry : counts) {
                output(m_data->palette->valueFor(entry.first), entry.second);
            }
        }
    }

    /**
     * Check if all entries are the same value
     * Useful for optimization (e.g., skip sections that are all air)
     */
    bool isSingleValue() const {
        return m_data->palette->getSize() == 1;
    }

    /**
     * Get the single value if isSingleValue() is true
     */
    T getSingleValue() const {
        return m_data->palette->valueFor(0);
    }

    // =========================================================================
    // Serialization support
    // Reference: PalettedContainer.java read/write methods
    // =========================================================================

    /**
     * Get all palette entries for serialization
     * Returns entries in order (index 0 to paletteSize-1)
     */
    std::vector<T> getPaletteEntries() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<T> entries;
        int32_t size = m_data->palette->getSize();
        entries.reserve(size);
        for (int32_t i = 0; i < size; ++i) {
            entries.push_back(m_data->palette->valueFor(i));
        }
        return entries;
    }

    /**
     * Get raw storage data for serialization
     * Returns packed int64 array
     */
    std::vector<int64_t> getRawData() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_data->storage->getRaw();
    }

    /**
     * Get bits per entry used in storage
     */
    int32_t getBitsPerEntry() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_data->storage->getBits();
    }

    /**
     * Deserialize from palette entries and packed data
     * Reference: PalettedContainer.java read() for packed data format
     *
     * @param paletteEntries The palette values in order
     * @param packedData The packed storage data
     */
    void deserialize(const std::vector<T>& paletteEntries, const std::vector<int64_t>& packedData) {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (paletteEntries.empty()) {
            return;
        }

        // Calculate bits needed for palette
        int32_t paletteSize = static_cast<int32_t>(paletteEntries.size());
        int32_t bits = 0;
        if (paletteSize > 1) {
            bits = ceillog2(paletteSize);  // Use local ceillog2 from Palette.h
        }

        // Create new data with appropriate configuration
        auto config = m_strategy.getConfigurationForBitCount(bits);
        int32_t bitsInMemory = config->bitsInMemory();

        std::unique_ptr<BitStorage> storage;
        if (bitsInMemory == 0) {
            storage = std::make_unique<ZeroBitStorage>(m_strategy.entryCount());
        } else {
            // Create storage from packed data
            storage = std::make_unique<SimpleBitStorage>(bitsInMemory, m_strategy.entryCount(), packedData);
        }

        // Create palette from entries
        auto palette = config->createPalette(m_strategy, paletteEntries);

        m_data = std::make_unique<Data>(std::move(config), std::move(storage), std::move(palette));
    }
};

} // namespace util
} // namespace minecraft
