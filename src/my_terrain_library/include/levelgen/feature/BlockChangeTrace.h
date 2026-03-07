#pragma once

#include <functional>
#include <ostream>
#include <string>

namespace minecraft {
namespace levelgen {
namespace feature {

struct BlockChangeEvent {
    int x;
    int y;
    int z;
    std::string oldBlock;
    std::string newBlock;
};

struct BlockChangeTrace {
    static inline bool enabled = false;
    static inline std::ostream* stream = nullptr;
    static inline thread_local std::function<void(const BlockChangeEvent&)> callback;
    static inline thread_local std::string currentFeatureName;
    static inline thread_local int currentStep = -1;
    static inline thread_local int currentIndex = -1;

    static bool isEnabled() {
        return enabled && (stream != nullptr || static_cast<bool>(callback));
    }

    static void setCallback(std::function<void(const BlockChangeEvent&)> newCallback) {
        callback = std::move(newCallback);
    }

    static void clearCallback() {
        callback = nullptr;
    }

    static void log(int x, int y, int z, const std::string& oldBlock, const std::string& newBlock) {
        if (!enabled || oldBlock == newBlock) {
            return;
        }

        BlockChangeEvent event{x, y, z, oldBlock, newBlock};

        if (callback) {
            callback(event);
        }

        if (stream) {
            *stream << "BLOCK_SET STEP=" << currentStep << " IDX=" << currentIndex
                    << " " << currentFeatureName
                    << " pos=" << x << "," << y << "," << z
                    << " old=" << oldBlock << " new=" << newBlock << "\n";
        }
    }
};

} // namespace feature
} // namespace levelgen
} // namespace minecraft
