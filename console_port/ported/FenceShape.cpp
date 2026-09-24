// Adapted from original FenceTile/FenceGateTile collision and TileRenderer
// fence/gate cuboid layouts. Coordinates remain exact sixteenths.
#include "FenceShape.h"
namespace console {namespace {
bool cubeConnects(const BlockShapeAccess& level,int x,int y,int z,int id){
 const int neighbor=level.getTile(x,y,z);
 if(neighbor==id || neighbor==107)return true;
 // Tutorial-supported full cubes connect. Exclude non-cubes and plants.
 return neighbor>0 && !consoleIsFenceLike(neighbor) && !consoleIsStair(neighbor) && neighbor!=6 && neighbor!=8 && neighbor!=9 &&
  neighbor!=10 && neighbor!=11 && neighbor!=18 && neighbor!=20 && neighbor!=26 && neighbor!=27 &&
  neighbor!=28 && neighbor!=30 && neighbor!=31 && neighbor!=32 && neighbor!=37 && neighbor!=38 &&
  neighbor!=39 && neighbor!=40 && neighbor!=44 && neighbor!=50 && neighbor!=51 && neighbor!=54 && neighbor!=55 &&
  neighbor!=59 && neighbor!=64 && neighbor!=65 && neighbor!=66 && neighbor!=69 && neighbor!=70 &&
  neighbor!=71 && neighbor!=72 && neighbor!=75 && neighbor!=76 && neighbor!=77 && neighbor!=83 &&
  neighbor!=90 && neighbor!=96 && neighbor!=104 && neighbor!=105 && neighbor!=106 && neighbor!=115 &&
  neighbor!=131 && neighbor!=132 && neighbor!=141 && neighbor!=142;
}
void add(BlockShape& s,double x0,double y0,double z0,double x1,double y1,double z1){
 s.boxes.at(s.count++)={x0,y0,z0,x1,y1,z1};
}
}
bool consoleIsFenceLike(int id){return id==85 || id==107 || id==113;}
BlockShape consoleFenceCollisionShape(const BlockShapeAccess& level,int x,int y,int z){
 BlockShape s;const int id=level.getTile(x,y,z);
 if(id==107){
  const int data=level.getData(x,y,z);if(data&4)return s;
  if((data&3)==2 || (data&3)==3)add(s,0,0,6./16,1,1.5,10./16);
  else add(s,6./16,0,0,10./16,1.5,1);
  return s;
 }
 double west=6./16,east=10./16,north=6./16,south=10./16;
 if(cubeConnects(level,x,y,z-1,id))north=0;if(cubeConnects(level,x,y,z+1,id))south=1;
 if(cubeConnects(level,x-1,y,z,id))west=0;if(cubeConnects(level,x+1,y,z,id))east=1;
 add(s,west,0,north,east,1.5,south);return s;
}
BlockShape consoleFenceRenderShape(const BlockShapeAccess& level,int x,int y,int z){
 BlockShape s;const int id=level.getTile(x,y,z);
 if(id==107){
  const int direction=level.getData(x,y,z)&3;const bool open=level.getData(x,y,z)&4;
  const double h00=6./16,h01=9./16,h10=12./16,h11=15./16,h20=5./16,h21=1;
  if(direction==0 || direction==1){
   add(s,7./16,h20,0,9./16,h21,2./16);add(s,7./16,h20,14./16,9./16,h21,1);
   if(open){const double a=direction==0?9./16:1./16,b=direction==0?13./16:3./16,c=direction==0?15./16:7./16;
    add(s,b,h00,0,c,h11,2./16);add(s,b,h00,14./16,c,h11,1);
    add(s,a,h00,0,b,h01,2./16);add(s,a,h00,14./16,b,h01,1);
    add(s,a,h10,0,b,h11,2./16);add(s,a,h10,14./16,b,h11,1);
   }else{
    add(s,7./16,h00,6./16,9./16,h11,10./16);
    add(s,7./16,h00,2./16,9./16,h01,6./16);add(s,7./16,h10,2./16,9./16,h11,6./16);
    add(s,7./16,h00,10./16,9./16,h01,14./16);add(s,7./16,h10,10./16,9./16,h11,14./16);
   }
  }else{
   add(s,0,h20,7./16,2./16,h21,9./16);add(s,14./16,h20,7./16,1,h21,9./16);
   if(open){const double a=direction==3?9./16:1./16,b=direction==3?13./16:3./16,c=direction==3?15./16:7./16;
    add(s,0,h00,b,2./16,h11,c);add(s,14./16,h00,b,1,h11,c);
    add(s,0,h00,a,2./16,h01,b);add(s,14./16,h00,a,1,h01,b);
    add(s,0,h10,a,2./16,h11,b);add(s,14./16,h10,a,1,h11,b);
   }else{
    add(s,6./16,h00,7./16,10./16,h11,9./16);
    add(s,2./16,h00,7./16,6./16,h01,9./16);add(s,2./16,h10,7./16,6./16,h11,9./16);
    add(s,10./16,h00,7./16,14./16,h01,9./16);add(s,10./16,h10,7./16,14./16,h11,9./16);
   }
  }
  return s;
 }
 const bool w=cubeConnects(level,x-1,y,z,id),e=cubeConnects(level,x+1,y,z,id);
 const bool n=cubeConnects(level,x,y,z-1,id),south=cubeConnects(level,x,y,z+1,id);
 bool we=w||e,ns=n||south;if(!we&&!ns)we=true;
 add(s,6./16,0,6./16,10./16,1,10./16);
 const double x0=w?0:7./16,x1=e?1:9./16,z0=n?0:7./16,z1=south?1:9./16;
 if(we){add(s,x0,12./16,7./16,x1,15./16,9./16);add(s,x0,6./16,7./16,x1,9./16,9./16);}
 if(ns){add(s,7./16,12./16,z0,9./16,15./16,z1);add(s,7./16,6./16,z0,9./16,9./16,z1);}
 return s;
}
}
