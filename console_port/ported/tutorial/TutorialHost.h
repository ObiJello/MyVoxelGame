#pragma once
// Stand-in for the console build's stdafx.h, included by the imported tutorial
// sources in this folder (tools/import_tutorial.py rewrites their includes).
//
// The tutorial classes are compiled unchanged against the small subset of the
// engine they use: Minecraft::GetInstance (local player, level, stats), app
// (strings, game settings, game rules), ui (tutorial popup, menu state),
// InputManager, StorageManager (profile completion bits), MinecraftServer
// time, and Tile/Item/ItemInstance lookups. Each stand-in forwards to the
// game through console::TutorialEngine (src/TutorialEngine.h) or to the
// generated tables (strings, item descriptions, survival rules).
//
// The stand-in class names are macro-renamed (Level -> TutorialHost_Level, ...)
// so they cannot collide with the ported world-generation classes of the same
// name in other translation units.
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "AABB.h"
#include "Vec3.h"
#include "TutorialEngine.h"

using namespace std;

// The PS3 build: profile data comes from StorageManager (Tutorial::setCompleted).
#ifndef __PS3__
#define __PS3__
#endif
// Release builds define _CONTENT_PACKAGE (CraftTask's debug wprintf).
#ifndef _CONTENT_PACKAGE
#define _CONTENT_PACKAGE
#endif

typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef const wchar_t *LPCWSTR;
using __int64 = std::int64_t; // as platform/stdafx.h
#ifndef AUTO_VAR
#define AUTO_VAR(_var, _val) auto _var = _val
#endif
#define ZeroMemory(p, n) memset((p), 0, (n))

#define Tile TutorialHost_Tile
#define Item TutorialHost_Item
#define ItemInstance TutorialHost_ItemInstance
#define Level TutorialHost_Level
#define Entity TutorialHost_Entity
#define Mob TutorialHost_Mob
#define Player TutorialHost_Player
#define Inventory TutorialHost_Inventory
#define Abilities TutorialHost_Abilities
#define Material TutorialHost_Material
#define MobEffect TutorialHost_MobEffect
#define GameType TutorialHost_GameType
#define Stat TutorialHost_Stat
#define StatsCounter TutorialHost_StatsCounter
#define Minecraft TutorialHost_Minecraft
#define MinecraftServer TutorialHost_MinecraftServer
#define MultiplayerLocalPlayer TutorialHost_MultiplayerLocalPlayer
#define MultiPlayerGameMode TutorialHost_MultiPlayerGameMode
#define ClientConnection TutorialHost_ClientConnection
#define INetworkPlayer TutorialHost_INetworkPlayer
#define Packet TutorialHost_Packet
#define PlayerInfoPacket TutorialHost_PlayerInfoPacket
#define UIScene TutorialHost_UIScene
#define UIScene_CraftingMenu TutorialHost_UIScene_CraftingMenu
#define Recipy TutorialHost_Recipy
#define SandStoneTile TutorialHost_SandStoneTile
#define TallGrass TutorialHost_TallGrass
#define StoneSlabTile TutorialHost_StoneSlabTile
#define TreeTile TutorialHost_TreeTile
#define WallTile TutorialHost_WallTile
#define QuartzBlockTile TutorialHost_QuartzBlockTile
#define GameRuleDefinitions TutorialHost_GameRuleDefinitions
#define CMinecraftApp TutorialHost_CMinecraftApp
#define UIController TutorialHost_UIController
#define C4JInput TutorialHost_C4JInput
#define C4JStorage TutorialHost_C4JStorage
#define C_4JProfile TutorialHost_C_4JProfile
#define CTelemetryManager TutorialHost_CTelemetryManager
#define GAME_SETTINGS TutorialHost_GAME_SETTINGS
#define TutorialPopupInfo TutorialHost_TutorialPopupInfo
#define app TutorialHost_app
#define ui TutorialHost_ui
#define InputManager TutorialHost_InputManager
#define ProfileManager TutorialHost_ProfileManager
#define StorageManager TutorialHost_StorageManager
#define TelemetryManager TutorialHost_TelemetryManager

// Original enums and constants, included unchanged.
#include "../../source_full/Minecraft.Client/PS3Media/strings.h"
#include "../../source_full/Minecraft.Client/Common/Potion_Macros.h"
#include "../../source_full/Minecraft.Client/PS3/Sentient/TelemetryEnum.h"
#include "generated/AppEnums.inc"
#include "generated/InstanceOf.inc"
#include "generated/JoyButtons.inc"
#include "TutorialEnum.h"

class Tutorial;
class TutorialHost_Item;

DWORD GetTickCount();

class Tile
{
public:
    explicit Tile(int id) : id(id) {}
    const int id;
#include "generated/TileIds.inc"
    // Tile::tiles[id]: one stand-in per ID, created on first use.
    class Table { public: Tile *operator[](int id); };
    static Table tiles;
};
#include "generated/TileConstants.inc"

class Recipy
{
public:
#include "generated/RecipyGroups.inc"
};

class ItemInstance;

class Item
{
public:
    explicit Item(int id) : id(id) {}
    const int id;
#include "generated/ItemIds.inc"
#define ITEM_POINTER(name, value) static Item *name;
#include "generated/ItemPointers.inc"
#undef ITEM_POINTER
    // Item::items[id]: one stand-in per ID, created on first use.
    class Table { public: Item *operator[](int id); };
    static Table items;
    unsigned int getDescriptionId(int iData = -1);
    unsigned int getUseDescriptionId();
};

class ItemInstance : public enable_shared_from_this<ItemInstance>
{
public:
    ItemInstance(int id, int count, int auxValue) : id(id), count(count), auxValue(auxValue) {}
    ItemInstance(Item *item, int count, int auxValue) : id(item->id), count(count), auxValue(auxValue) {}
    int id;
    int count;
    int auxValue;
    int getAuxValue() { return auxValue; }
    int getDamageValue() { return auxValue; }
    int GetCount() { return count; }
    bool isDamageableItem();
    int getMaxDamage();
    unsigned int getDescriptionId(int iData = -1);
    unsigned int getUseDescriptionId();
    float getDestroySpeed(Tile *tile);
};

class Material
{
public:
    static Material *water;
};

class MobEffect
{
public:
    explicit MobEffect(int id) : id(id) {}
    const int id;
    int getId() { return id; }
    static MobEffect *effects[32];
    static MobEffect *fireResistance;
};

class GameType
{
public:
    explicit GameType(int id) : id(id) {}
    const int id;
    static GameType *SURVIVAL;
    static GameType *CREATIVE;
};

class Stat {};
class StatsCounter
{
public:
    unsigned int getTotalValue(Stat *stat) { return 0; }
};

class Entity
{
public:
    virtual ~Entity() {}
};
class Mob : public Entity {};

class Inventory
{
public:
    shared_ptr<ItemInstance> selected;
    shared_ptr<ItemInstance> getSelected() { return selected; }
};

class Abilities
{
public:
    bool instabuild = false;
};

class INetworkPlayer
{
public:
    unsigned char GetSmallId() { return 0; }
};

class Packet
{
public:
    virtual ~Packet() {}
};

class PlayerInfoPacket : public Packet
{
public:
    PlayerInfoPacket(unsigned char networkSmallId, short playerColourIndex, unsigned int playerPrivileges)
        : m_networkSmallId(networkSmallId), m_playerColourIndex(playerColourIndex), m_playerPrivileges(playerPrivileges) {}
    unsigned char m_networkSmallId;
    short m_playerColourIndex;
    unsigned int m_playerPrivileges;
};

class ClientConnection
{
public:
    void send(shared_ptr<Packet> packet);
    INetworkPlayer *getNetworkPlayer() { return &networkPlayer; }
private:
    INetworkPlayer networkPlayer;
};

class Player : public Mob
{
public:
    enum EPlayerGamePrivileges
    {
        ePlayerGamePrivilege_CannotMine = 0,
        ePlayerGamePrivilege_CannotBuild,
        ePlayerGamePrivilege_CannotAttackMobs,
        ePlayerGamePrivilege_CannotAttackPlayers,
        ePlayerGamePrivilege_Op,
        ePlayerGamePrivilege_CanFly,
        ePlayerGamePrivilege_ClassicHunger,
        ePlayerGamePrivilege_Invisible,
        ePlayerGamePrivilege_Invulnerable,
        ePlayerGamePrivilege_CreativeMode,
    };
    static void setPlayerGamePrivilege(unsigned int &uiGamePrivileges, EPlayerGamePrivileges privilege, unsigned int value);
    Inventory *inventory = &ownInventory;
    Abilities abilities;
private:
    Inventory ownInventory;
};

class MultiplayerLocalPlayer : public Player
{
public:
    Vec3 *getPos(float a);
    bool isUnderLiquid(Material *material);
    unsigned int getAllPlayerGamePrivileges();
    int GetXboxPad() { return 0; }
    ClientConnection *connection = &ownConnection;
private:
    ClientConnection ownConnection;
};

class Level
{
public:
    static const int TICKS_PER_DAY = 20 * 60 * 20;
    int getTile(int x, int y, int z);
    __int64 getTime();
    void setTime(__int64 time);
    void setOverrideTimeOfDay(__int64 time);
};

class MultiPlayerGameMode {};

class Minecraft
{
public:
    static Minecraft *GetInstance();
    shared_ptr<MultiplayerLocalPlayer> player;
    shared_ptr<MultiplayerLocalPlayer> localplayers[4];
    MultiPlayerGameMode *localgameModes[4] = {};
    Level *level = nullptr;
    StatsCounter *stats[4] = {};
    void playerLeftTutorial(int iPad);
    void playerStartedTutorial(int iPad) {}
};

class MinecraftServer
{
public:
    static void SetTimeOfDay(__int64 time);
    static void SetTime(__int64 time);
};

class GameRuleDefinitions
{
public:
    AABB *getNamedArea(const wstring &name);
};

class CMinecraftApp
{
public:
    LPCWSTR GetString(int id);
    unsigned char GetGameSettings(int iPad, eGameSetting eVal);
    GameRuleDefinitions *getGameRuleDefinitions();
    int GetLocalPlayerCount() { return 1; }
    void DebugPrintf(const char *, ...) {}
};
extern CMinecraftApp app;

class UIScene
{
public:
    virtual ~UIScene() {}
};

class UIScene_CraftingMenu : public UIScene
{
public:
    int getCurrentGroup();
    bool isItemSelected(int itemId);
};

typedef struct _TutorialPopupInfo
{
    UIScene *interactScene;
    LPCWSTR desc;
    LPCWSTR title;
    int icon;
    int iAuxVal;
    bool isFoil;
    bool allowFade;
    bool isReminder;
    Tutorial *tutorial;
    _TutorialPopupInfo()
    {
        interactScene = NULL;
        desc = L"";
        title = L"";
        icon = -1;
        iAuxVal = 0;
        isFoil = false;
        allowFade = true;
        isReminder = false;
        tutorial = NULL;
    }
} TutorialPopupInfo;

class UIController
{
public:
    void SetTutorial(int iPad, Tutorial *tutorial) {}
    void SetTutorialVisible(int iPad, bool visible);
    void SetTutorialDescription(int iPad, TutorialPopupInfo *info);
    void RemoveInteractSceneReference(int iPad, UIScene *scene) {}
    bool IsPauseMenuDisplayed(int iPad);
    bool GetMenuDisplayed(int iPad);
};
extern UIController ui;

class C4JInput
{
public:
    int GetValue(int iPad, int action, bool bRepeat = false);
    unsigned char GetJoypadMapVal(int iPad);
    unsigned int GetGameJoypadMaps(unsigned char layout, int action);
};
extern C4JInput InputManager;

class C_4JProfile
{
public:
    int GetPrimaryPad() { return 0; }
};
extern C_4JProfile ProfileManager;

// The tutorial fields of GAME_SETTINGS (App_structs.h).
typedef struct
{
    unsigned char ucTutorialCompletion[TUTORIAL_PROFILE_STORAGE_BYTES];
    unsigned int uiSpecialTutorialBitmask;
    bool bSettingsChanged;
} GAME_SETTINGS;

class C4JStorage
{
public:
    void *GetGameDefinedProfileData(int iPad);
};
extern C4JStorage StorageManager;

class CTelemetryManager
{
public:
    void RecordEnemyKilledOrOvercome(int, int, int, int, int, int, int, ETelemetryChallenges) {}
};
extern CTelemetryManager *TelemetryManager;

// Host side: the engine the stand-ins forward to, and the profile data.
namespace TutorialHost
{
    void setEngine(console::TutorialEngine *engine);
    console::TutorialEngine &engine();
    GAME_SETTINGS &profile();
}
