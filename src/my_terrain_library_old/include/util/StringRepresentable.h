#pragma once

#include <string>

namespace minecraft {
namespace util {

/**
 * StringRepresentable - Interface for objects that can be serialized to/from strings
 * Reference: net/minecraft/util/StringRepresentable.java
 *
 * Used by EnumProperty to serialize enum values to NBT.
 */
class StringRepresentable {
public:
    virtual ~StringRepresentable() = default;

    /**
     * Get the serialized name for this value
     * Reference: StringRepresentable.java getSerializedName()
     */
    virtual std::string getSerializedName() const = 0;
};

} // namespace util
} // namespace minecraft
