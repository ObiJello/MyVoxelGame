#pragma once

#include "util/Palette.h"
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <memory>

namespace minecraft {
namespace util {

/**
 * SimpleBitStorage - Compact storage for arrays of n-bit integers
 *
 * Matches Minecraft's SimpleBitStorage.java implementation exactly.
 * Stores multiple values per long, with each value using a fixed number of bits.
 * Uses magic number multiplication for fast division (matching Java).
 *
 * Reference: net/minecraft/util/SimpleBitStorage.java
 */
class SimpleBitStorage : public BitStorage {
private:
    // MAGIC array for fast division - matches Java exactly
    // Reference: SimpleBitStorage.java line 8
    // Each group of 3 values: [divideMul, divideAdd, divideShift]
    // Indexed by 3 * (valuesPerLong - 1)
    static constexpr int32_t MAGIC[] = {
        -1, -1, 0,                          // valuesPerLong = 1
        INT32_MIN, 0, 0,                    // valuesPerLong = 2 (0x80000000)
        1431655765, 1431655765, 0,          // valuesPerLong = 3
        INT32_MIN, 0, 1,                    // valuesPerLong = 4
        858993459, 858993459, 0,            // valuesPerLong = 5
        715827882, 715827882, 0,            // valuesPerLong = 6
        613566756, 613566756, 0,            // valuesPerLong = 7
        INT32_MIN, 0, 2,                    // valuesPerLong = 8
        477218588, 477218588, 0,            // valuesPerLong = 9
        429496729, 429496729, 0,            // valuesPerLong = 10
        390451572, 390451572, 0,            // valuesPerLong = 11
        357913941, 357913941, 0,            // valuesPerLong = 12
        330382099, 330382099, 0,            // valuesPerLong = 13
        306783378, 306783378, 0,            // valuesPerLong = 14
        286331153, 286331153, 0,            // valuesPerLong = 15
        INT32_MIN, 0, 3,                    // valuesPerLong = 16
        252645135, 252645135, 0,            // valuesPerLong = 17
        238609294, 238609294, 0,            // valuesPerLong = 18
        226050910, 226050910, 0,            // valuesPerLong = 19
        214748364, 214748364, 0,            // valuesPerLong = 20
        204522252, 204522252, 0,            // valuesPerLong = 21
        195225786, 195225786, 0,            // valuesPerLong = 22
        186737708, 186737708, 0,            // valuesPerLong = 23
        178956970, 178956970, 0,            // valuesPerLong = 24
        171798691, 171798691, 0,            // valuesPerLong = 25
        165191049, 165191049, 0,            // valuesPerLong = 26
        159072862, 159072862, 0,            // valuesPerLong = 27
        153391689, 153391689, 0,            // valuesPerLong = 28
        148102320, 148102320, 0,            // valuesPerLong = 29
        143165576, 143165576, 0,            // valuesPerLong = 30
        138547332, 138547332, 0,            // valuesPerLong = 31
        INT32_MIN, 0, 4,                    // valuesPerLong = 32
        130150524, 130150524, 0,            // valuesPerLong = 33
        126322567, 126322567, 0,            // valuesPerLong = 34
        122713351, 122713351, 0,            // valuesPerLong = 35
        119304647, 119304647, 0,            // valuesPerLong = 36
        116080197, 116080197, 0,            // valuesPerLong = 37
        113025455, 113025455, 0,            // valuesPerLong = 38
        110127366, 110127366, 0,            // valuesPerLong = 39
        107374182, 107374182, 0,            // valuesPerLong = 40
        104755299, 104755299, 0,            // valuesPerLong = 41
        102261126, 102261126, 0,            // valuesPerLong = 42
        99882960, 99882960, 0,              // valuesPerLong = 43
        97612893, 97612893, 0,              // valuesPerLong = 44
        95443717, 95443717, 0,              // valuesPerLong = 45
        93368854, 93368854, 0,              // valuesPerLong = 46
        91382282, 91382282, 0,              // valuesPerLong = 47
        89478485, 89478485, 0,              // valuesPerLong = 48
        87652393, 87652393, 0,              // valuesPerLong = 49
        85899345, 85899345, 0,              // valuesPerLong = 50
        84215045, 84215045, 0,              // valuesPerLong = 51
        82595524, 82595524, 0,              // valuesPerLong = 52
        81037118, 81037118, 0,              // valuesPerLong = 53
        79536431, 79536431, 0,              // valuesPerLong = 54
        78090314, 78090314, 0,              // valuesPerLong = 55
        76695844, 76695844, 0,              // valuesPerLong = 56
        75350303, 75350303, 0,              // valuesPerLong = 57
        74051160, 74051160, 0,              // valuesPerLong = 58
        72796055, 72796055, 0,              // valuesPerLong = 59
        71582788, 71582788, 0,              // valuesPerLong = 60
        70409299, 70409299, 0,              // valuesPerLong = 61
        69273666, 69273666, 0,              // valuesPerLong = 62
        68174084, 68174084, 0,              // valuesPerLong = 63
        INT32_MIN, 0, 5                     // valuesPerLong = 64
    };

    std::vector<int64_t> m_data;
    int32_t m_bits;           // Bits per value (1-32)
    int64_t m_mask;           // Bitmask for a single value
    int32_t m_size;           // Number of values stored
    int32_t m_valuesPerLong;  // How many values fit in one long
    int32_t m_divideMul;      // Magic multiplier for fast division
    int32_t m_divideAdd;      // Magic addend for fast division
    int32_t m_divideShift;    // Shift for fast division

    /**
     * Calculate cell index using fast division
     * Reference: SimpleBitStorage.java lines 75-79
     */
    int32_t cellIndex(int32_t bitIndex) const {
        // Convert to unsigned for the multiplication
        uint64_t mul = static_cast<uint32_t>(m_divideMul);
        uint64_t add = static_cast<uint32_t>(m_divideAdd);
        return static_cast<int32_t>(((static_cast<int64_t>(bitIndex) * mul + add) >> 32) >> m_divideShift);
    }

public:
    /**
     * Exception for initialization errors
     * Reference: SimpleBitStorage.java lines 168-172
     */
    class InitializationException : public std::runtime_error {
    public:
        explicit InitializationException(const std::string& message)
            : std::runtime_error(message) {}
    };

    /**
     * Constructor with bits and size only
     * Reference: SimpleBitStorage.java lines 48-50
     */
    SimpleBitStorage(int32_t bits, int32_t size)
        : SimpleBitStorage(bits, size, static_cast<const int64_t*>(nullptr)) {}

    /**
     * Constructor with initial values array
     * Reference: SimpleBitStorage.java lines 18-46
     */
    SimpleBitStorage(int32_t bits, int32_t size, const int32_t* values)
        : SimpleBitStorage(bits, size, static_cast<const int64_t*>(nullptr))
    {
        if (values != nullptr) {
            int32_t outputIndex = 0;
            int32_t inputOffset = 0;

            // Pack full longs
            while (inputOffset <= size - m_valuesPerLong) {
                int64_t packedValue = 0;

                for (int32_t indexInLong = m_valuesPerLong - 1; indexInLong >= 0; --indexInLong) {
                    packedValue <<= bits;
                    packedValue |= static_cast<int64_t>(values[inputOffset + indexInLong]) & m_mask;
                }

                m_data[outputIndex++] = packedValue;
                inputOffset += m_valuesPerLong;
            }

            // Pack remainder
            int32_t remainderCount = size - inputOffset;
            if (remainderCount > 0) {
                int64_t lastPackedValue = 0;

                for (int32_t indexInLong = remainderCount - 1; indexInLong >= 0; --indexInLong) {
                    lastPackedValue <<= bits;
                    lastPackedValue |= static_cast<int64_t>(values[inputOffset + indexInLong]) & m_mask;
                }

                m_data[outputIndex] = lastPackedValue;
            }
        }
    }

    /**
     * Constructor with raw data array
     * Reference: SimpleBitStorage.java lines 52-73
     */
    SimpleBitStorage(int32_t bits, int32_t size, const int64_t* data)
        : m_bits(bits)
        , m_size(size)
    {
        // Validate bits range (1-32 for block storage)
        if (bits < 1 || bits > 32) {
            throw std::invalid_argument("Bits must be between 1 and 32");
        }

        m_mask = (1LL << bits) - 1LL;
        m_valuesPerLong = 64 / bits;

        // Get magic values for fast division
        int32_t row = 3 * (m_valuesPerLong - 1);
        m_divideMul = MAGIC[row + 0];
        m_divideAdd = MAGIC[row + 1];
        m_divideShift = MAGIC[row + 2];

        // Calculate required array length
        int32_t requiredLength = (size + m_valuesPerLong - 1) / m_valuesPerLong;

        if (data != nullptr) {
            m_data.assign(data, data + requiredLength);
        } else {
            m_data.resize(requiredLength, 0);
        }
    }

    /**
     * Constructor with raw data vector
     */
    SimpleBitStorage(int32_t bits, int32_t size, const std::vector<int64_t>& data)
        : m_bits(bits)
        , m_size(size)
    {
        if (bits < 1 || bits > 32) {
            throw std::invalid_argument("Bits must be between 1 and 32");
        }

        m_mask = (1LL << bits) - 1LL;
        m_valuesPerLong = 64 / bits;

        int32_t row = 3 * (m_valuesPerLong - 1);
        m_divideMul = MAGIC[row + 0];
        m_divideAdd = MAGIC[row + 1];
        m_divideShift = MAGIC[row + 2];

        int32_t requiredLength = (size + m_valuesPerLong - 1) / m_valuesPerLong;
        if (static_cast<int32_t>(data.size()) != requiredLength) {
            throw InitializationException("Invalid length given for storage, got: " +
                std::to_string(data.size()) + " but expected: " + std::to_string(requiredLength));
        }

        m_data = data;
    }

    /**
     * Get and set value at index (atomic swap)
     * Reference: SimpleBitStorage.java lines 81-90
     */
    int32_t getAndSet(int32_t index, int32_t value) override {
        int32_t cellIdx = cellIndex(index);
        int64_t cellValue = m_data[cellIdx];
        int32_t bitIndex = (index - cellIdx * m_valuesPerLong) * m_bits;
        int32_t oldValue = static_cast<int32_t>((cellValue >> bitIndex) & m_mask);
        m_data[cellIdx] = (cellValue & ~(m_mask << bitIndex)) |
                         ((static_cast<int64_t>(value) & m_mask) << bitIndex);
        return oldValue;
    }

    /**
     * Set value at index
     * Reference: SimpleBitStorage.java lines 92-99
     */
    void set(int32_t index, int32_t value) override {
        int32_t cellIdx = cellIndex(index);
        int64_t cellValue = m_data[cellIdx];
        int32_t bitIndex = (index - cellIdx * m_valuesPerLong) * m_bits;
        m_data[cellIdx] = (cellValue & ~(m_mask << bitIndex)) |
                         ((static_cast<int64_t>(value) & m_mask) << bitIndex);
    }

    /**
     * Get value at index
     * Reference: SimpleBitStorage.java lines 101-107
     */
    int32_t get(int32_t index) const override {
        int32_t cellIdx = cellIndex(index);
        int64_t cellValue = m_data[cellIdx];
        int32_t bitIndex = (index - cellIdx * m_valuesPerLong) * m_bits;
        return static_cast<int32_t>((cellValue >> bitIndex) & m_mask);
    }

    /**
     * Get raw data array (const)
     * Reference: SimpleBitStorage.java lines 109-111
     */
    const std::vector<int64_t>& getRaw() const override {
        return m_data;
    }

    /**
     * Get raw data array (mutable)
     */
    std::vector<int64_t>& getRaw() override {
        return m_data;
    }

    /**
     * Get number of values stored
     * Reference: SimpleBitStorage.java lines 113-115
     */
    int32_t getSize() const override {
        return m_size;
    }

    /**
     * Get bits per value
     * Reference: SimpleBitStorage.java lines 117-119
     */
    int32_t getBits() const override {
        return m_bits;
    }

    /**
     * Iterate all values
     * Reference: SimpleBitStorage.java lines 121-135
     */
    void getAll(const std::function<void(int32_t)>& consumer) const override {
        int32_t count = 0;

        for (int64_t cellValue : m_data) {
            for (int32_t i = 0; i < m_valuesPerLong; ++i) {
                consumer(static_cast<int32_t>(cellValue & m_mask));
                cellValue >>= m_bits;
                ++count;
                if (count >= m_size) {
                    return;
                }
            }
        }
    }

    /**
     * Unpack all values to an array
     * Reference: SimpleBitStorage.java lines 137-161
     */
    void unpack(int32_t* output) const override {
        int32_t dataLength = static_cast<int32_t>(m_data.size());
        int32_t outputOffset = 0;

        // Unpack all full longs
        for (int32_t i = 0; i < dataLength - 1; ++i) {
            int64_t cellValue = m_data[i];

            for (int32_t indexInLong = 0; indexInLong < m_valuesPerLong; ++indexInLong) {
                output[outputOffset + indexInLong] = static_cast<int32_t>(cellValue & m_mask);
                cellValue >>= m_bits;
            }

            outputOffset += m_valuesPerLong;
        }

        // Unpack remainder
        int32_t remainder = m_size - outputOffset;
        if (remainder > 0) {
            int64_t cellValue = m_data[dataLength - 1];

            for (int32_t indexInLong = 0; indexInLong < remainder; ++indexInLong) {
                output[outputOffset + indexInLong] = static_cast<int32_t>(cellValue & m_mask);
                cellValue >>= m_bits;
            }
        }
    }

    /**
     * Unpack to vector
     */
    std::vector<int32_t> unpack() const {
        std::vector<int32_t> result(m_size);
        unpack(result.data());
        return result;
    }

    /**
     * Create a copy of this storage
     * Reference: SimpleBitStorage.java lines 164-166
     */
    std::unique_ptr<BitStorage> copy() const override {
        return std::make_unique<SimpleBitStorage>(m_bits, m_size, m_data);
    }
};

// Define the static constexpr member
constexpr int32_t SimpleBitStorage::MAGIC[];

} // namespace util
} // namespace minecraft
