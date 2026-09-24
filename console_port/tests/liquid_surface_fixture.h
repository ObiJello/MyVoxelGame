#pragma once
#include "LiquidSurface.h"
#include <array>
struct LiquidSurfaceFixture:console::LiquidSurfaceAccess {
    std::array<int,4> cells{1,0,0,0},metadata{};
    bool above=false;
    static int index(int x,int z){return (unsigned(x)&1)+2*(unsigned(z)&1);}
    bool sameLiquid(int x,int y,int z)const override{return y==41?above:y==40 && cells[index(x,z)]==1;}
    bool solidMaterial(int x,int,int z)const override{return cells[index(x,z)]==2;}
    int data(int x,int,int z)const override{return metadata[index(x,z)];}
};
