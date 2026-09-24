#include "MonsterRoomDecoration.h"
#include "WorldGenLevel.h"
#include "Random.h"
#include <array>

namespace console {
namespace {
DungeonItem randomItem(Random& random){
    DungeonItem item;
    switch(random.nextInt(12)){
    case 0:item.id=329;break; // saddle
    case 1:item.id=265;item.count=random.nextInt(4)+1;break;
    case 2:item.id=297;break; // bread
    case 3:item.id=296;item.count=random.nextInt(4)+1;break;
    case 4:item.id=289;item.count=random.nextInt(4)+1;break;
    case 5:item.id=287;item.count=random.nextInt(4)+1;break;
    case 6:item.id=325;break;
    case 7:if(random.nextInt(100)==0)item.id=322;break;
    case 8:if(random.nextInt(2)==0){item.id=331;item.count=random.nextInt(4)+1;}break;
    case 9:if(random.nextInt(10)==0)item.id=2256+random.nextInt(2);break;
    case 10:item.id=351;item.damage=3;break;
    case 11:{
        // Enchantment::staticCtor inserts these into validEnchantments in ID
        // order. The max levels come from the corresponding source subclasses.
        static constexpr std::array<std::pair<int,int>,22> enchants{{
            {0,4},{1,4},{2,4},{3,4},{4,4},{5,3},{6,1},{7,3},
            {16,5},{17,5},{18,5},{19,2},{20,2},{21,3},
            {32,5},{33,1},{34,3},{35,3},{48,5},{49,2},{50,1},{51,1}}};
        const auto [id,maxLevel]=enchants[random.nextInt(int(enchants.size()))];
        item.id=403;item.enchantmentId=id;
        item.enchantmentLevel=1+random.nextInt(maxLevel);
        break;
    }
    }
    if(item.id && !item.count)item.count=1;
    return item;
}
}
bool decorateMonsterRoom(Level& level,Random& random,int x,int y,int z,
                         std::vector<DungeonTile>& tiles){
    constexpr int hr=3;
    const int xr=random.nextInt(2)+2,zr=random.nextInt(2)+2;
    int holes=0;
    for(int xx=x-xr-1;xx<=x+xr+1;++xx)
        for(int yy=y-1;yy<=y+hr+1;++yy)
            for(int zz=z-zr-1;zz<=z+zr+1;++zz){
                auto* material=level.getMaterial(xx,yy,zz);
                if((yy==y-1 || yy==y+hr+1) && !material->isSolid())return false;
                if((xx==x-xr-1 || xx==x+xr+1 || zz==z-zr-1 || zz==z+zr+1) &&
                   yy==y && level.isEmptyTile(xx,yy,zz) && level.isEmptyTile(xx,yy+1,zz))++holes;
            }
    if(holes<1 || holes>5)return false;
    for(int xx=x-xr-1;xx<=x+xr+1;++xx)
        for(int yy=y+hr;yy>=y-1;--yy)
            for(int zz=z-zr-1;zz<=z+zr+1;++zz){
                if(xx==x-xr-1 || yy==y-1 || zz==z-zr-1 ||
                   xx==x+xr+1 || yy==y+hr+1 || zz==z+zr+1){
                    if(yy>=0 && !level.getMaterial(xx,yy-1,zz)->isSolid())level.setTile(xx,yy,zz,0);
                    else if(level.getMaterial(xx,yy,zz)->isSolid()){
                        if(yy==y-1 && random.nextInt(4)!=0)level.setTile(xx,yy,zz,48);
                        else level.setTile(xx,yy,zz,4);
                    }
                }else level.setTile(xx,yy,zz,0);
            }
    for(int cc=0;cc<2;++cc)for(int attempt=0;attempt<3;++attempt){
        const int xc=x+random.nextInt(xr*2+1)-xr,zc=z+random.nextInt(zr*2+1)-zr;
        if(!level.isEmptyTile(xc,y,zc))continue;
        int walls=0;
        walls+=level.getMaterial(xc-1,y,zc)->isSolid();walls+=level.getMaterial(xc+1,y,zc)->isSolid();
        walls+=level.getMaterial(xc,y,zc-1)->isSolid();walls+=level.getMaterial(xc,y,zc+1)->isSolid();
        if(walls!=1)continue;
        level.setTile(xc,y,zc,54);
        DungeonTile chest;chest.x=xc;chest.y=y;chest.z=zc;
        std::array<DungeonItem,27> slots{};
        for(int j=0;j<8;++j){auto item=randomItem(random);if(item.id){item.slot=random.nextInt(27);slots[item.slot]=item;}}
        for(const auto& item:slots)if(item.id)chest.items.push_back(item);
        tiles.push_back(std::move(chest));
        break;
    }
    level.setTile(x,y,z,52);
    DungeonTile spawner;spawner.x=x;spawner.y=y;spawner.z=z;spawner.spawner=true;
    switch(random.nextInt(4)){
    case 0:spawner.entityId=L"Skeleton";break;
    case 1:case 2:spawner.entityId=L"Zombie";break;
    case 3:spawner.entityId=L"Spider";break;
    }
    tiles.push_back(std::move(spawner));
    return true;
}
}
