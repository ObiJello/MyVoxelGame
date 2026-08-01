// File: src/common/data/DataComponentType.hpp
//
// Mirrors net/minecraft/core/component/DataComponentType.java — a registered,
// typed key for stack-level metadata. Each DataComponentType is uniquely
// identified by name (matches MC's ResourceLocation-based registration).
//
// In MC, DataComponentType<T> is a parameterized record carrying a Codec (disk)
// and a StreamCodec (network). In C++ we split it:
//   - DataComponentTypeBase   — non-template base for type-erased storage in
//                                DataComponentMap (which holds heterogeneous
//                                component values). Carries the network id +
//                                type-erased codec entry points.
//   - DataComponentType<T>    — the user-facing typed key; carries the C++
//                                value type via T so set/get operations are
//                                compile-time type-checked, plus the typed
//                                serialize/deserialize function pointers
//                                (MC: DataComponentType.streamCodec()).
//
// networkId semantics: 0 = component never crosses the wire (in-memory only);
// non-zero ids are OUR protocol's stable ids assigned in DataComponents.hpp —
// deliberately NOT MC's registry ordinals (our wire protocol is bespoke; both
// endpoints ship in one binary).
#pragma once

#include "../network/PacketRegistry.hpp"  // Network::PacketBuffer / PacketReader (header-only)
#include <memory>
#include <string>

namespace Game {

    class DataComponentTypeBase {
    public:
        std::string name;
        uint32_t    networkId = 0;   // 0 = not network-serializable

        // Self-registers into the network-id registry when netId != 0
        // (registry lives in DataComponents.cpp; Meyers singleton so
        // static-init order across TUs is safe).
        explicit DataComponentTypeBase(std::string n, uint32_t netId = 0);
        virtual ~DataComponentTypeBase() = default;

        bool HasNetworkCodec() const { return networkId != 0; }

        // Type-erased codec — mirrors MC DataComponentType.streamCodec().
        // Only DataComponentMap's Serialize/Deserialize call these.
        virtual void SerializeErased(Network::PacketBuffer&, const void* /*value*/) const {}
        virtual std::shared_ptr<void> DeserializeErased(Network::PacketReader&) const { return nullptr; }
    };

    template<typename T>
    class DataComponentType : public DataComponentTypeBase {
    public:
        using SerializeFn   = void (*)(Network::PacketBuffer&, const T&);
        using DeserializeFn = T    (*)(Network::PacketReader&);

        explicit DataComponentType(std::string n, uint32_t netId = 0,
                                   SerializeFn ser = nullptr, DeserializeFn de = nullptr)
            : DataComponentTypeBase(std::move(n), netId), m_ser(ser), m_de(de) {}

        void SerializeErased(Network::PacketBuffer& buffer, const void* value) const override {
            if (m_ser) m_ser(buffer, *static_cast<const T*>(value));
        }

        std::shared_ptr<void> DeserializeErased(Network::PacketReader& reader) const override {
            if (!m_de) return nullptr;
            return std::make_shared<T>(m_de(reader));
        }

    private:
        SerializeFn   m_ser = nullptr;
        DeserializeFn m_de  = nullptr;
    };

    namespace DataComponents {
        // Look up a component type by its wire id. Returns nullptr for unknown
        // ids (protocol error — caller throws). Defined in DataComponents.cpp.
        const DataComponentTypeBase* ById(uint32_t networkId);
    }

} // namespace Game
