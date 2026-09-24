// Survival rules (Tile/Player/Inventory/tool classes) and their World integration.
#include "World.h"
#include "SurvivalRules.h"
#include "TileSurvival.h"
#include "CraftingRecipes.h"
#include "DroppedItemMesh.h"
#include "Random.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
static void require(bool good,const char* message){if(!good)throw std::runtime_error(message);}
static bool near(float a,float b){return std::abs(a-b)<=1e-6f*std::max(1.f,std::abs(b));}
int main(){try{
 using namespace console;
 // ---- Tile::getDestroyProgress / Player::getDestroySpeed / canDestroy ----
 require(near(consoleDestroyProgress(1,0,false),1/1.5f/100),"Stone by hand uses the unharvestable rate");
 require(near(consoleDestroyProgress(1,270,false),2/1.5f/30),"Wooden pickaxe mines stone at tier speed");
 require(near(consoleDestroyProgress(1,278,false),8/1.5f/30),"Diamond pickaxe mines stone at tier speed");
 require(near(consoleDestroyProgress(3,0,false),1/.5f/30),"Dirt by hand");
 require(near(consoleDestroyProgress(3,269,false),2/.5f/30),"Wooden shovel on dirt");
 require(near(consoleDestroyProgress(17,271,false),2/2.f/30),"Hatchet uses wood material speed on logs");
 require(near(consoleDestroyProgress(5,258,false),6/2.f/30),"Hatchet uses wood material speed on planks");
 require(near(consoleDestroyProgress(49,257,false),1/50.f/100),"Iron pickaxe cannot harvest obsidian");
 require(near(consoleDestroyProgress(49,278,false),8/50.f/30),"Diamond pickaxe harvests obsidian");
 require(near(consoleDestroyProgress(56,274,false),1/3.f/100),"Stone pickaxe cannot harvest diamond ore");
 require(near(consoleDestroyProgress(56,257,false),6/3.f/30),"Iron pickaxe harvests diamond ore");
 require(near(consoleDestroyProgress(18,359,false),15/.2f/30),"Shears cut leaves at 15");
 require(near(consoleDestroyProgress(30,267,false),15/4.f/30),"Swords cut webs at 15");
 require(near(consoleDestroyProgress(1,270,true),2/5.f/1.5f/30),"Underwater mining is five times slower");
 require(near(consoleDestroyProgress(1,270,false,1,-1),2*1.4f/1.5f/30),"Haste II adds 40%");
 require(consoleDestroyProgress(7,278,false)==0,"Bedrock is indestructible");
 require(std::isinf(consoleDestroyProgress(37,0,false)),"Flowers break instantly");
 require(consolePlayerCanDestroy(3,0) && !consolePlayerCanDestroy(1,0) && !consolePlayerCanDestroy(78,0) &&
         consolePlayerCanDestroy(78,269) && consolePlayerCanDestroy(30,359) && !consolePlayerCanDestroy(30,0),
         "Material::isAlwaysDestroyable and canDestroySpecial");
 // Every block the client can hold is registered in the extracted table.
 for(int id=1;id<256;++id)if(validBlock(static_cast<std::uint8_t>(id)))
  require(consoleSurvivalTile(id)!=nullptr,"Every client block has survival properties");
 require(consoleSurvivalTile(7)->destroyTime==-1 && consoleSurvivalTile(49)->destroyTime==50 &&
         consoleSurvivalTile(53)->destroyTime==consoleSurvivalTile(5)->destroyTime &&
         consoleSurvivalTile(130)->destroyTime==22.5f,"Extracted destroy times, including stair inheritance");
 // ---- Drops (spawnResources/getResource/getResourceCount) ----
 Random random(42);
 const auto one=[&](int tile,int data,int held){auto drops=consoleTileDrops(tile,data,held,random);
  require(drops.size()==1,"Expected one drop");return drops[0];};
 require(one(1,0,270).id==4,"Stone drops cobblestone");
 require(one(2,0,0).id==3 && one(60,0,0).id==3,"Grass and farmland drop dirt");
 require(one(16,0,270).id==263 && one(56,0,257).id==264,"Ores drop their items");
 require(one(17,2,0).damage==2 && one(35,14,0).damage==14,"Aux values follow getSpawnResourcesAuxValue");
 require(one(18,1,359).id==18 && one(18,1,359).damage==1,"Shears keep the leaf type");
 require(consoleTileDrops(43,3,270,random).size()==2 && one(44,11,270).damage==3,"Slabs drop halves by type");
 require(one(64,0,0).id==324 && consoleTileDrops(64,8,0,random).empty(),"Doors drop from the lower half only");
 require(consoleTileDrops(20,0,0,random).empty() && consoleTileDrops(102,0,0,random).empty(),"Glass drops nothing");
 require(consoleTileDrops(130,0,278,random).size()==8,"Ender chest drops eight obsidian");
 for(int i=0;i<50;++i){const auto lapis=consoleTileDrops(21,0,274,random);
  require(lapis.size()>=4 && lapis.size()<=8 && lapis[0].id==351 && lapis[0].damage==4,"Lapis drops 4-8 blue dye");}
 int saplings=0;for(int i=0;i<4000;++i)for(const auto& drop:consoleTileDrops(18,0,0,random))saplings+=drop.id==6;
 require(saplings>100 && saplings<320,"Leaves drop saplings about one time in twenty");
 require(consoleToolMineDamage(270,1)==1 && consoleToolMineDamage(270,37)==0 &&
         consoleToolMineDamage(267,1)==2 && consoleItemMaxDamage(270)==59 && consoleItemMaxDamage(359)==238,
         "Tool wear and durability");
 require(consoleFood(297)->nutrition==5 && consoleFood(322)->canAlwaysEat && !consoleFood(1),"Food registrations");
 // ---- ItemRenderer / destroy-stage meshes ----
 DroppedItem cube;cube.id=4;cube.position={10,64,10};
 const auto cubeMesh=buildDroppedItemMesh(cube,0,0,0xf000f0);
 require(cubeMesh.terrain.size()==36 && cubeMesh.items.empty(),"A dropped block is a six-faced cube");
 DroppedItem stick;stick.id=280;stick.position={10,64,10};
 const auto stickMesh=buildDroppedItemMesh(stick,0,0,0);
 require(stickMesh.items.size()==6 && stickMesh.terrain.empty(),"A dropped item is one sprite");
 DroppedItem flower;flower.id=37;flower.position={10,64,10};
 require(buildDroppedItemMesh(flower,0,0,0).terrain.size()==6,"Plants drop as terrain sprites");
 const auto crack=buildDestroyStageMesh(1,2,3,5,0);
 require(crack.size()==36 && (near(crack[0].u,5/16.f) || near(crack[0].u,6/16.f)),"Crack uses destroy_5");
 bool crackTile=true;for(const auto& vertex:crack)crackTile&=vertex.v>=15/16.f-1e-6f;
 require(crackTile,"Destroy stages live on the last terrain row");
 // ---- World integration ----
 World world;world.generate(9,true);world.setSurvival(true);
 require(world.survival(),"Survival game type");
 const int x=40,z=40,ground=world.surface(x,z);
 const Vec3 feet{x+.5,double(ground+1),z+1.5};
 world.setPlayerPosition(feet);
 require(world.set(x,ground+1,z,Stone),"Place test stone");
 require(world.setCreativeHotbarItem(0,270),"Hold a wooden pickaxe");
 require(near(world.destroyProgress(x,ground+1,z,0),2/1.5f/30),"World destroy progress uses the held tool");
 require(world.destroyBlock(x,ground+1,z,0) && world.get(x,ground+1,z)==Air,"Survival destroys the block");
 require(world.droppedItems().size()==1 && world.droppedItems()[0].id==4,"Stone pops cobblestone");
 require(world.carriedItems()[0].damage==1,"Pickaxe wears by one");
 for(int i=0;i<30;++i)world.tickTime();
 require(world.droppedItems().size()==1 && world.droppedItems()[0].position.y<ground+1.1,"The drop settles on the ground");
 const auto rest=world.droppedItems()[0].position;
 world.setPlayerPosition({rest.x+1,double(ground+1),rest.z}); // walk over to it
 world.tickTime();
 require(world.droppedItems().empty(),"Dropped cobblestone is picked up");
 world.setPlayerPosition(feet);
 bool cobble=false;for(const auto& item:world.carriedItems())cobble|=item.id==4 && item.count==1;
 require(cobble,"Cobblestone reaches the carried inventory");
 // Placing consumes the carried stack in survival.
 int cobbleSlot=-1;for(int i=0;i<36;++i)if(world.carriedItems()[i].id==4)cobbleSlot=i;
 require(world.consumeCarried(cobbleSlot) && world.carriedItems()[cobbleSlot].id==0,"Placement consumes the item");
 // Hand-mined stone yields nothing.
 require(world.set(x+2,ground+1,z,Stone) && world.setCreativeHotbarItem(1,3),"Stone for hand test");
 require(world.destroyBlock(x+2,ground+1,z,2) && world.droppedItems().empty(),"Unharvestable stone drops nothing");
 // Player::causeFallDamage.
 world.playerLanded(3);require(world.playerHealth()==20,"Three blocks is safe");
 world.playerLanded(10);require(world.playerHealth()==13,"Ten blocks deals seven damage");
 for(int i=0;i<25;++i)world.tickTime();
 // Drowning: water at head height empties the air supply, then hurts.
 const int healthBefore=world.playerHealth();
 require(world.set(x,ground+2,z+1,Water),"Water at head height");
 for(int i=0;i<320 && world.playerAir()>0;++i)world.tickTime();
 require(world.playerAir()<=0,"Air runs out under water");
 const int breathless=world.playerHealth();
 for(int i=0;i<21;++i)world.tickTime();
 require(world.playerHealth()<=breathless-1 && healthBefore>0,"Drowning hurts");
 require(world.set(x,ground+2,z+1,Air),"Remove the water");
 for(int i=0;i<5;++i)world.tickTime();
 require(world.playerAir()==300,"Air refills out of water");
 // Hunger and eating.
 world.playerWalked(5000,true,false);
 for(int i=0;i<40;++i)world.tickTime();
 const int hungry=world.playerFoodLevel();
 require(hungry<20,"Exhaustion lowers food");
 require(world.setCreativeHotbarItem(3,297,0,2) && world.canEatCarried(3),"Bread is edible when hungry");
 require(world.eatCarried(3) && world.playerFoodLevel()==std::min(20,hungry+5) && world.carriedItems()[3].count==1,
         "Eating bread restores five food");
 // Crafting from carried ingredients.
 require(world.setCreativeHotbarItem(4,17,0,2),"Carry two oak logs");
 const auto& recipes=consoleCraftingRecipes();
 const CraftingRecipe* planks=nullptr;const CraftingRecipe* table=nullptr;
 for(const auto& recipe:recipes){if(recipe.id==5 && recipe.damage==0)planks=&recipe;if(recipe.id==58)table=&recipe;}
 require(planks && table && world.canCraft(*planks) && !world.canCraft(*table),"Recipe availability");
 require(world.craft(*planks) && world.craft(*planks),"Craft planks twice");
 require(world.canCraft(*table) && world.craft(*table),"Craft a crafting table");
 int tables=0,plankCount=0;for(const auto& item:world.carriedItems()){tables+=item.id==58?item.count:0;plankCount+=item.id==5?item.count:0;}
 require(tables==1 && plankCount==4,"Crafting consumed and produced the right counts");
 // Death drops the inventory and respawn resets vitals.
 const int carried=[&]{int n=0;for(const auto& item:world.carriedItems())n+=item.id!=0;return n;}();
 for(int i=0;i<40 && !world.playerDead();++i){world.hurtPlayer(20);world.tickTime();}
 require(world.playerDead(),"Player dies");
 world.tickTime();
 bool emptied=true;for(const auto& item:world.carriedItems())emptied&=item.id==0;
 require(emptied && int(world.droppedItems().size())==carried,"Death drops every carried stack");
 world.respawnPlayer();
 require(world.playerHealth()==20 && world.playerFoodLevel()==20 && world.playerAir()==300,"Respawn resets vitals");
 // Save/load keeps the game type, player position and dropped items.
 const auto path=std::filesystem::temp_directory_path()/("console-survival-"+std::to_string(getpid())+".inner");
 struct Cleanup{std::filesystem::path file;~Cleanup(){std::error_code error;std::filesystem::remove(file,error);}} cleanup{path};
 world.setPlayerPosition({feet.x+30,feet.y,feet.z+30}); // stay away from the drops
 const auto drops=world.droppedItems().size();
 world.save(path);
 World loaded;require(loaded.load(path),"Survival world reloads");
 require(loaded.survival(),"Survival game type survives save/load");
 require(loaded.savedPlayerPosition() && std::abs(loaded.savedPlayerPosition()->x-(feet.x+30))<1e-9,"Player position survives");
 require(loaded.droppedItems().size()==drops,"Dropped items survive save/load");
 std::cout<<"Survival rules and world integration passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
