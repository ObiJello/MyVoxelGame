#include "BlockPlacement.h"
#include "Facing.h"
#include <iostream>
#include <limits>
#include <stdexcept>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
 using namespace console;constexpr double pi=3.14159265358979323846;
 const int directions[]={Facing::SOUTH,Facing::WEST,Facing::NORTH,Facing::EAST};
 for(int turn=-4;turn<=4;++turn)for(int quadrant=0;quadrant<4;++quadrant){
  double yaw=turn*2*pi+quadrant*pi/2;
  require(consolePlacementFacing(10,80,10,{15,80,15},yaw)==directions[quadrant],"Native heading maps to original facing across wrapped rotations");
 }
 require(consolePlacementFacing(10,80,10,{10.5,81,10.5},0)==Facing::UP,"Nearby player above uses vertical axis");
 require(consolePlacementFacing(10,80,10,{10.5,78,10.5},0)==Facing::DOWN,"Nearby player below uses vertical axis");
 require(consolePlacementFacing(10,80,10,{12,90,10.5},0)==Facing::SOUTH,"Exactly two blocks away excludes vertical override");
 require(consolePlacementFacing(10,80,10,{10.5,80.17,10.5},0)==Facing::SOUTH,"Below upper height threshold keeps horizontal facing");
 require(consolePlacementFacing(10,80,10,{10.5,80.19,10.5},0)==Facing::UP,"Above upper height threshold uses vertical facing");
 for(int d=0;d<16;++d)for(int f=0;f<6;++f){
  int axis=f<2?0:(f==Facing::WEST || f==Facing::EAST)?4:8;
  require(consoleLogPlacementData(d,f)==((d&3)|axis),"Placed logs preserve species and replace previous orientation bits");
 }
 bool rejected=false;try{consolePlacementFacing(0,0,0,{},std::numeric_limits<double>::infinity());}catch(const std::invalid_argument&){rejected=true;}
 require(rejected,"Nonfinite headings rejected before conversion");
 World world;
 require(world.placeBlock(10,180,10,Log,2,{15,180,15},pi/2),"Horizontal birch log placement succeeds");
 require(world.getData(10,180,10)==6 && textureTile(Log,2,world.getData(10,180,10))==21,"Placed log metadata drives horizontal end texture");
 require(world.placeBlock(20,180,20,Log,3,{20.5,182.1,20.5},0) && world.getData(20,180,20)==3,"Standing above places vertical jungle log");
 world.set(30,180,30,Water);world.setData(30,180,30,7);
 require(!world.placeBlock(30,180,30,Log,0,{30.5,180,30.5},0),"Player collision rejects placement");
 require(world.get(30,180,30)==Water && world.getData(30,180,30)==7,"Rejected placement restores water depth metadata");
 require(!world.placeBlock(10,180,10,Stone,0,{15,180,15},0) && world.getData(10,180,10)==6,"Occupied block remains unchanged");
 require(!world.placeBlock(-1,180,10,Log,0,{15,180,15},0),"Outside placement rejected");
 for(int direction=0;direction<4;++direction){
  const int x=40+direction*3;
  require(world.placeBlock(x,180,10,static_cast<Block>(130),0,{60,180,20},direction*pi/2),
          "Source ender chest can be placed from the creative inventory");
  const int expected[]={Facing::SOUTH,Facing::WEST,Facing::NORTH,Facing::EAST};
  require(world.getData(x,180,10)==expected[direction],
          "Ender chest facing follows the player's horizontal heading");
 }
 require(world.placeBlock(60,180,10,static_cast<Block>(130),0,{60.5,183,10.5},0) &&
         world.getData(60,180,10)==Facing::SOUTH,
         "Ender chest facing ignores nearby player's vertical placement override");
 std::cout<<"Original log placement direction, species and collision rollback checks passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
