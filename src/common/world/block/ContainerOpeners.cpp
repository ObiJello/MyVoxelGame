// File: src/common/world/block/ContainerOpeners.cpp
#include "common/world/block/ContainerOpeners.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"

#include <map>
#include <string>
#include <tuple>

namespace Game::ContainerOpeners {

    namespace {

        enum class Kind : uint8_t { None, Barrel, ShulkerBox, CopperChest };

        bool EndsWith(const std::string& s, const char* suffix) {
            const size_t n = std::char_traits<char>::length(suffix);
            return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
        }

        Kind KindOf(BlockID id) {
            if (id == BlockID::Barrel) return Kind::Barrel;
            const std::string& slug = BlockRegistry::Get(id).registrySlug;
            if (slug == "copper_chest" || EndsWith(slug, "_copper_chest")) return Kind::CopperChest;
            if (slug == "shulker_box" ||
                (slug.size() > 12 && slug.compare(slug.size() - 12, 12, "_shulker_box") == 0)) {
                return Kind::ShulkerBox;
            }
            return Kind::None;
        }

        struct Key {
            const ILevelWrite* level;
            int x, y, z;
            bool operator<(const Key& o) const {
                return std::tie(level, x, y, z) < std::tie(o.level, o.x, o.y, o.z);
            }
        };

        std::map<Key, int>& Counts() {
            static std::map<Key, int> counts;
            return counts;
        }

        float Pitch(ILevelWrite& level) {
            JavaRandom* r = level.Random();
            return r ? r->NextFloat() * 0.1f + 0.9f : 1.0f;
        }

        // MC BarrelBlockEntity.playSound: half a block out of the facing.
        glm::dvec3 BarrelSoundPos(BlockState state, const glm::ivec3& pos) {
            glm::dvec3 at = glm::dvec3(pos) + glm::dvec3(0.5);
            const std::string_view facing = state.GetValueByName("facing");
            if (facing == "north") at.z -= 0.5;
            else if (facing == "south") at.z += 0.5;
            else if (facing == "west") at.x -= 0.5;
            else if (facing == "east") at.x += 0.5;
            else if (facing == "up") at.y += 0.5;
            else if (facing == "down") at.y -= 0.5;
            return at;
        }

        void Signal(ILevelWrite& level, const glm::ivec3& pos, bool open) {
            const BlockState state = level.GetBlockState(pos.x, pos.y, pos.z);
            switch (KindOf(state.Block())) {
                case Kind::Barrel:
                    // BarrelBlockEntity.openersCounter onOpen / onClose:
                    // playSound then updateBlockState (the OPEN property).
                    level.PlaySound(nullptr, BarrelSoundPos(state, pos),
                                    open ? SoundEvents::BARREL_OPEN : SoundEvents::BARREL_CLOSE,
                                    SoundSource::Blocks, 0.5f, Pitch(level));
                    if (state.HasProperty(PropertyId::OPEN)) {
                        level.SetBlock(pos.x, pos.y, pos.z, state.SetName(PropertyId::OPEN, open ? "true" : "false"),
                                       World::UpdateFlags::All);
                    }
                    break;
                case Kind::CopperChest: {
                    // ChestBlockEntity.playSound with CopperChestBlock
                    // .getHingeSound(weatherState, open): weathered and
                    // oxidized creak, the rest share the base hinge.
                    const std::string& slug = BlockRegistry::Get(state.Block()).registrySlug;
                    const bool weathered = slug.find("weathered_") != std::string::npos;
                    const bool oxidized  = slug.find("oxidized_") != std::string::npos;
                    const char* event =
                        weathered ? (open ? SoundEvents::COPPER_CHEST_WEATHERED_OPEN : SoundEvents::COPPER_CHEST_WEATHERED_CLOSE)
                      : oxidized  ? (open ? SoundEvents::COPPER_CHEST_OXIDIZED_OPEN  : SoundEvents::COPPER_CHEST_OXIDIZED_CLOSE)
                                  : (open ? SoundEvents::COPPER_CHEST_OPEN           : SoundEvents::COPPER_CHEST_CLOSE);
                    level.PlaySound(nullptr, pos, event, SoundSource::Blocks, 0.5f, Pitch(level));
                    break;
                }
                case Kind::ShulkerBox:
                    // ShulkerBoxBlockEntity.startOpen:185 / stopOpen:197.
                    level.PlaySound(nullptr, pos, open ? SoundEvents::SHULKER_BOX_OPEN : SoundEvents::SHULKER_BOX_CLOSE,
                                    SoundSource::Blocks, 0.5f, Pitch(level));
                    break;
                case Kind::None:
                    break;
            }
        }

    } // namespace

    bool Handles(const ILevelWrite& level, const glm::ivec3& pos) {
        return KindOf(level.GetBlock(pos.x, pos.y, pos.z)) != Kind::None;
    }

    void StartOpen(ILevelWrite& level, const glm::ivec3& pos) {
        if (!Handles(level, pos)) return;
        int& count = Counts()[Key{&level, pos.x, pos.y, pos.z}];
        if (++count == 1) Signal(level, pos, true);
    }

    void StopOpen(ILevelWrite& level, const glm::ivec3& pos) {
        auto& counts = Counts();
        const auto it = counts.find(Key{&level, pos.x, pos.y, pos.z});
        if (it == counts.end()) return;
        if (--it->second > 0) return;
        counts.erase(it);
        // The block may have been broken with the menu open: nothing to close.
        if (Handles(level, pos)) Signal(level, pos, false);
    }

} // namespace Game::ContainerOpeners
