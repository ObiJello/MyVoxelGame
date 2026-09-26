// The PS3 front-end menus (UIScene_*) as data: controls, navigation and the
// settings they write.
#include "ConsoleMenus.h"
#include "ConsoleUiAnimation.h"
#include "ConsoleCredits.h"
#include "ConsoleStrings.h"
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <string>

using namespace console;
static void require(bool good,const std::string& message){if(!good)throw std::runtime_error(message);}
static std::string text(const char* name){return consoleString(consoleStringId(name));}

int main(){try{
    // Golden timeline samples from skin.swf and Panorama720.swf (30 fps).
    namespace anim=console_ui_animation;
    require(anim::panoramaFrame(0)==0 && anim::panoramaFrame(1.0/30)==1 &&
            anim::panoramaFrame(4099.0/30)==4099 && anim::panoramaFrame(4100.0/30)==0,
            "Panorama SWF frame wrap");
    require(std::abs(anim::savePanelFadeIn(0)-133.f/256)<.0001f &&
            std::abs(anim::savePanelFadeIn(1.0/30)-141.f/256)<.0001f &&
            std::abs(anim::savePanelFadeIn(18.0/30)-248.f/256)<.0001f &&
            anim::savePanelFadeIn(19.0/30)==1.f,"Recess panel SWF fade frames");
    require(anim::buttonPressed(0) && anim::buttonPressed(9.0/30) &&
            !anim::buttonPressed(10.0/30),"Button SWF press frames");
    require(anim::scrollArrowAlpha(0,false)==1.f &&
            std::abs(anim::scrollArrowAlpha(4.0/30,false)-64.f/256)<.0001f &&
            anim::scrollArrowAlpha(7.0/30,false)==1.f &&
            std::abs(anim::scrollArrowAlpha(4.0/30,true)-64.f/256)<.0001f &&
            anim::scrollArrowAlpha(8.0/30,true)==1.f,"Scroll arrow SWF fade frames");
    GameSettings settings;
    ConsoleMenus menus(settings);

    // UIScene_MainMenu on PS3: no Exit Game or Achievements.
    menus.show(MenuScene::MainMenu,{});
    require(menus.controls().size()==4 && menus.controls()[0].label==text("IDS_PLAY_GAME") &&
            menus.controls()[3].label==text("IDS_DOWNLOADABLECONTENT"),"Main menu buttons");
    require(menus.tooltips()[1]==-1,"The main menu has no Back");
    menus.input(MenuInput::Up);
    require(menus.focus()==3,"Focus wraps");
    menus.input(MenuInput::Accept);
    require(menus.scene()==MenuScene::MessageBox && menus.message()->text==consoleStringId("IDS_NO_DLCOFFERS"),"The store has no offers");
    menus.input(MenuInput::Accept);
    require(menus.scene()==MenuScene::MainMenu,"The message box closes");

    // Play Game -> the world list.
    menus.show(MenuScene::MainMenu,{false,false,{"My World"}});
    menus.input(MenuInput::Accept);
    require(menus.scene()==MenuScene::LoadOrJoin && menus.controls()[1].label==text("IDS_PLAY_TUTORIAL") &&
            menus.controls().back().label=="My World","World list");
    menus.setFocus(1);
    require(menus.input(MenuInput::Accept).action==MenuAction::StartTutorial,"Play Tutorial");
    menus.setFocus(3);
    const auto load=menus.input(MenuInput::Accept);
    require(load.action==MenuAction::LoadSave && load.index==0,"Load a save");

    // Create New World and More Options.
    menus.setFocus(0);menus.input(MenuInput::Accept);
    require(menus.scene()==MenuScene::CreateWorld && menus.newWorld().name==text("IDS_DEFAULT_WORLD_NAME"),"Default world name");
    require(menus.controls()[2].label==text("IDS_GAMEMODE_SURVIVAL"),"Survival by default");
    menus.setFocus(2);menus.input(MenuInput::Accept);
    require(menus.controls()[2].label==text("IDS_GAMEMODE_CREATIVE") && !menus.newWorld().survival,"Game mode toggles");
    menus.setFocus(4);menus.input(MenuInput::Accept);
    require(menus.scene()==MenuScene::MoreOptions && !menus.controls()[0].enabled,"Online options are disabled offline");
    require(menus.focus()==3 && menus.focusedDescription()==consoleStringId("IDS_GAMEOPTION_PVP"),"Focus starts past disabled options");
    menus.input(MenuInput::Up);
    require(menus.focus()==10,"Up from the first enabled option wraps past the disabled ones");
    menus.setFocus(9);menus.input(MenuInput::Accept);
    require(menus.newWorld().superflat,"Superflat checkbox");
    menus.input(MenuInput::Back);
    require(menus.scene()==MenuScene::CreateWorld && menus.newWorld().superflat,"More Options keeps its values");
    menus.setFocus(5);menus.input(MenuInput::Accept);
    require(menus.scene()==MenuScene::MessageBox && menus.message()->text==consoleStringId("IDS_CONFIRM_START_CREATIVE"),"Creative asks first");
    require(menus.input(MenuInput::Accept).action==MenuAction::CreateWorld,"OK creates the world");

    // Help & Options -> Settings -> Options: checkboxes are written on Back.
    menus.show(MenuScene::MainMenu,{});
    menus.setFocus(2);menus.input(MenuInput::Accept);
    require(menus.scene()==MenuScene::HelpAndOptions && menus.controls().size()==5,"Help & Options");
    menus.setFocus(3);menus.input(MenuInput::Accept);
    require(menus.scene()==MenuScene::Settings && menus.controls().size()==6,"Settings");
    menus.setFocus(0);menus.input(MenuInput::Accept);
    require(menus.scene()==MenuScene::SettingsOptions && menus.controls().size()==6,"Options out of game has the difficulty");
    menus.setFocus(1);menus.input(MenuInput::Accept);
    require(settings.get(GameSetting::Hints)==1,"Hints are not written until Back");
    menus.setFocus(4);
    require(menus.controls()[4].label==text("IDS_SLIDER_AUTOSAVE")+": 30 "+text("IDS_MINUTES"),"Autosave label");
    menus.input(MenuInput::Left);menus.input(MenuInput::Left);
    require(settings.get(GameSetting::Autosave)==0 && menus.controls()[4].label==text("IDS_SLIDER_AUTOSAVE_OFF"),"Autosave off");
    menus.setFocus(5);menus.input(MenuInput::Right);
    require(settings.get(GameSetting::Difficulty)==2 && menus.focusedDescription()==consoleStringId("IDS_DIFFICULTY_NORMAL"),"Difficulty");
    require(menus.input(MenuInput::Back).action==MenuAction::SettingsChanged && settings.get(GameSetting::Hints)==0,"Back commits hints");

    // Audio / Control sliders write at once.
    menus.setFocus(1);menus.input(MenuInput::Accept);
    menus.input(MenuInput::Left);
    require(settings.get(GameSetting::MusicVolume)==99 && menus.controls()[0].label==text("IDS_SLIDER_MUSIC")+": 99%","Music slider");
    menus.input(MenuInput::Back);
    menus.setFocus(2);menus.input(MenuInput::Accept);
    menus.input(MenuInput::Right);
    require(settings.get(GameSetting::SensitivityInGame)==101,"Sensitivity reaches 200%");
    menus.input(MenuInput::Back);

    // Reset to Defaults: the second button (OK) resets.
    menus.setFocus(5);menus.input(MenuInput::Accept);
    menus.setFocus(1);menus.input(MenuInput::Accept);
    require(settings.get(GameSetting::MusicVolume)==100 && settings.get(GameSetting::Hints)==1 &&
            settings.get(GameSetting::Difficulty)==1,"Reset to defaults");
    menus.input(MenuInput::Back);

    // Controls: layout buttons, invert and southpaw.
    menus.setFocus(2);menus.input(MenuInput::Accept);
    require(menus.scene()==MenuScene::Controls && menus.focus()==0,"Controls focus the current layout");
    menus.input(MenuInput::Right);
    require(menus.controlsLayout()==1 && settings.get(GameSetting::ControlScheme)==0,"Focus previews a layout");
    menus.input(MenuInput::Accept);
    require(settings.get(GameSetting::ControlScheme)==1,"Pressing chooses it");
    menus.setFocus(4);menus.input(MenuInput::Accept);
    require(settings.get(GameSetting::ControlSouthPaw)==1,"Southpaw");
    menus.input(MenuInput::Back);

    // How To Play pages: A next, X previous.
    menus.setFocus(1);menus.input(MenuInput::Accept);
    require(menus.controls().size()==19,"How To Play topics");
    menus.setFocus(7);menus.input(MenuInput::Accept);
    require(menus.howToPlayText()==consoleStringId("IDS_HOW_TO_PLAY_CRAFTING"),"Crafting page");
    menus.input(MenuInput::Accept);
    require(menus.howToPlayText()==consoleStringId("IDS_HOW_TO_PLAY_CRAFT_TABLE"),"Next page");
    menus.input(MenuInput::X);menus.input(MenuInput::X);
    require(menus.howToPlayText()==consoleStringId("IDS_HOW_TO_PLAY_ENDERCHEST"),"Previous page");

    // Pause: B resumes, Exit Game asks with three buttons.
    menus.show(MenuScene::Pause,{true});
    require(menus.controls().size()==5,"Pause menu");
    require(menus.input(MenuInput::Back).action==MenuAction::ResumeGame && !menus.active(),"B resumes");
    menus.show(MenuScene::Pause,{true});
    menus.setFocus(4);menus.input(MenuInput::Accept);
    require(menus.message() && menus.message()->buttons.size()==3,"Exit asks to save");
    menus.setFocus(2);
    require(menus.input(MenuInput::Accept).action==MenuAction::ExitWithoutSaving,"Exit without saving");
    menus.show(MenuScene::Pause,{true});
    menus.setFocus(1);menus.input(MenuInput::Accept);menus.setFocus(3);menus.input(MenuInput::Accept);menus.setFocus(0);menus.input(MenuInput::Accept);
    require(menus.scene()==MenuScene::SettingsOptions && menus.controls().size()==5,"In game the difficulty is a host option");

    std::size_t credits=0;
    const auto* lines=consoleCredits(credits);
    require(credits>50 && std::string(lines[0].format)=="MOJANG" && lines[0].size==3,"Credits roll");

    std::cout<<"menu tests passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
