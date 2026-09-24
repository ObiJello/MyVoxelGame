#include "TutorialHost.h"
#include "ConsoleStrings.h"
#include "ItemDescriptions.h"
#include "SurvivalRules.h"

#include <map>
#include <stdexcept>

namespace
{
    console::TutorialEngine *g_engine = nullptr;
    GAME_SETTINGS g_profile = {};

    // UTF-8 strings.resx text as the wide strings app.GetString returns.
    wstring widen(const char *text)
    {
        wstring out;
        const unsigned char *p = reinterpret_cast<const unsigned char *>(text);
        while (*p)
        {
            unsigned int c = *p++;
            int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
            if (extra) c &= 0x3F >> extra;
            for (int i = 0; i < extra && (*p & 0xC0) == 0x80; ++i) c = (c << 6) | (*p++ & 0x3F);
            out.push_back(static_cast<wchar_t>(c));
        }
        return out;
    }

    shared_ptr<MultiplayerLocalPlayer> g_localPlayer = make_shared<MultiplayerLocalPlayer>();
    Level g_level;
    StatsCounter g_stats;
    MultiPlayerGameMode g_gameMode;
    GameRuleDefinitions g_gameRules;

    Minecraft &minecraftInstance()
    {
        static Minecraft minecraft = [] {
            Minecraft m;
            m.player = g_localPlayer;
            m.localplayers[0] = g_localPlayer;
            m.localgameModes[0] = &g_gameMode;
            m.level = &g_level;
            m.stats[0] = &g_stats;
            return m;
        }();
        return minecraft;
    }
}

namespace TutorialHost
{
    void setEngine(console::TutorialEngine *engine) { g_engine = engine; }
    console::TutorialEngine &engine()
    {
        if (!g_engine) throw std::logic_error("tutorial used without an engine");
        return *g_engine;
    }
    GAME_SETTINGS &profile() { return g_profile; }
}

CMinecraftApp app;
UIController ui;
C4JInput InputManager;
C_4JProfile ProfileManager;
C4JStorage StorageManager;
CTelemetryManager *TelemetryManager = new CTelemetryManager();

DWORD GetTickCount() { return TutorialHost::engine().tickCount(); }

// Tile / Item tables -------------------------------------------------------

Tile *Tile::Table::operator[](int id)
{
    static std::map<int, std::unique_ptr<Tile>> tiles;
    auto &tile = tiles[id];
    if (!tile) tile = std::make_unique<Tile>(id);
    return tile.get();
}
Tile::Table Tile::tiles;

Item *Item::Table::operator[](int id)
{
    static std::map<int, std::unique_ptr<Item>> items;
    auto &item = items[id];
    if (!item) item = std::make_unique<Item>(id);
    return item.get();
}
Item::Table Item::items;
#define ITEM_POINTER(name, value) Item *Item::name = Item::items[value];
#include "generated/ItemPointers.inc"
#undef ITEM_POINTER

unsigned int Item::getDescriptionId(int iData) { return console::consoleDescriptionId(id, iData); }
unsigned int Item::getUseDescriptionId() { return console::consoleUseDescriptionId(id); }

// ItemInstance::getDescriptionId asks Item::getDescriptionId(instance), which
// TileItem answers without the data value.
unsigned int ItemInstance::getDescriptionId(int) { return console::consoleDescriptionId(id, id < 256 ? -1 : auxValue); }
unsigned int ItemInstance::getUseDescriptionId() { return console::consoleUseDescriptionId(id); }
bool ItemInstance::isDamageableItem() { return console::consoleItemMaxDamage(id) > 0; }
int ItemInstance::getMaxDamage() { return console::consoleItemMaxDamage(id); }
float ItemInstance::getDestroySpeed(Tile *tile) { return console::consoleItemDestroySpeed(id, tile->id); }

Material *Material::water = new Material();

MobEffect *MobEffect::effects[32] = {};
MobEffect *MobEffect::fireResistance = MobEffect::effects[12] = new MobEffect(12);

GameType *GameType::SURVIVAL = new GameType(0);
GameType *GameType::CREATIVE = new GameType(1);

// Player -------------------------------------------------------------------

void Player::setPlayerGamePrivilege(unsigned int &uiGamePrivileges, EPlayerGamePrivileges privilege, unsigned int value)
{
    if (value != 0) uiGamePrivileges |= (1 << privilege);
    else uiGamePrivileges &= ~(1 << privilege);
}

Vec3 *MultiplayerLocalPlayer::getPos(float)
{
    double x, y, z;
    TutorialHost::engine().playerPosition(x, y, z);
    return Vec3::newTemp(x, y, z);
}

bool MultiplayerLocalPlayer::isUnderLiquid(Material *material)
{
    return material == Material::water && TutorialHost::engine().playerUnderWater();
}

unsigned int MultiplayerLocalPlayer::getAllPlayerGamePrivileges()
{
    abilities.instabuild = TutorialHost::engine().playerCreative();
    return abilities.instabuild ? (1u << ePlayerGamePrivilege_CreativeMode) : 0u;
}

void ClientConnection::send(shared_ptr<Packet> packet)
{
    if (auto info = dynamic_pointer_cast<PlayerInfoPacket>(packet))
    {
        const bool creative = (info->m_playerPrivileges & (1u << Player::ePlayerGamePrivilege_CreativeMode)) != 0;
        TutorialHost::engine().setPlayerCreative(creative);
        g_localPlayer->abilities.instabuild = creative;
    }
}

// Minecraft / level --------------------------------------------------------

Minecraft *Minecraft::GetInstance()
{
    Minecraft &minecraft = minecraftInstance();
    minecraft.player->abilities.instabuild = TutorialHost::engine().playerCreative();
    return &minecraft;
}

void Minecraft::playerLeftTutorial(int) { TutorialHost::engine().playerLeftTutorial(); }

int Level::getTile(int x, int y, int z) { return TutorialHost::engine().tile(x, y, z); }
__int64 Level::getTime() { return TutorialHost::engine().levelTime(); }
void Level::setTime(__int64 time) { TutorialHost::engine().setLevelTime(time); }
void Level::setOverrideTimeOfDay(__int64 time) { TutorialHost::engine().setOverrideTimeOfDay(time); }
void MinecraftServer::SetTimeOfDay(__int64 time) { TutorialHost::engine().setOverrideTimeOfDay(time); }
void MinecraftServer::SetTime(__int64 time) { TutorialHost::engine().setLevelTime(time); }

// app / ui / input / storage -----------------------------------------------

LPCWSTR CMinecraftApp::GetString(int id)
{
    static std::map<int, wstring> cache;
    auto it = cache.find(id);
    if (it == cache.end()) it = cache.emplace(id, widen(console::consoleString(id))).first;
    return it->second.c_str();
}

unsigned char CMinecraftApp::GetGameSettings(int, eGameSetting eVal)
{
    return TutorialHost::engine().gameSetting(eVal);
}

GameRuleDefinitions *CMinecraftApp::getGameRuleDefinitions() { return &g_gameRules; }

AABB *GameRuleDefinitions::getNamedArea(const wstring &name)
{
    const console::TutorialArea *area = TutorialHost::engine().namedArea(name);
    if (!area) return NULL;
    static std::map<wstring, std::unique_ptr<AABB>> boxes;
    auto &box = boxes[name];
    if (!box) box.reset(AABB::newPermanent(area->x0, area->y0, area->z0, area->x1, area->y1, area->z1));
    else box->set(area->x0, area->y0, area->z0, area->x1, area->y1, area->z1);
    return box.get();
}

void UIController::SetTutorialVisible(int, bool visible) { TutorialHost::engine().setTutorialVisible(visible); }

void UIController::SetTutorialDescription(int, TutorialPopupInfo *info)
{
    console::TutorialPopup popup;
    popup.description = info->desc ? info->desc : L"";
    popup.title = info->title ? info->title : L"";
    popup.icon = info->icon;
    popup.iconAux = info->iAuxVal;
    popup.isFoil = info->isFoil;
    popup.allowFade = info->allowFade;
    popup.isReminder = info->isReminder;
    TutorialHost::engine().setTutorialDescription(popup);
}

bool UIController::IsPauseMenuDisplayed(int) { return TutorialHost::engine().pauseMenuDisplayed(); }
bool UIController::GetMenuDisplayed(int) { return TutorialHost::engine().menuDisplayed(); }

int C4JInput::GetValue(int, int action, bool) { return TutorialHost::engine().inputValue(action); }

unsigned int C4JInput::GetGameJoypadMaps(unsigned char, int action)
{
    switch (action)
    {
#define JOYPAD_MAP(action, buttons) case action: return buttons;
#include "generated/JoypadMap.inc"
#undef JOYPAD_MAP
    default: return 0;
    }
}

void *C4JStorage::GetGameDefinedProfileData(int) { return &g_profile; }

int UIScene_CraftingMenu::getCurrentGroup() { return TutorialHost::engine().craftingGroup(); }
bool UIScene_CraftingMenu::isItemSelected(int itemId) { return TutorialHost::engine().craftingItemSelected(itemId); }
