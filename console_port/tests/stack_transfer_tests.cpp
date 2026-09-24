#include "ContainerItems.h"
#include "CompoundTag.h"
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace console;
static void check(bool b,const char* m){if(!b)throw std::runtime_error(m);}
static void add(CompoundTag& root,int slot,int id,int count,int damage=0,int marker=0){
 if(!root.contains(L"Items"))root.put(L"Items",new TagList());
 auto t=std::make_unique<CompoundTag>();t->putByte(L"Slot",slot);t->putShort(L"id",id);t->putByte(L"Count",count);t->putShort(L"Damage",damage);
 if(marker){auto tag=std::make_unique<CompoundTag>();tag->putInt(L"4jdata",marker);t->put(L"tag",tag.get());tag.release();}
 root.getList(L"Items")->add(t.get());t.release();
}
int main(){try{
 check(consoleItemStackLimit(331)==64 && consoleItemStackLimit(323)==16 && consoleItemStackLimit(325)==16,"Source redstone, sign and empty bucket limits");
 check(consoleItemStackLimit(326)==1 && consoleItemStackLimit(346)==1 && consoleItemStackLimit(403)==1 && consoleItemStackLimit(383)==16,"Filled buckets, rods, books and console spawn eggs");
 CompoundTag source,left,right;add(source,0,331,31);add(right,3,331,60);
 check(moveContainerStacks(source,36,0,{&left,&right},{27,27},0),"Move rounded-up half");
 check(containerItems(source,36)[0].count==15 && containerItems(right,27)[3].count==64 && containerItems(left,27)[0].count==12,"Merge later half before earlier empty slots");
 check(moveContainerStacks(source,36,0,{&left,&right},{27,27},1),"Move single item");
 check(containerItems(source,36)[0].count==14 && containerItems(left,27)[0].count==13,"Single move updates both counts");
 check(moveContainerStacks(source,36,0,{&left,&right},{27,27}),"Move all remainder");
 check(!containerItems(source,36)[0].id && containerItems(left,27)[0].count==27,"Full merge removes empty source slot");
 CompoundTag marked,dest;add(marked,0,331,10,0,7);add(dest,0,331,20,0,8);
 check(moveContainerStack(marked,27,0,dest,36) && containerItems(dest,36)[0].count==20 && containerItems(dest,36)[1].marker==7,"Different objective tags never merge");
 CompoundTag aux,colors;add(aux,0,351,5,6);add(colors,0,351,8,7);check(moveContainerStack(aux,27,0,colors,36) && containerItems(colors,36)[1].damage==6,"Different dye metadata remains separate");
 CompoundTag signs,small;add(signs,0,323,10);add(small,0,323,15);check(moveContainerStack(signs,27,0,small,1),"Partial transfer into full destination");
 check(containerItems(signs,27)[0].count==9 && containerItems(small,1)[0].count==16,"Partial capacity leaves remainder in source");
 check(!moveContainerStack(signs,27,0,small,1),"Full stack cannot overflow");
 CompoundTag odd,one;add(odd,0,4,5);check(moveContainerStack(odd,27,0,one,36,0) && containerItems(odd,27)[0].count==2 && containerItems(one,36)[0].count==3,"Odd half rounds upward");
 CompoundTag rods,rodBag;add(rods,0,346,1);add(rodBag,0,346,1);check(moveContainerStack(rods,27,0,rodBag,36) && containerItems(rodBag,36)[0].count==1 && containerItems(rodBag,36)[1].count==1,"Nonstackable tools stay separate");
 CompoundTag valid,malformed;add(valid,0,4,8);add(malformed,0,4,1);add(malformed,0,4,1);bool rejected=false;
 try{moveContainerStack(valid,27,0,malformed,36);}catch(const std::exception&){rejected=true;}
 check(rejected && containerItems(valid,27)[0].count==8,"Malformed destination leaves source unchanged");
 // The untouched later destination must remain valid when the first one receives everything.
 CompoundTag a,b,c;add(a,0,4,1);check(moveContainerStacks(a,27,0,{&b,&c},{27,27}) && containerItems(c,27).size()==27,"Unused empty destination remains valid");
 std::cout<<"Source stack limits, merges, halves, singles, NBT separation and conservation passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
