// File: src/common/world/block/FlowerPotBlock.cpp
#include "common/world/block/FlowerPotBlock.hpp"

#include "common/core/Log.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"

#include <string>
#include <string_view>
#include <unordered_map>

namespace Game {

    namespace {

        // Filled once by RegisterFlowerPotBehaviors (block registration runs
        // before any level exists) and only read afterwards, from any thread.
        std::array<BlockID, BlockRegistry::Size> s_contentOf{};   // pot -> plant
        std::array<BlockID, BlockRegistry::Size> s_pottedOf{};    // plant -> pot
        std::array<bool, BlockRegistry::Size>    s_isPot{};

        size_t Index(BlockID id) { return static_cast<size_t>(id); }
        bool   InRange(BlockID id) { return Index(id) < BlockRegistry::Size; }

        // The plant slug a potted block's slug names. Blocks.java registers
        // POTTED_AZALEA / POTTED_FLOWERING_AZALEA under the ids
        // potted_azalea_bush / potted_flowering_azalea_bush; every other pot
        // (vanilla and engine) is "potted_" + the plant's id.
        std::string_view ContentSlugOf(std::string_view pottedSlug) {
            constexpr std::string_view kPrefix = "potted_";
            if (pottedSlug.substr(0, kPrefix.size()) != kPrefix) return {};
            const std::string_view content = pottedSlug.substr(kPrefix.size());
            if (content == "azalea_bush")           return "azalea";
            if (content == "flowering_azalea_bush") return "flowering_azalea";
            return content;
        }

        // MC `itemStack.consume(1, player)`: nothing is used up with infinite
        // materials (creative).
        void ConsumeOne(ItemStack& stack, const IUsePlayer* player) {
            if (player && player->isCreative()) return;
            if (stack.count > 0) --stack.count;
            if (stack.count <= 0) stack.Clear();
        }

        // FlowerPotBlock.useItemOn.
        UseResult PotUseItemOn(ItemStack& stack, ILevelWrite* level, const glm::ivec3& pos,
                               IUsePlayer* player, uint32_t /*hand*/,
                               const BlockHitResult& /*hit*/) {
            if (!level) return UseResult::Pass;
            // `itemStack.getItem() instanceof BlockItem blockItem ?
            //  POTTED_BY_CONTENT.getOrDefault(blockItem.getBlock(), AIR) : AIR`
            const BlockID held = (!stack.IsEmpty() && ItemRegistry::IsBlockItem(stack.itemId))
                                     ? ItemRegistry::ToBlock(stack.itemId) : BlockID::Air;
            const BlockID newContents = FlowerPot::PottedOf(held);
            if (newContents == BlockID::Air) return UseResult::TryEmptyHandInteraction;

            const BlockID self = level->GetBlockState(pos.x, pos.y, pos.z).Block();
            // `!this.isEmpty()` — a plant is already in the pot.
            if (self != BlockID::FlowerPot) return UseResult::Consume;

            // level.setBlockAndUpdate(pos, newContents) — on both sides, as in
            // MC: the client's write is the prediction the server's ack keeps.
            level->SetBlock(pos.x, pos.y, pos.z, BlockStates::Default(newContents),
                            World::UpdateFlags::All);
            // level.gameEvent(player, BLOCK_CHANGE, pos); (awardStat(POT_FLOWER)
            // — no statistics here.)
            level->GameEvent(player ? player->GameEventSource() : nullptr,
                             GameEventId::BlockChange, pos);
            ConsumeOne(stack, player);
            return UseResult::Success;
        }

        // FlowerPotBlock.useWithoutItem.
        UseResult PotUseWithoutItem(ILevelWrite* level, const glm::ivec3& pos, IUsePlayer* player,
                                    const BlockHitResult& /*hit*/) {
            if (!level) return UseResult::Pass;
            const BlockID self = level->GetBlockState(pos.x, pos.y, pos.z).Block();
            const BlockID plant = FlowerPot::ContentOf(self);
            // `this.isEmpty()` — nothing to take out.
            if (plant == BlockID::Air) return UseResult::Consume;

            // `ItemStack plant = new ItemStack(this.potted); if
            // (!player.addItem(plant)) player.drop(plant, false)`. Server
            // side: the inventory is the server's, and the client sees the
            // stack arrive with the slot sync.
            if (!level->IsClientSide() && player) {
                player->AddItemOrDrop(ItemStack(ItemRegistry::FromBlock(plant), 1));
            }
            level->SetBlock(pos.x, pos.y, pos.z, BlockStates::Default(BlockID::FlowerPot),
                            World::UpdateFlags::All);
            level->GameEvent(player ? player->GameEventSource() : nullptr,
                             GameEventId::BlockChange, pos);
            return UseResult::Success;
        }

    } // namespace

    namespace FlowerPot {

        BlockID ContentOf(BlockID pot) {
            return InRange(pot) ? s_contentOf[Index(pot)] : BlockID::Air;
        }

        BlockID PottedOf(BlockID content) {
            if (content == BlockID::Air || !InRange(content)) return BlockID::Air;
            return s_pottedOf[Index(content)];
        }

        bool IsFlowerPot(BlockID id) {
            return InRange(id) && s_isPot[Index(id)];
        }

        BlockID CloneBlock(BlockID id) {
            const BlockID plant = ContentOf(id);
            return plant != BlockID::Air ? plant : id;
        }

    } // namespace FlowerPot

    void RegisterFlowerPotBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        s_contentOf.fill(BlockID::Air);
        s_pottedOf.fill(BlockID::Air);
        s_isPot.fill(false);

        // First block per slug: promoted state variants share their base
        // block's slug, and the base row is the one a block item places.
        std::unordered_map<std::string_view, BlockID> bySlug;
        bySlug.reserve(blocks.size());
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (blocks[i].registrySlug.empty()) continue;
            bySlug.emplace(blocks[i].registrySlug, static_cast<BlockID>(i));
        }

        auto wire = [&](size_t i) {
            s_isPot[i] = true;
            blocks[i].useItemOn      = &PotUseItemOn;
            blocks[i].useWithoutItem = &PotUseWithoutItem;
        };

        wire(Index(BlockID::FlowerPot));
        int potted = 0, unmatched = 0;
        for (size_t i = 0; i < blocks.size(); ++i) {
            const std::string_view contentSlug = ContentSlugOf(blocks[i].registrySlug);
            if (contentSlug.empty()) continue;
            // Only the base row of a potted slug (no promoted variants exist
            // for pots, but the rule keeps the map one-to-one).
            if (bySlug.at(blocks[i].registrySlug) != static_cast<BlockID>(i)) continue;
            const auto it = bySlug.find(contentSlug);
            if (it == bySlug.end()) {
                ++unmatched;
                Log::Warning("[FlowerPot] '%s' names no plant block '%.*s' — it cannot be "
                             "filled or emptied", blocks[i].registrySlug.c_str(),
                             static_cast<int>(contentSlug.size()), contentSlug.data());
                continue;
            }
            s_contentOf[i] = it->second;
            s_pottedOf[Index(it->second)] = static_cast<BlockID>(i);
            wire(i);
            ++potted;
        }
        Log::Info("[FlowerPot] %d potted plants wired (%d unmatched)", potted, unmatched);
    }

} // namespace Game
