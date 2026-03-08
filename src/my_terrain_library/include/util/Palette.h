#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <memory>
#include <unordered_map>
#include <algorithm>

namespace minecraft {
namespace util {

// Forward declarations
template<typename T> class Palette;
template<typename T> class Strategy;

/**
 * BitStorage interface - base for SimpleBitStorage and ZeroBitStorage
 * Reference: net/minecraft/util/BitStorage.java
 */
class BitStorage {
public:
    virtual ~BitStorage() = default;
    virtual int32_t getAndSet(int32_t index, int32_t value) = 0;
    virtual void set(int32_t index, int32_t value) = 0;
    virtual int32_t get(int32_t index) const = 0;
    virtual const std::vector<int64_t>& getRaw() const = 0;
    virtual std::vector<int64_t>& getRaw() = 0;
    virtual int32_t getSize() const = 0;
    virtual int32_t getBits() const = 0;
    virtual void getAll(const std::function<void(int32_t)>& consumer) const = 0;
    virtual void unpack(int32_t* output) const = 0;
    virtual std::unique_ptr<BitStorage> copy() const = 0;
};

/**
 * ZeroBitStorage - Storage for single-value palettes (0 bits per entry)
 * Reference: net/minecraft/util/ZeroBitStorage.java
 */
class ZeroBitStorage : public BitStorage {
private:
    static std::vector<int64_t> EMPTY;
    int32_t m_size;

public:
    explicit ZeroBitStorage(int32_t size) : m_size(size) {}

    int32_t getAndSet(int32_t index, int32_t value) override {
        // Only value 0 is allowed
        return 0;
    }

    void set(int32_t index, int32_t value) override {
        // Only value 0 is allowed
    }

    int32_t get(int32_t index) const override {
        return 0;
    }

    const std::vector<int64_t>& getRaw() const override {
        return EMPTY;
    }

    std::vector<int64_t>& getRaw() override {
        return EMPTY;
    }

    int32_t getSize() const override {
        return m_size;
    }

    int32_t getBits() const override {
        return 0;
    }

    void getAll(const std::function<void(int32_t)>& consumer) const override {
        for (int32_t i = 0; i < m_size; ++i) {
            consumer(0);
        }
    }

    void unpack(int32_t* output) const override {
        std::fill(output, output + m_size, 0);
    }

    std::unique_ptr<BitStorage> copy() const override {
        return std::make_unique<ZeroBitStorage>(m_size);
    }
};

// Static member definition
inline std::vector<int64_t> ZeroBitStorage::EMPTY;

/**
 * PaletteResize - Callback interface for palette resizing
 * Reference: net/minecraft/world/level/chunk/PaletteResize.java
 */
template<typename T>
class PaletteResize {
public:
    virtual ~PaletteResize() = default;
    virtual int32_t onResize(int32_t bits, T lastAddedValue) = 0;

    /**
     * Returns a resizer that throws if resize is attempted
     */
    static PaletteResize<T>& noResizeExpected() {
        static struct NoResizeHandler : public PaletteResize<T> {
            int32_t onResize(int32_t bits, T lastAddedValue) override {
                throw std::runtime_error("Unexpected palette resize");
            }
        } handler;
        return handler;
    }
};

/**
 * IdMap - Registry interface for mapping IDs to values
 * Reference: net/minecraft/core/IdMap.java
 */
template<typename T>
class IdMap {
public:
    virtual ~IdMap() = default;
    virtual int32_t getId(T value) const = 0;
    virtual T byId(int32_t id) const = 0;
    virtual T byIdOrThrow(int32_t id) const = 0;
    virtual int32_t size() const = 0;
};

/**
 * MissingPaletteEntryException
 * Reference: net/minecraft/world/level/chunk/MissingPaletteEntryException.java
 */
class MissingPaletteEntryException : public std::runtime_error {
public:
    explicit MissingPaletteEntryException(int32_t id)
        : std::runtime_error("Missing Palette entry for id " + std::to_string(id)) {}
};

/**
 * Palette interface - maps values to compact integer IDs
 * Reference: net/minecraft/world/level/chunk/Palette.java
 */
template<typename T>
class Palette {
public:
    virtual ~Palette() = default;

    /**
     * Get or create ID for a value
     * Returns the ID, or calls resizeHandler.onResize() if palette is full
     */
    virtual int32_t idFor(T value, PaletteResize<T>& resizeHandler) = 0;

    /**
     * Check if palette might contain a value matching predicate
     */
    virtual bool maybeHas(const std::function<bool(T)>& predicate) const = 0;

    /**
     * Get value for an ID
     */
    virtual T valueFor(int32_t id) const = 0;

    /**
     * Get number of entries in palette
     */
    virtual int32_t getSize() const = 0;

    /**
     * Create a copy of this palette
     */
    virtual std::unique_ptr<Palette<T>> copy() const = 0;
};

/**
 * SingleValuePalette - Palette with only one value (0 bits)
 * Reference: net/minecraft/world/level/chunk/SingleValuePalette.java
 */
template<typename T>
class SingleValuePalette : public Palette<T> {
private:
    T m_value;
    bool m_hasValue;

public:
    SingleValuePalette() : m_value(), m_hasValue(false) {}

    explicit SingleValuePalette(const std::vector<T>& paletteEntries) : m_hasValue(false) {
        if (!paletteEntries.empty()) {
            if (paletteEntries.size() > 1) {
                throw std::invalid_argument("Can't initialize SingleValuePalette with more than 1 value");
            }
            m_value = paletteEntries[0];
            m_hasValue = true;
        }
    }

    int32_t idFor(T value, PaletteResize<T>& resizeHandler) override {
        if (m_hasValue && m_value != value) {
            return resizeHandler.onResize(1, value);
        }
        m_value = value;
        m_hasValue = true;
        return 0;
    }

    bool maybeHas(const std::function<bool(T)>& predicate) const override {
        if (!m_hasValue) {
            throw std::runtime_error("Use of an uninitialized palette");
        }
        return predicate(m_value);
    }

    T valueFor(int32_t id) const override {
        if (m_hasValue && id == 0) {
            return m_value;
        }
        throw MissingPaletteEntryException(id);
    }

    int32_t getSize() const override {
        return 1;
    }

    std::unique_ptr<Palette<T>> copy() const override {
        if (!m_hasValue) {
            throw std::runtime_error("Use of an uninitialized palette");
        }
        auto result = std::make_unique<SingleValuePalette<T>>();
        result->m_value = m_value;
        result->m_hasValue = true;
        return result;
    }
};

/**
 * LinearPalette - Palette using linear search (1-4 bits, up to 16 entries)
 * Reference: net/minecraft/world/level/chunk/LinearPalette.java
 */
template<typename T>
class LinearPalette : public Palette<T> {
private:
    std::vector<T> m_values;
    int32_t m_bits;
    int32_t m_size;

public:
    LinearPalette(int32_t bits, const std::vector<T>& paletteEntries = {})
        : m_bits(bits)
        , m_size(0)
    {
        m_values.resize(1 << bits);
        if (paletteEntries.size() > m_values.size()) {
            throw std::invalid_argument("Can't initialize LinearPalette with too many entries");
        }
        for (size_t i = 0; i < paletteEntries.size(); ++i) {
            m_values[i] = paletteEntries[i];
        }
        m_size = static_cast<int32_t>(paletteEntries.size());
    }

    int32_t idFor(T value, PaletteResize<T>& resizeHandler) override {
        // Linear search for existing value
        for (int32_t i = 0; i < m_size; ++i) {
            if (m_values[i] == value) {
                return i;
            }
        }

        // Add new value if space available
        int32_t index = m_size;
        if (index < static_cast<int32_t>(m_values.size())) {
            m_values[index] = value;
            ++m_size;
            return index;
        }

        // Need to resize
        return resizeHandler.onResize(m_bits + 1, value);
    }

    bool maybeHas(const std::function<bool(T)>& predicate) const override {
        for (int32_t i = 0; i < m_size; ++i) {
            if (predicate(m_values[i])) {
                return true;
            }
        }
        return false;
    }

    T valueFor(int32_t id) const override {
        if (id >= 0 && id < m_size) {
            return m_values[id];
        }
        throw MissingPaletteEntryException(id);
    }

    int32_t getSize() const override {
        return m_size;
    }

    std::unique_ptr<Palette<T>> copy() const override {
        auto result = std::make_unique<LinearPalette<T>>(m_bits);
        result->m_values = m_values;
        result->m_size = m_size;
        return result;
    }
};

/**
 * HashMapPalette - Palette using hash map (5-8 bits, 17-256 entries)
 * Reference: net/minecraft/world/level/chunk/HashMapPalette.java
 */
template<typename T>
class HashMapPalette : public Palette<T> {
private:
    std::vector<T> m_idToValue;
    std::unordered_map<T, int32_t> m_valueToId;
    int32_t m_bits;

public:
    explicit HashMapPalette(int32_t bits, const std::vector<T>& paletteEntries = {})
        : m_bits(bits)
    {
        m_idToValue.reserve(1 << bits);
        for (const T& value : paletteEntries) {
            int32_t id = static_cast<int32_t>(m_idToValue.size());
            m_idToValue.push_back(value);
            m_valueToId[value] = id;
        }
    }

    int32_t idFor(T value, PaletteResize<T>& resizeHandler) override {
        auto it = m_valueToId.find(value);
        if (it != m_valueToId.end()) {
            return it->second;
        }

        // Add new value
        int32_t id = static_cast<int32_t>(m_idToValue.size());
        m_idToValue.push_back(value);
        m_valueToId[value] = id;

        // Check if we need to resize
        if (id >= (1 << m_bits)) {
            return resizeHandler.onResize(m_bits + 1, value);
        }

        return id;
    }

    bool maybeHas(const std::function<bool(T)>& predicate) const override {
        for (const T& value : m_idToValue) {
            if (predicate(value)) {
                return true;
            }
        }
        return false;
    }

    T valueFor(int32_t id) const override {
        if (id >= 0 && id < static_cast<int32_t>(m_idToValue.size())) {
            return m_idToValue[id];
        }
        throw MissingPaletteEntryException(id);
    }

    int32_t getSize() const override {
        return static_cast<int32_t>(m_idToValue.size());
    }

    std::vector<T> getEntries() const {
        return m_idToValue;
    }

    std::unique_ptr<Palette<T>> copy() const override {
        auto result = std::make_unique<HashMapPalette<T>>(m_bits);
        result->m_idToValue = m_idToValue;
        result->m_valueToId = m_valueToId;
        return result;
    }
};

/**
 * GlobalPalette - Direct registry ID passthrough (9+ bits)
 * Reference: net/minecraft/world/level/chunk/GlobalPalette.java
 */
template<typename T>
class GlobalPalette : public Palette<T> {
private:
    const IdMap<T>* m_registry;

public:
    explicit GlobalPalette(const IdMap<T>* registry) : m_registry(registry) {}

    int32_t idFor(T value, PaletteResize<T>& resizeHandler) override {
        int32_t id = m_registry->getId(value);
        return id == -1 ? 0 : id;
    }

    bool maybeHas(const std::function<bool(T)>& predicate) const override {
        return true;  // Could contain anything
    }

    T valueFor(int32_t id) const override {
        T value = m_registry->byId(id);
        if (value == T()) {  // Assuming default-constructed T is "null"
            throw MissingPaletteEntryException(id);
        }
        return value;
    }

    int32_t getSize() const override {
        return m_registry->size();
    }

    std::unique_ptr<Palette<T>> copy() const override {
        return std::make_unique<GlobalPalette<T>>(m_registry);
    }
};

/**
 * Configuration - Defines palette configuration for a bit count
 * Reference: net/minecraft/world/level/chunk/Configuration.java
 */
template<typename T>
class Configuration {
public:
    virtual ~Configuration() = default;
    virtual bool alwaysRepack() const = 0;
    virtual int32_t bitsInMemory() const = 0;
    virtual int32_t bitsInStorage() const = 0;
    virtual std::unique_ptr<Palette<T>> createPalette(
        Strategy<T>& strategy,
        const std::vector<T>& paletteEntries) const = 0;
};

/**
 * SimpleConfiguration - Standard palette configuration
 */
template<typename T>
class SimpleConfiguration : public Configuration<T> {
public:
    enum class PaletteType {
        SINGLE_VALUE,
        LINEAR,
        HASHMAP
    };

private:
    PaletteType m_type;
    int32_t m_bits;

public:
    SimpleConfiguration(PaletteType type, int32_t bits)
        : m_type(type), m_bits(bits) {}

    bool alwaysRepack() const override { return false; }
    int32_t bitsInMemory() const override { return m_bits; }
    int32_t bitsInStorage() const override { return m_bits; }

    std::unique_ptr<Palette<T>> createPalette(
        Strategy<T>& strategy,
        const std::vector<T>& paletteEntries) const override
    {
        switch (m_type) {
            case PaletteType::SINGLE_VALUE:
                return std::make_unique<SingleValuePalette<T>>(paletteEntries);
            case PaletteType::LINEAR:
                return std::make_unique<LinearPalette<T>>(m_bits, paletteEntries);
            case PaletteType::HASHMAP:
                return std::make_unique<HashMapPalette<T>>(m_bits, paletteEntries);
            default:
                throw std::runtime_error("Unknown palette type");
        }
    }
};

/**
 * GlobalConfiguration - Uses global palette (direct registry IDs)
 */
template<typename T>
class GlobalConfiguration : public Configuration<T> {
private:
    int32_t m_bitsInMemory;
    int32_t m_bitsInStorage;

public:
    GlobalConfiguration(int32_t bitsInMemory, int32_t bitsInStorage)
        : m_bitsInMemory(bitsInMemory), m_bitsInStorage(bitsInStorage) {}

    bool alwaysRepack() const override { return true; }
    int32_t bitsInMemory() const override { return m_bitsInMemory; }
    int32_t bitsInStorage() const override { return m_bitsInStorage; }

    std::unique_ptr<Palette<T>> createPalette(
        Strategy<T>& strategy,
        const std::vector<T>& paletteEntries) const override;  // Defined after Strategy
};

/**
 * Mth::ceillog2 - Calculate minimum bits needed for n distinct values
 */
inline int32_t ceillog2(int32_t n) {
    if (n <= 0) return 0;
    n = n - 1;
    int32_t bits = 0;
    while (n > 0) {
        n >>= 1;
        ++bits;
    }
    return bits;
}

/**
 * Strategy - Determines palette configuration for entry counts
 * Reference: net/minecraft/world/level/chunk/Strategy.java
 */
template<typename T>
class Strategy {
private:
    const IdMap<T>* m_globalMap;
    std::unique_ptr<GlobalPalette<T>> m_globalPalette;
    int32_t m_globalPaletteBitsInMemory;
    int32_t m_bitsPerAxis;
    int32_t m_entryCount;

protected:
    Strategy(const IdMap<T>* globalMap, int32_t bitsPerAxis)
        : m_globalMap(globalMap)
        , m_globalPalette(std::make_unique<GlobalPalette<T>>(globalMap))
        , m_globalPaletteBitsInMemory(ceillog2(globalMap->size()))
        , m_bitsPerAxis(bitsPerAxis)
        , m_entryCount(1 << (bitsPerAxis * 3))
    {}

public:
    virtual ~Strategy() = default;

    int32_t entryCount() const { return m_entryCount; }

    /**
     * Calculate 3D index from coordinates
     * Reference: Strategy.java line 90
     * For blocks (bitsPerAxis=4): (y << 8) | (z << 4) | x
     * For biomes (bitsPerAxis=2): (y << 4) | (z << 2) | x
     */
    int32_t getIndex(int32_t x, int32_t y, int32_t z) const {
        return ((y << m_bitsPerAxis) | z) << m_bitsPerAxis | x;
    }

    const IdMap<T>* globalMap() const { return m_globalMap; }
    GlobalPalette<T>* globalPalette() const { return m_globalPalette.get(); }
    int32_t globalPaletteBitsInMemory() const { return m_globalPaletteBitsInMemory; }

    virtual std::unique_ptr<Configuration<T>> getConfigurationForBitCount(int32_t entryBits) const = 0;

    std::unique_ptr<Configuration<T>> getConfigurationForPaletteSize(int32_t paletteSize) const {
        int32_t bits = ceillog2(paletteSize);
        return getConfigurationForBitCount(bits);
    }
};

/**
 * BlockStateStrategy - Strategy for block storage (16x16x16 = 4096 entries)
 * Reference: Strategy.java createForBlockStates()
 */
template<typename T>
class BlockStateStrategy : public Strategy<T> {
private:
    using PaletteType = typename SimpleConfiguration<T>::PaletteType;

public:
    explicit BlockStateStrategy(const IdMap<T>* registry)
        : Strategy<T>(registry, 4) {}  // 4 bits per axis = 16x16x16

    std::unique_ptr<Configuration<T>> getConfigurationForBitCount(int32_t entryBits) const override {
        switch (entryBits) {
            case 0:
                return std::make_unique<SimpleConfiguration<T>>(PaletteType::SINGLE_VALUE, 0);
            case 1:
            case 2:
            case 3:
            case 4:
                return std::make_unique<SimpleConfiguration<T>>(PaletteType::LINEAR, 4);
            case 5:
                return std::make_unique<SimpleConfiguration<T>>(PaletteType::HASHMAP, 5);
            case 6:
                return std::make_unique<SimpleConfiguration<T>>(PaletteType::HASHMAP, 6);
            case 7:
                return std::make_unique<SimpleConfiguration<T>>(PaletteType::HASHMAP, 7);
            case 8:
                return std::make_unique<SimpleConfiguration<T>>(PaletteType::HASHMAP, 8);
            default:
                return std::make_unique<GlobalConfiguration<T>>(
                    this->globalPaletteBitsInMemory(), entryBits);
        }
    }
};

/**
 * BiomeStrategy - Strategy for biome storage (4x4x4 = 64 entries)
 * Reference: Strategy.java createForBiomes()
 */
template<typename T>
class BiomeStrategy : public Strategy<T> {
private:
    using PaletteType = typename SimpleConfiguration<T>::PaletteType;

public:
    explicit BiomeStrategy(const IdMap<T>* registry)
        : Strategy<T>(registry, 2) {}  // 2 bits per axis = 4x4x4

    std::unique_ptr<Configuration<T>> getConfigurationForBitCount(int32_t entryBits) const override {
        switch (entryBits) {
            case 0:
                return std::make_unique<SimpleConfiguration<T>>(PaletteType::SINGLE_VALUE, 0);
            case 1:
                return std::make_unique<SimpleConfiguration<T>>(PaletteType::LINEAR, 1);
            case 2:
                return std::make_unique<SimpleConfiguration<T>>(PaletteType::LINEAR, 2);
            case 3:
                return std::make_unique<SimpleConfiguration<T>>(PaletteType::LINEAR, 3);
            default:
                return std::make_unique<GlobalConfiguration<T>>(
                    this->globalPaletteBitsInMemory(), entryBits);
        }
    }
};

// Define GlobalConfiguration::createPalette after Strategy is complete
template<typename T>
std::unique_ptr<Palette<T>> GlobalConfiguration<T>::createPalette(
    Strategy<T>& strategy,
    const std::vector<T>& paletteEntries) const
{
    return std::make_unique<GlobalPalette<T>>(strategy.globalMap());
}

} // namespace util
} // namespace minecraft
