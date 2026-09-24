#include "TutorialSession.h"
// The imported tutorial and its host stand-ins (macro-renamed engine types).
#include "TutorialHost.h"
#include "FullTutorial.h"

#include <cstring>

namespace console {
unsigned consoleJoypadButtons(int action){return InputManager.GetGameJoypadMaps(0,action);}
namespace {
shared_ptr<ItemInstance> stack(int id,int aux,int count=1){
    return id>0?make_shared<ItemInstance>(id,count,aux):nullptr;
}
}

// The UIScene the menus hand to changeTutorialState. XuiCraftingTask
// reinterpret_casts the crafting menu's scene to UIScene_CraftingMenu.
struct TutorialSession::Scenes {
    UIScene menu;
    UIScene_CraftingMenu crafting;
};

TutorialSession::TutorialSession(TutorialEngine& engine,const std::array<std::uint8_t,ProfileBytes>& profile)
    :engine_(engine),scenes_(std::make_unique<Scenes>()){
    static_assert(ProfileBytes==TUTORIAL_PROFILE_STORAGE_BYTES);
    static_assert(ActionCount==MINECRAFT_ACTION_MAX && InventoryAction==MINECRAFT_ACTION_INVENTORY &&
                  MenuCancel==ACTION_MENU_CANCEL && Jump==MINECRAFT_ACTION_JUMP);
    static_assert(Enderchests==e_Tutorial_State_Enderchests && FoodBar==e_Tutorial_State_Food_Bar);
    TutorialHost::setEngine(&engine_);
    GAME_SETTINGS& settings=TutorialHost::profile();
    std::memcpy(settings.ucTutorialCompletion,profile.data(),ProfileBytes);
    settings.uiSpecialTutorialBitmask=0;
    settings.bSettingsChanged=false;
    if(Tutorial::s_completableTasks.empty())Tutorial::staticCtor();
    // FullTutorialMode::FullTutorialMode
    tutorial_.reset(new FullTutorial(0));
}

TutorialSession::~TutorialSession(){
    TutorialHost::setEngine(&engine_);
    tutorial_.reset();
    TutorialHost::setEngine(nullptr);
}

void TutorialSession::tick(){
    TutorialHost::setEngine(&engine_);
    if(!tutorial_->m_allTutorialsComplete)tutorial_->tick();
}

bool TutorialSession::isTutorial()const{return !tutorial_->m_fullTutorialComplete;}
bool TutorialSession::allTutorialsComplete()const{return tutorial_->m_allTutorialsComplete;}
int TutorialSession::currentState()const{return tutorial_->getCurrentState();}

std::array<std::uint8_t,TutorialSession::ProfileBytes> TutorialSession::profile()const{
    std::array<std::uint8_t,ProfileBytes> bytes{};
    std::memcpy(bytes.data(),TutorialHost::profile().ucTutorialCompletion,ProfileBytes);
    return bytes;
}

bool TutorialSession::profileChanged(){
    GAME_SETTINGS& settings=TutorialHost::profile();
    const bool changed=settings.bSettingsChanged;
    settings.bSettingsChanged=false;
    return changed;
}

void TutorialSession::startDestroyBlock(int heldId,int heldAux,int heldCount,int tileId){
    TutorialHost::setEngine(&engine_);
    if(!tutorial_->m_allTutorialsComplete)
        tutorial_->startDestroyBlock(stack(heldId,heldAux,heldCount),Tile::tiles[tileId]);
}

void TutorialSession::destroyBlock(int tileId,int heldId,int damageBefore,int damageAfter){
    TutorialHost::setEngine(&engine_);
    if(tutorial_->m_allTutorialsComplete)return;
    tutorial_->destroyBlock(Tile::tiles[tileId]);
    shared_ptr<ItemInstance> item=stack(heldId,damageAfter);
    if(item!=NULL && item->isDamageableItem()){
        const int max=item->getMaxDamage();
        if(damageAfter>damageBefore && damageAfter>(max/2))tutorial_->itemDamaged(item);
    }
}

void TutorialSession::useItemOnBefore(int heldId,int heldAux,int heldCount,int x,int y,int z){
    TutorialHost::setEngine(&engine_);
    if(!tutorial_->m_allTutorialsComplete)
        tutorial_->useItemOn(Minecraft::GetInstance()->level,stack(heldId,heldAux,heldCount),x,y,z,false);
}

void TutorialSession::useItemOnAfter(int heldId,int heldAux,int heldCountBefore,int heldCountAfter,bool used){
    TutorialHost::setEngine(&engine_);
    if(!tutorial_->m_allTutorialsComplete && used && heldId>0 && heldCountBefore>heldCountAfter)
        tutorial_->useItemOn(stack(heldId,heldAux,heldCountAfter));
}

void TutorialSession::attack(int heldId,int heldAux,bool targetIsMob){
    TutorialHost::setEngine(&engine_);
    if(tutorial_->m_allTutorialsComplete)return;
    auto player=Minecraft::GetInstance()->player;
    player->inventory->selected=stack(heldId,heldAux);
    shared_ptr<Entity> target=targetIsMob?shared_ptr<Entity>(make_shared<Mob>()):make_shared<Entity>();
    tutorial_->attack(player,target);
}

void TutorialSession::completeUsingItem(int id,int aux){
    TutorialHost::setEngine(&engine_);
    if(id>0)tutorial_->completeUsingItem(stack(id,aux));
}

void TutorialSession::onTake(int id,int aux,int count,unsigned countAnyAux,unsigned countThisAux){
    TutorialHost::setEngine(&engine_);
    if(id>0)tutorial_->onTake(stack(id,aux,count),countAnyAux,countThisAux);
}

void TutorialSession::onCrafted(int id,int aux,int count){
    TutorialHost::setEngine(&engine_);
    if(id>0)tutorial_->onCrafted(stack(id,aux,count));
}

void TutorialSession::createItemSelected(int id,int aux,bool canMake){
    TutorialHost::setEngine(&engine_);
    if(id>0)tutorial_->createItemSelected(stack(id,aux),canMake);
}

void TutorialSession::onSelectedItemChanged(int id,int aux){
    TutorialHost::setEngine(&engine_);
    tutorial_->onSelectedItemChanged(stack(id,aux));
}

void TutorialSession::onLookAt(int tileId,int data){
    TutorialHost::setEngine(&engine_);
    tutorial_->onLookAt(tileId,data);
}

void TutorialSession::onLookAtEntity(int instanceOfType){
    TutorialHost::setEngine(&engine_);
    tutorial_->onLookAtEntity(static_cast<eINSTANCEOF>(instanceOfType));
}

void TutorialSession::onEffectChanged(int effectId,bool removed){
    TutorialHost::setEngine(&engine_);
    tutorial_->onEffectChanged(effectId>=0 && effectId<32?MobEffect::effects[effectId]:NULL,removed);
}

bool TutorialSession::canMoveToPosition(double xo,double yo,double zo,double xt,double yt,double zt){
    TutorialHost::setEngine(&engine_);
    return tutorial_->canMoveToPosition(xo,yo,zo,xt,yt,zt);
}

bool TutorialSession::isInputAllowed(int action){
    TutorialHost::setEngine(&engine_);
    return tutorial_->m_allTutorialsComplete || tutorial_->isInputAllowed(action);
}

bool TutorialSession::menuInput(int action,bool popupVisible){
    TutorialHost::setEngine(&engine_);
    tutorial_->handleUIInput(action);
    return !(popupVisible && !tutorial_->isInputAllowed(action));
}

void TutorialSession::startRiding(bool minecart){
    TutorialHost::setEngine(&engine_);
    tutorial_->changeTutorialState(minecart?e_Tutorial_State_Riding_Minecart:e_Tutorial_State_Riding_Boat);
}

void TutorialSession::stopRiding(){
    TutorialHost::setEngine(&engine_);
    tutorial_->changeTutorialState(e_Tutorial_State_Gameplay);
}

void TutorialSession::hungry(){
    TutorialHost::setEngine(&engine_);
    tutorial_->changeTutorialState(e_Tutorial_State_Food_Bar);
}

void TutorialSession::openMenu(int menuState,bool craftingScene){
    TutorialHost::setEngine(&engine_);
    if(menuOpen_)closeMenu();
    previousState_=tutorial_->getCurrentState();
    menuOpen_=true;
    UIScene* scene=craftingScene?static_cast<UIScene*>(&scenes_->crafting):&scenes_->menu;
    tutorial_->changeTutorialState(static_cast<eTutorial_State>(menuState),scene);
}

void TutorialSession::closeMenu(){
    TutorialHost::setEngine(&engine_);
    if(!menuOpen_)return;
    menuOpen_=false;
    tutorial_->changeTutorialState(static_cast<eTutorial_State>(previousState_));
}

void TutorialSession::showTutorialPopup(bool show){
    TutorialHost::setEngine(&engine_);
    tutorial_->showTutorialPopup(show);
}
}
