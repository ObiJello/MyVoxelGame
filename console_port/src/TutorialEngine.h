#pragma once
#include <cstdint>
#include <string>

namespace console {
// TutorialPopupInfo (UIStructs.h) as handed to UIController::SetTutorialDescription.
// An empty description hides the popup.
struct TutorialPopup {
    std::wstring description,title;
    int icon=-1,iconAux=0; // TUTORIAL_NO_ICON is -1
    bool isFoil=false,allowFade=true,isReminder=false;
};

// A NamedArea from the tutorial's GameRules (LevelRules).
struct TutorialArea { double x0,y0,z0,x1,y1,z1; };

// What the console tutorial reads from and asks of the game. The imported
// tutorial classes reach these through the stand-ins in
// ported/tutorial/TutorialHost.h (Minecraft::GetInstance, app, ui,
// InputManager, StorageManager, MinecraftServer); each method names the
// original call it answers. Player/pad index is always 0.
class TutorialEngine {
public:
    virtual ~TutorialEngine()=default;
    // localplayers[pad]->getPos(1)
    virtual void playerPosition(double& x,double& y,double& z)=0;
    // localplayers[pad]->isUnderLiquid(Material::water)
    virtual bool playerUnderWater()=0;
    // localplayers[pad]->abilities.instabuild
    virtual bool playerCreative()=0;
    // PlayerInfoPacket with ePlayerGamePrivilege_CreativeMode (ChangeStateConstraint)
    virtual void setPlayerCreative(bool creative)=0;
    // level->getTile
    virtual int tile(int x,int y,int z)=0;
    // level->getTime / setTime and MinecraftServer::SetTime
    virtual std::int64_t levelTime()=0;
    virtual void setLevelTime(std::int64_t time)=0;
    // level->setOverrideTimeOfDay and MinecraftServer::SetTimeOfDay (-1 clears)
    virtual void setOverrideTimeOfDay(std::int64_t time)=0;
    // InputManager.GetValue(pad, EControllerActions action)
    virtual int inputValue(int action)=0;
    // ui.GetMenuDisplayed / ui.IsPauseMenuDisplayed
    virtual bool menuDisplayed()=0;
    virtual bool pauseMenuDisplayed()=0;
    // GetTickCount: milliseconds
    virtual std::uint32_t tickCount()=0;
    // app.GetGameSettings(pad, eGameSetting)
    virtual unsigned char gameSetting(int setting)=0;
    // app.getGameRuleDefinitions()->getNamedArea; null when absent
    virtual const TutorialArea* namedArea(const std::wstring& name)=0;
    // ui.SetTutorialVisible / ui.SetTutorialDescription
    virtual void setTutorialVisible(bool visible)=0;
    virtual void setTutorialDescription(const TutorialPopup& popup)=0;
    // Minecraft::playerLeftTutorial
    virtual void playerLeftTutorial()=0;
    // UIScene_CraftingMenu::getCurrentGroup / isItemSelected (XuiCraftingTask)
    virtual int craftingGroup()=0;
    virtual bool craftingItemSelected(int itemId)=0;
};
}
