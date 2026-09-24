#pragma once
#include "GameSettings.h"
#include <array>
#include <functional>
#include <string>
#include <vector>

namespace console {
// The PS3 front-end and pause menus (Common/UI UIScene_*), as data: which
// controls each scene has (from its constructor, with the PS3, single player,
// full version and offline branches), what they do, and which settings they
// write (app.SetGameSettings). Rendering lives in the game.
enum class MenuScene {
    MainMenu,LoadOrJoin,CreateWorld,MoreOptions,HelpAndOptions,HowToPlayMenu,HowToPlay,Controls,
    Settings,SettingsOptions,SettingsAudio,SettingsControl,SettingsGraphics,SettingsUI,Credits,
    Pause,Death,MessageBox
};

enum class MenuControlKind { Button,Checkbox,Slider,TextField,ListItem };

struct MenuControl {
    MenuControlKind kind=MenuControlKind::Button;
    int id=0;
    std::string label;        // button/checkbox text, the slider's full label, a text field's title
    std::string text;         // text field contents (empty shows `placeholder`)
    std::string placeholder;
    bool checked=false;
    int value=0,min=0,max=0;
    bool enabled=true;
    int description=-1;       // string shown for the focused control (More Options, Settings)
};

// The parameters the Create World scene and More Options collect
// (LaunchMoreOptionsMenuInitData). Online options are off: there is no PSN.
struct NewWorldOptions {
    std::string name,seed;
    bool survival=true;
    bool onlineGame=false,inviteOnly=false,friendsOfFriends=false;
    bool pvp=true,trustPlayers=true,fireSpreads=true,tnt=true,hostPrivileges=false,resetNether=false;
    bool structures=true,superflat=false,bonusChest=false;
};

enum class MenuAction {
    None,StartTutorial,StartClassicTutorial,CreateWorld,LoadSave,ResumeGame,SaveGame,
    ExitAndSave,ExitWithoutSaving,Respawn,EditWorldName,EditSeed,SettingsChanged
};
struct MenuEvent { MenuAction action=MenuAction::None; int index=-1; };

// What the menus need to know about the game.
struct MenuContext {
    bool inGame=false;
    bool creative=false;      // Controls menu: jump/fly and sneak/fly labels
    std::vector<std::string> saves;
};

// Menu navigation in terms of the ACTION_MENU_* actions.
enum class MenuInput { Up,Down,Left,Right,Accept,Back,X,Y };

class ConsoleMenus {
public:
    explicit ConsoleMenus(GameSettings& settings);

    // Replace the stack with a root scene (the main menu, pause or death).
    void show(MenuScene root,const MenuContext& context);
    void close(){stack_.clear();}
    bool active()const{return !stack_.empty();}
    MenuScene scene()const;
    // The scene the stack was opened with (main menu, pause or death).
    MenuScene root()const{return stack_.empty()?MenuScene::MainMenu:stack_.front().scene;}
    // Scenes below the top, bottom first (the message box's owner).
    MenuScene below()const;
    const std::vector<MenuControl>& controls()const;
    int focus()const;
    void setFocus(int index);
    const MenuContext& context()const{return context_;}
    NewWorldOptions& newWorld(){return newWorld_;}

    MenuEvent input(MenuInput input);
    // A click on a control: focus it and press it.
    MenuEvent click(int index);

    // IDS_* of the text shown with the focused control (-1 none).
    int focusedDescription()const;
    // Scene title (IDS_*), -1 when the scene has none.
    int title()const;
    // Tooltips (ui.SetTooltips): IDS_* for A, B and X (-1 hides one).
    std::array<int,3> tooltips()const;

    // Message box (UIController::RequestMessageBox).
    // `text` is an IDS_*; `note` replaces it for the port's own notices.
    struct Message { int title=-1,text=-1; std::vector<int> buttons; std::string note; };
    const Message* message()const;

    // How To Play: the page (IDS_* of its text) and its index.
    int howToPlayText()const;
    int howToPlayPage()const{return page_;}
    // Credits scroll, advanced by the game in UI units.
    float creditsScroll=0;

    // Controls menu: the currently shown layout (focus on 1-3 previews it).
    int controlsLayout()const{return layoutPreview_;}

    // Refresh labels after a setting changed outside the menus.
    void refresh();
private:
    struct Frame {
        MenuScene scene;
        std::vector<MenuControl> controls;
        int focus=0;
        Message message;
        std::function<MenuEvent(int)> onButton; // message box result
    };
    GameSettings& settings_;
    MenuContext context_;
    NewWorldOptions newWorld_;
    std::vector<Frame> stack_;
    int page_=0,layoutPreview_=0;
    Frame& top(){return stack_.back();}
    const Frame& top()const{return stack_.back();}
    void push(MenuScene scene);
    std::vector<MenuControl> build(MenuScene scene)const;
    MenuEvent press(int index);
    MenuEvent back();
    void commit(const Frame& frame);
    void requestMessage(int title,int text,std::vector<int> buttons,std::function<MenuEvent(int)> result,std::string note={});
    void notice(int title,std::string note);
    void sliderLabel(MenuScene scene,MenuControl& control)const;
};
}
