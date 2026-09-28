// File: src/client/renderer/gui/CreativeHotbars.cpp
#include "CreativeHotbars.hpp"
#include "AbstractContainerScreen.hpp"   // PlayerInventoryMenu

#include "client/entity/Player.hpp"
#include "client/input/KeyMapping.hpp"
#include "common/core/Log.hpp"
#include "common/core/SaveVersion.hpp"
#include "common/entity/Inventory.hpp"
#include "common/inventory/InventoryMenu.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "common/text/TextComponent.hpp"
#include "platform/GameDirectory.hpp"
#include "server/world/storage/NBTParser.hpp"
#include "server/world/storage/anvil/ItemStackNbt.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace Render::CreativeHotbars {

    namespace {

        std::array<Hotbar, kHotbarCount> g_hotbars;
        bool g_loaded = false;

        std::filesystem::path FilePath() {
            // MC HotbarManager: new File(gameDirectory, "hotbar.nbt").
            return std::filesystem::path(Platform::g_gameDirectory.GetGameDirectory()) / "hotbar.nbt";
        }

        // MC HotbarManager.load: NbtIo.read (uncompressed), each "i" a list
        // of item compounds decoded with ItemStack.OPTIONAL_CODEC.
        void Load() {
            g_loaded = true;
            for (auto& bar : g_hotbars) bar.fill(Game::ItemStack{});
            const std::filesystem::path path = FilePath();
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) return;
            std::ifstream f(path, std::ios::binary);
            if (!f) {
                Log::Warning("[Hotbars] cannot open %s", path.string().c_str());
                return;
            }
            const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (raw.empty()) return;
            const auto root = std::dynamic_pointer_cast<::World::NBTTagCompound>(::World::NBTParser::Parse(raw));
            if (!root) {
                Log::Warning("[Hotbars] %s is not an NBT compound", path.string().c_str());
                return;
            }
            for (int i = 0; i < kHotbarCount; ++i) {
                const auto list = std::dynamic_pointer_cast<::World::NBTTagList>(root->GetTag(std::to_string(i)));
                if (!list) continue;
                for (int slot = 0; slot < kSlots && slot < static_cast<int>(list->value.size()); ++slot) {
                    const auto item = std::dynamic_pointer_cast<::World::NBTTagCompound>(list->value[slot]);
                    if (item) g_hotbars[i][slot] = Game::Anvil::ReadItemStack(*item);
                }
            }
        }

        void EnsureLoaded() {
            if (!g_loaded) Load();
        }

        // MC HotbarManager.save: DataVersion plus the nine lists.
        void SaveFile() {
            Game::Nbt::Writer w;
            w.BeginRootCompound();
            w.Int("DataVersion", Game::Save::kDefaultDataVersion);
            for (int i = 0; i < kHotbarCount; ++i) {
                auto list = w.BeginList(std::to_string(i), Game::Nbt::TagType::Compound);
                for (const Game::ItemStack& stack : g_hotbars[i]) {
                    w.ListCompoundBegin(list);
                    if (!stack.IsEmpty()) Game::Anvil::WriteItemStackBody(w, stack);
                    w.ListCompoundEnd(list);
                }
                w.EndList(list);
            }
            w.EndRootCompound();
            if (!w.ok()) {
                Log::Warning("[Hotbars] failed to encode hotbar.nbt");
                return;
            }
            const std::filesystem::path path = FilePath();
            const std::filesystem::path tmp = path.string() + ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f) {
                    Log::Warning("[Hotbars] cannot write %s", tmp.string().c_str());
                    return;
                }
                const auto& bytes = w.Bytes();
                f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!f) {
                    Log::Warning("[Hotbars] cannot write %s", tmp.string().c_str());
                    return;
                }
            }
            std::error_code ec;
            std::filesystem::rename(tmp, path, ec);
            if (ec) Log::Warning("[Hotbars] cannot replace %s: %s", path.string().c_str(), ec.message().c_str());
        }

        std::string KeyName(const Input::KeyMapping* mapping) {
            return mapping ? mapping->key.DisplayName() : std::string("?");
        }

    } // namespace

    const Hotbar& Get(int index) {
        EnsureLoaded();
        return g_hotbars[static_cast<size_t>(std::clamp(index, 0, kHotbarCount - 1))];
    }

    bool IsEmpty(int index) {
        for (const auto& stack : Get(index)) {
            if (!stack.IsEmpty()) return false;
        }
        return true;
    }

    std::string SaveFrom(int index, const Game::ClientPlayer& player) {
        EnsureLoaded();
        if (index < 0 || index >= kHotbarCount) return {};
        // MC Hotbar.storeFrom: a copy of each hotbar stack.
        Hotbar& bar = g_hotbars[static_cast<size_t>(index)];
        for (int slot = 0; slot < kSlots; ++slot) {
            bar[slot] = player.inventory.GetSlot(Game::Inventory::HotbarToIndex(slot));
            if (bar[slot].IsEmpty()) bar[slot] = Game::ItemStack{};
        }
        SaveFile();
        // "Item hotbar saved (restore with %1$s+%2$s)" — the LOAD activator
        // and this hotbar's number key.
        return Game::Text::GetString(Game::Text::Component::Translatable(
            "inventory.hotbarSaved",
            {Game::Text::Component::Literal(KeyName(Input::Binds::LoadToolbarActivator)),
             Game::Text::Component::Literal(KeyName(Input::Binds::Hotbar[index]))}));
    }

    std::vector<Network::InventoryClickC2SPacket> BuildLoadClicks(int index, const Game::ClientPlayer& player) {
        std::vector<Network::InventoryClickC2SPacket> out;
        if (index < 0 || index >= kHotbarCount) return out;
        const Hotbar& bar = Get(index);
        Game::InventoryMenu* menu = PlayerInventoryMenu();
        for (int slot = 0; slot < kSlots; ++slot) {
            // MC: player.getInventory().setItem(i, stack) +
            // gameMode.handleCreativeModeItemAdd(stack, 36 + i) — here one
            // exact-stack creative slot write (CREATIVE_FILL_SLOT, button 1).
            Network::InventoryClickC2SPacket p{};
            p.slotIndex      = static_cast<int16_t>(Game::Inventory::HotbarToIndex(slot));
            p.button         = 1;
            p.action         = static_cast<uint8_t>(Network::ContainerInput::CREATIVE_FILL_SLOT);
            p.creativeStack  = bar[slot];
            p.creativeItemId = bar[slot].itemId;
            if (menu) {
                p.stateId     = menu->stateId;
                p.containerId = menu->containerId;
                menu->creative = player.IsCreative();
                const auto result = menu->DoClick(p);
                p.hasPrediction = true;
                for (uint8_t changed : result.changedSlots) {
                    if (menu->IsValidSlotIndex(changed)) {
                        p.predictedSlots.emplace_back(changed, menu->GetSlot(changed).GetItem());
                    }
                }
                p.predictedCarried = menu->getCarried();
            }
            out.push_back(std::move(p));
        }
        return out;
    }

} // namespace Render::CreativeHotbars
