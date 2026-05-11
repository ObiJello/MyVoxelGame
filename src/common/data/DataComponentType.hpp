// File: src/common/data/DataComponentType.hpp
//
// Mirrors net/minecraft/core/component/DataComponentType.java — a registered,
// typed key for stack-level metadata. Each DataComponentType is uniquely
// identified by name (matches MC's ResourceLocation-based registration). For
// now the type carries only the name + (deferred) per-component codec hooks
// for save/network serialization.
//
// In MC, DataComponentType<T> is a parameterized record. In C++ we split it:
//   - DataComponentTypeBase   — non-template base for type-erased storage in
//                                DataComponentMap (which holds heterogeneous
//                                component values).
//   - DataComponentType<T>    — the user-facing typed key; carries the C++
//                                value type via T so set/get operations are
//                                compile-time type-checked.
#pragma once

#include <string>

namespace Game {

    class DataComponentTypeBase {
    public:
        std::string name;
        explicit DataComponentTypeBase(std::string n) : name(std::move(n)) {}
        virtual ~DataComponentTypeBase() = default;
    };

    template<typename T>
    class DataComponentType : public DataComponentTypeBase {
    public:
        explicit DataComponentType(std::string n) : DataComponentTypeBase(std::move(n)) {}
    };

} // namespace Game
