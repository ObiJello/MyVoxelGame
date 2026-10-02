// File: src/common/data/components/ComponentTooltips.cpp
#include "ComponentTooltips.hpp"

#include <array>

namespace Game::ComponentTooltips {

    namespace {
        // Meyers singleton: providers register from static objects in other
        // TUs, before or after this one initialises.
        std::array<std::vector<Registration>, static_cast<size_t>(Slot::Count)>& Table() {
            static std::array<std::vector<Registration>, static_cast<size_t>(Slot::Count)> table;
            return table;
        }
    }

    void Register(Slot slot, const DataComponentTypeBase* type, Provider provider) {
        const auto index = static_cast<size_t>(slot);
        if (index >= static_cast<size_t>(Slot::Count) || !provider) return;
        Table()[index].push_back(Registration{slot, type, provider});
    }

    const std::vector<Registration>& ForSlot(Slot slot) {
        static const std::vector<Registration> kNone;
        const auto index = static_cast<size_t>(slot);
        if (index >= static_cast<size_t>(Slot::Count)) return kNone;
        return Table()[index];
    }

} // namespace Game::ComponentTooltips
