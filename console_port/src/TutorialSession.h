#pragma once
#include "TutorialEngine.h"
#include <array>
#include <cstdint>
#include <memory>

class FullTutorial;

namespace console {
// InputManager.GetGameJoypadMaps: the PS3 (_360_JOY_BUTTON_*) buttons bound to
// an EControllerActions value by DefineActions (circle/cross not swapped).
unsigned consoleJoypadButtons(int action);

// The console tutorial game mode: FullTutorialMode/TutorialMode
// (Common/Tutorial) and the engine call sites that feed the tutorial
// (Minecraft.cpp look-at and hotbar, LocalPlayer::onCrafted/handleCollectItem,
// MultiplayerLocalPlayer riding/effects/completeUsingItem/move, the UIScene
// menus' changeTutorialState, IUIScene_CraftingMenu and ClientConnection's
// food-bar check). The tutorial itself is the original FullTutorial compiled
// from ported/tutorial.
//
// State and hint IDs are eTutorial_State / eTutorial_Hint (TutorialEnum.h);
// actions are EControllerActions (App_enums.h), mirrored below.
class TutorialSession {
public:
    // EControllerActions values the game produces.
    enum Action {
        MenuA=0,MenuB,MenuX,MenuY,MenuUp,MenuDown,MenuRight,MenuLeft,MenuPageUp,MenuPageDown,
        MenuRightScroll,MenuLeftScroll,MenuStickPress,MenuOtherStickPress,MenuOtherStickUp,
        MenuOtherStickDown,MenuOtherStickLeft,MenuOtherStickRight,MenuPauseMenu,MenuOk,MenuCancel,
        Jump,Forward,Backward,Left,Right,LookLeft,LookRight,LookUp,LookDown,Use,ActionButton,
        LeftScroll,RightScroll,InventoryAction,PauseMenu,Drop,SneakToggle,CraftingAction,RenderThirdPerson,
        GameInfo,DpadLeft,DpadRight,DpadUp,DpadDown,ActionCount
    };
    // eTutorial_State values the game switches to.
    enum State {
        Gameplay=0,InventoryMenu,Crafting2x2Menu,Crafting3x3Menu,FurnaceMenu,RidingMinecart,
        RidingBoat,Fishing,Bed,ContainerMenu,TrapMenu,RedstoneAndPiston,Portal,
        CreativeInventoryMenu,FoodBar,CreativeMode,Brewing,BrewingMenu,Enchanting,EnchantingMenu,
        Farming,Breeding,Golem,Trading,TradingMenu,Anvil,AnvilMenu,Enderchests
    };
    static constexpr std::size_t ProfileBytes=64; // TUTORIAL_PROFILE_STORAGE_BYTES

    // `profile` is the saved tutorial completion bits (GAME_SETTINGS
    // ucTutorialCompletion); read it back with profile() after ticks.
    TutorialSession(TutorialEngine& engine,const std::array<std::uint8_t,ProfileBytes>& profile);
    ~TutorialSession();
    TutorialSession(const TutorialSession&)=delete;
    TutorialSession& operator=(const TutorialSession&)=delete;

    // TutorialMode::tick (runs Tutorial::tick until everything is complete).
    void tick();
    // FullTutorialMode::isTutorial: false once the gameplay track is done.
    bool isTutorial()const;
    bool allTutorialsComplete()const;
    int currentState()const;
    std::array<std::uint8_t,ProfileBytes> profile()const;
    bool profileChanged();

    // TutorialMode::startDestroyBlock / destroyBlock. damageBefore/After are
    // the held item's damage around the block break.
    void startDestroyBlock(int heldId,int heldAux,int heldCount,int tileId);
    void destroyBlock(int tileId,int heldId,int damageBefore,int damageAfter);
    // TutorialMode::useItemOn for a real use: call useItemOnBefore before
    // MultiPlayerGameMode::useItemOn runs (UseTileTask reads the tile there)
    // and useItemOnAfter with its result and the held count before/after.
    void useItemOnBefore(int heldId,int heldAux,int heldCount,int x,int y,int z);
    void useItemOnAfter(int heldId,int heldAux,int heldCountBefore,int heldCountAfter,bool used);
    // TutorialMode::attack (DiggerItemHint only cares whether it is a Mob).
    void attack(int heldId,int heldAux,bool targetIsMob);
    // MultiplayerLocalPlayer::completeUsingItem (eating, drinking).
    void completeUsingItem(int id,int aux);
    // LocalPlayer::handleCollectItem: an inventory slot now holds `id`;
    // counts cover the 36 main inventory slots.
    void onTake(int id,int aux,int count,unsigned countAnyAux,unsigned countThisAux);
    // ItemInstance::onCraftedBy / IUIScene_CraftingMenu.
    void onCrafted(int id,int aux,int count);
    void createItemSelected(int id,int aux,bool canMake);
    // Minecraft hotbar scroll (id 0 = empty hand).
    void onSelectedItemChanged(int id,int aux);
    void onLookAt(int tileId,int data);
    void onLookAtEntity(int instanceOfType);
    void onEffectChanged(int effectId,bool removed);
    bool canMoveToPosition(double xo,double yo,double zo,double xt,double yt,double zt);
    bool isInputAllowed(int action);
    // IUIScene_AbstractContainerMenu/CraftingMenu::handleKeyDown: tells the
    // tutorial about a menu key and returns false when the key is blocked
    // (the popup is visible and the key is constrained).
    bool menuInput(int action,bool popupVisible);
    // Riding changes (MultiplayerLocalPlayer::ride).
    void startRiding(bool minecart);
    void stopRiding();
    // ClientConnection::handleSetHealth: food < FoodConstants::HEAL_LEVEL - 1.
    void hungry();
    // UIScene_*Menu constructors and destructors: remember the previous state,
    // switch to the menu state with the scene, and restore it on close.
    void openMenu(int menuState,bool craftingScene=false);
    void closeMenu();
    void showTutorialPopup(bool show);
private:
    struct Scenes;
    TutorialEngine& engine_;
    std::unique_ptr<FullTutorial> tutorial_;
    std::unique_ptr<Scenes> scenes_;
    int previousState_=0;
    bool menuOpen_=false;
};
}
