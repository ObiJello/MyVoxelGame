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
enum class Screen { Main, Worlds, CreateWorld, FindingSeed, Loading, Playing, Pause, Inventory, Chest, Furnace, Brewing, Crafting, Dead, Options, Controls, Notice };
struct App {
    enum class LoadKind { Create, Tutorial, ArchivedTutorial, Existing };
    struct LoadResult {
        std::unique_ptr<World> next;
        std::shared_ptr<World> previous;
        std::filesystem::path path,previousPath;
        bool previousLoaded=false;
        std::string error;
    };
    GLFWwindow* window=nullptr;
    std::unique_ptr<Renderer> renderer;
    World world;
    Screen screen=Screen::Main,returnScreen=Screen::Main;
    Vec3 position{};
    double yaw=.65,pitch=-.12,verticalSpeed=0,sensitivity=.0022,viewDistance=120;
    double worldTickSeconds=0;
    int potionUseTicks=0,potionUseSlot=-1;
    bool horizontalCollision=false,grounded=false,flying=false,invert=false,loaded=false,seedEditing=false,enderChestOpen=false,furnaceFuelTarget=false;
    int selection=0,slot=0,chestX=0,chestY=0,chestZ=0,chestSlots=27;
    bool nameEditing=false;
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
    std::size_t worldPage=0;
    double lastX=0,lastY=0,toastUntil=0,lastSave=0,lastEdit=0,ignoreMouseUntil=0;
    bool scripted=false,screenshotRequested=false;
    bool mouseReady=false;
    std::string seedText,toast,notice;
    std::unique_ptr<ConsoleSeedSearch> seedSearch;
    std::future<LoadResult> loadJob;
    std::optional<LoadResult> loadReady;
    std::shared_ptr<std::atomic<int>> loadPhase;
    Screen loadReturnScreen=Screen::Worlds;
    std::uint64_t seedAttempts=0;
    std::filesystem::path dataDir;
    GLFWgamepadstate previousPad{};
    bool leftMouse=false,rightMouse=false,usedBlockHeld=false;
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
    void change(Screen s){screen=s;selection=0;seedEditing=false;nameEditing=false;
        if(s!=Screen::FindingSeed)seedSearch.reset();
        if(s==Screen::Worlds){try{savedWorlds=listSavedWorlds(dataDir);}catch(const std::exception& e){message(e.what());savedWorlds.clear();}worldPage=0;}mouseReady=false;leftMouse=rightMouse=usedBlockHeld=false;potionUseTicks=0;potionUseSlot=-1;ignoreMouseUntil=glfwGetTime()+.15;
        glfwSetInputMode(window,GLFW_CURSOR,s==Screen::Playing?GLFW_CURSOR_DISABLED:GLFW_CURSOR_NORMAL);}
    void save(){if(loaded){world.save(savePath());lastSave=glfwGetTime();}}
    void beginLoad(LoadKind kind,const std::filesystem::path& selected={},std::int64_t value=0){
        if(loadJob.valid() || loadReady)return;
        const bool previousLoaded=loaded;
        const auto previousPath=savePath(),destinationDir=dataDir;
        const auto name=worldName;
        const bool flat=flatWorld,survivalMode=survivalWorld;
        std::shared_ptr<World> previous;
        if(previousLoaded){previous=std::make_shared<World>();previous->swapWith(world);}
        loaded=false;loadReturnScreen=screen==Screen::FindingSeed?Screen::CreateWorld:screen;
        change(Screen::Loading);
        loadPhase=std::make_shared<std::atomic<int>>(previousLoaded?0:1);
        auto phase=loadPhase;
        try{loadJob=std::async(std::launch::async,[kind,selected,value,destinationDir,name,flat,survivalMode,previousPath,previousLoaded,
                                                   previous,phase]() mutable -> LoadResult {
            LoadResult result;result.previous=previous;result.previousLoaded=previousLoaded;result.previousPath=previousPath;
            try{
                if(result.previous){phase->store(0);result.previous->save(previousPath);}
                phase->store(1);
                result.next=std::make_unique<World>();
                if(kind==LoadKind::Existing){
                    if(selected.empty() || !result.next->load(selected))throw std::runtime_error("Cannot load the selected world");
                    result.path=selected.extension()==".mcp"?destinationDir/"world.inner":selected;
                }else{
                    result.path=allocateWorldSave(destinationDir);
                    if(kind==LoadKind::Tutorial)result.next->generateTutorial(std::filesystem::path(CONSOLE_ASSET_DIR)/"tutorial");
                    else if(kind==LoadKind::ArchivedTutorial)result.next->generateArchivedTutorial(std::filesystem::path(CONSOLE_ASSET_DIR)/"tutorial");
                    else {result.next->generate(value,flat);result.next->setName(name);result.next->setSurvival(survivalMode);}
                }
                phase->store(2);
                phase->store(3);
                if(kind!=LoadKind::Existing || selected.extension()==".mcp")result.next->save(result.path);
            }catch(const std::exception& error){result.error=error.what();result.next.reset();}
            phase->store(4);
            return result;
        });}catch(...){
            if(previous)world.swapWith(*previous);
            loaded=previousLoaded;change(loadReturnScreen);loadPhase.reset();throw;
        }
    }
    void finishLoad(bool wait=false){
        if(!loadReady){
            if(!loadJob.valid() || (!wait && loadJob.wait_for(std::chrono::seconds(0))!=std::future_status::ready))return;
            try{loadReady.emplace(loadJob.get());}
            catch(const std::exception& error){loaded=false;change(loadReturnScreen);message(error.what());loadPhase.reset();return;}
            if(!loadReady->error.empty()){
                if(loadReady->previous){world.swapWith(*loadReady->previous);activeSave=loadReady->previousPath;}
                loaded=loadReady->previousLoaded;change(loaded?Screen::Pause:loadReturnScreen);
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
            position=saved?*saved:world.spawn();verticalSpeed=0;grounded=false;flying=false;worldTickSeconds=0;
            fallDistance=0;mineProgress=0;mineY=-1;eatUseTicks=0;
            world.setPlayerPosition(position);
            if(world.isTutorial()){yaw=(81.59-180)*3.14159265358979323846/180;pitch=0;}
            lastSave=glfwGetTime();change(Screen::Playing);
            message(world.isTutorial()?"Tutorial world - creative exploration":
                    world.survival()?"Survival mode - E inventory, C crafting, Q drop":"Creative mode - press E for blocks, F to fly");
            loadReady.reset();loadPhase.reset();
        }catch(const std::exception& error){
            if(loadReady && loadReady->previous){world.swapWith(*loadReady->previous);activeSave=loadReady->previousPath;}
            loaded=loadReady && loadReady->previousLoaded;
            change(loaded?Screen::Pause:loadReturnScreen);message(error.what());
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
        }catch(const std::exception& e){if(screen==Screen::FindingSeed)change(Screen::CreateWorld);message(e.what());}
    }
    void startTutorial(bool archived=false){
        try{beginLoad(archived?LoadKind::ArchivedTutorial:LoadKind::Tutorial);if(scripted)finishLoad(true);
        }catch(const std::exception& error){message(error.what());}
    }
    std::vector<std::string> buttons()const {
        switch(screen){
        case Screen::Main:return {"Play Game","Leaderboards","Help & Options","Downloadable Content","Exit Game"};
        case Screen::Worlds:{
            std::vector<std::string> entries{"Create New World","Tutorial World","Classic Tutorial World"};
            for(std::size_t i=worldPage*4;i<std::min(savedWorlds.size(),worldPage*4+4);++i)
                entries.push_back(savedWorlds[i].name.substr(0,25));
            entries.push_back("Previous Page");entries.push_back("Next Page");entries.push_back("Back");return entries;
        }
        case Screen::CreateWorld:return {"World Name: "+worldName+(nameEditing?"_":""),"Seed: "+(seedText.empty() && !seedEditing?std::string("Random"):seedText)+(seedEditing?"_":""),"World Type: "+std::string(flatWorld?"Superflat":"Default"),"Game Mode: "+std::string(survivalWorld?"Survival":"Creative"),"Create New World","Back"};
        case Screen::Dead:return {"Respawn","Exit to Title"};
        case Screen::FindingSeed:return {"Cancel"};
        case Screen::Loading:return {};
        case Screen::Pause:return {"Resume Game","Help & Options","Save Game","Save and Exit"};
        case Screen::Options:return {"Controls","Sensitivity: "+std::to_string(int(sensitivity*10000)),"Invert Look: "+std::string(invert?"On":"Off"),"View Distance: "+std::to_string(int(viewDistance)),"Back"};
        case Screen::Controls:case Screen::Notice:return {"Back"};
        default:return {};
        }
    }
    void back(){
        if(screen==Screen::Loading || screen==Screen::Dead)return;
        if(screen==Screen::Playing)change(Screen::Pause);
        else if(screen==Screen::Crafting)change(Screen::Playing);
        else if(screen==Screen::Pause || screen==Screen::Inventory || screen==Screen::Chest || screen==Screen::Furnace || screen==Screen::Brewing)change(Screen::Playing);
        else if(screen==Screen::Options)change(returnScreen);
        else if(screen==Screen::Controls)change(Screen::Options);
        else if(screen==Screen::CreateWorld)change(Screen::Worlds);
        else if(screen==Screen::FindingSeed)change(Screen::CreateWorld);
        else change(Screen::Main);
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
            if(selection<3)moved=world.transferFurnaceItem(chestX,chestY,chestZ,selection,true,0,amount);
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
            if(selection<4)moved=world.transferBrewingItem(chestX,chestY,chestZ,selection,true,0,amount);
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
        case Screen::Main:
            if(selection==0)change(Screen::Worlds);
            else if(selection==2){returnScreen=Screen::Main;change(Screen::Options);}
            else if(selection==4)glfwSetWindowShouldClose(window,1);
            else {notice=selection==1?"PlayStation Network leaderboards are not ported.":"The console content store is not ported.";change(Screen::Notice);}
            break;
        case Screen::Worlds:
            {
            auto count=std::min<std::size_t>(4,savedWorlds.size()-worldPage*4);
            if(selection==0)change(Screen::CreateWorld);
            else if(selection==1)startTutorial();
            else if(selection==2)startTutorial(true);
            else if(selection<=int(count)+2){
                const auto& entry=savedWorlds[worldPage*4+selection-3];
                if(entry.readable)start(false,entry.path);else message("This saved world could not be read.");
            }else if(selection==int(count)+3){if(worldPage>0){--worldPage;selection=0;}}
            else if(selection==int(count)+4){if((worldPage+1)*4<savedWorlds.size()){++worldPage;selection=0;}}
            else change(Screen::Main);break;
        }
        case Screen::CreateWorld:
            if(selection==0){nameEditing=true;seedEditing=false;}else if(selection==1){seedEditing=true;nameEditing=false;}
            else if(selection==2)flatWorld=!flatWorld;else if(selection==3)survivalWorld=!survivalWorld;
            else if(selection==4)start(true);else change(Screen::Worlds);break;
        case Screen::Dead:
            if(selection==0){world.respawnPlayer();position=world.spawn();verticalSpeed=0;fallDistance=0;world.setPlayerPosition(position);change(Screen::Playing);}
            else try{save();loaded=false;change(Screen::Main);}catch(const std::exception& e){message(e.what());}
            break;
        case Screen::Crafting:{
            const auto list=craftingList();
            if(selection>=0 && selection<int(list.size())){
                if(world.craft(*list[selection]))message("Crafted "+itemDisplayName(list[selection]->id,list[selection]->damage));
                else message("Missing ingredients");
            }
            break;
        }
        case Screen::FindingSeed:change(Screen::CreateWorld);break;
        case Screen::Pause:
            if(selection==0)change(Screen::Playing);
            else if(selection==1){returnScreen=Screen::Pause;change(Screen::Options);}
            else try{save();if(selection==3){loaded=false;change(Screen::Main);}message("World saved");}catch(const std::exception& e){message(e.what());}
            break;
        case Screen::Options:
            if(selection==0)change(Screen::Controls);
            else if(selection==1)sensitivity=sensitivity>=.004? .001:sensitivity+.0005;
            else if(selection==2)invert=!invert;
            else if(selection==3)viewDistance=viewDistance>=160?80:viewDistance+40;
            else change(returnScreen);
            break;
        case Screen::Controls:change(Screen::Options);break;
        case Screen::Notice:change(Screen::Main);break;
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
    void key(int key,int action){
        if(action!=GLFW_PRESS && action!=GLFW_REPEAT)return;
        if(key==GLFW_KEY_F2){screenshotRequested=true;return;}
        if(action==GLFW_REPEAT && (key==GLFW_KEY_E || key==GLFW_KEY_F || key==GLFW_KEY_ESCAPE ||
                                   key==GLFW_KEY_TAB))return;
        if(key==GLFW_KEY_ESCAPE){if(seedEditing || nameEditing){seedEditing=nameEditing=false;return;}back();return;}
        if(screen==Screen::Playing){
            if(key>=GLFW_KEY_1 && key<=GLFW_KEY_9)slot=key-GLFW_KEY_1;
            if(key==GLFW_KEY_E){change(Screen::Inventory);if(world.survival())inventoryCategory=creativeTabCount();}
            if(key==GLFW_KEY_C && world.survival()){craftingTable=false;craftScroll=0;change(Screen::Crafting);}
            // Abilities::mayfly is creative-only.
            if(key==GLFW_KEY_F && !world.survival()){flying=!flying;verticalSpeed=0;message(flying?"Flying enabled":"Flying disabled");}
            if(key==GLFW_KEY_Q){
                const bool stack=glfwGetKey(window,GLFW_KEY_LEFT_CONTROL)==GLFW_PRESS;
                try{world.dropCarried(slot,stack,eye(),yaw,pitch);}catch(const std::exception& e){message(e.what());}
            }
            if(key==GLFW_KEY_SPACE && grounded && !world.playerInWater()){
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
        if(seedEditing || nameEditing){auto& value=seedEditing?seedText:worldName;if(key==GLFW_KEY_BACKSPACE && !value.empty())value.pop_back();if(key==GLFW_KEY_ENTER)seedEditing=nameEditing=false;return;}
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
        const int count=static_cast<int>(buttons().size());
        if(count==0)return;
        if(key==GLFW_KEY_UP || key==GLFW_KEY_LEFT)selection=(selection+count-1)%count;
        if(key==GLFW_KEY_DOWN || key==GLFW_KEY_RIGHT)selection=(selection+1)%count;
        if(key==GLFW_KEY_ENTER || key==GLFW_KEY_SPACE)activate();
    }
    void look(double x,double y){
        if(screen!=Screen::Playing || scripted || glfwGetTime()<ignoreMouseUntil){lastX=x;lastY=y;mouseReady=false;return;}
        if(mouseReady){yaw+=(x-lastX)*sensitivity;pitch+=(y-lastY)*sensitivity*(invert?1:-1);pitch=std::clamp(pitch,-1.55,1.55);}
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
        if(!place && world.attackEntity(eye(),direction(),heldItem.id,entityReach())){world.playerAttacked(slot);return;}
        auto h=world.raycast(eye(),direction(),blockReach());if(!h.hit)return;
        bool changed=false;
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
        if(place && (world.get(h.x,h.y,h.z)==64 || world.get(h.x,h.y,h.z)==71 || world.get(h.x,h.y,h.z)==107)){
            if(!usedBlockHeld){world.useBlock(h.x,h.y,h.z);if(!world.streaming() && !renderer->rebuilding())renderer->beginRebuild(world);usedBlockHeld=true;}
            return;
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
            if(tileId>0 && tileId<256)
                changed=world.placeBlock(h.px,h.py,h.pz,static_cast<Block>(tileId),
                                         heldItem.id<256?heldItem.damage:0,position,yaw);
            // TileItem/TilePlanterItem::useOn uses one item in survival.
            if(changed)world.consumeCarried(slot);
        }else if(world.get(h.x,h.y,h.z)!=Bedrock)changed=world.destroyBlock(h.x,h.y,h.z,slot);
        if(changed){
            world.updateLiquidNeighbors(place?h.px:h.x,place?h.py:h.y,place?h.pz:h.z);
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
            lastEdit=glfwGetTime();world.playerAttacked(slot);mineProgress=0;mineY=-1;renderer->setDestroyStage(-1);return;
        }
        const auto h=world.raycast(eye(),direction(),blockReach());
        if(!h.hit){mineProgress=0;mineY=-1;renderer->setDestroyStage(-1);return;}
        const auto destroy=[&]{
            if(world.destroyBlock(h.x,h.y,h.z,slot)){
                world.updateLiquidNeighbors(h.x,h.y,h.z);
                if(!world.streaming() && !renderer->rebuilding())renderer->beginRebuild(world);
            }
        };
        for(int tick=0;tick<ticks;++tick){
            if(mineDelay>0){--mineDelay;continue;}
            if(mineY<0 || h.x!=mineX || h.y!=mineY || h.z!=mineZ){
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
    void update(double dt){
        GLFWgamepadstate pad{};bool hasPad=false;
        for(int i=GLFW_JOYSTICK_1;i<=GLFW_JOYSTICK_LAST;++i)if(glfwGetGamepadState(i,&pad)){hasPad=true;break;}
        if(hasPad){
            auto pressed=[&](int b){return pad.buttons[b] && !previousPad.buttons[b];};
            if(pressed(GLFW_GAMEPAD_BUTTON_START) || pressed(GLFW_GAMEPAD_BUTTON_B))back();
            if(pressed(GLFW_GAMEPAD_BUTTON_DPAD_UP))key(GLFW_KEY_UP,GLFW_PRESS);
            if(pressed(GLFW_GAMEPAD_BUTTON_DPAD_DOWN))key(GLFW_KEY_DOWN,GLFW_PRESS);
            if(pressed(GLFW_GAMEPAD_BUTTON_DPAD_LEFT))key(GLFW_KEY_LEFT,GLFW_PRESS);
            if(pressed(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT))key(GLFW_KEY_RIGHT,GLFW_PRESS);
            if(screen==Screen::Chest && pressed(GLFW_GAMEPAD_BUTTON_X))moveChestSelection(0);
            if(screen==Screen::Chest && pressed(GLFW_GAMEPAD_BUTTON_Y))moveChestSelection(1);
            if(screen==Screen::Furnace && pressed(GLFW_GAMEPAD_BUTTON_X))moveFurnaceSelection(0);
            if(screen==Screen::Furnace && pressed(GLFW_GAMEPAD_BUTTON_Y))moveFurnaceSelection(1);
            if(screen==Screen::Brewing && pressed(GLFW_GAMEPAD_BUTTON_X))moveBrewingSelection(0);
            if(screen==Screen::Brewing && pressed(GLFW_GAMEPAD_BUTTON_Y))moveBrewingSelection(1);
            if(pressed(GLFW_GAMEPAD_BUTTON_A))key(screen==Screen::Playing?GLFW_KEY_SPACE:GLFW_KEY_ENTER,GLFW_PRESS);
            if(pressed(GLFW_GAMEPAD_BUTTON_X) && loaded && world.survival() && (screen==Screen::Playing || screen==Screen::Crafting))
                key(GLFW_KEY_C,GLFW_PRESS);
            if(pressed(GLFW_GAMEPAD_BUTTON_Y) && (screen==Screen::Playing || screen==Screen::Inventory))key(GLFW_KEY_E,GLFW_PRESS);
            if(pressed(GLFW_GAMEPAD_BUTTON_LEFT_THUMB) && screen==Screen::Playing)key(GLFW_KEY_F,GLFW_PRESS);
            if(pressed(GLFW_GAMEPAD_BUTTON_LEFT_THUMB) && screen==Screen::Furnace)key(GLFW_KEY_F,GLFW_PRESS);
            if(pressed(GLFW_GAMEPAD_BUTTON_LEFT_THUMB) && screen==Screen::Brewing)key(GLFW_KEY_F,GLFW_PRESS);
            if((pressed(GLFW_GAMEPAD_BUTTON_LEFT_BUMPER) || pressed(GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER)) && (screen==Screen::Inventory || screen==Screen::Crafting))key(GLFW_KEY_TAB,GLFW_PRESS);
            else{
                if(pressed(GLFW_GAMEPAD_BUTTON_LEFT_BUMPER))slot=(slot+8)%9;
                if(pressed(GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER))slot=(slot+1)%9;
            }
        }
        previousPad=pad;
        if(screen==Screen::Loading){finishLoad();return;}
        if(screen==Screen::FindingSeed){
            try{auto seed=seedSearch->step();++seedAttempts;if(seed)start(true,{},seed);}
            catch(const std::exception& error){change(Screen::CreateWorld);message(error.what());}
            return;
        }
        if(screen!=Screen::Playing && screen!=Screen::Furnace && screen!=Screen::Brewing && screen!=Screen::Chest &&
           screen!=Screen::Inventory && screen!=Screen::Crafting && screen!=Screen::Dead)return;
        try{if(world.streamAround(position))renderer->beginRebuild(world);
            if(!world.streaming())renderer->stepRebuild(world);}
        catch(const std::exception& error){change(Screen::Pause);message(error.what());return;}
        renderer->tickLighting(dt);
        world.setPlayerPosition(position);
        worldTickSeconds+=dt;
        const auto beforeWorldTick=world.revision;
        int elapsedWorldTicks=0;
        while(worldTickSeconds>=.05){world.tickTime();worldTickSeconds-=.05;++elapsedWorldTicks;}
        if(world.revision!=beforeWorldTick && !world.streaming() && !renderer->rebuilding())renderer->beginRebuild(world);
        if(world.playerDead() && screen!=Screen::Dead){change(Screen::Dead);return;}
        if(screen!=Screen::Playing){potionUseTicks=0;eatUseTicks=0;mineProgress=0;mineY=-1;renderer->setDestroyStage(-1);return;}
        const bool survival=world.survival();
        if(survival)flying=false;
        auto held=[&](int k){return glfwGetKey(window,k)==GLFW_PRESS;};
        double forward=held(GLFW_KEY_W)-held(GLFW_KEY_S),right=held(GLFW_KEY_D)-held(GLFW_KEY_A);
        auto dead=[](float x){return std::abs(x)<.18f?0.f:(x-std::copysign(.18f,x))/.82f;};
        if(hasPad){forward-=dead(pad.axes[GLFW_GAMEPAD_AXIS_LEFT_Y]);right+=dead(pad.axes[GLFW_GAMEPAD_AXIS_LEFT_X]);
            yaw+=dead(pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_X])*dt*sensitivity*1100;
            pitch+=dead(pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y])*dt*sensitivity*900*(invert?1:-1);pitch=std::clamp(pitch,-1.55,1.55);}
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
        moveAxis(0,velocity.x*dt);moveAxis(2,velocity.z*dt);
        if(climbing)verticalSpeed=horizontalCollision?4:velocity.y;
        const bool up=held(GLFW_KEY_SPACE)||(hasPad && pad.buttons[GLFW_GAMEPAD_BUTTON_A]);
        if(flying){verticalSpeed=0;moveAxis(1,(up-(held(GLFW_KEY_LEFT_SHIFT)||(hasPad && pad.buttons[GLFW_GAMEPAD_BUTTON_X])))*speed*dt);}
        else if(inLiquid && !climbing){
            // Mob::travel in liquids: slow sinking, jump swims upward.
            grounded=false;
            verticalSpeed=up?std::min(verticalSpeed+20*dt,2.6):std::max(verticalSpeed-8*dt,-2.4);
            moveAxis(1,verticalSpeed*dt);
        }
        else {grounded=false;if(!climbing)verticalSpeed=std::max(verticalSpeed-24*dt,-35.0);moveAxis(1,verticalSpeed*dt);}
        // Entity::move fall distance; water, ladders and flight reset it.
        if(position.y<before.y)fallDistance+=before.y-position.y;
        if(flying || climbing || inLiquid)fallDistance=0;
        if(grounded){if(fallDistance>0)world.playerLanded(fallDistance);fallDistance=0;}
        world.playerWalked(std::hypot(position.x-before.x,position.z-before.z),sprinting,inWater);
        const bool mining=leftMouse || (hasPad && pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER]>.1);
        if(!survival){if(mining)edit(false);renderer->setDestroyStage(-1);}
        else if(mining)mine(elapsedWorldTicks);
        else{mineProgress=0;mineY=-1;renderer->setDestroyStage(-1);}
        const bool usingRight=rightMouse || (hasPad && pad.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER]>.1);
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
                if(world.drinkPotion(selected.damage))message("Potion used");
            }
        }else if(usingRight && edible && !blockUse){
            // FoodItem::use -> startUsingItem for EAT_DURATION ticks.
            if(potionUseSlot!=slot){potionUseSlot=slot;eatUseTicks=0;}
            eatUseTicks+=elapsedWorldTicks;
            if(eatUseTicks>=consoleEatDuration){
                eatUseTicks=0;
                const auto name=itemDisplayName(selected.id,selected.damage);
                if(world.eatCarried(slot))message("Ate "+name);
            }
        }else{
            potionUseTicks=0;potionUseSlot=-1;eatUseTicks=0;
            if(usingRight)edit(true);else usedBlockHeld=false;
        }
        if(!world.streaming() && glfwGetTime()-lastSave>30)try{save();}catch(const std::exception& e){message(e.what());lastSave=glfwGetTime();}
    }
    float buttonY()const{return screen==Screen::Dead?160:screen==Screen::Main?125:screen==Screen::Worlds?100:screen==Screen::Controls || screen==Screen::Notice?286:screen==Screen::Pause?135:139;}
    float buttonWidth()const{return screen==Screen::Main?225.f:200.f;}
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
        auto entries=buttons();float left=renderer->uiWidth()/2-buttonWidth()/2;
        for(int i=0;i<int(entries.size());++i)if(x>=left && x<left+buttonWidth() && y>=buttonY()+i*25 && y<buttonY()+i*25+20){selection=i;activate();break;}
    }
    void draw(){
        int w,h;glfwGetFramebufferSize(window,&w,&h);renderer->resize(w,h);auto& r=*renderer;float cw=r.uiWidth();
        bool inWorld=loaded && screen!=Screen::Loading && screen!=Screen::Main && screen!=Screen::Worlds && screen!=Screen::CreateWorld && screen!=Screen::FindingSeed && screen!=Screen::Notice;
        if(inWorld)r.world(world,eye(),yaw,pitch,viewDistance,screen==Screen::Playing?world.raycast(eye(),direction(),blockReach()):Hit{});
        else {glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);}
        r.beginUI();
        if(!inWorld){
            const float shift=std::fmod(float(glfwGetTime())*.004f,1.f);
            r.sprite("panorama_n",0,0,cw,360,{shift,0,shift+.62f,1});
            r.rect(0,0,cw,360,{0,0,0,.12});
        }
        if(screen==Screen::Playing){
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
            r.text("Position: "+std::to_string(int(position.x))+", "+std::to_string(int(position.y))+", "+std::to_string(int(position.z)),16,16);
            r.text(carried[slot].id==373 && !(carried[slot].damage&0x4000)?"L2  Drink":
                   consoleFood(carried[slot].id) && survival?"L2  Eat":"L2  Place",16,325);r.text("R2  Mine / Attack",16,338);
            r.prompt('T',"Inventory",cw-96,325);r.prompt('X',"Jump",cw-96,338);
            if(survival)r.prompt('S',"Crafting",cw-96,312);
            if(flying)r.text("Flying",16,29);
            if(world.streaming() || r.rebuilding())r.text("Loading terrain...",16,flying?42:29);
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
        }else if(screen==Screen::Dead){
            // DeathScreen: red wash, title and the two source buttons.
            r.rect(0,0,cw,360,{.45f,.05f,.05f,.55f});
            r.centered("You died!",90,2);
            r.centered("Score: "+std::to_string(world.playerTotalExperience()),125,1);
            auto entries=buttons();
            for(int i=0;i<int(entries.size());++i){float y=160+i*25.f,width=200;r.sprite(i==selection?"button_focus":"button",cw/2-width/2,y,width,20);
                r.text(entries[i],(cw-r.textWidth(entries[i]))/2,y+6,1,i==selection?glm::vec4(1,1,.65,1):glm::vec4(1));}
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
        }else{
            if(inWorld)r.rect(0,0,cw,360,{0,0,0,.65});
            if(screen==Screen::Main){
                // The loose PS3 title image includes an Xbox subtitle; use only its common wordmark.
                r.sprite("logo",cw/2-142.75f,38,285.5f,40,{0,0,1,80/138.f});
                r.centered("PLAYSTATION 3 EDITION",88,1.35f,{.82,.82,.82,1});
            }else r.centered(screen==Screen::Worlds?"Play Game":screen==Screen::CreateWorld?"Create New World":screen==Screen::FindingSeed?"Finding World Seed":screen==Screen::Loading?"Loading World":screen==Screen::Pause?"Game Paused":screen==Screen::Options?"Help & Options":screen==Screen::Controls?"Controls":"Console Services",65,2);
            if(screen==Screen::Worlds){
                r.centered("Load / Create",91);
                if(savedWorlds.empty())r.centered("No saved worlds",315,.8f);
                else r.centered("Page "+std::to_string(worldPage+1)+" / "+std::to_string((savedWorlds.size()+3)/4),335,.8f);
            }
            if(screen==Screen::FindingSeed)r.centered("Searching... "+std::to_string(seedAttempts),108,1);
            if(screen==Screen::Loading){
                constexpr const char* stages[]={"Saving previous world...","Preparing chunks...","Preparing world...","Saving world...","Building the world view..."};
                const int stage=loadPhase?std::clamp(loadPhase->load(),0,4):0;
                r.centered(stages[stage],146,1);
            }
            if(screen==Screen::CreateWorld)r.centered("Offline Game",108,.85f);
            if(screen==Screen::Controls){
                const char* lines[]={"W A S D / Left Stick - Move   Ctrl - Sprint","Mouse / Right Stick - Look","Space / Cross - Jump / Swim / Fly Up","Shift / Square - Fly Down (creative)","F / L3 - Toggle Flight (creative)","Left Mouse / R2 - Mine / Attack","Right Mouse / L2 - Place / Use / Eat","1-9 / Wheel / L1 R1 - Select Item","E / Triangle - Inventory   C / Square - Crafting","Q - Drop Item (Ctrl+Q: Stack)","Esc / Start - Pause   F2 - Screenshot"};
                for(int i=0;i<11;++i)r.centered(lines[i],104+i*14,.85f);
            }
            if(screen==Screen::Notice){r.centered(notice,158,.9f);r.centered("This build supports local survival and creative play.",180,.9f);}
            auto entries=buttons();
            for(int i=0;i<int(entries.size());++i){float y=buttonY()+i*25,width=buttonWidth();r.sprite(i==selection?"button_focus":"button",cw/2-width/2,y,width,20);
                auto color=i==selection?glm::vec4(1,1,.65,1):glm::vec4(1);
                float scale=std::min(1.f,(width-10)/std::max(1.f,r.textWidth(entries[i])));
                r.text(entries[i],(cw-r.textWidth(entries[i],scale))/2,y+6,scale,color);}
            if(screen!=Screen::Loading){r.prompt('X',"Select",20,329);if(screen!=Screen::Main)r.prompt('O',"Back",95,329);}
        }
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
        glfwSetKeyCallback(window,[](GLFWwindow* w,int key,int,int action,int){static_cast<App*>(glfwGetWindowUserPointer(w))->key(key,action);});
        glfwSetCharCallback(window,[](GLFWwindow* w,unsigned c){auto a=static_cast<App*>(glfwGetWindowUserPointer(w));if(a->seedEditing && a->seedText.size()<60 && c>=32 && c<127)a->seedText+=char(c);if(a->nameEditing && a->worldName.size()<25 && c>=32 && c<127)a->worldName+=char(c);});
        glfwSetCursorPosCallback(window,[](GLFWwindow* w,double x,double y){static_cast<App*>(glfwGetWindowUserPointer(w))->look(x,y);});
        glfwSetScrollCallback(window,[](GLFWwindow* w,double,double y){auto a=static_cast<App*>(glfwGetWindowUserPointer(w));
            if(a->screen==Screen::Playing && y!=0)a->slot=(a->slot+(y>0?8:1))%9;
            else if(a->screen==Screen::Inventory && y!=0)a->turnCreativePage(y>0?-1:1);
            else if(a->screen==Screen::Crafting && y!=0){const int count=int(a->craftingList().size());
                if(count)a->selection=std::clamp(a->selection+(y>0?-1:1),0,count-1);}
        });
        glfwSetMouseButtonCallback(window,[](GLFWwindow* w,int button,int action,int){auto a=static_cast<App*>(glfwGetWindowUserPointer(w));
            if(button==GLFW_MOUSE_BUTTON_LEFT)a->leftMouse=action==GLFW_PRESS;
            if(button==GLFW_MOUSE_BUTTON_RIGHT)a->rightMouse=action==GLFW_PRESS;
            if((a->screen==Screen::Chest || a->screen==Screen::Furnace || a->screen==Screen::Brewing) && button==GLFW_MOUSE_BUTTON_RIGHT && action==GLFW_PRESS)a->click(0);
            if(a->screen!=Screen::Playing && button==GLFW_MOUSE_BUTTON_LEFT && action==GLFW_PRESS)a->click();});
        glfwSetWindowFocusCallback(window,[](GLFWwindow* w,int focused){auto a=static_cast<App*>(glfwGetWindowUserPointer(w));if(!focused && a->screen==Screen::Playing){a->leftMouse=a->rightMouse=false;a->change(Screen::Pause);}});
        double previous=glfwGetTime();int frame=0;
        while(true){
            glfwPollEvents();double now=glfwGetTime(),dt=std::clamp(now-previous,0.0,.05);previous=now;
            if(glfwWindowShouldClose(window)){
                try{app.save();break;}catch(const std::exception& e){app.message(e.what());glfwSetWindowShouldClose(window,0);app.change(Screen::Pause);}
            }
            try{app.update(dt);app.draw();}
            catch(const std::exception& e){
                if(!smokeDir.empty())throw;
                std::cerr<<"Console port frame error: "<<e.what()<<'\n';
                if(app.screen==Screen::Playing)app.change(Screen::Pause);
                app.message(e.what());
            }
            if(app.screenshotRequested){app.screenshotRequested=false;
                try{app.renderer->screenshot(app.dataDir/"screenshots"/("capture-"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count())+".png"));app.message("Screenshot saved");}
                catch(const std::exception& e){app.message(e.what());}}
            if(!smokeDir.empty()){
                if(frame==3)app.renderer->screenshot(smokeDir/"menu.png");
                if(frame==4){app.seedText="1";app.survivalWorld=false;app.change(Screen::Worlds);app.selection=0;app.activate();app.selection=4;app.activate();if(!app.loaded)throw std::runtime_error("Smoke world failed to start");app.flying=true;app.position.y+=9;app.pitch=-.35;}
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
                    app.world.swapCarriedSlots(0,9);
                    app.change(Screen::Inventory);app.inventoryCategory=creativeTabCount();app.selection=9;app.activate();
                    if(app.selectedItem().id!=Stone || app.world.carriedItems()[9].id)
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
                if(frame==17)app.change(Screen::Dead);
                if(frame==18){glFinish();auto error=glGetError();if(error!=GL_NO_ERROR)throw std::runtime_error("OpenGL error: "+std::to_string(error));
                    std::cout<<"Smoke test passed: menu, world, inventory, furnace, brewing, chest, survival HUD/crafting/death, edit/save/reload; "<<app.renderer->triangleCount()<<" world triangles\n";break;}
            }
            glfwSwapBuffers(window);++frame;
        }
    }catch(const std::exception& e){std::cerr<<"Console port: "<<e.what()<<'\n';result=1;}
    glfwDestroyWindow(window);glfwTerminate();return result;
}
