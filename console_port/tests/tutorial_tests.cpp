// The original FullTutorial (ported/tutorial) driven through TutorialSession
// with a scripted engine, plus the tutorial pack's LevelRules and strings.
#include "TutorialSession.h"
#include "TutorialSchematics.h"
#include "ConsoleStrings.h"
#include "ItemDescriptions.h"
#include "RichText.h"
#include "GameSettings.h"
#include <unistd.h>
#include <array>
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace console;
using Session=TutorialSession;

static void require(bool good,const std::string& message){if(!good)throw std::runtime_error(message);}

static std::wstring text(const char* name){
    const int id=consoleStringId(name);
    require(id>=0,std::string("unknown string ")+name);
    std::wstring out;
    for(const unsigned char* p=reinterpret_cast<const unsigned char*>(consoleString(id));*p;){
        unsigned c=*p++;int extra=c>=0xF0?3:c>=0xE0?2:c>=0xC0?1:0;
        if(extra)c&=0x3F>>extra;
        for(int i=0;i<extra;++i)c=(c<<6)|(*p++&0x3F);
        out.push_back(wchar_t(c));
    }
    return out;
}

struct FakeEngine:TutorialEngine{
    const TutorialLevelRules* rules=nullptr;
    double x=97,y=72,z=-106;
    bool water=false,creative=false,menu=false,pause=false,left=false;
    std::int64_t time=1000,overrideTime=-1;
    std::uint32_t ms=1000;
    std::array<int,Session::ActionCount> input{};
    std::map<std::tuple<int,int,int>,int> tiles;
    std::vector<TutorialPopup> popups;
    bool visible=true;
    int group=0,selectedItem=0;
    std::map<int,unsigned char> settings{{14 /*eGameSetting_Hints*/,1},{23 /*eGameSetting_DisplayHUD*/,1}};

    void playerPosition(double& px,double& py,double& pz)override{px=x;py=y;pz=z;}
    bool playerUnderWater()override{return water;}
    bool playerCreative()override{return creative;}
    void setPlayerCreative(bool c)override{creative=c;}
    int tile(int tx,int ty,int tz)override{auto it=tiles.find({tx,ty,tz});return it==tiles.end()?0:it->second;}
    std::int64_t levelTime()override{return time;}
    void setLevelTime(std::int64_t t)override{time=t;}
    void setOverrideTimeOfDay(std::int64_t t)override{overrideTime=t;}
    int inputValue(int action)override{return action>=0 && action<Session::ActionCount?input[action]:0;}
    bool menuDisplayed()override{return menu;}
    bool pauseMenuDisplayed()override{return pause;}
    std::uint32_t tickCount()override{return ms;}
    unsigned char gameSetting(int s)override{auto it=settings.find(s);return it==settings.end()?0:it->second;}
    const TutorialArea* namedArea(const std::wstring& name)override{
        auto it=rules->namedAreas.find(name);return it==rules->namedAreas.end()?nullptr:&it->second;
    }
    void setTutorialVisible(bool v)override{visible=v;}
    void setTutorialDescription(const TutorialPopup& popup)override{popups.push_back(popup);}
    void playerLeftTutorial()override{left=true;}
    int craftingGroup()override{return group;}
    bool craftingItemSelected(int id)override{return selectedItem==id;}

    std::wstring shown()const{return popups.empty()?L"":popups.back().description;}
};

// The popup text is the task description followed by its prompt.
static bool starts(const std::wstring& shown,const char* name){
    const auto t=text(name);
    const bool good=shown.compare(0,t.size(),t)==0;
    if(!good)std::wcerr<<L"shown: "<<shown.substr(0,120)<<L"\n";
    return good;
}
static bool ends(const std::wstring& shown,const char* name){
    const auto t=text(name);return shown.size()>=t.size() && shown.compare(shown.size()-t.size(),t.size(),t)==0;
}

int main(int argc,char** argv){try{
    require(argc==2,"usage: console_tutorial_tests <assets/tutorial>");
    const TutorialSchematics pack{std::filesystem::path(argv[1])};
    const auto& rules=pack.levelRules();

    // ---- LevelRules and languages.loc from Tutorial.pck ----
    require(rules.namedAreas.size()==14,"Tutorial.pck names fourteen areas");
    const auto& area=rules.namedAreas.at(L"tutorialArea");
    require(area.x0==54 && area.y0==0 && area.z0==-156 && area.x1==109 && area.y1==127 && area.z1==-87,"tutorialArea bounds");
    const auto& piston=rules.namedAreas.at(L"pistonArea");
    require(piston.x0==9 && piston.z1==-94,"pistonArea bounds");
    require(!rules.namedAreas.contains(L"pistonBridgeArea"),"The commented-out bridge area is absent");
    require(rules.updatePlayer && rules.updatePlayer->hasSpawn && rules.updatePlayer->spawnX==97 &&
            rules.updatePlayer->spawnY==72 && rules.updatePlayer->spawnZ==-106,"UpdatePlayer spawn");
    require(rules.updatePlayer->hasYRot && std::abs(rules.updatePlayer->yRot-81.59f)<1e-4f,"UpdatePlayer yRot");
    require(rules.updatePlayer->health==12 && rules.updatePlayer->food==12,"UpdatePlayer health and food");
    require(rules.updatePlayer->items.size()==1 && rules.updatePlayer->items[0].id==364 &&
            rules.updatePlayer->items[0].slot==10,"The steak is added to slot 10");
    require(rules.collectGoals.size()==12 && rules.completeAllDescription==L"IDS_COLLECTED_MUSIC_DISCS","Music disc goal");
    require(pack.strings().at(L"IDS_COLLECTED_MUSIC_DISCS")==L"You have found {*progress*} of {*goal*} Music Discs!",
            "languages.loc English string");

    // ---- Tables the hints read ----
    require(consoleDescriptionId(1)==consoleStringId("IDS_TILE_STONE") &&
            consoleUseDescriptionId(1)==consoleStringId("IDS_DESC_STONE"),"Stone description IDs");
    require(consoleDescriptionId(17,2)==consoleStringId("IDS_TILE_LOG_BIRCH"),"TreeTile::getDescriptionId uses TREE_NAMES");
    require(consoleDescriptionId(5,1)==consoleStringId("IDS_TILE_SPRUCEWOOD_PLANKS"),"Planks use the MultiTextureTileItem names");
    require(consoleDescriptionId(145,4)==consoleStringId("IDS_TILE_ANVIL_SLIGHTLYDAMAGED"),"AnvilTileItem shifts the data");
    require(consoleDescriptionId(280)==consoleStringId("IDS_ITEM_STICK"),"Item descriptions");

    // ---- Popup text formatting (FormatHTMLString with the PS3 map) ----
    require(consoleActionGlyph(Session::Jump)==PadGlyph::Cross && consoleActionGlyph(Session::Use)==PadGlyph::L2 &&
            consoleActionGlyph(Session::ActionButton)==PadGlyph::R2 && consoleActionGlyph(Session::CraftingAction)==PadGlyph::Square &&
            consoleActionGlyph(Session::InventoryAction)==PadGlyph::Triangle && consoleActionGlyph(Session::Drop)==PadGlyph::Circle &&
            consoleActionGlyph(Session::LeftScroll)==PadGlyph::L1,"PS3 DefineActions glyphs");
    {
        auto spans=parseConsoleRichText(text("IDS_TUTORIAL_TASK_MOVE"));
        require(spans.size()==3 && spans[0].text=="Use" && spans[1].glyph==PadGlyph::LeftStick &&
                spans[2].text==" to move around.","Move text uses the left stick image");
        spans=parseConsoleRichText(text("IDS_TUTORIAL_TASK_MOVE"),true);
        require(spans[1].glyph==PadGlyph::RightStick,"Southpaw swaps the move image");
        spans=parseConsoleRichText(L"{*T3*}HEAD{*ETW*}{*B*}body {*C2*}green{*EF*} end");
        require(spans.size()==5 && spans[0].colour==0xffff55 && spans[1].lineBreak && spans[2].colour==0xffffff &&
                spans[3].text=="green" && spans[3].colour==0x109e10 && spans[4].text==" end","Colour tokens");
        int id=0,aux=0;
        require(consoleRichTextIcon(L"Make a {*SticksIcon*}stick",id,aux) && id==280,"Fixed icon token");
        require(consoleRichTextIcon(L"x{*ICON*}35:14{*/ICON*}",id,aux) && id==35 && aux==14,"ICON tag");
        const auto lines=layoutRichText(parseConsoleRichText(L"aa bb cc"),5,[](const RichSpan&){return 2.f;},
                                         [](const std::string& s){return float(s.size());});
        require(lines.size()==2 && lines[0].pieces.size()==2 && lines[1].pieces[0].span.text=="cc","Word wrap");
    }

    // ---- GAME_SETTINGS profile ----
    {
        GameSettings settings;
        require(settings.get(GameSetting::Gamma)==50 && settings.get(GameSetting::Hints)==1 &&
                settings.get(GameSetting::Autosave)==2 && settings.get(GameSetting::InterfaceOpacity)==80 &&
                settings.get(GameSetting::UISize)==1 && settings.get(GameSetting::Difficulty)==1,"SetDefaultOptions");
        settings.set(GameSetting::Difficulty,9);
        require(settings.get(GameSetting::Difficulty)==3,"Difficulty is two bits");
        settings.tutorialCompletion()[3]=0x42;settings.setSpecialTutorialCompletion(4);
        const auto file=std::filesystem::temp_directory_path()/("console_settings_"+std::to_string(::getpid()));
        settings.save(file);
        GameSettings loaded;loaded.load(file);std::filesystem::remove(file);
        require(loaded.get(GameSetting::Difficulty)==3 && loaded.tutorialCompletion()[3]==0x42 &&
                loaded.specialTutorialBitmask()==16,"Settings round trip");
    }

    // ---- FullTutorial through TutorialSession ----
    FakeEngine engine;engine.rules=&rules;
    std::array<std::uint8_t,Session::ProfileBytes> profile{};
    Session session(engine,profile);
    auto tick=[&](std::uint32_t ms=50){engine.ms+=ms;session.tick();};
    auto press=[&](int action){engine.input[action]=1;tick();engine.input[action]=0;};
    auto waitShown=[&]{tick(2100);}; // TUTORIAL_MINIMUM_DISPLAY_MESSAGE_TIME

    tick(0);
    require(engine.popups.empty(),"Nothing is shown during the first 1.5 seconds");
    tick(1600);
    require(engine.overrideTime==8000,"The full tutorial freezes the time of day at 8000");
    require(session.isTutorial() && session.currentState()==Session::Gameplay,"Starts on the gameplay track");
    require(starts(engine.shown(),"IDS_TUTORIAL_TASK_OVERVIEW"),"The overview is shown first");
    require(!session.isInputAllowed(Session::Jump),"Jump shares the A button, which the overview constrains");
    require(session.isInputAllowed(Session::Forward),"Movement is not constrained");
    press(Session::MenuA);
    require(starts(engine.shown(),"IDS_TUTORIAL_TASK_OVERVIEW"),"A does nothing before the minimum display time");
    waitShown();
    require(ends(engine.shown(),"IDS_TUTORIAL_PROMPT_PRESS_A_TO_CONTINUE"),"The prompt appears after the minimum time");
    press(Session::MenuA);
    require(engine.shown()==text("IDS_TUTORIAL_TASK_LOOK"),"Look is next");
    require(!session.isInputAllowed(Session::Jump),"The A constraint lingers (TUTORIAL_CONSTRAINT_DELAY_REMOVE_TICKS)");
    for(int i=0;i<16;++i)tick();
    require(session.isInputAllowed(Session::Jump),"The A constraint is lifted after 15 ticks");

    // Controller tasks complete from the live input values.
    for(int a:{Session::LookUp,Session::LookDown,Session::LookLeft,Session::LookRight})engine.input[a]=1;
    tick();
    for(int a:{Session::LookUp,Session::LookDown,Session::LookLeft,Session::LookRight})engine.input[a]=0;
    tick();
    require(engine.shown()==text("IDS_TUTORIAL_TASK_MOVE"),"Move follows look");
    for(int a:{Session::Forward,Session::Backward,Session::Left,Session::Right})engine.input[a]=1;
    tick();
    for(int a:{Session::Forward,Session::Backward,Session::Left,Session::Right})engine.input[a]=0;
    tick();
    require(starts(engine.shown(),"IDS_TUTORIAL_TASK_SPRINT"),"Sprint info follows move");
    waitShown();press(Session::MenuA);
    require(engine.shown()==text("IDS_TUTORIAL_TASK_JUMP"),"Jump");
    waitShown();press(Session::Jump);tick();
    require(engine.shown()==text("IDS_TUTORIAL_TASK_MINE"),"Mine");
    waitShown();press(Session::ActionButton);tick();
    require(engine.shown()==text("IDS_TUTORIAL_TASK_CHOP_WOOD"),"Chop wood");
    session.onTake(17,0,3,3,3);tick();
    require(engine.shown()==text("IDS_TUTORIAL_TASK_CHOP_WOOD"),"Three logs are not enough");
    session.onTake(17,1,1,4,1);tick();
    require(engine.shown()==text("IDS_TUTORIAL_TASK_SCROLL"),"Four logs of any type complete the pickup");
    press(Session::RightScroll);tick();
    require(engine.shown()==text("IDS_TUTORIAL_TASK_INVENTORY"),"Open the inventory");

    // Menus switch state with their scene and restore it on close.
    press(Session::InventoryAction);
    engine.menu=true;session.openMenu(Session::InventoryMenu);tick();
    require(session.currentState()==Session::InventoryMenu,"The inventory menu state");
    require(starts(engine.shown(),"IDS_TUTORIAL_TASK_INV_OVERVIEW"),"Inventory overview");
    waitShown();
    require(ends(engine.shown(),"IDS_TUTORIAL_PROMPT_INV_OVERVIEW"),"Inventory overview prompt");
    require(!session.menuInput(Session::MenuUp,engine.visible),"Menu navigation is blocked while the choice is shown");
    session.menuInput(Session::MenuB,engine.visible);tick();
    // FullTutorialActiveTask completes (silently) on the following tick.
    tick();tick();
    require(starts(engine.shown(),"IDS_TUTORIAL_TASK_INV_EXIT"),"B skips the inventory lesson to the exit task");
    engine.menu=false;session.closeMenu();tick();
    require(session.currentState()==Session::Gameplay,"Closing the menu restores gameplay");
    require(starts(engine.shown(),"IDS_TUTORIAL_TASK_FOOD_BAR_DEPLETE"),"The food bar lessons follow the inventory");
    for(int i=0;i<3;++i){waitShown();press(Session::MenuA);}
    require(engine.shown()==text("IDS_TUTORIAL_TASK_FOOD_BAR_EAT_STEAK"),"Eat the steak");
    session.completeUsingItem(364,0);tick();
    require(engine.shown()==text("IDS_TUTORIAL_TASK_CRAFTING"),"Crafting follows eating");

    // Area-triggered lessons: walking into the piston area switches state.
    engine.x=15;engine.y=74;engine.z=-100;tick();
    require(session.currentState()==Session::RedstoneAndPiston,"The piston area starts the redstone lesson");
    require(starts(engine.shown(),"IDS_TUTORIAL_REDSTONE_OVERVIEW"),"Redstone overview");
    engine.x=97;engine.y=72;engine.z=-106;tick();
    require(session.currentState()==Session::Gameplay,"Leaving the area returns to gameplay");

    // Look-at hints: title and use description of the block.
    engine.popups.clear();
    tick(8000); // let the task message age past the hint delay
    session.onLookAt(1,0);
    require(!engine.popups.empty() && engine.popups.back().title==text("IDS_TILE_STONE") &&
            starts(engine.shown(),"IDS_DESC_STONE") && engine.popups.back().icon==1,"Stone look-at hint");
    require(session.profileChanged() && (session.profile()[3]&0x02)!=0,"The rock hint is saved in the profile bits");

    std::cout<<"tutorial tests passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
