#include "ConsoleMenus.h"
#include "ConsoleStrings.h"

#include <algorithm>
#include <stdexcept>

namespace console {
namespace {
int ids(const char* name){
    const int id=consoleStringId(name);
    if(id<0)throw std::logic_error(std::string("missing string ")+name);
    return id;
}
std::string text(const char* name){return consoleString(ids(name));}

// Control ids per scene (the eControl_* / BUTTON_* enums).
enum MainButtons { PlayGame,Leaderboards,HelpAndOptionsButton,UnlockOrDLC };
enum LoadButtons { CreateNewWorld=-3,PlayTutorial=-2,ClassicTutorial=-1 };
enum CreateControls { EditWorldName,EditSeed,GameModeToggle,DifficultySlider,MoreOptionsButton,NewWorld };
enum MoreOptions { Online,InviteOnly,AllowFoF,Pvp,TrustSystem,FireSpreads,Tnt,HostPrivileges,ResetNether,Structures,FlatWorld,BonusChest };
enum HelpButtons { ChangeSkin,HowToPlayButton,ControlsButton,SettingsButton,CreditsButton };
enum ControlsControls { Layout0,Layout1,Layout2,InvertLook,Southpaw };
enum SettingsButtons { AllOptions,AllAudio,AllControl,AllGraphics,AllUI,ResetToDefaults };
enum OptionsControls { ViewBob,ShowHints,ShowTooltips,InGameGamertags,Autosave,Difficulty };
enum AudioControls { Music,Sound };
enum ControlControls { SensitivityInGame,SensitivityInMenu };
enum GraphicsControls { Clouds,BedrockFog,CustomSkinAnim,Gamma,InterfaceOpacity };
enum UIControls { DisplayHUD,DisplayHand,DeathMessages,AnimatedCharacter,SplitscreenVertical,SplitscreenGamertags,UISize,UISizeSplitscreen };
enum PauseButtons { ResumeGame,PauseHelpAndOptions,PauseLeaderboards,SaveGame,ExitGame };
enum DeathButtons { Respawn,DeathExitGame };

// UIScene_HowToPlayMenu::m_uiHTPButtonNameA / m_uiHTPSceneA (not _XBOX) and
// the EHowToPlayPage order of UIScene_HowToPlay::gs_aPageDefs.
constexpr const char* kHowToPlayButtons[]{
    "IDS_HOW_TO_PLAY_MENU_WHATSNEW","IDS_HOW_TO_PLAY_MENU_BASICS","IDS_HOW_TO_PLAY_MENU_MULTIPLAYER",
    "IDS_HOW_TO_PLAY_MENU_HUD","IDS_HOW_TO_PLAY_MENU_CREATIVE","IDS_HOW_TO_PLAY_MENU_INVENTORY",
    "IDS_HOW_TO_PLAY_MENU_CHESTS","IDS_HOW_TO_PLAY_MENU_CRAFTING","IDS_HOW_TO_PLAY_MENU_FURNACE",
    "IDS_HOW_TO_PLAY_MENU_DISPENSER","IDS_HOW_TO_PLAY_MENU_BREWING","IDS_HOW_TO_PLAY_MENU_ENCHANTMENT",
    "IDS_HOW_TO_PLAY_MENU_ANVIL","IDS_HOW_TO_PLAY_MENU_FARMANIMALS","IDS_HOW_TO_PLAY_MENU_BREEDANIMALS",
    "IDS_HOW_TO_PLAY_MENU_TRADING","IDS_HOW_TO_PLAY_MENU_NETHERPORTAL","IDS_HOW_TO_PLAY_MENU_THEEND",
    "IDS_HOW_TO_PLAY_MENU_HOSTOPTIONS"};
constexpr int kHowToPlayButtonPage[]{0,1,2,3,4,5,6,9,11,12,13,14,15,16,17,18,19,20,21};
constexpr const char* kHowToPlayPages[]{
    "IDS_HOW_TO_PLAY_WHATSNEW","IDS_HOW_TO_PLAY_BASICS","IDS_HOW_TO_PLAY_MULTIPLAYER","IDS_HOW_TO_PLAY_HUD",
    "IDS_HOW_TO_PLAY_CREATIVE","IDS_HOW_TO_PLAY_INVENTORY","IDS_HOW_TO_PLAY_CHEST","IDS_HOW_TO_PLAY_LARGECHEST",
    "IDS_HOW_TO_PLAY_ENDERCHEST","IDS_HOW_TO_PLAY_CRAFTING","IDS_HOW_TO_PLAY_CRAFT_TABLE","IDS_HOW_TO_PLAY_FURNACE",
    "IDS_HOW_TO_PLAY_DISPENSER","IDS_HOW_TO_PLAY_BREWING","IDS_HOW_TO_PLAY_ENCHANTMENT","IDS_HOW_TO_PLAY_ANVIL",
    "IDS_HOW_TO_PLAY_FARMANIMALS","IDS_HOW_TO_PLAY_BREEDANIMALS","IDS_HOW_TO_PLAY_TRADING",
    "IDS_HOW_TO_PLAY_NETHERPORTAL","IDS_HOW_TO_PLAY_THEEND","IDS_HOW_TO_PLAY_HOSTOPTIONS"};
constexpr int kHowToPlayPageCount=sizeof(kHowToPlayPages)/sizeof(kHowToPlayPages[0]);

// UIScene_SettingsOptionsMenu::m_iDifficultyTitleSettingA / m_iDifficultySettingA.
constexpr const char* kDifficultyTitles[]{"IDS_DIFFICULTY_TITLE_PEACEFUL","IDS_DIFFICULTY_TITLE_EASY",
                                          "IDS_DIFFICULTY_TITLE_NORMAL","IDS_DIFFICULTY_TITLE_HARD"};
constexpr const char* kDifficultyText[]{"IDS_DIFFICULTY_PEACEFUL","IDS_DIFFICULTY_EASY",
                                        "IDS_DIFFICULTY_NORMAL","IDS_DIFFICULTY_HARD"};
// UIScene_LaunchMoreOptionsMenu::handleFocusChange descriptions.
constexpr const char* kMoreOptionsText[]{"IDS_GAMEOPTION_ONLINE","IDS_GAMEOPTION_INVITEONLY","IDS_GAMEOPTION_ALLOWFOF",
    "IDS_GAMEOPTION_PVP","IDS_GAMEOPTION_TRUST","IDS_GAMEOPTION_FIRE_SPREADS","IDS_GAMEOPTION_TNT_EXPLODES",
    "IDS_GAMEOPTION_HOST_PRIVILEGES","IDS_GAMEOPTION_RESET_NETHER","IDS_GAMEOPTION_STRUCTURES",
    "IDS_GAMEOPTION_SUPERFLAT","IDS_GAMEOPTION_BONUS_CHEST"};
constexpr const char* kMoreOptionsLabels[]{"IDS_ONLINE_GAME","IDS_INVITE_ONLY","IDS_ALLOWFRIENDSOFFRIENDS",
    "IDS_PLAYER_VS_PLAYER","IDS_TRUST_PLAYERS","IDS_FIRE_SPREADS","IDS_TNT_EXPLODES","IDS_HOST_PRIVILEGES",
    "IDS_RESET_NETHER","IDS_GENERATE_STRUCTURES","IDS_SUPERFLAT_WORLD","IDS_BONUS_CHEST"};

MenuControl button(int id,const std::string& label){MenuControl c;c.id=id;c.label=label;return c;}
MenuControl checkbox(int id,const std::string& label,bool checked){
    MenuControl c;c.kind=MenuControlKind::Checkbox;c.id=id;c.label=label;c.checked=checked;return c;
}
MenuControl slider(int id,int min,int max,int value){
    MenuControl c;c.kind=MenuControlKind::Slider;c.id=id;c.min=min;c.max=max;c.value=std::clamp(value,min,max);return c;
}
}

ConsoleMenus::ConsoleMenus(GameSettings& settings):settings_(settings){}

MenuScene ConsoleMenus::scene()const{
    if(stack_.empty())throw std::logic_error("no menu");
    return top().scene;
}
MenuScene ConsoleMenus::below()const{
    if(stack_.size()<2)return scene();
    return stack_[stack_.size()-2].scene;
}
const std::vector<MenuControl>& ConsoleMenus::controls()const{return top().controls;}
int ConsoleMenus::focus()const{return top().focus;}
void ConsoleMenus::setFocus(int index){
    if(stack_.empty() || index<0 || index>=int(top().controls.size()) || !top().controls[index].enabled)return;
    top().focus=index;
    if(top().scene==MenuScene::Controls && index<=Layout2)layoutPreview_=index; // handleFocusChange
}
const ConsoleMenus::Message* ConsoleMenus::message()const{
    return !stack_.empty() && top().scene==MenuScene::MessageBox?&top().message:nullptr;
}
int ConsoleMenus::howToPlayText()const{return ids(kHowToPlayPages[std::clamp(page_,0,kHowToPlayPageCount-1)]);}

void ConsoleMenus::show(MenuScene root,const MenuContext& context){
    context_=context;
    stack_.clear();
    push(root);
}

void ConsoleMenus::push(MenuScene scene){
    Frame frame;
    frame.scene=scene;
    if(scene==MenuScene::CreateWorld){
        // UIScene_CreateWorldMenu: IDS_DEFAULT_WORLD_NAME, survival, the
        // More Options defaults, and (offline) no online game.
        newWorld_=NewWorldOptions{};
        newWorld_.name=text("IDS_DEFAULT_WORLD_NAME");
    }
    if(scene==MenuScene::Controls)layoutPreview_=settings_.get(GameSetting::ControlScheme);
    frame.controls=build(scene);
    frame.focus=0;
    while(frame.focus<int(frame.controls.size()) && !frame.controls[frame.focus].enabled)++frame.focus;
    if(scene==MenuScene::Controls)frame.focus=layoutPreview_;
    stack_.push_back(std::move(frame));
    if(scene==MenuScene::Credits)creditsScroll=0;
}

void ConsoleMenus::sliderLabel(MenuScene scene,MenuControl& c)const{
    auto percent=[&](const char* name){return text(name)+": "+std::to_string(c.value)+"%";};
    switch(scene){
    case MenuScene::CreateWorld:case MenuScene::SettingsOptions:
        if(scene==MenuScene::SettingsOptions && c.id==Autosave){
            c.label=c.value==0?text("IDS_SLIDER_AUTOSAVE_OFF"):
                text("IDS_SLIDER_AUTOSAVE")+": "+std::to_string(c.value*15)+" "+text("IDS_MINUTES");
        }else c.label=text("IDS_SLIDER_DIFFICULTY")+": "+text(kDifficultyTitles[c.value]);
        break;
    case MenuScene::SettingsAudio:c.label=percent(c.id==Music?"IDS_SLIDER_MUSIC":"IDS_SLIDER_SOUND");break;
    case MenuScene::SettingsControl:
        c.label=percent(c.id==SensitivityInGame?"IDS_SLIDER_SENSITIVITY_INGAME":"IDS_SLIDER_SENSITIVITY_INMENU");break;
    case MenuScene::SettingsGraphics:c.label=percent(c.id==Gamma?"IDS_SLIDER_GAMMA":"IDS_SLIDER_INTERFACEOPACITY");break;
    case MenuScene::SettingsUI:
        c.label=text(c.id==UISize?"IDS_SLIDER_UISIZE":"IDS_SLIDER_UISIZESPLITSCREEN")+": "+std::to_string(c.value);break;
    default:break;
    }
}

std::vector<MenuControl> ConsoleMenus::build(MenuScene scene)const{
    std::vector<MenuControl> c;
    const bool inGame=context_.inGame;
    auto get=[&](GameSetting s){return int(settings_.get(s));};
    switch(scene){
    case MenuScene::MainMenu:
        // PS3: no Exit Game (PS button) and no Achievements button.
        c.push_back(button(PlayGame,text("IDS_PLAY_GAME")));
        c.push_back(button(Leaderboards,text("IDS_LEADERBOARDS")));
        c.push_back(button(HelpAndOptionsButton,text("IDS_HELP_AND_OPTIONS")));
        c.push_back(button(UnlockOrDLC,text("IDS_DOWNLOADABLECONTENT")));
        break;
    case MenuScene::LoadOrJoin:{
        // UpdateGamesList: Create New World, the level generators (the
        // tutorial, IDS_PLAY_TUTORIAL) and then the saves.
        auto item=[&](int id,const std::string& label){MenuControl m=button(id,label);m.kind=MenuControlKind::ListItem;c.push_back(m);};
        item(CreateNewWorld,text("IDS_CREATE_NEW_WORLD"));
        item(PlayTutorial,text("IDS_PLAY_TUTORIAL"));
        // The archived tutorial save supplied with the port (not a console entry).
        item(ClassicTutorial,"Classic Tutorial World");
        for(int i=0;i<int(context_.saves.size());++i)item(i,context_.saves[i]);
        break;
    }
    case MenuScene::CreateWorld:{
        MenuControl name;name.kind=MenuControlKind::TextField;name.id=EditWorldName;
        name.label=text("IDS_WORLD_NAME");name.text=newWorld_.name;c.push_back(name);
        MenuControl seed;seed.kind=MenuControlKind::TextField;seed.id=EditSeed;
        seed.label=text("IDS_CREATE_NEW_WORLD_SEED");seed.text=newWorld_.seed;
        seed.placeholder=text("IDS_CREATE_NEW_WORLD_RANDOM_SEED");c.push_back(seed);
        c.push_back(button(GameModeToggle,text(newWorld_.survival?"IDS_GAMEMODE_SURVIVAL":"IDS_GAMEMODE_CREATIVE")));
        c.push_back(slider(DifficultySlider,0,3,get(GameSetting::Difficulty)));
        c.push_back(button(MoreOptionsButton,text("IDS_MORE_OPTIONS")));
        c.push_back(button(NewWorld,text("IDS_CREATE_NEW_WORLD")));
        break;
    }
    case MenuScene::MoreOptions:{
        const bool values[]{newWorld_.onlineGame,newWorld_.inviteOnly,newWorld_.friendsOfFriends,newWorld_.pvp,
                            newWorld_.trustPlayers,newWorld_.fireSpreads,newWorld_.tnt,newWorld_.hostPrivileges,
                            newWorld_.resetNether,newWorld_.structures,newWorld_.superflat,newWorld_.bonusChest};
        for(int i=0;i<=BonusChest;++i){
            // LaunchMoreOptionsMenu720.swf leaves ResetNether outside the
            // visible canvas in the new-world flow (at 40,-50).
            if(i==ResetNether)continue;
            auto box=checkbox(i,text(kMoreOptionsLabels[i]),values[i]);
            box.description=ids(kMoreOptionsText[i]);
            // Not signed in to PSN: the online options are disabled.
            if(i<=AllowFoF)box.enabled=false;
            c.push_back(box);
        }
        break;
    }
    case MenuScene::HelpAndOptions:
        c.push_back(button(ChangeSkin,text("IDS_CHANGE_SKIN")));
        c.push_back(button(HowToPlayButton,text("IDS_HOW_TO_PLAY")));
        c.push_back(button(ControlsButton,text("IDS_CONTROLS")));
        c.push_back(button(SettingsButton,text("IDS_SETTINGS")));
        c.push_back(button(CreditsButton,text("IDS_CREDITS")));
        break;
    case MenuScene::HowToPlayMenu:
        for(int i=0;i<int(sizeof(kHowToPlayButtons)/sizeof(kHowToPlayButtons[0]));++i){
            MenuControl m=button(i,text(kHowToPlayButtons[i]));m.kind=MenuControlKind::ListItem;c.push_back(m);
        }
        break;
    case MenuScene::Controls:
        c.push_back(button(Layout0,"1"));c.push_back(button(Layout1,"2"));c.push_back(button(Layout2,"3"));
        c.push_back(checkbox(InvertLook,text("IDS_INVERT_LOOK"),get(GameSetting::ControlInvertLook)!=0));
        c.push_back(checkbox(Southpaw,text("IDS_SOUTHPAW"),get(GameSetting::ControlSouthPaw)!=0));
        break;
    case MenuScene::Settings:
        c.push_back(button(AllOptions,text("IDS_OPTIONS")));
        c.push_back(button(AllAudio,text("IDS_AUDIO")));
        c.push_back(button(AllControl,text("IDS_CONTROL")));
        c.push_back(button(AllGraphics,text("IDS_GRAPHICS")));
        c.push_back(button(AllUI,text("IDS_USER_INTERFACE")));
        c.push_back(button(ResetToDefaults,text("IDS_RESET_TO_DEFAULTS")));
        break;
    case MenuScene::SettingsOptions:
        c.push_back(checkbox(ViewBob,text("IDS_VIEW_BOBBING"),get(GameSetting::ViewBob)!=0));
        c.push_back(checkbox(ShowHints,text("IDS_HINTS"),get(GameSetting::Hints)!=0));
        c.push_back(checkbox(ShowTooltips,text("IDS_IN_GAME_TOOLTIPS"),get(GameSetting::Tooltips)!=0));
        c.push_back(checkbox(InGameGamertags,text("IDS_IN_GAME_GAMERTAGS"),get(GameSetting::GamertagsVisible)!=0));
        c.push_back(slider(Autosave,0,8,get(GameSetting::Autosave)));
        // In a game the difficulty is a host option, not a profile setting.
        if(!inGame){
            auto difficulty=slider(Difficulty,0,3,get(GameSetting::Difficulty));
            difficulty.description=ids(kDifficultyText[difficulty.value]);
            c.push_back(difficulty);
        }
        break;
    case MenuScene::SettingsAudio:
        c.push_back(slider(Music,0,100,get(GameSetting::MusicVolume)));
        c.push_back(slider(Sound,0,100,get(GameSetting::SoundFXVolume)));
        break;
    case MenuScene::SettingsControl:
        c.push_back(slider(SensitivityInGame,0,200,get(GameSetting::SensitivityInGame)));
        c.push_back(slider(SensitivityInMenu,0,200,get(GameSetting::SensitivityInMenu)));
        break;
    case MenuScene::SettingsGraphics:
        c.push_back(checkbox(Clouds,text("IDS_CHECKBOX_RENDER_CLOUDS"),get(GameSetting::Clouds)!=0));
        c.push_back(checkbox(BedrockFog,text("IDS_CHECKBOX_RENDER_BEDROCKFOG"),get(GameSetting::BedrockFog)!=0));
        c.push_back(checkbox(CustomSkinAnim,text("IDS_CHECKBOX_CUSTOM_SKIN_ANIM"),get(GameSetting::CustomSkinAnim)!=0));
        c.push_back(slider(Gamma,0,100,get(GameSetting::Gamma)));
        c.push_back(slider(InterfaceOpacity,0,100,get(GameSetting::InterfaceOpacity)));
        break;
    case MenuScene::SettingsUI:
        c.push_back(checkbox(DisplayHUD,text("IDS_CHECKBOX_DISPLAY_HUD"),get(GameSetting::DisplayHUD)!=0));
        c.push_back(checkbox(DisplayHand,text("IDS_CHECKBOX_DISPLAY_HAND"),get(GameSetting::DisplayHand)!=0));
        c.push_back(checkbox(DeathMessages,text("IDS_CHECKBOX_DEATH_MESSAGES"),get(GameSetting::DeathMessages)!=0));
        c.push_back(checkbox(AnimatedCharacter,text("IDS_CHECKBOX_ANIMATED_CHARACTER"),get(GameSetting::AnimatedCharacter)!=0));
        c.push_back(checkbox(SplitscreenVertical,text("IDS_CHECKBOX_VERTICAL_SPLIT_SCREEN"),get(GameSetting::SplitScreenVertical)!=0));
        c.push_back(checkbox(SplitscreenGamertags,text("IDS_CHECKBOX_DISPLAY_SPLITSCREENGAMERTAGS"),get(GameSetting::DisplaySplitscreenGamertags)!=0));
        c.push_back(slider(UISize,1,3,get(GameSetting::UISize)+1));
        c.push_back(slider(UISizeSplitscreen,1,3,get(GameSetting::UISizeSplitscreen)+1));
        break;
    case MenuScene::Pause:
        // PS3: no Achievements button.
        c.push_back(button(ResumeGame,text("IDS_RESUME_GAME")));
        c.push_back(button(PauseHelpAndOptions,text("IDS_HELP_AND_OPTIONS")));
        c.push_back(button(PauseLeaderboards,text("IDS_LEADERBOARDS")));
        c.push_back(button(SaveGame,text("IDS_SAVE_GAME")));
        c.push_back(button(ExitGame,text("IDS_EXIT_GAME")));
        break;
    case MenuScene::Death:
        c.push_back(button(Respawn,text("IDS_RESPAWN")));
        c.push_back(button(DeathExitGame,text("IDS_EXIT_GAME")));
        break;
    case MenuScene::HowToPlay:case MenuScene::Credits:case MenuScene::MessageBox:
        break;
    }
    for(auto& control:c)if(control.kind==MenuControlKind::Slider)sliderLabel(scene,control);
    return c;
}

void ConsoleMenus::refresh(){
    if(stack_.empty() || top().scene==MenuScene::MessageBox)return;
    const int focus=top().focus;
    // Keep uncommitted checkbox states (they are written on Back).
    const auto previous=top().controls;
    top().controls=build(top().scene);
    for(std::size_t i=0;i<top().controls.size() && i<previous.size();++i)
        if(top().controls[i].kind==MenuControlKind::Checkbox && previous[i].id==top().controls[i].id)
            top().controls[i].checked=previous[i].checked;
    top().focus=std::clamp(focus,0,std::max(0,int(top().controls.size())-1));
}

int ConsoleMenus::focusedDescription()const{
    if(stack_.empty() || top().controls.empty())return -1;
    return top().controls[top().focus].description;
}

int ConsoleMenus::title()const{
    switch(scene()){
    case MenuScene::LoadOrJoin:return ids("IDS_START_GAME");
    case MenuScene::CreateWorld:return ids("IDS_CREATE_NEW_WORLD");
    case MenuScene::MoreOptions:return ids("IDS_WORLD_OPTIONS");
    case MenuScene::Death:return ids("IDS_YOU_DIED");
    case MenuScene::MessageBox:return top().message.title;
    default:return -1;
    }
}

std::array<int,3> ConsoleMenus::tooltips()const{
    const int select=ids("IDS_TOOLTIPS_SELECT"),backId=ids("IDS_TOOLTIPS_BACK");
    switch(scene()){
    case MenuScene::MainMenu:return {select,-1,-1};
    case MenuScene::Death:return {select,-1,-1};
    case MenuScene::Credits:return {-1,backId,-1};
    case MenuScene::HowToPlay:{
        // UIScene_HowToPlay::updateTooltips.
        const int next=ids("IDS_HOW_TO_PLAY_NEXT"),prev=ids("IDS_HOW_TO_PLAY_PREV");
        if(page_==0)return {next,backId,-1};
        if(page_+1==kHowToPlayPageCount)return {-1,backId,prev};
        return {next,backId,prev};
    }
    case MenuScene::MessageBox:return {select,-1,-1};
    default:return {select,backId,-1};
    }
}

void ConsoleMenus::notice(int titleId,std::string note){
    requestMessage(titleId,-1,{ids("IDS_CONFIRM_OK")},[](int){return MenuEvent{};},std::move(note));
}

void ConsoleMenus::requestMessage(int titleId,int textId,std::vector<int> buttons,std::function<MenuEvent(int)> result,std::string note){
    Frame frame;
    frame.scene=MenuScene::MessageBox;
    frame.message={titleId,textId,std::move(buttons),std::move(note)};
    for(int i=0;i<int(frame.message.buttons.size());++i)frame.controls.push_back(button(i,consoleString(frame.message.buttons[i])));
    frame.onButton=std::move(result);
    stack_.push_back(std::move(frame));
}

void ConsoleMenus::commit(const Frame& frame){
    auto checked=[&](int id){
        for(const auto& c:frame.controls)if(c.id==id && c.kind==MenuControlKind::Checkbox)return c.checked;
        return false;
    };
    auto has=[&](int id){
        return std::any_of(frame.controls.begin(),frame.controls.end(),[&](const MenuControl& c){return c.id==id && c.kind==MenuControlKind::Checkbox;});
    };
    switch(frame.scene){
    case MenuScene::SettingsOptions:
        // UIScene_SettingsOptionsMenu::handleInput(ACTION_MENU_CANCEL)
        settings_.set(GameSetting::ViewBob,checked(ViewBob));
        settings_.set(GameSetting::GamertagsVisible,checked(InGameGamertags));
        settings_.set(GameSetting::Hints,checked(ShowHints));
        settings_.set(GameSetting::Tooltips,checked(ShowTooltips));
        break;
    case MenuScene::SettingsGraphics:
        settings_.set(GameSetting::Clouds,checked(Clouds));
        if(has(BedrockFog))settings_.set(GameSetting::BedrockFog,checked(BedrockFog));
        if(has(CustomSkinAnim))settings_.set(GameSetting::CustomSkinAnim,checked(CustomSkinAnim));
        break;
    case MenuScene::SettingsUI:
        settings_.set(GameSetting::DisplayHUD,checked(DisplayHUD));
        settings_.set(GameSetting::DisplayHand,checked(DisplayHand));
        settings_.set(GameSetting::DisplaySplitscreenGamertags,checked(SplitscreenGamertags));
        settings_.set(GameSetting::DeathMessages,checked(DeathMessages));
        settings_.set(GameSetting::AnimatedCharacter,checked(AnimatedCharacter));
        settings_.set(GameSetting::SplitScreenVertical,checked(SplitscreenVertical));
        break;
    default:break;
    }
}

MenuEvent ConsoleMenus::back(){
    if(stack_.empty())return {};
    const Frame frame=top();
    switch(frame.scene){
    case MenuScene::MainMenu:case MenuScene::Death:return {}; // no Back
    case MenuScene::MessageBox:{
        // Declining a message box is its first (Cancel) button, when it has one.
        stack_.pop_back();
        return {};
    }
    case MenuScene::Pause:stack_.clear();return {MenuAction::ResumeGame};
    default:break;
    }
    commit(frame);
    stack_.pop_back();
    if(!stack_.empty())refresh();
    const bool settingsScene=frame.scene==MenuScene::SettingsOptions || frame.scene==MenuScene::SettingsGraphics ||
                             frame.scene==MenuScene::SettingsUI || frame.scene==MenuScene::Settings ||
                             frame.scene==MenuScene::Controls;
    return settingsScene?MenuEvent{MenuAction::SettingsChanged}:MenuEvent{};
}

MenuEvent ConsoleMenus::click(int index){
    if(stack_.empty() || index<0 || index>=int(top().controls.size()) || !top().controls[index].enabled)return {};
    setFocus(index);
    return press(index);
}

MenuEvent ConsoleMenus::press(int index){
    auto& frame=top();
    if(index<0 || index>=int(frame.controls.size()))return {};
    auto& control=frame.controls[index];
    if(!control.enabled)return {};
    if(control.kind==MenuControlKind::Checkbox){
        control.checked=!control.checked;
        switch(frame.scene){
        case MenuScene::MoreOptions:{
            // UIScene_LaunchMoreOptionsMenu::handleCheckboxToggled writes the params.
            bool* fields[]{&newWorld_.onlineGame,&newWorld_.inviteOnly,&newWorld_.friendsOfFriends,&newWorld_.pvp,
                           &newWorld_.trustPlayers,&newWorld_.fireSpreads,&newWorld_.tnt,&newWorld_.hostPrivileges,
                           &newWorld_.resetNether,&newWorld_.structures,&newWorld_.superflat,&newWorld_.bonusChest};
            *fields[control.id]=control.checked;
            break;
        }
        case MenuScene::Controls:
            // UIScene_ControlsMenu::handleCheckboxToggled writes at once.
            settings_.set(control.id==InvertLook?GameSetting::ControlInvertLook:GameSetting::ControlSouthPaw,control.checked);
            return {MenuAction::SettingsChanged};
        default:break;
        }
        return {};
    }
    if(control.kind==MenuControlKind::Slider)return {};
    const int id=control.id;
    switch(frame.scene){
    case MenuScene::MainMenu:
        if(id==PlayGame)push(MenuScene::LoadOrJoin);
        else if(id==HelpAndOptionsButton)push(MenuScene::HelpAndOptions);
        // No PlayStation Network: the store has no offers, leaderboards are unavailable.
        else if(id==UnlockOrDLC)requestMessage(ids("IDS_DOWNLOADABLECONTENT"),ids("IDS_NO_DLCOFFERS"),{ids("IDS_CONFIRM_OK")},[](int){return MenuEvent{};});
        else notice(ids("IDS_LEADERBOARDS"),"Leaderboards use the PlayStation Network, which this port does not connect to.");
        return {};
    case MenuScene::LoadOrJoin:
        if(id==CreateNewWorld){push(MenuScene::CreateWorld);return {};}
        if(id==PlayTutorial)return {MenuAction::StartTutorial};
        if(id==ClassicTutorial)return {MenuAction::StartClassicTutorial};
        return {MenuAction::LoadSave,id};
    case MenuScene::CreateWorld:
        switch(id){
        case EditWorldName:return {MenuAction::EditWorldName};
        case EditSeed:return {MenuAction::EditSeed};
        case GameModeToggle:
            newWorld_.survival=!newWorld_.survival;
            control.label=text(newWorld_.survival?"IDS_GAMEMODE_SURVIVAL":"IDS_GAMEMODE_CREATIVE");
            return {};
        case MoreOptionsButton:push(MenuScene::MoreOptions);return {};
        case NewWorld:
            // IUIScene_StartGame: creative mode or host privileges ask first.
            if(!newWorld_.survival || newWorld_.hostPrivileges){
                requestMessage(ids("IDS_TITLE_START_GAME"),ids(!newWorld_.survival?"IDS_CONFIRM_START_CREATIVE":"IDS_CONFIRM_START_HOST_PRIVILEGES"),
                               {ids("IDS_CONFIRM_OK"),ids("IDS_CONFIRM_CANCEL")},
                               [](int choice){return choice==0?MenuEvent{MenuAction::CreateWorld}:MenuEvent{};});
                return {};
            }
            return {MenuAction::CreateWorld};
        default:return {};
        }
    case MenuScene::HelpAndOptions:
        switch(id){
        case ChangeSkin:
            notice(ids("IDS_CHANGE_SKIN"),"The skin packs are not part of the supplied files, so the default skin is used.");
            return {};
        case HowToPlayButton:push(MenuScene::HowToPlayMenu);return {};
        case ControlsButton:push(MenuScene::Controls);return {};
        case SettingsButton:push(MenuScene::Settings);return {};
        case CreditsButton:push(MenuScene::Credits);return {};
        default:return {};
        }
    case MenuScene::HowToPlayMenu:
        page_=kHowToPlayButtonPage[id];
        push(MenuScene::HowToPlay);
        return {};
    case MenuScene::Controls:
        // UIScene_ControlsMenu::handlePress: choose the layout.
        settings_.set(GameSetting::ControlScheme,static_cast<unsigned char>(id));
        layoutPreview_=id;
        return {MenuAction::SettingsChanged};
    case MenuScene::Settings:
        switch(id){
        case AllOptions:push(MenuScene::SettingsOptions);return {};
        case AllAudio:push(MenuScene::SettingsAudio);return {};
        case AllControl:push(MenuScene::SettingsControl);return {};
        case AllGraphics:push(MenuScene::SettingsGraphics);return {};
        case AllUI:push(MenuScene::SettingsUI);return {};
        case ResetToDefaults:
            // UIScene_SettingsMenu::ResetDefaultsDialogReturned: the OK button
            // (EMessage_ResultDecline, the second button) resets.
            requestMessage(ids("IDS_DEFAULTS_TITLE"),ids("IDS_DEFAULTS_TEXT"),{ids("IDS_CONFIRM_CANCEL"),ids("IDS_CONFIRM_OK")},
                           [this](int choice){
                               if(choice!=1)return MenuEvent{};
                               settings_.setDefaults(!context_.inGame);
                               return MenuEvent{MenuAction::SettingsChanged};
                           });
            return {};
        default:return {};
        }
    case MenuScene::Pause:
        switch(id){
        case ResumeGame:stack_.clear();return {MenuAction::ResumeGame};
        case PauseHelpAndOptions:push(MenuScene::HelpAndOptions);return {};
        case PauseLeaderboards:
            notice(ids("IDS_LEADERBOARDS"),"Leaderboards use the PlayStation Network, which this port does not connect to.");
            return {};
        case SaveGame:
            // PerformActionSaveGame: the save exists, so confirm overwriting it.
            requestMessage(ids("IDS_TITLE_SAVE_GAME"),ids("IDS_CONFIRM_SAVE_GAME"),{ids("IDS_CONFIRM_CANCEL"),ids("IDS_CONFIRM_OK")},
                           [](int choice){return choice==1?MenuEvent{MenuAction::SaveGame}:MenuEvent{};});
            return {};
        case ExitGame:
            // The host's exit: Cancel / Exit and save / Exit without saving.
            requestMessage(ids("IDS_EXIT_GAME"),ids("IDS_CONFIRM_EXIT_GAME"),
                           {ids("IDS_CONFIRM_CANCEL"),ids("IDS_EXIT_GAME_SAVE"),ids("IDS_EXIT_GAME_NO_SAVE")},
                           [](int choice){
                               return choice==1?MenuEvent{MenuAction::ExitAndSave}:
                                      choice==2?MenuEvent{MenuAction::ExitWithoutSaving}:MenuEvent{};
                           });
            return {};
        default:return {};
        }
    case MenuScene::Death:
        if(id==Respawn){stack_.clear();return {MenuAction::Respawn};}
        requestMessage(ids("IDS_EXIT_GAME"),ids("IDS_CONFIRM_EXIT_GAME"),
                       {ids("IDS_CONFIRM_CANCEL"),ids("IDS_EXIT_GAME_SAVE"),ids("IDS_EXIT_GAME_NO_SAVE")},
                       [](int choice){
                           return choice==1?MenuEvent{MenuAction::ExitAndSave}:
                                  choice==2?MenuEvent{MenuAction::ExitWithoutSaving}:MenuEvent{};
                       });
        return {};
    case MenuScene::MessageBox:{
        auto result=frame.onButton;
        stack_.pop_back();
        return result?result(id):MenuEvent{};
    }
    default:return {};
    }
}

MenuEvent ConsoleMenus::input(MenuInput input){
    if(stack_.empty())return {};
    auto& frame=top();
    const int count=int(frame.controls.size());
    switch(input){
    case MenuInput::Back:return back();
    case MenuInput::Up:case MenuInput::Down:{
        if(frame.scene==MenuScene::HowToPlay || count==0)return {};
        const int step=input==MenuInput::Up?-1:1;
        int next=frame.focus;
        for(int i=0;i<count;++i){
            next=(next+step+count)%count;
            if(frame.controls[next].enabled){setFocus(next);break;}
        }
        return {};
    }
    case MenuInput::Left:case MenuInput::Right:{
        if(frame.scene==MenuScene::Controls && frame.focus<=Layout2){
            // The layout buttons sit in a row.
            setFocus(std::clamp(frame.focus+(input==MenuInput::Left?-1:1),int(Layout0),int(Layout2)));
            return {};
        }
        if(count==0)return {};
        auto& control=frame.controls[frame.focus];
        if(control.kind!=MenuControlKind::Slider)return {};
        const int value=std::clamp(control.value+(input==MenuInput::Left?-1:1),control.min,control.max);
        if(value==control.value)return {};
        control.value=value;
        sliderLabel(frame.scene,control);
        // handleSliderMove writes the setting at once.
        switch(frame.scene){
        case MenuScene::CreateWorld:case MenuScene::SettingsOptions:
            if(frame.scene==MenuScene::SettingsOptions && control.id==Autosave)settings_.set(GameSetting::Autosave,value);
            else{settings_.set(GameSetting::Difficulty,value);control.description=frame.scene==MenuScene::SettingsOptions?ids(kDifficultyText[value]):-1;}
            break;
        case MenuScene::SettingsAudio:settings_.set(control.id==Music?GameSetting::MusicVolume:GameSetting::SoundFXVolume,value);break;
        case MenuScene::SettingsControl:
            settings_.set(control.id==SensitivityInGame?GameSetting::SensitivityInGame:GameSetting::SensitivityInMenu,value);break;
        case MenuScene::SettingsGraphics:settings_.set(control.id==Gamma?GameSetting::Gamma:GameSetting::InterfaceOpacity,value);break;
        case MenuScene::SettingsUI:
            settings_.set(control.id==UISize?GameSetting::UISize:GameSetting::UISizeSplitscreen,value-1);break;
        default:break;
        }
        return {MenuAction::SettingsChanged};
    }
    case MenuInput::Accept:
        if(frame.scene==MenuScene::HowToPlay){
            if(page_+1<kHowToPlayPageCount)++page_;
            return {};
        }
        if(frame.scene==MenuScene::Credits || count==0)return {};
        return press(frame.focus);
    case MenuInput::X:
        if(frame.scene==MenuScene::HowToPlay && page_>0)--page_;
        return {};
    case MenuInput::Y:
        return {};
    }
    return {};
}
}
