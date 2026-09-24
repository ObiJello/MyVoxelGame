// RECONSTRUCTED recipe table; see CraftingRecipes.h. Item and tile IDs are
// the registered IDs from Item::staticCtor and Tile::staticCtor.
#include "CraftingRecipes.h"
#include <utility>
namespace console {
namespace {
using I=CraftingIngredient;
constexpr int PLANKS=5,COBBLE=4,STICK=280,IRON=265,GOLD=266,DIAMOND=264,STRING=287;
CraftingRecipe make(int id,int count,int damage,std::vector<I> ingredients,bool table,const char* group){
    return {id,count,damage,std::move(ingredients),table,group};
}
std::vector<CraftingRecipe> build(){
    std::vector<CraftingRecipe> r;
    const auto add=[&](int id,int count,std::vector<I> in,bool table,const char* group,int damage=0){
        r.push_back(make(id,count,damage,std::move(in),table,group));
    };
    // Structures
    for(int wood=0;wood<4;++wood)add(PLANKS,4,{{17,wood,1}},false,"Structures",wood);
    add(58,1,{{PLANKS,-1,4}},false,"Structures");                       // crafting table
    add(61,1,{{COBBLE,-1,8}},true,"Structures");                        // furnace
    add(54,1,{{PLANKS,-1,8}},true,"Structures");                        // chest
    add(65,3,{{STICK,-1,7}},true,"Structures");                         // ladder
    add(85,2,{{STICK,-1,6}},true,"Structures");                         // fence
    add(107,1,{{STICK,-1,4},{PLANKS,-1,2}},true,"Structures");          // fence gate
    add(324,1,{{PLANKS,-1,6}},true,"Structures");                       // wooden door
    add(330,1,{{IRON,-1,6}},true,"Structures");                         // iron door
    add(98,4,{{1,-1,4}},false,"Structures");                            // stone bricks
    add(24,1,{{12,-1,4}},false,"Structures");                           // sandstone
    add(45,1,{{336,-1,4}},false,"Structures");                          // bricks
    add(112,1,{{405,-1,4}},false,"Structures");                         // nether brick
    add(82,1,{{337,-1,4}},false,"Structures");                          // clay block
    add(80,1,{{332,-1,4}},false,"Structures");                          // snow block
    add(89,1,{{348,-1,4}},false,"Structures");                          // glowstone
    add(35,1,{{STRING,-1,4}},false,"Structures");                       // wool
    add(102,16,{{20,-1,6}},true,"Structures");                          // glass pane
    add(101,16,{{IRON,-1,6}},true,"Structures");                        // iron bars
    add(53,4,{{PLANKS,-1,6}},true,"Structures");                        // stairs
    add(67,4,{{COBBLE,-1,6}},true,"Structures");
    add(108,4,{{45,-1,6}},true,"Structures");
    add(109,4,{{98,-1,6}},true,"Structures");
    add(128,4,{{24,-1,6}},true,"Structures");
    add(114,4,{{112,-1,6}},true,"Structures");
    add(44,6,{{1,-1,3}},true,"Structures",0);                           // slabs
    add(44,6,{{24,-1,3}},true,"Structures",1);
    add(126,6,{{PLANKS,-1,3}},true,"Structures",0);
    add(44,6,{{COBBLE,-1,3}},true,"Structures",3);
    add(44,6,{{45,-1,3}},true,"Structures",4);
    add(44,6,{{98,-1,3}},true,"Structures",5);
    add(42,1,{{IRON,-1,9}},true,"Structures");                          // storage blocks
    add(41,1,{{GOLD,-1,9}},true,"Structures");
    add(57,1,{{DIAMOND,-1,9}},true,"Structures");
    add(22,1,{{351,4,9}},true,"Structures");
    add(133,1,{{388,-1,9}},true,"Structures");
    add(103,1,{{360,-1,9}},true,"Structures");                          // melon block
    add(47,1,{{PLANKS,-1,6},{340,-1,3}},true,"Structures");            // bookshelf
    add(46,1,{{289,-1,5},{12,-1,4}},true,"Structures");                // TNT
    // Tools and weapons
    const int materials[5]{PLANKS,COBBLE,IRON,DIAMOND,GOLD};
    const int swords[5]{268,272,267,276,283},shovels[5]{269,273,256,277,284};
    const int picks[5]{270,274,257,278,285},axes[5]{271,275,258,279,286},hoes[5]{290,291,292,293,294};
    for(int m=0;m<5;++m){
        add(picks[m],1,{{materials[m],-1,3},{STICK,-1,2}},true,"Tools");
        add(shovels[m],1,{{materials[m],-1,1},{STICK,-1,2}},true,"Tools");
        add(axes[m],1,{{materials[m],-1,3},{STICK,-1,2}},true,"Tools");
        add(hoes[m],1,{{materials[m],-1,2},{STICK,-1,2}},true,"Tools");
        add(swords[m],1,{{materials[m],-1,2},{STICK,-1,1}},true,"Tools");
    }
    add(STICK,4,{{PLANKS,-1,2}},false,"Tools");
    add(50,4,{{STICK,-1,1},{263,-1,1}},false,"Tools");                  // torch (coal or charcoal)
    add(261,1,{{STICK,-1,3},{STRING,-1,3}},true,"Tools");               // bow
    add(262,4,{{318,-1,1},{STICK,-1,1},{288,-1,1}},true,"Tools");      // arrows
    add(259,1,{{IRON,-1,1},{318,-1,1}},false,"Tools");                  // flint and steel
    add(359,1,{{IRON,-1,2}},false,"Tools");                             // shears
    add(325,1,{{IRON,-1,3}},true,"Tools");                              // bucket
    add(346,1,{{STICK,-1,3},{STRING,-1,2}},true,"Tools");               // fishing rod
    add(345,1,{{IRON,-1,4},{331,-1,1}},true,"Tools");                   // compass
    add(347,1,{{GOLD,-1,4},{331,-1,1}},true,"Tools");                   // clock
    // Food
    add(297,1,{{296,-1,3}},true,"Food");                                // bread
    add(281,4,{{PLANKS,-1,3}},true,"Food");                             // bowl
    add(282,1,{{281,-1,1},{39,-1,1},{40,-1,1}},false,"Food");          // mushroom stew
    add(353,1,{{338,-1,1}},false,"Food");                               // sugar
    add(361,4,{{86,-1,1}},false,"Food");                                // pumpkin seeds
    add(362,1,{{360,-1,1}},false,"Food");                               // melon seeds
    // Mechanisms and transport
    add(69,1,{{STICK,-1,1},{COBBLE,-1,1}},false,"Mechanisms");          // lever
    add(77,1,{{1,-1,1}},false,"Mechanisms");                            // stone button
    add(70,1,{{1,-1,2}},false,"Mechanisms");                            // stone pressure plate
    add(72,1,{{PLANKS,-1,2}},false,"Mechanisms");                       // wooden pressure plate
    add(76,1,{{331,-1,1},{STICK,-1,1}},false,"Mechanisms");             // redstone torch
    add(25,1,{{PLANKS,-1,8},{331,-1,1}},true,"Mechanisms");             // note block
    add(84,1,{{PLANKS,-1,8},{DIAMOND,-1,1}},true,"Mechanisms");         // jukebox
    add(66,16,{{IRON,-1,6},{STICK,-1,1}},true,"Transport");             // rails
    add(328,1,{{IRON,-1,5}},true,"Transport");                          // minecart
    add(333,1,{{PLANKS,-1,5}},true,"Transport");                        // boat
    // Decoration and miscellaneous
    add(91,1,{{86,-1,1},{50,-1,1}},false,"Decoration");                 // jack o'lantern
    add(355,1,{{35,-1,3},{PLANKS,-1,3}},true,"Decoration");             // bed
    add(323,3,{{PLANKS,-1,6},{STICK,-1,1}},true,"Decoration");          // sign
    add(339,3,{{338,-1,3}},true,"Decoration");                          // paper
    add(340,1,{{339,-1,3}},false,"Decoration");                         // book
    add(116,1,{{340,-1,1},{DIAMOND,-1,2},{49,-1,4}},true,"Decoration"); // enchantment table
    add(379,1,{{369,-1,1},{COBBLE,-1,3}},true,"Decoration");            // brewing stand
    add(380,1,{{IRON,-1,7}},true,"Decoration");                         // cauldron
    add(351,3,{{352,-1,1}},false,"Decoration",15);                      // bone meal
    add(351,2,{{38,-1,1}},false,"Decoration",1);                        // rose red
    add(351,2,{{37,-1,1}},false,"Decoration",11);                       // dandelion yellow
    add(IRON,9,{{42,-1,1}},false,"Decoration");                         // blocks back to items
    add(GOLD,9,{{41,-1,1}},false,"Decoration");
    add(DIAMOND,9,{{57,-1,1}},false,"Decoration");
    add(351,9,{{22,-1,1}},false,"Decoration",4);
    add(388,9,{{133,-1,1}},false,"Decoration");
    return r;
}
}
const std::vector<CraftingRecipe>& consoleCraftingRecipes(){
    static const std::vector<CraftingRecipe> recipes=build();
    return recipes;
}
}
