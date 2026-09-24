#pragma once
#include "LiquidFlow.h"
#include <map>
#include <tuple>
struct LiquidFlowFixture:console::LiquidFlowAccess {
    // kind: 0 air, 1 fluid, 2 wall, 3 ice.
    struct Cell {int kind=0,data=0;};
    std::map<std::tuple<int,int,int>,Cell> cells;
    Cell cell(int x,int y,int z)const{auto i=cells.find({x,y,z});return i==cells.end()?Cell{}:i->second;}
    bool sameLiquid(int x,int y,int z)const override{return cell(x,y,z).kind==1;}
    bool solidMaterial(int x,int y,int z)const override{return cell(x,y,z).kind>=2;}
    bool blocksMotion(int x,int y,int z)const override{return solidMaterial(x,y,z);}
    bool ice(int x,int y,int z)const override{return cell(x,y,z).kind==3;}
    int data(int x,int y,int z)const override{return cell(x,y,z).data;}
};
