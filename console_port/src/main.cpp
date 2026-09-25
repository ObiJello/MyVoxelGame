#include "Renderer.h"
#include "WorldLibrary.h"
#include "ConsoleSeed.h"
#include "LadderShape.h"
#include "ItemIcons.h"
#include "FurnaceSimulation.h"
#include "BrewingSimulation.h"
#include "CreativeCatalog.h"
#include "ItemNames.h"
#include "ItemPlacement.h"
#include "SpawnEggColors.h"
#include "SurvivalRules.h"
#include "CraftingRecipes.h"
#include "TutorialSession.h"
#include "TutorialContainers.h"
#include "RichText.h"
#include "GameSettings.h"
#include "ConsoleStrings.h"
#include "ItemDescriptions.h"
#include "ConsoleMenus.h"
#include "ConsoleCredits.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <string_view>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <cwchar>
#include <cstdio>
#include <set>

using namespace console;
namespace {
std::array<std::array<float,2>,4> brewingSlotPositions(float cx){
    // xuiscene_brewingstand.xui positions on the PS3 1280x720 canvas / 2.
    return {{{{cx-48.f,113.5f}},{{cx-13.5f,124.f}},{{cx+21.f,113.5f}},{{cx-13.5f,70.f}}}};
}
std::array<std::array<float,2>,3> furnaceSlotPositions(float cx){
    // xuiscene_furnace.xui ingredient, fuel, and result positions / 2.
    return {{{{cx-30.5f,71.5f}},{{cx-30.5f,118.5f}},{{cx+47.f,90.5f}}}};
}
std::array<float,2> carriedSlotPosition(float cx,int slot,float inventoryY,float hotbarY){
    return {cx-94+(slot%9)*21,slot<9?hotbarY:inventoryY+((slot-9)/9)*21};
}
int carriedClickSlot(float x,float y,float cx,float inventoryY,float hotbarY){
    const int col=int(std::floor((x-(cx-94))/21));
    if(col<0 || col>=9)return -1;
    if(y>=hotbarY && y<hotbarY+21)return col;
    if(y>=inventoryY && y<inventoryY+63)return 9+int((y-inventoryY)/21)*9+col;
    return -1;
}
std::string itemDisplayName(int id,int damage){
 if(id==373)return potionDisplayName(damage);
 if(const char* sourceName=consoleSourceItemName(id,damage))return sourceName;
 if(id<256)return blockName(static_cast<Block>(id));
 return consoleItemIcon(id,damage).name;
}
void drawItemIcon(Renderer& r,int id,int damage,float x,float y,float size=16){
 if(id<256){r.blockIcon(static_cast<Block>(id),x,y,size,damage);return;}
 const auto cell=[](int tile){return glm::vec4((tile%16)/16.f,(tile/16)/16.f,
                                         (tile%16+1)/16.f,(tile/16+1)/16.f);};
 const auto tint=[](std::uint32_t color){return glm::vec4(((color>>16)&255)/255.f,
     ((color>>8)&255)/255.f,(color&255)/255.f,1);};
 if(id==373){
  // PotionItem draws the tinted contents first, then the drinkable or splash
  // bottle outline. PreStitchedTextureMap assigns atlas cells 141, 140, 154.
  const std::uint32_t color=potionColor(damage);
  r.sprite("items",x,y,size,size,cell(141),tint(color));
  r.sprite("items",x,y,size,size,cell((damage&0x4000)?154:140));
  return;
 }
 if(id==383){
  // MonsterPlacerItem::getLayerIcon/getColor: source atlas slots 153 and 169.
  const auto colors=consoleSourceEggColors(damage);
  r.sprite("items",x,y,size,size,cell(153),tint(colors.base));
  r.sprite("items",x,y,size,size,cell(169),tint(colors.spots));
  return;
 }
 const auto icon=consoleItemIcon(id,damage);
 if(icon.tile>=0)r.sprite("items",x,y,size,size,
     {(icon.tile%16)/16.f,(icon.tile/16)/16.f,(icon.tile%16+1)/16.f,(icon.tile/16+1)/16.f});
 else r.text("?",x+size/3,y+size/4);
}
void drawSlotFrame(Renderer& r,float x,float y,float size,bool selected,bool holder=true){
 if(holder)r.sprite("icon_holder",x,y,size,size);
 if(selected){
  // The source ItemButton/ItemButtonBrewing focus visual overlays a green
  // gradient on the 38- or 48-pixel inset without replacing the slot image.
  r.rect(x+1,y+1,size-2,size-2,{.48f,.72f,.48f,.52f});
 }
}
// AddItemRuleDefinition::addItemToContainer: a fixed slot or the first space.
void addRuleItem(World& world,const TutorialItem& item){
    if(item.slot>=0 && item.slot<36)world.setCarriedItem(item.slot,item.id,item.count,item.aux,item.dataTag);
    else world.addCarriedItem(item.id,item.count,item.aux);
}
// UpdatePlayerRuleDefinition::postProcessPlayer (health, food, items); the
// position and rotation are applied when the player is placed.
void applyUpdatePlayer(World& world){
    const auto* rules=world.tutorialRules();
    if(!rules || !rules->updatePlayer)return;
    const auto& update=*rules->updatePlayer;
    if(update.hasHealth)world.setPlayerHealth(update.health);
    if(update.hasFood)world.setPlayerFood(update.food);
    for(const auto& item:update.items)addRuleItem(world,item);
}
// PlayerList::placeNewPlayer: every new player is given a map in slot 9
// (Item::map_Id), then the level rules post-process the player.
void setUpNewPlayer(World& world){
    world.setCarriedItem(9,358,1,0);
    applyUpdatePlayer(world);
}
std::wstring widen(const std::string& text){
    std::wstring out;
    for(std::size_t i=0;i<text.size();){
        unsigned c=static_cast<unsigned char>(text[i++]);
        const int extra=c>=0xF0?3:c>=0xE0?2:c>=0xC0?1:0;
        if(extra)c&=0x3F>>extra;
        for(int k=0;k<extra && i<text.size();++k)c=(c<<6)|(static_cast<unsigned char>(text[i++])&0x3F);
        out.push_back(static_cast<wchar_t>(c));
    }
    return out;
}
std::string narrow(const std::wstring& text){
    std::string out;
    for(wchar_t w:text){
        const auto c=static_cast<std::uint32_t>(w);
        if(c<0x80)out+=char(c);
        else if(c<0x800){out+=char(0xC0|(c>>6));out+=char(0x80|(c&0x3F));}
        else {out+=char(0xE0|(c>>12));out+=char(0x80|((c>>6)&0x3F));out+=char(0x80|(c&0x3F));}
    }
    return out;
}
// app.GetString with the IDS_* name.
std::string consoleText(const char* name){return consoleString(consoleStringId(name));}
// The PS3 buttons (_360_JOY_BUTTON_*) held on a GLFW gamepad, sticks as
// directions; southpaw swaps the sticks (C4JInput with eGameSetting_ControlSouthPaw).
unsigned padButtons(const GLFWgamepadstate& pad,bool southpaw){
    unsigned bits=0;
    auto button=[&](int b,unsigned bit){if(pad.buttons[b]==GLFW_PRESS)bits|=bit;};
    button(GLFW_GAMEPAD_BUTTON_A,0x1);button(GLFW_GAMEPAD_BUTTON_B,0x2);button(GLFW_GAMEPAD_BUTTON_X,0x4);
    button(GLFW_GAMEPAD_BUTTON_Y,0x8);button(GLFW_GAMEPAD_BUTTON_START,0x10);button(GLFW_GAMEPAD_BUTTON_BACK,0x20);
    button(GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER,0x40);button(GLFW_GAMEPAD_BUTTON_LEFT_BUMPER,0x80);
    button(GLFW_GAMEPAD_BUTTON_RIGHT_THUMB,0x100);button(GLFW_GAMEPAD_BUTTON_LEFT_THUMB,0x200);
    button(GLFW_GAMEPAD_BUTTON_DPAD_UP,0x400);button(GLFW_GAMEPAD_BUTTON_DPAD_DOWN,0x800);
    button(GLFW_GAMEPAD_BUTTON_DPAD_LEFT,0x1000);button(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT,0x2000);
    const int lx=southpaw?GLFW_GAMEPAD_AXIS_RIGHT_X:GLFW_GAMEPAD_AXIS_LEFT_X,ly=southpaw?GLFW_GAMEPAD_AXIS_RIGHT_Y:GLFW_GAMEPAD_AXIS_LEFT_Y;
    const int rx=southpaw?GLFW_GAMEPAD_AXIS_LEFT_X:GLFW_GAMEPAD_AXIS_RIGHT_X,ry=southpaw?GLFW_GAMEPAD_AXIS_LEFT_Y:GLFW_GAMEPAD_AXIS_RIGHT_Y;
    if(pad.axes[lx]>.5f)bits|=0x4000;if(pad.axes[lx]<-.5f)bits|=0x8000;
    if(pad.axes[ly]>.5f)bits|=0x100000;if(pad.axes[ly]<-.5f)bits|=0x200000;
    if(pad.axes[rx]>.5f)bits|=0x40000;if(pad.axes[rx]<-.5f)bits|=0x80000;
    if(pad.axes[ry]>.5f)bits|=0x10000;if(pad.axes[ry]<-.5f)bits|=0x20000;
    if(pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER]>.1f)bits|=0x400000;
    if(pad.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER]>.1f)bits|=0x800000;
    return bits;
}
struct Box { float x=0,y=0,w=0,h=0; bool contains(double px,double py)const{return w>0 && px>=x && px<x+w && py>=y && py<y+h;} };
enum class Screen { Menu, FindingSeed, Loading, Playing, Inventory, Chest, Furnace, Brewing, Crafting };
struct App {
    enum class LoadKind { Create, Tutorial, ArchivedTutorial, Existing };
    struct LoadResult {
        std::unique_ptr<World> next;
        std::shared_ptr<World> previous;
        std::filesystem::path path,previousPath;
        bool previousLoaded=false;
        std::string error;
        LoadKind kind=LoadKind::Existing;
    };
    GLFWwindow* window=nullptr;
    std::unique_ptr<Renderer> renderer;
    World world;
    Screen screen=Screen::Menu;
    Vec3 position{};
    double yaw=.65,pitch=-.12,verticalSpeed=0,viewDistance=120;
    // Explosion::explode adds to the player's xd/zd; the port keeps it in
    // blocks per second and lets Mob::travel's friction wear it off.
    Vec3 knockback{};
    double worldTickSeconds=0;
    int potionUseTicks=0,potionUseSlot=-1;
    bool horizontalCollision=false,grounded=false,flying=false,loaded=false,enderChestOpen=false,furnaceFuelTarget=false;
    int selection=0,slot=0,chestX=0,chestY=0,chestZ=0,chestSlots=27;
    // The Create World text field being typed into (0 name, 1 seed).
    int editingField=-1;
    // The key that opened a text field also sends its character; skip it.
    bool editCharGuard=false;
    double lastJumpPress=-1;
    int inventoryCategory=0,brewingBottleTarget=0;
    std::array<int,8> creativePage{};
    bool flatWorld=false;
    // Survival: CreateWorldMenu game mode, fall tracking, mining progress
    // (MultiPlayerGameMode destroyProgress/destroyDelay) and item use timers.
    bool survivalWorld=true,craftingTable=false;
    double fallDistance=0;
    float mineProgress=0;
    int mineX=0,mineY=-1,mineZ=0,mineDelay=0,eatUseTicks=0,craftScroll=0;
    // Recipy::eGroupType tabs in IUIScene_CraftingMenu order.
    static constexpr std::array<const char*,7> craftingGroups{"Structure","Tool","Food","Armour","Mechanism","Transport","Decoration"};
    int craftingGroup=0;
    std::vector<const CraftingRecipe*> craftingList()const{
        std::vector<const CraftingRecipe*> list;
        for(const auto& recipe:consoleCraftingRecipes())
            if((craftingTable || !recipe.needsTable) && std::string_view(recipe.group)==craftingGroups[craftingGroup])
                list.push_back(&recipe);
        return list;
    }
    std::string worldName="New World";
    std::filesystem::path activeSave;
    std::vector<SavedWorld> savedWorlds;
    double lastX=0,lastY=0,toastUntil=0,lastSave=0,lastEdit=0,ignoreMouseUntil=0;
    bool scripted=false,screenshotRequested=false;
    bool mouseReady=false;
    std::string seedText,toast;
    std::unique_ptr<ConsoleSeedSearch> seedSearch;
    std::future<LoadResult> loadJob;
    std::optional<LoadResult> loadReady;
    std::shared_ptr<std::atomic<int>> loadPhase;
    std::uint64_t seedAttempts=0;
    std::filesystem::path dataDir;
    GLFWgamepadstate previousPad{};
    unsigned padBits=0,previousPadBits=0;
    bool leftMouse=false,rightMouse=false,usedBlockHeld=false;
    // ---- Console tutorial (FullTutorialMode) and the player's profile ----
    GameSettings settings;
    // The front-end and pause menus (UIScene_*).
    ConsoleMenus menus{settings};
    float howToPlayScroll=0;
    int howToPlayShown=-1;
    bool tutorialPaused=false;
    std::unique_ptr<TutorialSession> tutorial;
    // UIComponent_TutorialPopup: the description, fade timer and visibility.
    TutorialPopup tutorialPopup;
    bool tutorialPopupVisible=false,tutorialPopupHidden=false,tutorialModeActive=false;
    double tutorialPopupTime=0;
    // InputManager.GetValue for the current frame (EControllerActions).
    std::array<int,TutorialSession::ActionCount> actionValues{};
    double lookDX=0,lookDY=0;
    int scrollSteps=0;
    bool padInputLast=false;
    // Engine state compared each tick to raise tutorial events.
    std::vector<ContainerItem> tutorialCarried;
    std::vector<PotionEffect> tutorialEffects;
    int tutorialSlot=-1,tutorialFood=-1;
    std::set<int> collectedGoals;
    std::filesystem::path settingsPath()const{return dataDir/"settings.dat";}
    void saveSettings(){try{settings.save(settingsPath());}catch(const std::exception& e){message(e.what());}}
    bool inputAllowed(int action)const{return !tutorial || tutorial->isInputAllowed(action);}
    // eGameSetting_Sensitivity_InGame (0-200%) and eGameSetting_ControlInvertLook.
    double lookSensitivity()const{return .0022*settings.get(GameSetting::SensitivityInGame)/100.0;}
    bool invertLook()const{return settings.get(GameSetting::ControlInvertLook)!=0;}
    int controlScheme()const{return settings.get(GameSetting::ControlScheme);}
    bool padAction(int action)const{return (padBits&consoleJoypadButtons(action,controlScheme()))!=0;}
    bool padActionPressed(int action)const{
        const unsigned bits=consoleJoypadButtons(action,controlScheme());
        return (padBits&bits) && !(previousPadBits&bits);
    }
    bool pauseMenuShown()const{return screen==Screen::Menu && loaded && menus.active() && menus.root()==MenuScene::Pause;}
    bool deathMenuShown()const{return screen==Screen::Menu && loaded && menus.active() && menus.root()==MenuScene::Death;}
    struct TutorialBridge final:TutorialEngine{
        App& app;
        explicit TutorialBridge(App& owner):app(owner){}
        // Level coordinates: the client world is offset by half its size.
        void playerPosition(double& x,double& y,double& z)override{
            x=app.position.x-World::width/2;y=app.position.y+1.62;z=app.position.z-World::depth/2;
        }
        bool playerUnderWater()override{return app.world.playerUnderWater();}
        bool playerCreative()override{return !app.world.survival();}
        void setPlayerCreative(bool creative)override{app.world.setSurvival(!creative);}
        int tile(int x,int y,int z)override{return app.world.get(x+World::width/2,y,z+World::depth/2);}
        std::int64_t levelTime()override{return app.world.time();}
        void setLevelTime(std::int64_t time)override{app.world.setTime(time);}
        void setOverrideTimeOfDay(std::int64_t time)override{app.world.setOverrideTimeOfDay(time);}
        int inputValue(int action)override{return action>=0 && action<TutorialSession::ActionCount?app.actionValues[action]:0;}
        bool menuDisplayed()override{return app.screen!=Screen::Playing;}
        bool pauseMenuDisplayed()override{return app.pauseMenuShown();}
        std::uint32_t tickCount()override{return static_cast<std::uint32_t>(glfwGetTime()*1000);}
        unsigned char gameSetting(int setting)override{return app.settings.get(static_cast<GameSetting>(setting));}
        const TutorialArea* namedArea(const std::wstring& name)override{
            const auto* rules=app.world.tutorialRules();if(!rules)return nullptr;
            auto it=rules->namedAreas.find(name);return it==rules->namedAreas.end()?nullptr:&it->second;
        }
        void setTutorialVisible(bool visible)override{
            app.tutorialPopupHidden=!visible;
            if(visible)app.tutorialPopupTime=glfwGetTime();
        }
        void setTutorialDescription(const TutorialPopup& popup)override{
            app.tutorialPopup=popup;app.tutorialPopupVisible=!popup.description.empty();app.tutorialPopupTime=glfwGetTime();
        }
        void playerLeftTutorial()override{app.tutorialModeActive=false;app.world.setTutorialSpawning(true);}
        int craftingGroup()override{return app.craftingGroup;}
        bool craftingItemSelected(int itemId)override{
            const auto list=app.craftingList();
            return app.selection>=0 && app.selection<int(list.size()) && list[app.selection]->id==itemId;
        }
    };
    std::unique_ptr<TutorialBridge> tutorialBridge;
    // UIComponent_TutorialPopup visibility: SetVisible plus the fade timer.
    bool tutorialPopupShown()const{
        if(!tutorial || !tutorialPopupVisible || tutorialPopupHidden)return false;
        return !tutorialPopup.allowFade || glfwGetTime()-tutorialPopupTime<7.0; // TUTORIAL_DISPLAY_MESSAGE_TIME
    }
    void startTutorialSession(){
        tutorialBridge=std::make_unique<TutorialBridge>(*this);
        tutorialPopupVisible=tutorialPopupHidden=false;
        tutorialCarried=world.carriedItems();tutorialEffects=world.activePotionEffects();
        tutorialSlot=slot;tutorialFood=world.playerFoodLevel();collectedGoals.clear();tutorialPaused=false;
        world.setTutorialSpawning(false);
        tutorial=std::make_unique<TutorialSession>(*tutorialBridge,settings.tutorialCompletion());
        tutorialModeActive=true;
    }
    void endTutorialSession(){
        if(tutorial){
            if(tutorial->profileChanged()){settings.tutorialCompletion()=tutorial->profile();saveSettings();}
            tutorial.reset();
        }
        tutorialBridge.reset();tutorialPopupVisible=false;tutorialModeActive=false;
    }
    // The tutorial menu states of the UIScene_*Menu constructors.
    static int tutorialMenuState(Screen s,bool creative,bool table){
        switch(s){
        case Screen::Inventory:return creative?TutorialSession::CreativeInventoryMenu:TutorialSession::InventoryMenu;
        case Screen::Crafting:return table?TutorialSession::Crafting3x3Menu:TutorialSession::Crafting2x2Menu;
        case Screen::Furnace:return TutorialSession::FurnaceMenu;
        case Screen::Brewing:return TutorialSession::BrewingMenu;
        case Screen::Chest:return TutorialSession::ContainerMenu;
        default:return -1;
        }
    }
    ContainerItem selectedItem()const{return world.carriedItems()[slot];}
    int inventoryCount()const{
        if(inventoryCategory==creativeTabCount())return 36;
        const auto tab=creativeTab(inventoryCategory);
        return std::min(50,std::max(0,int(tab.items.size())-creativePage[inventoryCategory]*50));
    }
    CreativeEntry selectedCreativeEntry()const{
        const auto tab=creativeTab(inventoryCategory);
        return tab.items[creativePage[inventoryCategory]*50+selection];
    }
    void turnCreativePage(int delta){
        if(inventoryCategory>=creativeTabCount())return;
        const int pages=std::max(1,int((creativeTab(inventoryCategory).items.size()+49)/50));
        creativePage[inventoryCategory]=std::clamp(creativePage[inventoryCategory]+delta,0,pages-1);
        selection=0;
    }
    bool equipCreativeItem(int id,int damage=0){
        const auto carried=world.carriedItems();
        const bool quick=glfwGetKey(window,GLFW_KEY_LEFT_SHIFT)==GLFW_PRESS ||
                         glfwGetKey(window,GLFW_KEY_RIGHT_SHIFT)==GLFW_PRESS;
        const int limit=std::min(64,consoleItemStackLimit(id));
        int count=quick?limit:1,target=-1;
        if(limit>1)for(int i=0;i<9;++i)if(carried[i].id==id && carried[i].damage==damage &&
            carried[i].marker==0 && !carried[i].enchanted && carried[i].count+count<=limit){
            target=i;count+=carried[i].count;break;
        }
        if(target<0)for(int i=0;i<9;++i)if(!carried[i].id){target=i;break;}
        if(target<0)target=slot;
        if(!world.setCreativeHotbarItem(target,id,damage,count))return false;
        slot=target;return true;
    }
    std::filesystem::path savePath()const{return activeSave.empty()?dataDir/"world.inner":activeSave;}
    void message(const std::string& s){toast=s;toastUntil=glfwGetTime()+5;}
    void change(Screen s){
        if(tutorial && s!=screen){
            // The container menus switch the tutorial state with their scene
            // and restore it on close.
            const int before=tutorialMenuState(screen,!world.survival(),craftingTable);
            const Screen previous=screen;screen=s;
            if(before>=0)tutorial->closeMenu();
            const int after=tutorialMenuState(s,!world.survival(),craftingTable);
            if(after>=0)tutorial->openMenu(after,s==Screen::Crafting);
            screen=previous;
        }
        screen=s;selection=0;editingField=-1;
        // UIScene_PauseMenu hides the tutorial popup while it is open.
        if(tutorial && pauseMenuShown()!=tutorialPaused){tutorialPaused=pauseMenuShown();tutorial->showTutorialPopup(!tutorialPaused);}
        if(s!=Screen::FindingSeed)seedSearch.reset();
        mouseReady=false;leftMouse=rightMouse=usedBlockHeld=false;potionUseTicks=0;potionUseSlot=-1;ignoreMouseUntil=glfwGetTime()+.15;
        glfwSetInputMode(window,GLFW_CURSOR,s==Screen::Playing?GLFW_CURSOR_DISABLED:GLFW_CURSOR_NORMAL);}
    // Open a menu stack: the main menu, the pause menu or the death menu.
    void openMenu(MenuScene root){
        MenuContext context;
        context.inGame=loaded;context.creative=loaded && !world.survival();
        if(root==MenuScene::MainMenu){
            try{savedWorlds=listSavedWorlds(dataDir);}catch(const std::exception& e){message(e.what());savedWorlds.clear();}
            for(const auto& saved:savedWorlds)context.saves.push_back(saved.name.substr(0,25));
        }
        menus.show(root,context);howToPlayShown=-1;
        change(Screen::Menu);
    }
    void toTitle(){endTutorialSession();loaded=false;openMenu(MenuScene::MainMenu);}
    void respawn(){
        world.respawnPlayer();position=world.spawn();verticalSpeed=0;knockback={};fallDistance=0;world.setPlayerPosition(position);
        // PlayerList::respawn: in the tutorial, until the food bar lesson is
        // done, the player gets their health, hunger and steak back.
        if(tutorialModeActive && tutorial && !tutorial->isStateCompleted(TutorialSession::FoodBar))applyUpdatePlayer(world);
        change(Screen::Playing);
    }
    void handleMenuEvent(const MenuEvent& event){
        switch(event.action){
        case MenuAction::None:break;
        case MenuAction::StartTutorial:startTutorial();break;
        case MenuAction::StartClassicTutorial:startTutorial(true);break;
        case MenuAction::CreateWorld:{
            const auto& options=menus.newWorld();
            worldName=options.name;seedText=options.seed;survivalWorld=options.survival;flatWorld=options.superflat;
            start(true);break;
        }
        case MenuAction::LoadSave:
            if(event.index>=0 && event.index<int(savedWorlds.size())){
                const auto& entry=savedWorlds[event.index];
                if(entry.readable)start(false,entry.path);else message("This saved world could not be read.");
            }
            break;
        case MenuAction::ResumeGame:menus.close();change(Screen::Playing);break;
        case MenuAction::SaveGame:try{save();}catch(const std::exception& e){message(e.what());}break;
        case MenuAction::ExitAndSave:
            try{save();}catch(const std::exception& e){message(e.what());break;}
            toTitle();break;
        case MenuAction::ExitWithoutSaving:toTitle();break;
        case MenuAction::Respawn:menus.close();respawn();break;
        case MenuAction::EditWorldName:editingField=0;editCharGuard=true;break;
        case MenuAction::EditSeed:editingField=1;editCharGuard=true;break;
        case MenuAction::SettingsChanged:saveSettings();break;
        }
    }
    void save(){if(loaded){world.save(savePath());lastSave=glfwGetTime();}}
    void beginLoad(LoadKind kind,const std::filesystem::path& selected={},std::int64_t value=0){
        if(loadJob.valid() || loadReady)return;
        const bool previousLoaded=loaded;
        const auto previousPath=savePath(),destinationDir=dataDir;
        const auto name=worldName;
        const bool flat=flatWorld,survivalMode=survivalWorld;
        std::shared_ptr<World> previous;
        if(previousLoaded){previous=std::make_shared<World>();previous->swapWith(world);}
        endTutorialSession();
        loaded=false;
        change(Screen::Loading);
        loadPhase=std::make_shared<std::atomic<int>>(previousLoaded?0:1);
        auto phase=loadPhase;
        try{loadJob=std::async(std::launch::async,[kind,selected,value,destinationDir,name,flat,survivalMode,previousPath,previousLoaded,
                                                   previous,phase]() mutable -> LoadResult {
            LoadResult result;result.previous=previous;result.previousLoaded=previousLoaded;result.previousPath=previousPath;result.kind=kind;
            try{
                if(result.previous){phase->store(0);result.previous->save(previousPath);}
                phase->store(1);
                result.next=std::make_unique<World>();
                if(kind==LoadKind::Existing){
                    if(selected.empty() || !result.next->load(selected))throw std::runtime_error("Cannot load the selected world");
                    result.path=selected.extension()==".mcp"?destinationDir/"world.inner":selected;
                }else{
                    result.path=allocateWorldSave(destinationDir);
                    if(kind==LoadKind::Tutorial){
                        result.next->generateTutorial(std::filesystem::path(CONSOLE_ASSET_DIR)/"tutorial");
                        // eGameHostOption_Tutorial: survival (game type bits 0).
                        result.next->setSurvival(true);
                    }
                    else if(kind==LoadKind::ArchivedTutorial)result.next->generateArchivedTutorial(std::filesystem::path(CONSOLE_ASSET_DIR)/"tutorial");
                    else {result.next->generate(value,flat);result.next->setName(name);result.next->setSurvival(survivalMode);}
                    if(kind==LoadKind::Tutorial || kind==LoadKind::Create)setUpNewPlayer(*result.next);
                }
                phase->store(2);
                phase->store(3);
                if(kind!=LoadKind::Existing || selected.extension()==".mcp")result.next->save(result.path);
            }catch(const std::exception& error){result.error=error.what();result.next.reset();}
            phase->store(4);
            return result;
        });}catch(...){
            if(previous)world.swapWith(*previous);
            loaded=previousLoaded;returnFromLoad();loadPhase.reset();throw;
        }
    }
    // A failed load goes back to the menu it was started from (or to the
    // previous world's pause menu).
    void returnFromLoad(){
        if(loaded)openMenu(MenuScene::Pause);
        else if(menus.active())change(Screen::Menu);
        else openMenu(MenuScene::MainMenu);
    }
    void finishLoad(bool wait=false){
        if(!loadReady){
            if(!loadJob.valid() || (!wait && loadJob.wait_for(std::chrono::seconds(0))!=std::future_status::ready))return;
            try{loadReady.emplace(loadJob.get());}
            catch(const std::exception& error){loaded=false;returnFromLoad();message(error.what());loadPhase.reset();return;}
            if(!loadReady->error.empty()){
                if(loadReady->previous){world.swapWith(*loadReady->previous);activeSave=loadReady->previousPath;}
                loaded=loadReady->previousLoaded;returnFromLoad();
                message(loadReady->error);loadReady.reset();loadPhase.reset();return;
            }
            world.swapWith(*loadReady->next);
            activeSave=loadReady->path;loaded=true;
            try{renderer->beginRebuild(world);}catch(const std::exception&){}
        }
        try{
            if(!renderer->rebuilding())renderer->beginRebuild(world);
            if(wait){while(!renderer->stepRebuild(world,64)) {}}
            else if(!renderer->stepRebuild(world))return;
            // Resume at the saved player position when the save has one.
            const auto saved=world.savedPlayerPosition();
            position=saved?*saved:world.spawn();verticalSpeed=0;knockback={};grounded=false;flying=false;worldTickSeconds=0;
            fallDistance=0;mineProgress=0;mineY=-1;eatUseTicks=0;
            world.setPlayerPosition(position);
            if(world.isTutorial()){
                // UpdatePlayerRuleDefinition yRot (the archived tutorial uses the same value).
                const auto* rules=world.tutorialRules();
                const double yRot=rules && rules->updatePlayer && rules->updatePlayer->hasYRot?rules->updatePlayer->yRot:81.59;
                yaw=(yRot-180)*3.14159265358979323846/180;pitch=0;
            }
            endTutorialSession();
            // UIScene_LoadOrJoinMenu::LoadLevelGen sets tutorial mode only for a
            // fresh tutorial; a saved tutorial world plays normally.
            if(loadReady->kind==LoadKind::Tutorial)startTutorialSession();
            lastSave=glfwGetTime();menus.close();change(Screen::Playing);
            if(!tutorial)message(world.isTutorial()?"Classic tutorial world - creative exploration":
                    world.survival()?"Survival mode - E inventory, C crafting, Q drop":"Creative mode - press E for blocks, F to fly");
            loadReady.reset();loadPhase.reset();
        }catch(const std::exception& error){
            if(loadReady && loadReady->previous){world.swapWith(*loadReady->previous);activeSave=loadReady->previousPath;}
            loaded=loadReady && loadReady->previousLoaded;
            returnFromLoad();message(error.what());
            loadReady.reset();loadPhase.reset();
        }
    }
    void start(bool create,const std::filesystem::path& selected={},std::optional<std::int64_t> resolvedSeed={}){
        try {
            if(create){
                auto seed=resolvedSeed?resolvedSeed:parseConsoleSeed(std::u16string(seedText.begin(),seedText.end()));
                if(worldName.empty())throw std::runtime_error("Enter a world name");
                if(!seed){
                    seedSearch=std::make_unique<ConsoleSeedSearch>(std::chrono::steady_clock::now().time_since_epoch().count());
                    seedAttempts=0;change(Screen::FindingSeed);return;
                }
                beginLoad(LoadKind::Create,{},*seed);
            }else {
                beginLoad(LoadKind::Existing,selected);
            }
            if(scripted)finishLoad(true);
        }catch(const std::exception& e){if(screen==Screen::FindingSeed)change(Screen::Menu);message(e.what());}
    }
    void startTutorial(bool archived=false){
        try{beginLoad(archived?LoadKind::ArchivedTutorial:LoadKind::Tutorial);if(scripted)finishLoad(true);
        }catch(const std::exception& error){message(error.what());}
    }
    void back(){
        if(screen==Screen::Loading)return;
        if(screen==Screen::Playing)openMenu(MenuScene::Pause);
        else if(screen==Screen::Menu)handleMenuEvent(menus.input(MenuInput::Back));
        else if(screen==Screen::FindingSeed)change(Screen::Menu);
        else change(Screen::Playing);
    }
    void moveChestSelection(int amount=-1){
        try{const int itemSlot=selection<chestSlots?selection:selection-chestSlots;
            const bool take=selection<chestSlots;
            bool moved=enderChestOpen?world.transferEnderChestItem(chestX,chestY,chestZ,itemSlot,take,amount):
                world.transferChestItem(chestX,chestY,chestZ,itemSlot,take,amount);
            if(!moved)message("No items moved");}
        catch(const std::exception& error){message(error.what());}
    }
    void moveFurnaceSelection(int amount=-1){
        try{
            bool moved=false;
            if(selection<3){
                const auto before=world.furnaceItems(chestX,chestY,chestZ);
                moved=world.transferFurnaceItem(chestX,chestY,chestZ,selection,true,0,amount);
                if(moved && selection==2 && tutorial && before.size()>2 && before[2].id){
                    const auto after=world.furnaceItems(chestX,chestY,chestZ);
                    const int left=after.size()>2 && after[2].id==before[2].id?after[2].count:0;
                    if(before[2].count>left)tutorial->onCrafted(before[2].id,before[2].damage,before[2].count-left);
                }
            }
            else{
                int source=selection-3;auto carried=world.carriedItems();
                int target=furnaceFuelTarget?1:0;
                if(!furnaceFuelTarget && !furnaceRecipe(carried[source].id).id && furnaceFuelDuration(carried[source].id))target=1;
                moved=world.transferFurnaceItem(chestX,chestY,chestZ,source,false,target,amount);
            }
            if(!moved)message("No items moved");
        }catch(const std::exception& error){message(error.what());}
    }
    void moveBrewingSelection(int amount=-1){
        try{
            const int oldBottleBits=world.getData(chestX,chestY,chestZ);
            const auto bottle=selection<3?world.brewingItems(chestX,chestY,chestZ)[selection]:ContainerItem{};
            const auto before=selection<3?world.carriedItems():std::vector<ContainerItem>{};
            bool moved=false;
            if(selection<4){
                moved=world.transferBrewingItem(chestX,chestY,chestZ,selection,true,0,amount);
                if(moved && selection<3 && tutorial && bottle.id)tutorial->onCrafted(bottle.id,bottle.damage,1);
            }
            else{
                int source=selection-4;auto carried=world.carriedItems();
                if(isBrewingIngredient(carried[source].id))moved=world.transferBrewingItem(chestX,chestY,chestZ,source,false,3,amount);
                else if(carried[source].id==373 || carried[source].id==374){
                    auto stand=world.brewingItems(chestX,chestY,chestZ);
                    for(int i=0;i<3 && !moved;++i){int target=(brewingBottleTarget+i)%3;
                        if(!stand[target].id)moved=world.transferBrewingItem(chestX,chestY,chestZ,source,false,target,1);
                    }
                }
            }
            if(!moved)message("No items moved");
            else{
                if(bottle.id==373){
                    const auto after=world.carriedItems();
                    for(int i=0;i<int(after.size());++i)
                        if(after[i].id==373 && after[i].damage==bottle.damage &&
                           (before[i].id!=373 || before[i].damage!=bottle.damage ||
                            before[i].count<after[i].count)){
                            if(i<9)slot=i;
                            else world.swapCarriedSlots(slot,i);
                            break;
                        }
                }
                if(world.getData(chestX,chestY,chestZ)!=oldBottleBits &&
                   !world.streaming() && !renderer->rebuilding())renderer->beginRebuild(world);
            }
        }catch(const std::exception& error){message(error.what());}
    }
    void activate(){
        switch(screen){
        case Screen::Crafting:{
            const auto list=craftingList();
            if(selection>=0 && selection<int(list.size())){
                const auto& recipe=*list[selection];
                if(tutorial)tutorial->createItemSelected(recipe.id,recipe.damage,world.canCraft(recipe));
                if(world.craft(recipe)){
                    if(tutorial)tutorial->onCrafted(recipe.id,recipe.damage,recipe.count);
                    message("Crafted "+itemDisplayName(recipe.id,recipe.damage));
                }
                else if(!tutorial)message("Missing ingredients");
            }
            break;
        }
        case Screen::FindingSeed:change(Screen::Menu);break;
        case Screen::Chest:moveChestSelection();break;
        case Screen::Furnace:moveFurnaceSelection();break;
        case Screen::Brewing:moveBrewingSelection();break;
        case Screen::Inventory:
            if(inventoryCategory==creativeTabCount()){
                if(selection<9)slot=selection;
                else if(world.swapCarriedSlots(slot,selection))message("Moved item to quickbar");
            }else {const auto item=selectedCreativeEntry();
                if(equipCreativeItem(item.id,item.damage))message("Equipped item");}
            break;
        default:break;
        }
    }
    // The EControllerActions a key stands for inside a container menu.
    static int menuAction(int key){
        using S=TutorialSession;
        switch(key){
        case GLFW_KEY_ENTER:case GLFW_KEY_KP_ENTER:case GLFW_KEY_SPACE:return S::MenuA;
        case GLFW_KEY_ESCAPE:case GLFW_KEY_E:case GLFW_KEY_C:return S::MenuB;
        case GLFW_KEY_X:case GLFW_KEY_H:return S::MenuX;
        case GLFW_KEY_Y:case GLFW_KEY_R:return S::MenuY;
        case GLFW_KEY_UP:return S::MenuUp;case GLFW_KEY_DOWN:return S::MenuDown;
        case GLFW_KEY_LEFT:return S::MenuLeft;case GLFW_KEY_RIGHT:return S::MenuRight;
        case GLFW_KEY_TAB:case GLFW_KEY_PAGE_DOWN:case GLFW_KEY_RIGHT_BRACKET:return S::MenuRightScroll;
        case GLFW_KEY_PAGE_UP:case GLFW_KEY_LEFT_BRACKET:return S::MenuLeftScroll;
        default:return -1;
        }
    }
    void key(int key,int action){
        if(action!=GLFW_PRESS && action!=GLFW_REPEAT)return;
        if(key==GLFW_KEY_F2){screenshotRequested=true;return;}
        if(action==GLFW_PRESS && key!=GLFW_KEY_ESCAPE)padInputLast=false;
        // IUIScene_AbstractContainerMenu/CraftingMenu::handleKeyDown: the
        // tutorial sees every menu key and may hold it back.
        if(tutorial && tutorialMenuState(screen,!world.survival(),craftingTable)>=0 && action==GLFW_PRESS){
            const int menu=menuAction(key);
            if(menu>=0 && !tutorial->menuInput(menu,tutorialPopupShown()))return;
        }
        if(action==GLFW_REPEAT && (key==GLFW_KEY_E || key==GLFW_KEY_F || key==GLFW_KEY_ESCAPE ||
                                   key==GLFW_KEY_TAB))return;
        if(screen==Screen::Menu){menuKey(key,action);return;}
        if(key==GLFW_KEY_ESCAPE){back();return;}
        if(screen==Screen::Playing){
            if(key>=GLFW_KEY_1 && key<=GLFW_KEY_9)slot=key-GLFW_KEY_1;
            if(key==GLFW_KEY_E && inputAllowed(TutorialSession::InventoryAction)){
                if(world.survival())inventoryCategory=creativeTabCount();
                change(Screen::Inventory);
            }
            if(key==GLFW_KEY_C && world.survival() && inputAllowed(TutorialSession::CraftingAction)){craftingTable=false;craftScroll=0;change(Screen::Crafting);}
            // Abilities::mayfly is creative-only.
            if(key==GLFW_KEY_F && !world.survival()){flying=!flying;verticalSpeed=0;message(flying?"Flying enabled":"Flying disabled");}
            if(key==GLFW_KEY_Q && inputAllowed(TutorialSession::Drop)){
                const bool stack=glfwGetKey(window,GLFW_KEY_LEFT_CONTROL)==GLFW_PRESS;
                try{world.dropCarried(slot,stack,eye(),yaw,pitch);}catch(const std::exception& e){message(e.what());}
            }
            // LocalPlayer::aiStep: a double jump toggles creative flight.
            if(key==GLFW_KEY_SPACE && action==GLFW_PRESS && !world.survival()){
                const double now=glfwGetTime();
                if(now-lastJumpPress<.35){flying=!flying;verticalSpeed=0;lastJumpPress=-1;return;}
                lastJumpPress=now;
            }
            if(key==GLFW_KEY_SPACE && grounded && !flying && !world.playerInWater() && inputAllowed(TutorialSession::Jump)){
                verticalSpeed=8.4;grounded=false;
                world.playerJumped(glfwGetKey(window,GLFW_KEY_LEFT_CONTROL)==GLFW_PRESS);
            }
            return;
        }
        if(screen==Screen::Inventory && key==GLFW_KEY_E){back();return;}
        if(screen==Screen::Crafting){
            const int count=int(craftingList().size());
            if(key==GLFW_KEY_C){back();return;}
            if(key==GLFW_KEY_TAB){craftingGroup=(craftingGroup+1)%int(craftingGroups.size());selection=0;return;}
            if(count==0)return;
            if(key==GLFW_KEY_UP)selection=(selection+count-1)%count;
            if(key==GLFW_KEY_DOWN)selection=(selection+1)%count;
            if(key==GLFW_KEY_PAGE_UP || key==GLFW_KEY_LEFT)selection=std::max(0,selection-8);
            if(key==GLFW_KEY_PAGE_DOWN || key==GLFW_KEY_RIGHT)selection=std::min(count-1,selection+8);
            if(key==GLFW_KEY_ENTER || key==GLFW_KEY_SPACE)activate();
            return;
        }
        if(screen==Screen::Chest){
            if(key==GLFW_KEY_H){moveChestSelection(0);return;}
            if(key==GLFW_KEY_R){moveChestSelection(1);return;}
            if(key==GLFW_KEY_UP)selection=(selection+chestSlots+36-9)%(chestSlots+36);
            if(key==GLFW_KEY_DOWN)selection=(selection+9)%(chestSlots+36);
            if(key==GLFW_KEY_LEFT)selection=(selection+chestSlots+35)%(chestSlots+36);
            if(key==GLFW_KEY_RIGHT)selection=(selection+1)%(chestSlots+36);
            if(key==GLFW_KEY_ENTER || key==GLFW_KEY_SPACE)activate();
            return;
        }
        if(screen==Screen::Furnace){
            if(key==GLFW_KEY_F){furnaceFuelTarget=!furnaceFuelTarget;return;}
            if(key==GLFW_KEY_H){moveFurnaceSelection(0);return;}
            if(key==GLFW_KEY_R){moveFurnaceSelection(1);return;}
            if(key==GLFW_KEY_UP)selection=(selection+30)%39;
            if(key==GLFW_KEY_DOWN)selection=(selection+9)%39;
            if(key==GLFW_KEY_LEFT)selection=(selection+38)%39;
            if(key==GLFW_KEY_RIGHT)selection=(selection+1)%39;
            if(key==GLFW_KEY_ENTER || key==GLFW_KEY_SPACE)activate();
            return;
        }
        if(screen==Screen::Brewing){
            if(key==GLFW_KEY_F){brewingBottleTarget=(brewingBottleTarget+1)%3;return;}
            if(key==GLFW_KEY_H){moveBrewingSelection(0);return;}
            if(key==GLFW_KEY_R){moveBrewingSelection(1);return;}
            if(key==GLFW_KEY_UP)selection=(selection+31)%40;
            if(key==GLFW_KEY_DOWN)selection=(selection+9)%40;
            if(key==GLFW_KEY_LEFT)selection=(selection+39)%40;
            if(key==GLFW_KEY_RIGHT)selection=(selection+1)%40;
            if(key==GLFW_KEY_ENTER || key==GLFW_KEY_SPACE)activate();
            return;
        }
        if(screen==Screen::Inventory){
            if(world.survival() && (key==GLFW_KEY_TAB || key==GLFW_KEY_PAGE_UP || key==GLFW_KEY_PAGE_DOWN ||
                                    key==GLFW_KEY_LEFT_BRACKET || key==GLFW_KEY_RIGHT_BRACKET))return;
            if(key==GLFW_KEY_TAB){inventoryCategory=(inventoryCategory+1)%(creativeTabCount()+1);selection=0;return;}
            if(key==GLFW_KEY_PAGE_UP || key==GLFW_KEY_LEFT_BRACKET){turnCreativePage(-1);return;}
            if(key==GLFW_KEY_PAGE_DOWN || key==GLFW_KEY_RIGHT_BRACKET){turnCreativePage(1);return;}
            const int count=inventoryCount(),columns=inventoryCategory==creativeTabCount()?9:10;
            if(key==GLFW_KEY_LEFT)selection=(selection+count-1)%count;
            if(key==GLFW_KEY_RIGHT)selection=(selection+1)%count;
            if(key==GLFW_KEY_UP)selection=(selection+count-(columns%count))%count;
            if(key==GLFW_KEY_DOWN)selection=(selection+columns)%count;
            if(key==GLFW_KEY_ENTER || key==GLFW_KEY_SPACE)activate();
            return;
        }
        if(key==GLFW_KEY_ENTER || key==GLFW_KEY_SPACE)activate();
    }
    // Menu navigation (ACTION_MENU_*) from the keyboard, and typing into the
    // Create World text fields (the console's virtual keyboard).
    void menuKey(int key,int action){
        if(editingField>=0){
            auto& value=editingField==0?menus.newWorld().name:menus.newWorld().seed;
            if(key==GLFW_KEY_BACKSPACE && !value.empty()){
                while(!value.empty() && (static_cast<unsigned char>(value.back())&0xC0)==0x80)value.pop_back();
                if(!value.empty())value.pop_back();
            }
            if(key==GLFW_KEY_ENTER || key==GLFW_KEY_KP_ENTER || key==GLFW_KEY_ESCAPE || key==GLFW_KEY_TAB)editingField=-1;
            menus.refresh();return;
        }
        if(action==GLFW_REPEAT && key!=GLFW_KEY_UP && key!=GLFW_KEY_DOWN && key!=GLFW_KEY_LEFT && key!=GLFW_KEY_RIGHT)return;
        if(menus.active() && menus.scene()==MenuScene::HowToPlay && (key==GLFW_KEY_UP || key==GLFW_KEY_DOWN)){
            howToPlayScroll=std::max(0.f,howToPlayScroll+(key==GLFW_KEY_UP?-12.f:12.f));return;
        }
        std::optional<MenuInput> input;
        switch(key){
        case GLFW_KEY_UP:input=MenuInput::Up;break;
        case GLFW_KEY_DOWN:input=MenuInput::Down;break;
        case GLFW_KEY_LEFT:input=MenuInput::Left;break;
        case GLFW_KEY_RIGHT:input=MenuInput::Right;break;
        case GLFW_KEY_ENTER:case GLFW_KEY_KP_ENTER:case GLFW_KEY_SPACE:input=MenuInput::Accept;break;
        case GLFW_KEY_ESCAPE:case GLFW_KEY_BACKSPACE:input=MenuInput::Back;break;
        case GLFW_KEY_X:input=MenuInput::X;break;
        case GLFW_KEY_Y:input=MenuInput::Y;break;
        default:break;
        }
        if(input)menuInput(*input);
    }
    void menuInput(MenuInput input){
        if(!menus.active())return;
        // The pause menu's Back resumes; the death menu has none.
        handleMenuEvent(menus.input(input));
        if(screen==Screen::Menu && !menus.active() && loaded)change(Screen::Playing);
    }
    void look(double x,double y){
        if(screen==Screen::Menu && menus.active() && (std::abs(x-lastX)>1 || std::abs(y-lastY)>1)){
            // Pointing at a control focuses it.
            int w,h;glfwGetWindowSize(window,&w,&h);
            const double ux=x*renderer->uiWidth()/std::max(1,w),uy=y*360/std::max(1,h);
            const auto boxes=menuLayout();
            for(int i=0;i<int(boxes.size());++i)if(boxes[i].contains(ux,uy) && i!=menus.focus()){menus.setFocus(i);break;}
        }
        if(screen!=Screen::Playing || scripted || glfwGetTime()<ignoreMouseUntil){lastX=x;lastY=y;mouseReady=false;return;}
        if(mouseReady){
            lookDX+=x-lastX;lookDY+=y-lastY;padInputLast=false;
            if(inputAllowed(TutorialSession::LookLeft) || inputAllowed(TutorialSession::LookRight))yaw+=(x-lastX)*lookSensitivity();
            if(inputAllowed(TutorialSession::LookUp) || inputAllowed(TutorialSession::LookDown))pitch+=(y-lastY)*lookSensitivity()*(invertLook()?1:-1);
            pitch=std::clamp(pitch,-1.55,1.55);
        }
        lastX=x;lastY=y;mouseReady=true;
    }
    Vec3 eye()const{return {position.x,position.y+1.62,position.z};}
    // MultiPlayerGameMode::getPickRange and GameRenderer::pick: blocks 4.5
    // (creative 5), entities 3 (creative's far pick range 6).
    double blockReach()const{return loaded && world.survival()?4.5:5.0;}
    double entityReach()const{return loaded && world.survival()?3.0:6.0;}
    Vec3 direction()const{return {std::sin(yaw)*std::cos(pitch),std::sin(pitch),-std::cos(yaw)*std::cos(pitch)};}
    void edit(bool place){
        if(glfwGetTime()-lastEdit<.16)return;lastEdit=glfwGetTime();
        const auto heldItem=selectedItem();
        if(!place && world.attackEntity(eye(),direction(),heldItem.id,entityReach())){
            world.playerAttacked(slot);if(tutorial)tutorial->attack(heldItem.id,heldItem.damage,true);return;}
        auto h=world.raycast(eye(),direction(),blockReach());if(!h.hit)return;
        bool changed=false;
        if(place && tutorial)tutorial->useItemOnBefore(heldItem.id,heldItem.damage,heldItem.count,h.x-World::width/2,h.y,h.z-World::depth/2);
        if(place && world.get(h.x,h.y,h.z)==58){
            // WorkbenchTile::use opens the 3x3 crafting menu.
            craftingTable=true;craftScroll=0;change(Screen::Crafting);return;
        }
        if(place && world.get(h.x,h.y,h.z)==54){
            if(!world.canOpenChest(h.x,h.y,h.z)){message("The chest lid is blocked");return;}
            try{chestSlots=int(world.chestItems(h.x,h.y,h.z).size());world.carriedItems();}
            catch(const std::exception& error){message(error.what());return;}
            chestX=h.x;chestY=h.y;chestZ=h.z;enderChestOpen=false;change(Screen::Chest);return;
        }
        if(place && world.get(h.x,h.y,h.z)==static_cast<Block>(130)){
            if(!world.canOpenEnderChest(h.x,h.y,h.z)){message("The ender chest lid is blocked");return;}
            try{world.enderChestItems();world.carriedItems();}
            catch(const std::exception& error){message(error.what());return;}
            chestX=h.x;chestY=h.y;chestZ=h.z;chestSlots=27;enderChestOpen=true;
            change(Screen::Chest);return;
        }
        if(place && world.canOpenFurnace(h.x,h.y,h.z)){
            try{world.furnaceItems(h.x,h.y,h.z);world.carriedItems();}
            catch(const std::exception& error){message(error.what());return;}
            chestX=h.x;chestY=h.y;chestZ=h.z;furnaceFuelTarget=false;
            change(Screen::Furnace);return;
        }
        if(place && world.canOpenBrewingStand(h.x,h.y,h.z)){
            try{world.brewingItems(h.x,h.y,h.z);world.carriedItems();}
            catch(const std::exception& error){message(error.what());return;}
            chestX=h.x;chestY=h.y;chestZ=h.z;brewingBottleTarget=0;
            change(Screen::Brewing);return;
        }
        if(place && world.get(h.x,h.y,h.z)==static_cast<Block>(118) &&
           (heldItem.id==326 || heldItem.id==374)){
            if(!usedBlockHeld){changed=world.useCauldron(h.x,h.y,h.z,heldItem.id);
                if(!changed && heldItem.id==374 && world.getData(h.x,h.y,h.z)>0)message("Inventory is full");
                if(changed && !world.streaming() && !renderer->rebuilding())renderer->beginRebuild(world);
                usedBlockHeld=true;}
            return;
        }
        // Tile::use: doors, gates, trapdoors, levers, buttons, repeaters.
        if(place && world.usable(h.x,h.y,h.z)){
            if(!usedBlockHeld){world.useBlock(h.x,h.y,h.z,yaw);if(!world.streaming() && !renderer->rebuilding())renderer->beginRebuild(world);usedBlockHeld=true;}
            return;
        }
        if(place){
            // Item::useOn for hoes, seeds, bone meal and cocoa beans; the face
            // is the side of the target the adjacent cell lies on (Facing).
            const int face=h.py<h.y?0:h.py>h.y?1:h.pz<h.z?2:h.pz>h.z?3:h.px<h.x?4:5;
            if(!usedBlockHeld && world.useItemOn(h.x,h.y,h.z,face,slot)){
                usedBlockHeld=true;
                if(tutorial){
                    const auto after=selectedItem();
                    tutorial->useItemOnAfter(heldItem.id,heldItem.damage,heldItem.count,after.id==heldItem.id?after.count:0,true);
                }
                if(!world.streaming() && !renderer->rebuilding())renderer->beginRebuild(world);
                return;
            }
        }
        if(place && heldItem.id==383){
            const Block support=world.get(h.x,h.y,h.z);
            const double fenceOffset=h.py==h.y+1 && (support==Fence || support==NetherFence)?0.5:0.0;
            if(!world.spawnCreativeEgg(heldItem.damage,{h.px+.5,h.py+fenceOffset,h.pz+.5}))
                message("Could not spawn this mob here");
            else world.consumeCarried(slot); // MonsterPlacerItem::useOn uses one egg in survival.
            return;
        }
        if(place){
            const int tileId=heldItem.id<256?heldItem.id:placedTileForItem(heldItem.id);
            // The clicked face of the target block (Facing), as for useItemOn.
            const int face=h.py<h.y?0:h.py>h.y?1:h.pz<h.z?2:h.pz>h.z?3:h.px<h.x?4:5;
            if(tileId>0 && tileId<256)
                changed=world.placeBlock(h.px,h.py,h.pz,static_cast<Block>(tileId),
                                         heldItem.id<256?heldItem.damage:0,position,yaw,face);
            // TileItem/TilePlanterItem::useOn uses one item in survival.
            if(changed)world.consumeCarried(slot);
            if(tutorial){
                const auto after=selectedItem();
                tutorial->useItemOnAfter(heldItem.id,heldItem.damage,heldItem.count,after.id==heldItem.id?after.count:0,changed);
            }
        }else if(world.get(h.x,h.y,h.z)!=Bedrock)changed=world.destroyBlock(h.x,h.y,h.z,slot);
        if(changed){
            if(!world.streaming() && !renderer->rebuilding())renderer->beginRebuild(world);
        }
    }
    // MultiPlayerGameMode: continueDestroyBlock runs once per game tick while
    // the button is held. A new target goes through startDestroyBlock, which
    // breaks instant tiles at once; a finished timed break sets destroyDelay 5.
    // The crack shown is destroy stage (int)(progress*10)-1.
    void mine(int ticks){
        const auto heldItem=selectedItem();
        if(glfwGetTime()-lastEdit>=.16 && world.attackEntity(eye(),direction(),heldItem.id,entityReach())){
            lastEdit=glfwGetTime();world.playerAttacked(slot);if(tutorial)tutorial->attack(heldItem.id,heldItem.damage,true);
            mineProgress=0;mineY=-1;renderer->setDestroyStage(-1);return;
        }
        const auto h=world.raycast(eye(),direction(),blockReach());
        if(!h.hit){mineProgress=0;mineY=-1;renderer->setDestroyStage(-1);return;}
        const auto destroy=[&]{
            const int tileId=world.get(h.x,h.y,h.z);
            const auto before=selectedItem();
            const bool destroyed=world.destroyBlock(h.x,h.y,h.z,slot);
            if(tutorial){
                const auto after=selectedItem();
                tutorial->destroyBlock(tileId,before.id,before.damage,after.id==before.id?after.damage:before.damage);
            }
            if(destroyed){
                if(!world.streaming() && !renderer->rebuilding())renderer->beginRebuild(world);
            }
        };
        for(int tick=0;tick<ticks;++tick){
            if(mineDelay>0){--mineDelay;continue;}
            if(mineY<0 || h.x!=mineX || h.y!=mineY || h.z!=mineZ){
                if(tutorial)tutorial->startDestroyBlock(heldItem.id,heldItem.damage,heldItem.count,world.get(h.x,h.y,h.z));
                if(world.destroyProgress(h.x,h.y,h.z,slot)>=1){destroy();mineY=-1;break;}
                mineX=h.x;mineY=h.y;mineZ=h.z;mineProgress=0;continue;
            }
            mineProgress+=world.destroyProgress(h.x,h.y,h.z,slot);
            if(mineProgress>=1){destroy();mineProgress=0;mineY=-1;mineDelay=5;break;}
        }
        renderer->setDestroyStage(mineY>=0?std::min(9,int(mineProgress*10)-1):-1);
    }
    void moveAxis(int axis,double amount){
        const int steps=std::max(1,int(std::ceil(std::abs(amount)/.15)));
        for(int i=0;i<steps;++i){Vec3 next=position;double* coordinate=axis==0?&next.x:axis==1?&next.y:&next.z;*coordinate+=amount/steps;
            if(!world.collides(next))position=next;
            else {if(axis==1){if(amount<0)grounded=true;verticalSpeed=0;}else horizontalCollision=true;break;}}
    }
    // InputManager.GetValue: the EControllerActions values for this frame from
    // the keyboard/mouse and the PS3 layout of the gamepad (DefineActions).
    void sampleActions(bool hasPad){
        using S=TutorialSession;
        auto key=[&](int k){return glfwGetKey(window,k)==GLFW_PRESS;};
        auto mouse=[&](int b){return glfwGetMouseButton(window,b)==GLFW_PRESS;};
        // The buttons the selected controller layout binds to each action.
        auto pad=[&](int action){return hasPad && padAction(action);};
        auto& v=actionValues;v.fill(0);
        for(int action=0;action<S::ActionCount;++action)v[action]=pad(action);
        const bool menu=screen!=Screen::Playing;
        v[S::MenuA]|=key(GLFW_KEY_ENTER)||key(GLFW_KEY_KP_ENTER)||key(GLFW_KEY_SPACE);
        v[S::MenuB]|=key(GLFW_KEY_BACKSPACE)||(menu && key(GLFW_KEY_ESCAPE));
        v[S::MenuX]|=key(GLFW_KEY_X);v[S::MenuY]|=key(GLFW_KEY_Y);
        v[S::MenuUp]|=key(GLFW_KEY_UP);v[S::MenuDown]|=key(GLFW_KEY_DOWN);
        v[S::MenuLeft]|=key(GLFW_KEY_LEFT);v[S::MenuRight]|=key(GLFW_KEY_RIGHT);
        v[S::MenuPauseMenu]|=key(GLFW_KEY_ESCAPE);
        v[S::MenuOk]|=v[S::MenuA];v[S::MenuCancel]|=v[S::MenuB];
        v[S::Jump]|=key(GLFW_KEY_SPACE);
        v[S::Forward]|=key(GLFW_KEY_W);v[S::Backward]|=key(GLFW_KEY_S);
        v[S::Left]|=key(GLFW_KEY_A);v[S::Right]|=key(GLFW_KEY_D);
        v[S::LookLeft]|=lookDX<-1;v[S::LookRight]|=lookDX>1;
        v[S::LookUp]|=lookDY<-1;v[S::LookDown]|=lookDY>1;
        v[S::Use]|=mouse(GLFW_MOUSE_BUTTON_RIGHT);
        v[S::ActionButton]|=mouse(GLFW_MOUSE_BUTTON_LEFT);
        v[S::LeftScroll]|=scrollSteps<0;v[S::RightScroll]|=scrollSteps>0;
        v[S::InventoryAction]|=key(GLFW_KEY_E);
        v[S::CraftingAction]|=key(GLFW_KEY_C);
        v[S::Drop]|=key(GLFW_KEY_Q);
        v[S::PauseMenu]|=key(GLFW_KEY_ESCAPE);
        v[S::SneakToggle]|=key(GLFW_KEY_LEFT_SHIFT);
        v[S::RenderThirdPerson]|=key(GLFW_KEY_F5);
        v[S::GameInfo]|=key(GLFW_KEY_TAB);
        if(padBits)padInputLast=true;
    }
    // The engine events the console tutorial listens to, checked once per game
    // tick: look-at (Minecraft::tick tooltips), collected items
    // (LocalPlayer::handleCollectItem), effects (MultiplayerLocalPlayer),
    // hunger (ClientConnection::handleSetHealth), hotbar changes and the
    // CompleteAll music disc goal (ClientConnection progress packets).
    void tutorialEvents(){
        if(screen==Screen::Playing){
            if(auto entity=world.pickEntity(eye(),direction(),entityReach()))
                tutorial->onLookAtEntity(consoleEntityInstanceType(*entity));
            else if(auto h=world.raycast(eye(),direction(),blockReach());h.hit)
                tutorial->onLookAt(world.get(h.x,h.y,h.z),world.getData(h.x,h.y,h.z));
        }
        const auto carried=world.carriedItems();
        for(int i=0;i<int(carried.size()) && i<int(tutorialCarried.size());++i){
            const auto& now=carried[i];const auto& was=tutorialCarried[i];
            if(!now.id || (now.id==was.id && now.count==was.count && now.damage==was.damage))continue;
            unsigned anyAux=0,thisAux=0;
            for(const auto& item:carried)if(item.id==now.id){anyAux+=item.count;if(item.damage==now.damage)thisAux+=item.count;}
            tutorial->onTake(now.id,now.damage,now.count,anyAux,thisAux);
            collectGoal(now);
        }
        tutorialCarried=carried;
        const auto& effects=world.activePotionEffects();
        for(const auto& effect:effects){
            const bool had=std::any_of(tutorialEffects.begin(),tutorialEffects.end(),[&](const PotionEffect& e){return e.id==effect.id;});
            if(!had)tutorial->onEffectChanged(effect.id,false);
        }
        for(const auto& effect:tutorialEffects)
            if(std::none_of(effects.begin(),effects.end(),[&](const PotionEffect& e){return e.id==effect.id;}))
                tutorial->onEffectChanged(effect.id,true);
        tutorialEffects=effects;
        const int food=world.playerFoodLevel();
        // FoodConstants::HEAL_LEVEL - 1
        if(food!=tutorialFood && food<17 && world.survival())tutorial->hungry();
        tutorialFood=food;
        if(slot!=tutorialSlot){
            tutorialSlot=slot;
            tutorial->onSelectedItemChanged(carried[slot].id,carried[slot].damage);
        }
    }
    // CollectItemRuleDefinition::onCollectItem inside the CompleteAll goal:
    // the progress message and the profile's special completion flag.
    void collectGoal(const ContainerItem& item){
        const auto* rules=world.tutorialRules();
        if(!rules || !item.marker)return;
        for(const auto& goal:rules->collectGoals){
            if(goal.itemId!=item.id || goal.aux!=item.damage || goal.dataTag!=item.marker || item.count<goal.quantity)continue;
            if(!collectedGoals.insert(goal.dataTag).second)return;
            if(goal.dataTag>0 && goal.dataTag<=32){settings.setSpecialTutorialCompletion(goal.dataTag-1);saveSettings();}
            const auto* strings=world.tutorialStrings();
            if(!strings)return;
            auto description=strings->find(rules->completeAllDescription);
            if(description==strings->end())return;
            std::wstring text=description->second;
            auto replace=[&](const std::wstring& token,int value){
                for(std::size_t at;(at=text.find(token))!=std::wstring::npos;)text.replace(at,token.size(),std::to_wstring(value));
            };
            replace(L"{*progress*}",int(collectedGoals.size()));replace(L"{*goal*}",int(rules->collectGoals.size()));
            tutorial->setMessage(text,item.id,item.damage);
            return;
        }
    }
    void update(double dt){
        GLFWgamepadstate pad{};bool hasPad=false;
        for(int i=GLFW_JOYSTICK_1;i<=GLFW_JOYSTICK_LAST;++i)if(glfwGetGamepadState(i,&pad)){hasPad=true;break;}
        const bool southpaw=settings.get(GameSetting::ControlSouthPaw)!=0;
        padBits=hasPad?padButtons(pad,southpaw):0;
        if(hasPad){
            using S=TutorialSession;
            auto pressed=[&](int b){return pad.buttons[b] && !previousPad.buttons[b];};
            if(screen==Screen::Menu){
                if(editingField>=0){
                    if(padActionPressed(S::MenuA) || padActionPressed(S::MenuB)){editingField=-1;menus.refresh();}
                }else{
                    if(padActionPressed(S::MenuUp))menuKey(GLFW_KEY_UP,GLFW_PRESS);
                    if(padActionPressed(S::MenuDown))menuKey(GLFW_KEY_DOWN,GLFW_PRESS);
                    if(padActionPressed(S::MenuLeft))menuInput(MenuInput::Left);
                    if(padActionPressed(S::MenuRight))menuInput(MenuInput::Right);
                    if(padActionPressed(S::MenuA))menuInput(MenuInput::Accept);
                    else if(padActionPressed(S::MenuB))menuInput(MenuInput::Back);
                    else if(padActionPressed(S::MenuX))menuInput(MenuInput::X);
                    else if(padActionPressed(S::MenuY))menuInput(MenuInput::Y);
                    // UIScene_PauseMenu: START also resumes.
                    else if(padActionPressed(S::MenuPauseMenu) && menus.active() && menus.scene()==MenuScene::Pause)menuInput(MenuInput::Back);
                }
            }else if(screen==Screen::Playing){
                // The in-game actions of the selected controller layout.
                if(padActionPressed(S::PauseMenu))key(GLFW_KEY_ESCAPE,GLFW_PRESS);
                else{
                    if(padActionPressed(S::Jump))key(GLFW_KEY_SPACE,GLFW_PRESS);
                    if(padActionPressed(S::Drop))key(GLFW_KEY_Q,GLFW_PRESS);
                    if(padActionPressed(S::InventoryAction))key(GLFW_KEY_E,GLFW_PRESS);
                    if(padActionPressed(S::CraftingAction))key(GLFW_KEY_C,GLFW_PRESS);
                    if(padActionPressed(S::LeftScroll) && inputAllowed(S::LeftScroll))slot=(slot+8)%9;
                    if(padActionPressed(S::RightScroll) && inputAllowed(S::RightScroll))slot=(slot+1)%9;
                }
            }else{
                // Container menus: the ACTION_MENU_* buttons.
                if(padActionPressed(S::MenuB) || padActionPressed(S::MenuPauseMenu))key(GLFW_KEY_ESCAPE,GLFW_PRESS);
                if(padActionPressed(S::MenuUp))key(GLFW_KEY_UP,GLFW_PRESS);
                if(padActionPressed(S::MenuDown))key(GLFW_KEY_DOWN,GLFW_PRESS);
                if(padActionPressed(S::MenuLeft))key(GLFW_KEY_LEFT,GLFW_PRESS);
                if(padActionPressed(S::MenuRight))key(GLFW_KEY_RIGHT,GLFW_PRESS);
                if(screen==Screen::Chest && padActionPressed(S::MenuX))moveChestSelection(0);
                if(screen==Screen::Chest && padActionPressed(S::MenuY))moveChestSelection(1);
                if(screen==Screen::Furnace && padActionPressed(S::MenuX))moveFurnaceSelection(0);
                if(screen==Screen::Furnace && padActionPressed(S::MenuY))moveFurnaceSelection(1);
                if(screen==Screen::Brewing && padActionPressed(S::MenuX))moveBrewingSelection(0);
                if(screen==Screen::Brewing && padActionPressed(S::MenuY))moveBrewingSelection(1);
                if(padActionPressed(S::MenuA))key(GLFW_KEY_ENTER,GLFW_PRESS);
                if(screen==Screen::Crafting && padActionPressed(S::MenuX))key(GLFW_KEY_C,GLFW_PRESS);
                if(screen==Screen::Inventory && padActionPressed(S::MenuY))key(GLFW_KEY_E,GLFW_PRESS);
                if(pressed(GLFW_GAMEPAD_BUTTON_LEFT_THUMB) && (screen==Screen::Furnace || screen==Screen::Brewing))key(GLFW_KEY_F,GLFW_PRESS);
                if((padActionPressed(S::MenuLeftScroll) || padActionPressed(S::MenuRightScroll)) &&
                   (screen==Screen::Inventory || screen==Screen::Crafting))key(GLFW_KEY_TAB,GLFW_PRESS);
            }
        }
        previousPad=pad;previousPadBits=padBits;
        sampleActions(hasPad);
        if(screen==Screen::Menu && menus.active() && menus.scene()==MenuScene::Credits)menus.creditsScroll+=float(dt)*20;
        if(screen==Screen::Loading){finishLoad();return;}
        if(screen==Screen::FindingSeed){
            try{auto seed=seedSearch->step();++seedAttempts;if(seed)start(true,{},seed);}
            catch(const std::exception& error){change(Screen::Menu);message(error.what());}
            return;
        }
        if(screen!=Screen::Playing && screen!=Screen::Furnace && screen!=Screen::Brewing && screen!=Screen::Chest &&
           screen!=Screen::Inventory && screen!=Screen::Crafting && !deathMenuShown())return;
        try{if(world.streamAround(position))renderer->beginRebuild(world);
            if(!world.streaming())renderer->stepRebuild(world);}
        catch(const std::exception& error){openMenu(MenuScene::Pause);message(error.what());return;}
        renderer->tickLighting(dt);
        world.setPlayerPosition(position);
        worldTickSeconds+=dt;
        const auto beforeWorldTick=world.revision;
        int elapsedWorldTicks=0;
        while(worldTickSeconds>=.05){world.tickTime();worldTickSeconds-=.05;++elapsedWorldTicks;}
        // Pistons move the player (PistonPieceEntity::moveCollidedEntities ->
        // Entity::move): each axis as far as nothing blocks it.
        if(const auto push=world.takePlayerPush();push.x!=0 || push.y!=0 || push.z!=0){
            for(int axis=0;axis<3;++axis){
                Vec3 next=position;
                (axis==0?next.x:axis==1?next.y:next.z)+=axis==0?push.x:axis==1?push.y:push.z;
                if(!world.collides(next))position=next;
            }
            world.setPlayerPosition(position);
        }
        if(const auto kb=world.takePlayerKnockback();kb.x!=0 || kb.y!=0 || kb.z!=0){
            if(!flying){knockback.x+=kb.x*20;knockback.z+=kb.z*20;verticalSpeed+=kb.y*20;grounded=false;}
        }
        if(tutorial && elapsedWorldTicks>0){
            // TutorialMode::tick, once per game tick.
            for(int i=0;i<elapsedWorldTicks;++i){tutorialEvents();tutorial->tick();}
            lookDX=lookDY=0;scrollSteps=0;
            if(tutorial->profileChanged()){settings.tutorialCompletion()=tutorial->profile();saveSettings();}
        }
        if(world.revision!=beforeWorldTick && !world.streaming() && !renderer->rebuilding())renderer->beginRebuild(world);
        if(world.playerDead() && !deathMenuShown()){openMenu(MenuScene::Death);return;}
        if(screen!=Screen::Playing){potionUseTicks=0;eatUseTicks=0;mineProgress=0;mineY=-1;renderer->setDestroyStage(-1);return;}
        const bool survival=world.survival();
        if(survival)flying=false;
        auto held=[&](int k){return glfwGetKey(window,k)==GLFW_PRESS;};
        double forward=held(GLFW_KEY_W)-held(GLFW_KEY_S),right=held(GLFW_KEY_D)-held(GLFW_KEY_A);
        auto dead=[](float x){return std::abs(x)<.18f?0.f:(x-std::copysign(.18f,x))/.82f;};
        if(hasPad){
            // Southpaw moves with the right stick and looks with the left.
            const int moveX=southpaw?GLFW_GAMEPAD_AXIS_RIGHT_X:GLFW_GAMEPAD_AXIS_LEFT_X,moveY=southpaw?GLFW_GAMEPAD_AXIS_RIGHT_Y:GLFW_GAMEPAD_AXIS_LEFT_Y;
            const int lookX=southpaw?GLFW_GAMEPAD_AXIS_LEFT_X:GLFW_GAMEPAD_AXIS_RIGHT_X,lookY=southpaw?GLFW_GAMEPAD_AXIS_LEFT_Y:GLFW_GAMEPAD_AXIS_RIGHT_Y;
            forward-=dead(pad.axes[moveY]);right+=dead(pad.axes[moveX]);
            if(inputAllowed(TutorialSession::LookLeft) || inputAllowed(TutorialSession::LookRight))
                yaw+=dead(pad.axes[lookX])*dt*lookSensitivity()*1100;
            if(inputAllowed(TutorialSession::LookUp) || inputAllowed(TutorialSession::LookDown))
                pitch+=dead(pad.axes[lookY])*dt*lookSensitivity()*900*(invertLook()?1:-1);
            pitch=std::clamp(pitch,-1.55,1.55);}
        // Input::tick: an axis moves only if one of its directions is allowed.
        if(!inputAllowed(TutorialSession::Forward) && !inputAllowed(TutorialSession::Backward))forward=0;
        if(!inputAllowed(TutorialSession::Left) && !inputAllowed(TutorialSession::Right))right=0;
        double length=std::sqrt(forward*forward+right*right);if(length>1){forward/=length;right/=length;}
        // Player::canSprint: survival players need more than six food.
        const bool sprinting=held(GLFW_KEY_LEFT_CONTROL) && forward>0 && (!survival || world.playerFoodLevel()>6);
        const int feetTile=world.get(int(std::floor(position.x)),int(std::floor(position.y)),int(std::floor(position.z)));
        const bool inWater=world.playerInWater(),inLiquid=inWater || feetTile==10 || feetTile==11;
        const double speed=(flying?10:4.3*world.potionSpeedMultiplier())*(sprinting?(flying?1.6:1.3):1)*(inLiquid && !flying?.45:1);
        horizontalCollision=false;
        const bool climbing=!flying && (feetTile==65 || feetTile==106);
        Vec3 velocity{(std::sin(yaw)*forward+std::cos(yaw)*right)*speed,climbing?verticalSpeed-24*dt:verticalSpeed,(-std::cos(yaw)*forward+std::sin(yaw)*right)*speed};
        if(climbing)velocity=consoleLadderVelocity(velocity,held(GLFW_KEY_LEFT_SHIFT));
        const Vec3 before=position;
        if(knockback.x!=0 || knockback.z!=0){
            // Mob::travel: xd/zd *= 0.91 each tick, times the tile's 0.6
            // friction on the ground.
            velocity.x+=knockback.x;velocity.z+=knockback.z;
            const double decay=std::pow(grounded?.546:.91,dt*20);
            knockback.x*=decay;knockback.z*=decay;
            if(std::abs(knockback.x)<.01 && std::abs(knockback.z)<.01)knockback={};
        }
        moveAxis(0,velocity.x*dt);moveAxis(2,velocity.z*dt);
        if(climbing)verticalSpeed=horizontalCollision?4:velocity.y;
        const bool up=(held(GLFW_KEY_SPACE)||padAction(TutorialSession::Jump)) && inputAllowed(TutorialSession::Jump);
        if(flying){verticalSpeed=0;moveAxis(1,(up-(held(GLFW_KEY_LEFT_SHIFT)||padAction(TutorialSession::SneakToggle)))*speed*dt);}
        else if(inLiquid && !climbing){
            // Mob::travel in liquids: slow sinking, jump swims upward.
            grounded=false;
            verticalSpeed=up?std::min(verticalSpeed+20*dt,2.6):std::max(verticalSpeed-8*dt,-2.4);
            moveAxis(1,verticalSpeed*dt);
        }
        else {grounded=false;if(!climbing)verticalSpeed=std::max(verticalSpeed-24*dt,-35.0);moveAxis(1,verticalSpeed*dt);}
        // MultiplayerLocalPlayer::tick: a tutorial area constraint puts the
        // player back where they were horizontally.
        if(tutorial && !tutorial->canMoveToPosition(before.x-World::width/2,before.y+1.62,before.z-World::depth/2,
                                                    position.x-World::width/2,position.y+1.62,position.z-World::depth/2)){
            position.x=before.x;position.z=before.z;
        }
        // Entity::move fall distance; water, ladders and flight reset it.
        if(position.y<before.y)fallDistance+=before.y-position.y;
        if(flying || climbing || inLiquid)fallDistance=0;
        if(grounded){if(fallDistance>0)world.playerLanded(fallDistance);fallDistance=0;}
        world.playerWalked(std::hypot(position.x-before.x,position.z-before.z),sprinting,inWater);
        const bool mining=(leftMouse || padAction(TutorialSession::ActionButton)) && inputAllowed(TutorialSession::ActionButton);
        if(!survival){if(mining)edit(false);renderer->setDestroyStage(-1);}
        else if(mining)mine(elapsedWorldTicks);
        else{mineProgress=0;mineY=-1;renderer->setDestroyStage(-1);}
        const bool usingRight=(rightMouse || padAction(TutorialSession::Use)) && inputAllowed(TutorialSession::Use);
        const auto selected=selectedItem();
        const bool drinkable=selected.id==373 && !(selected.damage&0x4000);
        const bool edible=world.canEatCarried(slot);
        bool blockUse=false;
        if(usingRight && (drinkable || edible)){
            const auto target=world.raycast(eye(),direction(),blockReach());
            if(target.hit){const int block=world.get(target.x,target.y,target.z);
                blockUse=block==54 || block==58 || block==130 || world.canOpenFurnace(target.x,target.y,target.z) ||
                         world.canOpenBrewingStand(target.x,target.y,target.z) ||
                         block==64 || block==71 || block==107;
            }
        }
        if(usingRight && drinkable && !blockUse){
            if(potionUseSlot!=slot){potionUseSlot=slot;potionUseTicks=0;}
            potionUseTicks+=elapsedWorldTicks;
            if(potionUseTicks>=32){
                potionUseTicks=0;
                if(world.drinkPotion(selected.damage)){
                    if(tutorial)tutorial->completeUsingItem(selected.id,selected.damage);
                    message("Potion used");
                }
            }
        }else if(usingRight && edible && !blockUse){
            // FoodItem::use -> startUsingItem for EAT_DURATION ticks.
            if(potionUseSlot!=slot){potionUseSlot=slot;eatUseTicks=0;}
            eatUseTicks+=elapsedWorldTicks;
            if(eatUseTicks>=consoleEatDuration){
                eatUseTicks=0;
                const auto name=itemDisplayName(selected.id,selected.damage);
                if(world.eatCarried(slot)){
                    if(tutorial)tutorial->completeUsingItem(selected.id,selected.damage);
                    if(!tutorial)message("Ate "+name);
                }
            }
        }else{
            potionUseTicks=0;potionUseSlot=-1;eatUseTicks=0;
            if(usingRight)edit(true);else usedBlockHeld=false;
        }
        // eGameSetting_Autosave: every 15 minutes times the setting (0 off).
        const int autosave=settings.get(GameSetting::Autosave);
        if(autosave>0 && !world.streaming() && glfwGetTime()-lastSave>autosave*15.0*60.0)
            try{save();}catch(const std::exception& e){message(e.what());lastSave=glfwGetTime();}
    }
    // Word-wrapped console rich text at `scale`, with button images (or key
    // caps when playing with keyboard and mouse) `glyph` units high.
    std::vector<RichLine> richLines(const std::wstring& text,float width,float scale,float glyph,std::uint32_t colour=0xffffff)const{
        const auto spans=parseConsoleRichText(text,settings.get(GameSetting::ControlSouthPaw)!=0,colour,controlScheme());
        auto measure=[&](const std::string& t){return renderer->textWidth(t,scale);};
        auto glyphWidth=[&](const RichSpan& span){
            return (useKeyCap(span)?renderer->textWidth(keyboardLabel(span),glyph/16*.8f)+glyph*.5f:glyph)+1;
        };
        return layoutRichText(spans,width,glyphWidth,measure);
    }
    bool useKeyCap(const RichSpan& span)const{return !padInputLast && span.glyph!=PadGlyph::Shank && !keyboardLabel(span).empty();}
    void drawRichLines(Renderer& r,const std::vector<RichLine>& lines,float left,float y,float scale,float lineHeight,float glyph,bool shadow=true){
        for(const auto& line:lines){
            for(const auto& piece:line.pieces){
                const float x=left+piece.x;
                if(piece.span.glyph!=PadGlyph::None){
                    if(useKeyCap(piece.span))r.keyCap(keyboardLabel(piece.span),x,y-1,glyph);
                    else r.padGlyph(piece.span.glyph,x,y-1,glyph);
                }else{
                    const glm::vec4 colour(((piece.span.colour>>16)&255)/255.f,((piece.span.colour>>8)&255)/255.f,(piece.span.colour&255)/255.f,1);
                    r.text(piece.span.text,x,y,scale,colour,shadow);
                }
            }
            y+=lineHeight;
        }
    }
    std::wstring messageText()const{
        const auto* message=menus.message();
        if(!message)return {};
        return widen(message->note.empty()?consoleString(message->text):message->note);
    }
    // Control rectangles of the current menu scene in UI units (half the
    // PS3's 1280x720 canvas), shared by drawing and the mouse. `panel` is
    // the scene's backing panel (zero width when it has none).
    std::vector<Box> menuLayout(Box* panelOut=nullptr)const{
        std::vector<Box> boxes;Box panel;
        if(!menus.active() || !renderer){if(panelOut)*panelOut=panel;return boxes;}
        const float cw=renderer->uiWidth(),cx=cw/2;
        const auto& controls=menus.controls();
        const int n=int(controls.size());
        auto column=[&](float top){for(int i=0;i<n;++i)boxes.push_back({cx-112.5f,top+i*25,225,20});};
        auto rowHeight=[&](const MenuControl& c){return c.kind==MenuControlKind::Checkbox?17.f:c.kind==MenuControlKind::TextField?36.f:25.f;};
        switch(menus.scene()){
        case MenuScene::MainMenu:column(150);break;
        case MenuScene::Pause:case MenuScene::Death:case MenuScene::HelpAndOptions:case MenuScene::Settings:
            column(std::max(95.f,205-n*12.5f));break;
        case MenuScene::LoadOrJoin:case MenuScene::HowToPlayMenu:{
            const int visible=std::min(n,10);const float step=22;
            panel={cx-150,72,300,visible*step+14};
            const int first=std::clamp(menus.focus()-visible/2,0,std::max(0,n-visible));
            for(int i=0;i<n;++i){
                if(i<first || i>=first+visible)boxes.push_back({});
                else boxes.push_back({panel.x+8,panel.y+8+(i-first)*step,panel.w-16,20});
            }
            break;
        }
        case MenuScene::CreateWorld:case MenuScene::MoreOptions:case MenuScene::SettingsOptions:case MenuScene::SettingsAudio:
        case MenuScene::SettingsControl:case MenuScene::SettingsGraphics:case MenuScene::SettingsUI:{
            float total=16;
            for(const auto& c:controls)total+=rowHeight(c);
            const float top=menus.scene()==MenuScene::CreateWorld || menus.scene()==MenuScene::MoreOptions?62.f:std::max(62.f,170-total/2);
            panel={cx-150,top,300,total};
            float y=panel.y+8;
            for(const auto& c:controls){
                if(c.kind==MenuControlKind::TextField)boxes.push_back({panel.x+12,y+11,panel.w-24,18});
                else boxes.push_back({panel.x+12,y,panel.w-24,c.kind==MenuControlKind::Checkbox?14.f:20.f});
                y+=rowHeight(c);
            }
            break;
        }
        case MenuScene::Controls:{
            const float top=270;
            for(int i=0;i<3;++i)boxes.push_back({cx-105+i*72.f,top,66,20});
            boxes.push_back({cx-110,top+27,220,14});
            boxes.push_back({cx-110,top+44,220,14});
            break;
        }
        case MenuScene::MessageBox:{
            const auto* message=menus.message();
            const float width=320;
            const float textHeight=richLines(messageText(),width-24,.8f,10).size()*11.f;
            const float titleHeight=message && message->title>=0?18.f:0.f;
            const float height=16+titleHeight+textHeight+8+n*25;
            panel={cx-width/2,std::max(20.f,185-height/2),width,height};
            const float y=panel.y+8+titleHeight+textHeight+8;
            for(int i=0;i<n;++i)boxes.push_back({cx-110,y+i*25,220,20});
            break;
        }
        default:break;
        }
        if(panelOut)*panelOut=panel;
        return boxes;
    }
    void click(int amount=-1){
        int w,h;glfwGetWindowSize(window,&w,&h);double x,y;glfwGetCursorPos(window,&x,&y);x=x*renderer->uiWidth()/w;y=y*360/h;
        if(screen==Screen::Chest){
            const float cx=renderer->uiWidth()/2;
            if(chestSlots==27){
                int col=int(std::floor((x-(cx-95))/21));
                if(col>=0 && col<9 && y>=94 && y<157){selection=int((y-94)/21)*9+col;moveChestSelection(amount);}
                else if(int carried=carriedClickSlot(x,y,cx,175,245);carried>=0){selection=27+carried;moveChestSelection(amount);}
            }else{
                const int top=64,bag=top+(chestSlots/9)*20+18;
                int col=int(std::floor((x-(cx-90))/20));
                if(col>=0 && col<9){
                    if(y>=top && y<top+(chestSlots/9)*20){selection=int((y-top)/20)*9+col;moveChestSelection(amount);}
                    else if(y>=bag && y<bag+80){selection=chestSlots+int((y-bag)/20)*9+col;moveChestSelection(amount);}
                }
            }return;
        }
        if(screen==Screen::Furnace){
            const float cx=renderer->uiWidth()/2;
            const auto slots=furnaceSlotPositions(cx);
            for(int i=0;i<3;++i)if(x>=slots[i][0] && x<slots[i][0]+21 && y>=slots[i][1] && y<slots[i][1]+21){selection=i;moveFurnaceSelection(amount);return;}
            if(int carried=carriedClickSlot(x,y,cx,158,227);carried>=0){selection=3+carried;moveFurnaceSelection(amount);}return;
        }
        if(screen==Screen::Brewing){
            const float cx=renderer->uiWidth()/2;
            const auto slots=brewingSlotPositions(cx);
            for(int i=0;i<4;++i)if(x>=slots[i][0] && x<slots[i][0]+26 && y>=slots[i][1] && y<slots[i][1]+26){selection=i;moveBrewingSelection(amount);return;}
            if(int carried=carriedClickSlot(x,y,cx,168,237);carried>=0){selection=4+carried;moveBrewingSelection(amount);}return;
        }
        if(screen==Screen::Crafting){
            const auto list=craftingList();
            const int first=std::clamp(selection-4,0,std::max(0,int(list.size())-9));
            const float cx=renderer->uiWidth()/2;
            if(x>=cx-140 && x<cx+140 && y>=50){
                const int row=int((y-50)/28);
                if(row<9 && first+row<int(list.size())){selection=first+row;activate();}
            }
            return;
        }
        if(screen==Screen::Inventory){
            const bool playerInventory=inventoryCategory==creativeTabCount();
            const int top=playerInventory?144:105,columns=playerInventory?9:10;
            const int col=int(std::floor((x-(renderer->uiWidth()/2-(playerInventory?108:120)))/24));
            const int row=int(std::floor((y-top)/24)),candidate=row*columns+col;
            if(y>=top && row>=0 && row<(playerInventory?4:5) && col>=0 && col<columns &&
                candidate<inventoryCount()){selection=candidate;activate();}
            return;
        }
        if(screen!=Screen::Menu || !menus.active())return;
        if(editingField>=0){editingField=-1;menus.refresh();}
        if(amount==0){menuInput(MenuInput::Back);return;} // right click
        const auto scene=menus.scene();
        if(scene==MenuScene::HowToPlay){menuInput(MenuInput::Accept);return;}
        const auto boxes=menuLayout();
        for(int i=0;i<int(boxes.size());++i){
            if(!boxes[i].contains(x,y))continue;
            const auto& control=menus.controls()[i];
            if(control.kind==MenuControlKind::Slider){
                // Clicking a slider moves it towards the pointer.
                menus.setFocus(i);
                const float at=float(control.value-control.min)/std::max(1,control.max-control.min);
                const double thumb=boxes[i].x+4+at*(boxes[i].w-8);
                menuInput(x<thumb?MenuInput::Left:MenuInput::Right);
            }else{
                handleMenuEvent(menus.click(i));
                if(screen==Screen::Menu && !menus.active() && loaded)change(Screen::Playing);
            }
            return;
        }
    }
    // The keyboard key for a button image when playing with keyboard and mouse.
    std::string keyboardLabel(const RichSpan& span)const{
        const std::string& t=span.token;
        const bool menu=screen!=Screen::Playing;
        if(t=="CONTROLLER_ACTION_MOVE" || t=="CONTROLLER_VK_LS")return "W A S D";
        if(t=="CONTROLLER_ACTION_LOOK" || t=="CONTROLLER_VK_RS")return "Mouse";
        if(t=="CONTROLLER_MENU_NAVIGATE")return "Arrow Keys";
        if(t=="CONTROLLER_ACTION_JUMP")return "Space";
        if(t=="CONTROLLER_ACTION_SNEAK")return "Shift";
        if(t=="CONTROLLER_ACTION_USE" || t=="CONTROLLER_VK_LT")return "Right Click";
        if(t=="CONTROLLER_ACTION_ACTION" || t=="CONTROLLER_VK_RT")return "Left Click";
        if(t=="CONTROLLER_ACTION_LEFT_SCROLL" || t=="CONTROLLER_ACTION_RIGHT_SCROLL")return "Mouse Wheel";
        if(t=="CONTROLLER_VK_LB" || t=="CONTROLLER_VK_RB")return menu?"Tab":"Mouse Wheel";
        if(t=="CONTROLLER_ACTION_INVENTORY")return "E";
        if(t=="CONTROLLER_ACTION_CRAFTING")return "C";
        if(t=="CONTROLLER_ACTION_DROP")return "Q";
        if(t=="CONTROLLER_ACTION_CAMERA")return "F5";
        if(t=="CONTROLLER_VK_A")return "Enter";
        if(t=="CONTROLLER_VK_B")return menu?"Esc":"Backspace";
        if(t=="CONTROLLER_VK_X")return "X";
        if(t=="CONTROLLER_VK_Y")return "Y";
        if(t.rfind("CONTROLLER_ACTION_DPAD_",0)==0)return t.substr(23);
        return {};
    }
    // UIComponent_TutorialPopup: title, item icon and the formatted text.
    void drawTutorialPopup(Renderer& r){
        const float cw=r.uiWidth();
        const float width=std::min(250.f,cw*.4f),left=10,top=10,pad=6,scale=.8f,lineHeight=11,glyph=10;
        int iconId=0,iconAux=0;
        bool icon=false;
        if(tutorialPopup.icon>=0){iconId=tutorialPopup.icon;iconAux=tutorialPopup.iconAux;icon=true;}
        else icon=consoleRichTextIcon(tutorialPopup.description,iconId,iconAux) && iconId>0;
        std::wstring description=tutorialPopup.description;
        // _SetDescription: a reminder is prefixed with IDS_TUTORIAL_REMINDER.
        if(tutorialPopup.isReminder)description=widen(consoleText("IDS_TUTORIAL_REMINDER"))+description;
        const float textLeft=left+pad+(icon?24.f:0.f);
        const float textWidth=width-(textLeft-left)-pad;
        const auto lines=richLines(description,textWidth,scale,glyph);
        const std::string title=narrow(tutorialPopup.title);
        const float titleHeight=title.empty()?0.f:13.f;
        float height=pad*2+titleHeight+lines.size()*lineHeight;
        if(icon)height=std::max(height,pad*2+titleHeight+22);
        // eGameSetting_InterfaceOpacity fades the popup's backing.
        r.rect(left,top,width,height,{0,0,0,.9f*settings.get(GameSetting::InterfaceOpacity)/100.f});
        r.rect(left,top,width,1,{1,1,1,.35f});
        float y=top+pad;
        if(!title.empty()){r.text(title,left+pad,y,.9f,{1,1,.4f,1});y+=titleHeight;}
        if(icon && iconId>0)drawItemIcon(r,iconId,iconAux,left+pad,y,18);
        drawRichLines(r,lines,textLeft,y,scale,lineHeight,glyph);
    }
    // A button image (or the key when playing with keyboard and mouse) and
    // its label; returns the x after it.
    float drawTooltip(Renderer& r,float x,float y,PadGlyph glyph,const std::string& keyLabel,const std::string& label){
        x+=padInputLast || keyLabel.empty()?r.padGlyph(glyph,x,y-2,11):r.keyCap(keyLabel,x,y-2,11);
        x+=4;r.text(label,x,y,.85f);
        return x+r.textWidth(label,.85f)+14;
    }
    std::string actionKey(const char* token)const{RichSpan span;span.token=token;return keyboardLabel(span);}
    // Minecraft::tick's in-game tooltips: Jump (swim up), crafting or the
    // creative menu, inventory, and what Use and Action do to the target.
    void drawGameTooltips(Renderer& r){
        using S=TutorialSession;
        const int scheme=controlScheme();
        const bool survival=world.survival();
        const auto held=selectedItem();
        int jump=-1,use=-1,act=-1;
        if(world.playerUnderWater())jump=consoleStringId("IDS_TOOLTIPS_SWIMUP");
        if(held.id==373)use=consoleStringId(held.damage&0x4000?"IDS_TOOLTIPS_THROW":"IDS_TOOLTIPS_DRINK");
        else if(survival && world.canEatCarried(slot))use=consoleStringId("IDS_TOOLTIPS_EAT");
        if(world.pickEntity(eye(),direction(),entityReach()))act=consoleStringId("IDS_TOOLTIPS_HIT");
        else if(const auto h=world.raycast(eye(),direction(),blockReach());h.hit){
            act=consoleStringId("IDS_TOOLTIPS_MINE");
            const int block=world.get(h.x,h.y,h.z);
            // Chests open; workbenches, furnaces, brewing stands, wooden doors,
            // levers, buttons, trapdoors and gates are used.
            if(block==54 || block==130)use=consoleStringId("IDS_TOOLTIPS_OPEN");
            else if(block==58 || block==61 || block==62 || block==117 || block==116 || block==145 || block==64 ||
                    block==69 || block==77 || block==143 || block==96 || block==107)use=consoleStringId("IDS_TOOLTIPS_USE");
            else if(use<0 && held.id && (held.id<256 || placedTileForItem(held.id)>0))use=consoleStringId("IDS_TOOLTIPS_PLACE");
            else if(held.id>=290 && held.id<=294)use=consoleStringId("IDS_TOOLTIPS_TILL");
            else if(held.id==295 || held.id==372)use=consoleStringId("IDS_TOOLTIPS_PLANT");
            else if(held.id==351 && held.damage==15){
                // Bone meal grows these.
                for(int grows:{6,59,2,39,40,105,104,141,142})if(block==grows)use=consoleStringId("IDS_TOOLTIPS_GROW");
            }
        }
        float x=16;const float y=338;
        if(jump>=0)x=drawTooltip(r,x,y,consoleActionGlyph(S::Jump,scheme),actionKey("CONTROLLER_ACTION_JUMP"),consoleString(jump));
        if(survival)x=drawTooltip(r,x,y,consoleActionGlyph(S::CraftingAction,scheme),actionKey("CONTROLLER_ACTION_CRAFTING"),consoleText("IDS_CONTROLS_CRAFTING"));
        x=drawTooltip(r,x,y,consoleActionGlyph(S::InventoryAction,scheme),actionKey("CONTROLLER_ACTION_INVENTORY"),consoleText("IDS_CONTROLS_INVENTORY"));
        if(use>=0)x=drawTooltip(r,x,y,consoleActionGlyph(S::Use,scheme),actionKey("CONTROLLER_ACTION_USE"),consoleString(use));
        if(act>=0)x=drawTooltip(r,x,y,consoleActionGlyph(S::ActionButton,scheme),actionKey("CONTROLLER_ACTION_ACTION"),consoleString(act));
    }
    static std::string creditText(const ConsoleCredit& credit){
        std::string text=credit.format;
        for(int id:{credit.first,credit.second}){
            if(id<0)continue;
            const auto at=text.find("%ls");
            if(at!=std::string::npos)text.replace(at,3,consoleString(id));
        }
        return text;
    }
    // The controller picture's labels (UIScene_ControlsMenu::PositionAllText):
    // every button an action is bound to in the shown layout gets its text.
    void drawControlsDiagram(Renderer& r){
        using S=TutorialSession;
        const float cw=r.uiWidth(),cx=cw/2;
        const int layout=menus.controlsLayout();
        const bool creative=menus.context().creative,southpaw=settings.get(GameSetting::ControlSouthPaw)!=0;
        r.centered(consoleText(("IDS_CONTROLS_SCHEME"+std::to_string(layout)).c_str()),44,1.2f);
        std::vector<std::pair<const char*,int>> lines{
            {creative?"IDS_CONTROLS_JUMPFLY":"IDS_CONTROLS_JUMP",S::Jump},{"IDS_CONTROLS_INVENTORY",S::InventoryAction},
            {"IDS_CONTROLS_PAUSE",S::PauseMenu},{creative?"IDS_CONTROLS_SNEAKFLY":"IDS_CONTROLS_SNEAK",S::SneakToggle},
            {"IDS_CONTROLS_USE",S::Use},{"IDS_CONTROLS_ACTION",S::ActionButton},{"IDS_CONTROLS_HELDITEM",S::RightScroll},
            {"IDS_CONTROLS_HELDITEM",S::LeftScroll},{"IDS_CONTROLS_DROP",S::Drop},{"IDS_CONTROLS_CRAFTING",S::CraftingAction},
            {"IDS_CONTROLS_THIRDPERSON",S::RenderThirdPerson},{"IDS_CONTROLS_PLAYERS",S::GameInfo},
            {southpaw?"IDS_CONTROLS_LOOK":"IDS_CONTROLS_MOVE",S::Right},{southpaw?"IDS_CONTROLS_MOVE":"IDS_CONTROLS_LOOK",S::LookRight}};
        if(creative && layout==0)lines.push_back({"IDS_CONTROLS_DPAD",S::DpadLeft});
        struct Pad { unsigned bits; PadGlyph glyph; bool left; };
        static constexpr Pad pads[]{
            {0x800000,PadGlyph::L2,true},{0x80,PadGlyph::L1,true},{0x400,PadGlyph::DpadUp,true},{0x800,PadGlyph::DpadDown,true},
            {0x1000,PadGlyph::DpadLeft,true},{0x2000,PadGlyph::DpadRight,true},{0x4000,PadGlyph::LeftStick,true},
            {0x200,PadGlyph::L3,true},{0x20,PadGlyph::Select,true},
            {0x400000,PadGlyph::R2,false},{0x40,PadGlyph::R1,false},{0x8,PadGlyph::Triangle,false},{0x2,PadGlyph::Circle,false},
            {0x4,PadGlyph::Square,false},{0x1,PadGlyph::Cross,false},{0x40000,PadGlyph::RightStick,false},
            {0x100,PadGlyph::R3,false},{0x10,PadGlyph::Start,false}};
        std::array<std::string,std::size(pads)> labels;
        for(const auto& [text,action]:lines){
            const unsigned bits=consoleJoypadButtons(action,layout);
            for(std::size_t i=0;i<std::size(pads);++i)if(bits&pads[i].bits)labels[i]=consoleText(text);
        }
        r.rect(cx-230,60,460,200,{0,0,0,.55f});
        float left=70,right=70;
        for(std::size_t i=0;i<std::size(pads);++i){
            if(labels[i].empty())continue;
            float& y=pads[i].left?left:right;
            if(pads[i].left){
                r.padGlyph(pads[i].glyph,cx-34,y-2,12);
                r.text(labels[i],cx-40-r.textWidth(labels[i],.85f),y,.85f);
            }else{
                r.padGlyph(pads[i].glyph,cx+22,y-2,12);
                r.text(labels[i],cx+40,y,.85f);
            }
            y+=17;
        }
    }
    // The UIScene_* menus: the scene's panel and controls, its description,
    // and the button tooltips.
    void drawMenu(Renderer& r,bool inWorld){
        const float cw=r.uiWidth(),cx=cw/2;
        const MenuScene scene=menus.scene(),root=menus.root();
        const glm::vec4 ink{.25f,.25f,.25f,1},white{1,1,1,1},focusText{1,1,.63f,1},disabled{.55f,.55f,.55f,1};
        if(root==MenuScene::Death)r.rect(0,0,cw,360,{.45f,.05f,.05f,.55f});
        else if(inWorld)r.rect(0,0,cw,360,{0,0,0,.55f});
        const bool overMain=scene==MenuScene::MainMenu || (scene==MenuScene::MessageBox && menus.below()==MenuScene::MainMenu);
        if(overMain || scene==MenuScene::HelpAndOptions || scene==MenuScene::Settings){
            // The loose PS3 title image includes an Xbox subtitle; use only its common wordmark.
            r.sprite("logo",cx-142.75f,38,285.5f,40,{0,0,1,80/138.f});
            if(overMain)r.centered("PLAYSTATION 3 EDITION",88,1.35f,{.82,.82,.82,1});
        }
        const int titleId=menus.title();
        if(scene==MenuScene::Death)r.centered(consoleString(titleId),90,2);
        else if(titleId>=0 && scene!=MenuScene::MessageBox)r.centered(consoleString(titleId),44,1.4f);
        Box panel;
        const auto boxes=menuLayout(&panel);
        if(scene==MenuScene::MessageBox)r.rect(0,0,cw,360,{0,0,0,.45f});
        if(panel.w>0)r.panel(panel.x,panel.y,panel.w,panel.h);
        if(const auto* message=menus.message()){
            float y=panel.y+8;
            if(message->title>=0){const std::string title=consoleString(message->title);r.text(title,cx-r.textWidth(title)/2,y,1,ink,false);y+=18;}
            drawRichLines(r,richLines(messageText(),panel.w-24,.8f,10,0x383838),panel.x+12,y,.8f,11,10,false);
        }
        if(scene==MenuScene::Controls)drawControlsDiagram(r);
        if(scene==MenuScene::HowToPlay){
            const int page=menus.howToPlayPage();
            if(page!=howToPlayShown){howToPlayShown=page;howToPlayScroll=0;}
            const Box view{cx-230,40,460,280};
            r.rect(view.x-6,view.y-6,view.w+12,view.h+12,{0,0,0,.8f});
            const auto lines=richLines(widen(consoleString(menus.howToPlayText())),view.w-10,.8f,10);
            const float content=lines.size()*11.f;
            howToPlayScroll=std::clamp(howToPlayScroll,0.f,std::max(0.f,content-view.h));
            int fbw,fbh;glfwGetFramebufferSize(window,&fbw,&fbh);
            const float k=fbh/360.f;
            glEnable(GL_SCISSOR_TEST);
            glScissor(int(view.x*k),int(fbh-(view.y+view.h)*k),int(view.w*k),int(view.h*k));
            drawRichLines(r,lines,view.x+2,view.y+2-howToPlayScroll,.8f,11,10);
            glDisable(GL_SCISSOR_TEST);
            if(content>view.h){
                const float bar=view.h*view.h/content;
                r.rect(view.x+view.w+1,view.y+(view.h-bar)*howToPlayScroll/(content-view.h),3,bar,{1,1,1,.5f});
            }
        }
        if(scene==MenuScene::Credits){
            r.rect(0,0,cw,360,{0,0,0,.8f});
            std::size_t count=0;
            const auto* credits=consoleCredits(count);
            float y=360-menus.creditsScroll;
            constexpr float scales[]{.8f,1.f,1.3f,2.f},heights[]{11,13,17,26};
            for(std::size_t i=0;i<count;++i){
                const auto& credit=credits[i];
                const int size=std::clamp(credit.size,0,3);
                if(credit.first==-2){y+=30;continue;} // CREDIT_ICON: the middleware logos are not supplied
                if(y>-30 && y<370){
                    const std::string text=creditText(credit);
                    r.centered(text,y,scales[size],size==2?glm::vec4(1,1,.4f,1):white);
                }
                y+=heights[size];
            }
            if(y<-30)menus.creditsScroll=0;
        }
        const auto& controls=menus.controls();
        for(int i=0;i<int(controls.size()) && i<int(boxes.size());++i){
            const auto& c=controls[i];const Box& b=boxes[i];
            if(b.w<=0)continue;
            const bool focused=i==menus.focus();
            switch(c.kind){
            case MenuControlKind::Button:{
                const bool chosen=scene==MenuScene::Controls && c.id==settings.get(GameSetting::ControlScheme);
                r.sprite(focused?"button_focus":"button",b.x,b.y,b.w,b.h,{0,0,1,1},c.enabled?glm::vec4(1):glm::vec4(.6f,.6f,.6f,1));
                const float scale=std::min(1.f,(b.w-10)/std::max(1.f,r.textWidth(c.label)));
                r.text(c.label,b.x+(b.w-r.textWidth(c.label,scale))/2,b.y+6,scale,!c.enabled?disabled:focused || chosen?focusText:white);
                break;
            }
            case MenuControlKind::ListItem:
                r.rect(b.x,b.y,b.w,b.h,focused?glm::vec4(1,1,1,.6f):glm::vec4(0,0,0,.1f));
                r.text(c.label,b.x+6,b.y+6,1,ink,false);
                break;
            case MenuControlKind::TextField:{
                const bool editing=editingField==c.id;
                r.text(c.label,b.x,b.y-10,.8f,ink,false);
                r.rect(b.x-1,b.y-1,b.w+2,b.h+2,focused?glm::vec4(1,1,.63f,1):glm::vec4(.35f,.35f,.35f,1));
                r.rect(b.x,b.y,b.w,b.h,{0,0,0,1});
                if(c.text.empty() && !editing)r.text(c.placeholder,b.x+4,b.y+5,.8f,{.6f,.6f,.6f,1});
                else r.text(c.text+(editing && std::fmod(glfwGetTime(),1.0)<.5?"_":""),b.x+4,b.y+5,1,white);
                break;
            }
            case MenuControlKind::Checkbox:
                if(focused)r.rect(b.x-3,b.y-2,b.w+6,b.h+4,{1,1,1,.5f});
                r.rect(b.x,b.y+1,12,12,{.2f,.2f,.2f,1});
                r.rect(b.x+1,b.y+2,10,10,c.enabled?glm::vec4(.92f,.92f,.92f,1):glm::vec4(.6f,.6f,.6f,1));
                if(c.checked)r.rect(b.x+3,b.y+4,6,6,c.enabled?glm::vec4(.15f,.55f,.15f,1):glm::vec4(.35f,.35f,.35f,1));
                r.text(c.label,b.x+18,b.y+3,.85f,c.enabled?ink:disabled,false);
                break;
            case MenuControlKind::Slider:{
                const float at=float(c.value-c.min)/std::max(1,c.max-c.min);
                r.sprite(focused?"button_focus":"button",b.x,b.y,b.w,b.h);
                r.rect(b.x+3,b.y+b.h-5,(b.w-6)*at,2,{.5f,.85f,.4f,1});
                r.rect(b.x+2+(b.w-12)*at,b.y+1,8,b.h-2,{0,0,0,.45f});
                r.rect(b.x+3+(b.w-12)*at,b.y+2,6,b.h-4,{.85f,.85f,.85f,1});
                const float scale=std::min(1.f,(b.w-10)/std::max(1.f,r.textWidth(c.label)));
                r.text(c.label,b.x+(b.w-r.textWidth(c.label,scale))/2,b.y+6,scale,focused?focusText:white);
                break;
            }
            }
        }
        if(const int description=menus.focusedDescription();description>=0 && scene!=MenuScene::MessageBox && panel.w>0){
            const auto lines=richLines(widen(consoleString(description)),panel.w-16,.75f,9);
            const float y=panel.y+panel.h+6;
            r.rect(panel.x,y,panel.w,lines.size()*10+10,{0,0,0,.75f});
            drawRichLines(r,lines,panel.x+8,y+5,.75f,10,9);
        }
        // ui.SetTooltips: A, B and X.
        const auto tips=menus.tooltips();
        constexpr PadGlyph glyphs[3]{PadGlyph::Cross,PadGlyph::Circle,PadGlyph::Square};
        const char* keys[3]{"Enter","Esc","X"};
        float x=16;
        for(int i=0;i<3;++i)if(tips[i]>=0)x=drawTooltip(r,x,338,glyphs[i],keys[i],consoleString(tips[i]));
    }
    void draw(){
        int w,h;glfwGetFramebufferSize(window,&w,&h);renderer->resize(w,h);auto& r=*renderer;float cw=r.uiWidth();
        const bool inWorld=loaded && screen!=Screen::Loading && screen!=Screen::FindingSeed;
        if(inWorld)r.world(world,eye(),yaw,pitch,viewDistance,screen==Screen::Playing?world.raycast(eye(),direction(),blockReach()):Hit{});
        else {glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);}
        r.beginUI();
        if(!inWorld){
            const float shift=std::fmod(float(glfwGetTime())*.004f,1.f);
            r.sprite("panorama_n",0,0,cw,360,{shift,0,shift+.62f,1});
            r.rect(0,0,cw,360,{0,0,0,.12});
        }
        if(screen==Screen::Playing){
          // eGameSetting_DisplayHUD
          if(settings.get(GameSetting::DisplayHUD)){
            const auto carried=world.carriedItems();
            r.rect(cw/2-4,179.5,9,1,{1,1,1,.85});r.rect(cw/2-.5,176,1,9,{1,1,1,.85});
            r.sprite("gui",cw/2-91,328,182,22,{0,0,182/256.f,22/256.f});
            r.sprite("gui",cw/2-92+slot*20,327,24,24,{0,22/256.f,24/256.f,46/256.f});
            for(int i=0;i<9;++i){
                if(carried[i].id){
                    const float x=cw/2-88+i*20;
                    drawItemIcon(r,carried[i].id,carried[i].damage,x,331);
                    if(carried[i].count>1)r.text(std::to_string(carried[i].count),x+8,341,.65f);
                }
            }
            const bool survival=world.survival();
            // Gui::render from icons.png, relative to the hotbar top (328): XP bar
            // -8, hearts/food -18 (yLine1), air/armour -28 (yLine2). Hearts shake
            // at 4 health or less, ripple under Regeneration and blink white
            // while invulnerable; food shakes when saturation is exhausted.
            const int tickCount=int(world.time()&0x7fffffff);
            if(survival){
                const auto icon=[&](float x,float y,int u,int v){
                    r.sprite("icons",x,y,9,9,{u/256.f,v/256.f,(u+9)/256.f,(v+9)/256.f});};
                const float xLeft=cw/2-91,xRight=cw/2+91,yLine1=310,yLine2=300;
                const int progress=int(world.playerExperienceProgress()*183);
                r.sprite("icons",xLeft,320,182,5,{0,64/256.f,182/256.f,69/256.f});
                if(progress>0)r.sprite("icons",xLeft,320,float(progress),5,{0,69/256.f,progress/256.f,74/256.f});
                const int health=world.playerHealth(),food=world.playerFoodLevel(),invulnerable=world.playerInvulnerableTicks();
                const bool blink=invulnerable>=10 && invulnerable/3%2==1;
                const int lastHealth=world.playerLastHealth();
                bool poison=false,hunger=false,regeneration=false;
                for(const auto& effect:world.activePotionEffects()){poison|=effect.id==19;hunger|=effect.id==17;regeneration|=effect.id==10;}
                const int wave=regeneration?tickCount%25:-1;
                Random shake(std::int64_t(tickCount)*312871);
                for(int i=0;i<10;++i){
                    const int base=16+(poison?36:0);
                    float y=yLine1;
                    if(health<=4)y+=shake.nextInt(2);
                    if(i==wave)y-=2;
                    const float x=xLeft+i*8;
                    icon(x,y,16+(blink?9:0),0);
                    if(blink){if(i*2+1<lastHealth)icon(x,y,base+54,0);else if(i*2+1==lastHealth)icon(x,y,base+63,0);}
                    if(i*2+1<health)icon(x,y,base+36,0);else if(i*2+1==health)icon(x,y,base+45,0);
                }
                for(int i=0;i<10;++i){
                    const int base=16+(hunger?36:0),background=hunger?13:0;
                    float y=yLine1;
                    if(world.playerSaturation()<=0 && tickCount%(food*3+1)==0)y+=shake.nextInt(3)-1;
                    const float x=xRight-i*8-9;
                    icon(x,y,16+background*9,27);
                    if(i*2+1<food)icon(x,y,base+36,27);else if(i*2+1==food)icon(x,y,base+45,27);
                }
                const int air=world.playerAir();
                if(air<300){
                    const int count=int(std::ceil((air-2)*10.0/300)),extra=int(std::ceil(air*10.0/300))-count;
                    for(int i=0;i<count+extra;++i)icon(xRight-i*8-9,yLine2,i<count?16:25,18);
                }
            }
            if(const int level=world.playerExperienceLevel();level>0){
                const std::string text=std::to_string(level);
                const float x=(cw-r.textWidth(text,1))/2,y=survival?310.f:315.f;
                for(const auto& [dx,dy]:{std::pair{1.f,0.f},{-1.f,0.f},{0.f,1.f},{0.f,-1.f}})
                    r.text(text,x+dx,y+dy,1,{0,0,0,1},false);
                r.text(text,x,y,1,{.5f,1,.125f,1},false); // 0x80ff20
            }
            if(carried[slot].id)r.centered(itemDisplayName(carried[slot].id,carried[slot].damage),survival?290:312,1);
            if(settings.get(GameSetting::Tooltips))drawGameTooltips(r);
            if(world.streaming() || r.rebuilding()){
                const std::string loading=consoleText("IDS_PROGRESS_BUILDING_TERRAIN");
                r.text(loading,cw-r.textWidth(loading,.8f)-10,10,.8f);
            }
          }
        }else if(screen==Screen::Chest){
            const float cx=cw/2;
            r.rect(0,0,cw,360,{0,0,0,.55});
            if(chestSlots==27){
                r.panel(cx-107.5f,69,215,207.5f);
                r.text(enderChestOpen?"Ender Chest":"Chest",cx-94.5f,77,1,{.275f,.275f,.275f,1});
                r.text("Inventory",cx-94.5f,160,1,{.275f,.275f,.275f,1});
            }else{
                r.panel(cx-102,38,204,254);
                r.centered("Large Chest",46,1,{.2,.2,.2,1});r.centered("Inventory",189,1,{.2,.2,.2,1});
            }
            auto contents=enderChestOpen?world.enderChestItems():world.chestItems(chestX,chestY,chestZ);contents.resize(chestSlots);
            auto carried=world.carriedItems();contents.insert(contents.end(),carried.begin(),carried.end());
            for(int i=0;i<chestSlots+36;++i){const auto& item=contents[i];
                const auto point=chestSlots==27
                    ?(i<27?std::array<float,2>{cx-95+(i%9)*21.f,94+(i/9)*21.f}:carriedSlotPosition(cx,i-27,175,245))
                    :std::array<float,2>{cx-90+(i%9)*20.f,float(i<chestSlots?64+(i/9)*20:202+((i-chestSlots)/9)*20)};
                float x=point[0],y=point[1];const float size=chestSlots==27?21.f:18.f;
                drawSlotFrame(r,x,y,size,i==selection);
                if(!item.id)continue;
                const float iconSize=chestSlots==27?19.f:16.f;
                drawItemIcon(r,item.id,item.damage,x+(size-iconSize)/2,y+(size-iconSize)/2,iconSize);
                r.text(std::to_string(item.count),x+10,y+10,.7f);
            }
            const auto& chosen=contents[selection];
            if(chosen.id){std::string name=itemDisplayName(chosen.id,chosen.damage);
                r.centered(name+(chosen.enchanted?" (Enchanted)":""),312,.9f);}
            r.centered("Enter/Cross: All   H/Square: Half   R/Triangle: One",335,.8f);
            r.centered("Esc / Circle: Close",348,.7f);
        }else if(screen==Screen::Furnace){
            const float cx=cw/2;
            r.rect(0,0,cw,360,{0,0,0,.55});r.panel(cx-107,47.5f,214,215);
            r.text("Furnace",cx-94,54,1,{.275f,.275f,.275f,1});
            r.text("Inventory",cx-95,143,1,{.275f,.275f,.275f,1});
            r.sprite("arrow_off",cx+5.5f,94.5f,36,24);
            r.sprite("flame_off",cx-33,92.5f,24,24);
            auto contents=world.furnaceItems(chestX,chestY,chestZ);contents.resize(3);
            auto carried=world.carriedItems();contents.insert(contents.end(),carried.begin(),carried.end());
            const auto locations=furnaceSlotPositions(cx);
            for(int i=0;i<39;++i){const auto& item=contents[i];const auto point=i<3?locations[i]:carriedSlotPosition(cx,i-3,158,227);
                float x=point[0],y=point[1];
                drawSlotFrame(r,x,y,21,i==selection);
                if(!item.id)continue;drawItemIcon(r,item.id,item.damage,x+1,y+1,19);
                if(item.count>1)r.text(std::to_string(item.count),x+12,y+12,.7f);
            }
            auto progress=world.furnaceState(chestX,chestY,chestZ);
            if(progress.burn>0){const float lit=std::clamp(float(progress.burn)/std::max(1,progress.duration),0.f,1.f);
                r.sprite("flame_on",cx-33,116.5f-24*lit,24,24*lit,{0,1-lit,1,1});}
            if(progress.cook>0){const float cooked=std::clamp(float(progress.cook)/200.f,0.f,1.f);
                r.sprite("arrow_on",cx+5.5f,94.5f,36*cooked,24,{0,0,cooked,1});}
            if(contents[selection].id){const auto& item=contents[selection];
                r.centered(itemDisplayName(item.id,item.damage),312,.9f);}
            r.centered(std::string("F / L3: Deposit to ")+(furnaceFuelTarget?"Fuel":"Input"),324,.8f);
            r.centered("Enter/Cross: All   H/Square: Half   R/Triangle: One",336,.7f);
            r.centered("Esc / Circle: Close",348,.7f);
        }else if(screen==Screen::Brewing){
            const float cx=cw/2;
            r.rect(0,0,cw,360,{0,0,0,.55});r.panel(cx-107,47.5f,214,225);
            r.text("Brewing Stand",cx-95,54,1,{.275f,.275f,.275f,1});
            r.text("Inventory",cx-95,153,1,{.275f,.275f,.275f,1});
            r.sprite("brewing_stand",cx-48.5f,68.5f,96,86);
            r.sprite("brewing_arrow_off",cx+15,69.5f,13.5f,42);
            r.sprite("brewing_bubbles_off",cx-33,69.5f,18,42);
            auto contents=world.brewingItems(chestX,chestY,chestZ);contents.resize(4);
            auto carried=world.carriedItems();contents.insert(contents.end(),carried.begin(),carried.end());
            const auto locations=brewingSlotPositions(cx);
            for(int i=0;i<40;++i){const auto& item=contents[i];const auto point=i<4?locations[i]:carriedSlotPosition(cx,i-4,168,237);
                float x=point[0],y=point[1];
                const bool brewingSlot=i<4;
                drawSlotFrame(r,x,y,brewingSlot?26.f:21.f,i==selection,!brewingSlot);
                if(!item.id)continue;
                drawItemIcon(r,item.id,item.damage,x+1,y+1,brewingSlot?24.f:19.f);
                if(item.count>1)r.text(std::to_string(item.count),x+(brewingSlot?17.f:12.f),y+(brewingSlot?17.f:12.f),.7f);
            }
            const int remaining=world.brewingProgress(chestX,chestY,chestZ);
            if(remaining>0){
                const float arrow=float(remaining)/400.f;
                r.sprite("brewing_arrow_on",cx+15,69.5f,13.5f,42*arrow,{0,0,1,arrow});
                const float bubbles=float((400-remaining)%30)/30.f;
                if(bubbles>0)r.sprite("brewing_bubbles_on",cx-33,111.5f-42*bubbles,18,42*bubbles,{0,1-bubbles,1,1});
            }
            if(contents[selection].id){const auto& item=contents[selection];
                r.centered(itemDisplayName(item.id,item.damage),312,.9f);}
            r.centered("F / L3: Bottle position "+std::to_string(brewingBottleTarget+1),324,.8f);
            r.centered("Enter/Cross: All   H/Square: Half   R/Triangle: One",336,.7f);
            r.centered("Esc / Circle: Close",348,.7f);
        }else if(screen==Screen::Crafting){
            const float cx=cw/2;
            r.rect(0,0,cw,360,{0,0,0,.55});r.panel(cx-150,28,300,296);
            r.text(craftingTable?"Crafting Table":"Crafting",cx-137,36,1,{.275f,.275f,.275f,1});
            static constexpr std::array<const char*,7> tabNames{"Structures","Tools","Food","Armour","Mechanisms","Transport","Decoration"};
            const std::string tab=std::string("< ")+tabNames[craftingGroup]+" >";
            r.text(tab,cx+137-r.textWidth(tab,.8f),37,.8f,{.2f,.2f,.2f,1},false);
            const auto list=craftingList();
            const auto carried=world.carriedItems();
            constexpr int rows=9;
            const int first=std::clamp(selection-rows/2,0,std::max(0,int(list.size())-rows));
            for(int row=0;row<rows && first+row<int(list.size());++row){
                const auto& recipe=*list[first+row];
                const float y=52+row*28.f;
                const bool chosen=first+row==selection,craftable=world.canCraft(recipe);
                if(chosen)r.rect(cx-140,y-2,280,27,{.48f,.72f,.48f,.55f});
                drawSlotFrame(r,cx-138,y,23,false);
                drawItemIcon(r,recipe.id,recipe.damage,cx-135.5f,y+2.5f,18);
                const glm::vec4 ink=craftable?glm::vec4(.15,.15,.15,1):glm::vec4(.5,.5,.5,1);
                std::string name=itemDisplayName(recipe.id,recipe.damage);
                if(recipe.count>1)name+=" x"+std::to_string(recipe.count);
                r.text(name,cx-108,y+2,.85f,ink,false);
                std::string needs;
                for(const auto& ingredient:recipe.ingredients){
                    int have=0;for(const auto& item:carried)
                        if(item.id==ingredient.id && (ingredient.damage<0 || item.damage==ingredient.damage))have+=item.count;
                    if(!needs.empty())needs+="  ";
                    needs+=std::to_string(ingredient.count)+" "+itemDisplayName(ingredient.id,std::max(0,ingredient.damage))+
                           " ("+std::to_string(have)+")";
                }
                r.text(needs,cx-108,y+13,.6f,ink,false);
            }
            r.centered(std::to_string(list.empty()?0:selection+1)+" / "+std::to_string(list.size()),308,.7f,{.2,.2,.2,1});
            r.centered("Enter/Cross: Craft   Tab / L1 R1: Group   Esc / Circle: Close",336,.7f);
        }else if(screen==Screen::Inventory){
            const bool playerInventory=inventoryCategory==creativeTabCount();
            r.rect(0,0,cw,360,{0,0,0,.55});
            r.rect(cw/2-(playerInventory?122.f:132.f),playerInventory?91.f:48.f,
                   playerInventory?244.f:264.f,playerInventory?208.f:252.f,{.14,.14,.14,1});
            r.rect(cw/2-(playerInventory?120.f:130.f),playerInventory?93.f:50.f,
                   playerInventory?240.f:260.f,playerInventory?204.f:248.f,{.77,.77,.77,1});
            r.centered(playerInventory?"Inventory":creativeTab(inventoryCategory).name,
                       playerInventory?111.f:63.f,1,{.2,.2,.2,1});
            r.centered(world.survival()?"C / Square: Crafting":"Tab / L1 R1: Switch Category",playerInventory?127.f:82.f,.75f,{.2,.2,.2,1});
            if(playerInventory){
                const auto carried=world.carriedItems();
                for(int i=0;i<36;++i){
                    const float x=cw/2-108+(i%9)*24,y=144+(i/9)*24;
                    r.rect(x,y,22,22,i==selection?glm::vec4(1,1,.7,1):glm::vec4(.35,.35,.35,1));
                    if(carried[i].id){
                        drawItemIcon(r,carried[i].id,carried[i].damage,x+3,y+3);
                        if(carried[i].count>1)r.text(std::to_string(carried[i].count),x+10,y+13,.6f);
                    }
                }
                if(carried[selection].id)r.centered(itemDisplayName(carried[selection].id,carried[selection].damage),257,.85f,{.2,.2,.2,1});
                r.centered("Enter/Cross: Select or swap into quickbar",277,.75f,{.2,.2,.2,1});
            }else{
                const auto tab=creativeTab(inventoryCategory);
                for(int i=0;i<inventoryCount();++i){
                    const auto item=tab.items[creativePage[inventoryCategory]*50+i];
                    const float x=cw/2-120+(i%10)*24,y=105+(i/10)*24;
                    r.rect(x,y,22,22,i==selection?glm::vec4(1,1,.7,1):glm::vec4(.35,.35,.35,1));
                    drawItemIcon(r,item.id,item.damage,x+3,y+3);
                }
                const auto item=selectedCreativeEntry();
                r.centered(itemDisplayName(item.id,item.damage),243,.9f,{.2,.2,.2,1});
                r.centered("Enter/Cross: Take   Shift: Full Stack",263,.7f,{.2,.2,.2,1});
                const int pages=std::max(1,int((tab.items.size()+49)/50));
                r.centered("Page "+std::to_string(creativePage[inventoryCategory]+1)+" / "+std::to_string(pages)+"  [ / ]",281,.75f,{.2,.2,.2,1});
            }
        }else if(screen==Screen::Menu && menus.active()){
            drawMenu(r,inWorld);
        }else if(screen==Screen::FindingSeed || screen==Screen::Loading){
            // The console's progress screen: a message over the panorama.
            if(inWorld)r.rect(0,0,cw,360,{0,0,0,.65});
            std::string status;float progress=0;
            if(screen==Screen::FindingSeed){status=consoleText("IDS_PROGRESS_NEW_WORLD_SEED");progress=std::fmod(float(seedAttempts)/64.f,1.f);}
            else{
                const int stage=loadPhase?std::clamp(loadPhase->load(),0,4):0;
                const char* stages[]{"IDS_PROGRESS_SAVING_LEVEL","IDS_PROGRESS_INITIALISING_SERVER","IDS_PROGRESS_SAVING_CHUNKS",
                                     "IDS_PROGRESS_SAVING_TO_DISC","IDS_PROGRESS_BUILDING_TERRAIN"};
                status=consoleText(stages[stage]);progress=(stage+1)/5.f;
            }
            r.centered(status,160,1);
            r.rect(cw/2-100,180,200,6,{.2f,.2f,.2f,1});
            r.rect(cw/2-100,180,200*progress,6,{.5f,.85f,.4f,1});
        }
        if(inWorld && tutorialPopupShown())drawTutorialPopup(r);
        if(glfwGetTime()<toastUntil){float tw=r.textWidth(toast,.8f);r.rect((cw-tw)/2-6,292,tw+12,15,{0,0,0,.8f});r.centered(toast,296,.8f);}
    }
};
std::filesystem::path defaultData() {
    // Deliberately separate from the existing game's data and saves.
    const char* home=std::getenv("HOME");
    return home?std::filesystem::path(home)/"Library/Application Support/MinecraftConsolePort":std::filesystem::current_path()/"console_port_saves";
}
}
int main(int argc,char** argv){
    std::filesystem::path assets=CONSOLE_ASSET_DIR,data=defaultData(),smokeDir;
    for(int i=1;i<argc;++i){std::string arg=argv[i];
        if(arg=="--help"){std::cout<<"Minecraft console desktop port\n  --assets PATH     Asset folder\n  --data-dir PATH   Isolated saves/screenshots\n  --smoke-test PATH Render menu and gameplay screenshots, test editing/saving, then exit\n";return 0;}
        if(i+1>=argc){std::cerr<<"Missing value for "<<arg<<'\n';return 2;}
        if(arg=="--assets")assets=argv[++i];else if(arg=="--data-dir")data=argv[++i];else if(arg=="--smoke-test")smokeDir=argv[++i];else {std::cerr<<"Unknown option: "<<arg<<'\n';return 2;}
    }
    glfwSetErrorCallback([](int code,const char* description){std::cerr<<"GLFW "<<code<<": "<<description<<'\n';});
    if(!glfwInit())return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,2);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT,1);
    // Automated verification must not show an unfinished build to the user.
    glfwWindowHint(GLFW_VISIBLE,smokeDir.empty()?GLFW_TRUE:GLFW_FALSE);
    auto window=glfwCreateWindow(1280,720,"Minecraft - Console Port",nullptr,nullptr);
    if(!window){glfwTerminate();return 1;}
    glfwMakeContextCurrent(window);glfwSwapInterval(1);
    if(!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))){glfwDestroyWindow(window);glfwTerminate();return 1;}
    int result=0;
    try{
        App app;app.window=window;app.scripted=!smokeDir.empty();app.dataDir=smokeDir.empty()?data:smokeDir/"test-data";
        app.renderer=std::make_unique<Renderer>(assets);glfwSetWindowUserPointer(window,&app);
        // The profile's GAME_SETTINGS (options and tutorial progress).
        try{app.settings.load(app.settingsPath());}catch(const std::exception& e){app.message(e.what());}
        app.openMenu(MenuScene::MainMenu);
        glfwSetKeyCallback(window,[](GLFWwindow* w,int key,int,int action,int){static_cast<App*>(glfwGetWindowUserPointer(w))->key(key,action);});
        glfwSetCharCallback(window,[](GLFWwindow* w,unsigned c){auto a=static_cast<App*>(glfwGetWindowUserPointer(w));
            if(a->editCharGuard){a->editCharGuard=false;return;}
            if(a->screen!=Screen::Menu || a->editingField<0 || c<32 || c==127 || c>=0x800)return;
            // The seed is read as ASCII; world names take Latin letters too.
            auto& value=a->editingField==0?a->menus.newWorld().name:a->menus.newWorld().seed;
            std::size_t length=0;for(unsigned char ch:value)if((ch&0xC0)!=0x80)++length;
            if(a->editingField==1 && (c>=127 || length>=60))return;
            if(a->editingField==0 && length>=25)return;
            if(c<0x80)value+=char(c);else{value+=char(0xC0|(c>>6));value+=char(0x80|(c&0x3F));}
            a->menus.refresh();});
        glfwSetCursorPosCallback(window,[](GLFWwindow* w,double x,double y){static_cast<App*>(glfwGetWindowUserPointer(w))->look(x,y);});
        glfwSetScrollCallback(window,[](GLFWwindow* w,double,double y){auto a=static_cast<App*>(glfwGetWindowUserPointer(w));
            if(a->screen==Screen::Playing && y!=0){
                a->scrollSteps+=y>0?-1:1;a->padInputLast=false;
                if(a->inputAllowed(y>0?TutorialSession::LeftScroll:TutorialSession::RightScroll))a->slot=(a->slot+(y>0?8:1))%9;
            }
            else if(a->screen==Screen::Inventory && y!=0)a->turnCreativePage(y>0?-1:1);
            else if(a->screen==Screen::Menu && y!=0)a->menuKey(y>0?GLFW_KEY_UP:GLFW_KEY_DOWN,GLFW_PRESS);
            else if(a->screen==Screen::Crafting && y!=0){const int count=int(a->craftingList().size());
                if(count)a->selection=std::clamp(a->selection+(y>0?-1:1),0,count-1);}
        });
        glfwSetMouseButtonCallback(window,[](GLFWwindow* w,int button,int action,int){auto a=static_cast<App*>(glfwGetWindowUserPointer(w));
            if(button==GLFW_MOUSE_BUTTON_LEFT)a->leftMouse=action==GLFW_PRESS;
            if(button==GLFW_MOUSE_BUTTON_RIGHT)a->rightMouse=action==GLFW_PRESS;
            if((a->screen==Screen::Chest || a->screen==Screen::Furnace || a->screen==Screen::Brewing || a->screen==Screen::Menu) && button==GLFW_MOUSE_BUTTON_RIGHT && action==GLFW_PRESS)a->click(0);
            if(a->screen!=Screen::Playing && button==GLFW_MOUSE_BUTTON_LEFT && action==GLFW_PRESS)a->click();});
        glfwSetWindowFocusCallback(window,[](GLFWwindow* w,int focused){auto a=static_cast<App*>(glfwGetWindowUserPointer(w));if(!focused && a->screen==Screen::Playing){a->leftMouse=a->rightMouse=false;a->openMenu(MenuScene::Pause);}});
        double previous=glfwGetTime();int frame=0;
        while(true){
            glfwPollEvents();app.editCharGuard=false;double now=glfwGetTime(),dt=std::clamp(now-previous,0.0,.05);previous=now;
            if(glfwWindowShouldClose(window)){
                try{app.save();break;}catch(const std::exception& e){app.message(e.what());glfwSetWindowShouldClose(window,0);if(app.loaded)app.openMenu(MenuScene::Pause);}
            }
            try{app.update(dt);app.draw();}
            catch(const std::exception& e){
                if(!smokeDir.empty())throw;
                std::cerr<<"Console port frame error: "<<e.what()<<'\n';
                if(app.screen==Screen::Playing)app.openMenu(MenuScene::Pause);
                app.message(e.what());
            }
            if(app.screenshotRequested){app.screenshotRequested=false;
                try{app.renderer->screenshot(app.dataDir/"screenshots"/("capture-"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count())+".png"));app.message("Screenshot saved");}
                catch(const std::exception& e){app.message(e.what());}}
            if(!smokeDir.empty()){
                if(frame==2)app.renderer->screenshot(smokeDir/"menu.png");
                if(frame==3){
                    // Every menu scene draws: Help & Options, How To Play, Controls,
                    // each Settings page, Credits and the message boxes.
                    auto press=[&](int index){app.menus.setFocus(index);app.menuInput(MenuInput::Accept);app.draw();};
                    auto back=[&]{app.menuInput(MenuInput::Back);app.draw();};
                    press(2);press(1);press(7);press(1);back();back();press(2);back();press(3);
                    for(int i=0;i<5;++i){press(i);back();}
                    press(5);back();back();press(4);back();press(0);back();back();press(3);app.menuInput(MenuInput::Accept);
                    if(app.menus.scene()!=MenuScene::MainMenu || glGetError()!=GL_NO_ERROR)throw std::runtime_error("Smoke menus failed");
                }
                if(frame==4){
                    // Play Game -> Create New World (creative, seed 1).
                    app.menus.setFocus(0);app.menuInput(MenuInput::Accept);app.menus.setFocus(0);app.menuInput(MenuInput::Accept);
                    if(app.menus.scene()!=MenuScene::CreateWorld)throw std::runtime_error("Smoke create world menu missing");
                    app.menus.setFocus(1);app.menuInput(MenuInput::Accept);
                    if(app.editingField!=1)throw std::runtime_error("Smoke seed field not editable");
                    app.menus.newWorld().seed="1";app.menuKey(GLFW_KEY_ENTER,GLFW_PRESS);
                    app.menus.setFocus(2);app.menuInput(MenuInput::Accept);app.draw();
                    app.menus.setFocus(5);app.menuInput(MenuInput::Accept);app.draw();
                    app.menuInput(MenuInput::Accept); // IDS_CONFIRM_START_CREATIVE: OK
                    if(!app.loaded)throw std::runtime_error("Smoke world failed to start");
                    app.flying=true;app.position.y+=9;app.pitch=-.35;
                }
                if(frame==9){app.change(Screen::Playing);app.renderer->screenshot(smokeDir/"world.png");
                    auto h=app.world.raycast({64.5,95,64.5},{0,-1,0},96);if(!h.hit)throw std::runtime_error("Smoke raycast missed world");
                    auto camera=app.position;double cameraPitch=app.pitch;
                    app.position={h.x+.5,h.y+3.,h.z+.5};app.pitch=-1.55;
                    auto target=app.world.raycast(app.eye(),app.direction());if(!target.hit)throw std::runtime_error("Smoke mine target missing");
                    app.lastEdit=-1;app.edit(false);if(app.world.get(target.x,target.y,target.z)!=Air)throw std::runtime_error("Smoke mining failed");
                    app.slot=8;if(!app.world.setCreativeHotbarItem(8,Bricks))throw std::runtime_error("Smoke quickbar setup failed");
                    app.lastEdit=-1;app.edit(true);if(app.world.get(target.x,target.y,target.z)!=Bricks)throw std::runtime_error("Smoke placement failed");
                    app.position=camera;app.pitch=cameraPitch;
                    app.save();World loaded;if(!loaded.load(app.savePath()) || loaded.blockSnapshot()!=app.world.blockSnapshot())throw std::runtime_error("Smoke save roundtrip failed");
                    app.change(Screen::Inventory);}
                if(frame==12){app.renderer->screenshot(smokeDir/"inventory.png");
                    for(int tab=0;tab<creativeTabCount();++tab){
                        app.inventoryCategory=tab;
                        const int pages=int((creativeTab(tab).items.size()+49)/50);
                        for(int page=0;page<pages;++page){
                            app.creativePage[tab]=page;app.selection=0;app.draw();
                            if(app.inventoryCount()<1 || app.inventoryCount()>50 || glGetError()!=GL_NO_ERROR)
                                throw std::runtime_error("Smoke creative page failed");
                        }
                        app.creativePage[tab]=0;
                    }
                    app.inventoryCategory=0;app.selection=0;app.activate();
                    if(app.selectedItem().id!=Stone || app.world.carriedItems()[8].id!=Bricks)
                        throw std::runtime_error("Smoke creative quickbar failed");
                    // Slot 9 holds the map PlayerList gives every new player.
                    if(app.world.carriedItems()[9].id!=358)throw std::runtime_error("Smoke new player map missing");
                    app.world.swapCarriedSlots(0,18);
                    app.change(Screen::Inventory);app.inventoryCategory=creativeTabCount();app.selection=18;app.activate();
                    if(app.selectedItem().id!=Stone || app.world.carriedItems()[18].id)
                        throw std::runtime_error("Smoke backpack equip failed");
                    app.save();World equipped;
                    if(!equipped.load(app.savePath()) || equipped.carriedItems()[0].id!=Stone ||
                       equipped.carriedItems()[8].id!=Bricks)
                        throw std::runtime_error("Smoke quickbar save failed");
                    app.change(Screen::Furnace);}
                if(frame==13)app.change(Screen::Brewing);
                if(frame==14)app.change(Screen::Chest);
                if(frame==15){
                    // Survival rendering: HUD bars, a dropped item and the crack overlay.
                    app.world.setSurvival(true);app.change(Screen::Playing);
                    if(!app.world.setCreativeHotbarItem(0,270))throw std::runtime_error("Smoke survival tool failed");
                    app.slot=0;app.world.dropCarried(0,false,app.eye(),app.yaw,app.pitch);
                    if(app.world.droppedItems().empty())throw std::runtime_error("Smoke survival drop failed");
                    app.renderer->setDestroyStage(5);
                }
                if(frame==16){app.renderer->setDestroyStage(-1);app.craftingTable=true;app.change(Screen::Crafting);
                    if(app.craftingList().empty())throw std::runtime_error("Smoke crafting list empty");}
                if(frame==17){
                    app.openMenu(MenuScene::Death);
                    app.menus.setFocus(1);app.menuInput(MenuInput::Accept);app.draw();
                    if(!app.menus.message())throw std::runtime_error("Smoke death exit prompt missing");
                    app.menuInput(MenuInput::Back);app.openMenu(MenuScene::Pause);app.draw();
                    if(!app.pauseMenuShown())throw std::runtime_error("Smoke pause menu missing");
                    app.menuInput(MenuInput::Back);
                    if(app.screen!=Screen::Playing)throw std::runtime_error("Smoke pause did not resume");
                    app.openMenu(MenuScene::Death);
                }
                if(frame==18){
                    // Tutorial World: FullTutorialMode with the pack's LevelRules.
                    // Death menu -> Exit Game -> Exit without saving -> Play Game -> Play Tutorial.
                    app.menus.setFocus(1);app.menuInput(MenuInput::Accept);app.menus.setFocus(2);app.menuInput(MenuInput::Accept);
                    if(app.loaded || app.menus.scene()!=MenuScene::MainMenu)throw std::runtime_error("Smoke exit to title failed");
                    app.menus.setFocus(0);app.menuInput(MenuInput::Accept);app.menus.setFocus(1);app.menuInput(MenuInput::Accept);
                    if(!app.loaded || !app.tutorial || !app.world.survival())throw std::runtime_error("Smoke tutorial failed to start");
                    const auto carried=app.world.carriedItems();
                    if(app.world.playerHealth()!=12 || app.world.playerFoodLevel()!=12 || carried[10].id!=364 || carried[9].id!=358)
                        throw std::runtime_error("Smoke tutorial UpdatePlayer rule not applied");
                }
                if(frame==19){
                    // Past the 1.5 s start delay the overview lesson is shown.
                    for(int i=0;i<3;++i){glfwSetTime(glfwGetTime()+1);app.tutorial->tick();}
                    if(!app.tutorialPopupShown() || app.tutorialPopup.description.rfind(widen(consoleText("IDS_TUTORIAL_TASK_OVERVIEW")),0)!=0)
                        throw std::runtime_error("Smoke tutorial overview not shown");
                    if(app.world.dayTime()!=8000)throw std::runtime_error("Smoke tutorial time of day not frozen");
                }
                if(frame==20)app.renderer->screenshot(smokeDir/"tutorial.png");
                if(frame==21){glFinish();auto error=glGetError();if(error!=GL_NO_ERROR)throw std::runtime_error("OpenGL error: "+std::to_string(error));
                    std::cout<<"Smoke test passed: menu, world, inventory, furnace, brewing, chest, survival HUD/crafting/death, edit/save/reload, tutorial; "<<app.renderer->triangleCount()<<" world triangles\n";break;}
            }
            glfwSwapBuffers(window);++frame;
        }
    }catch(const std::exception& e){std::cerr<<"Console port: "<<e.what()<<'\n';result=1;}
    glfwDestroyWindow(window);glfwTerminate();return result;
}
