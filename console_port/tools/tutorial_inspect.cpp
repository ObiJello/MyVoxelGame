#include "TutorialSchematics.h"
#include "ConsoleDlcPack.h"
#include "ConsoleGameRuleFile.h"
#include <fstream>
#include <functional>
#include <iostream>
int main(int argc,char** argv){try{
 if(argc<2 || argc>3)throw std::runtime_error("Expected tutorial asset directory [--layout|--digest|--rules]");console::TutorialSchematics tutorial(argv[1]);std::map<int,std::size_t> blocks;
 if(argc==3){
  const std::string mode=argv[2];
  if(mode=="--rules"){
   std::ifstream file(std::filesystem::path(argv[1])/"Tutorial.pck",std::ios::binary);
   std::vector<unsigned char> package((std::istreambuf_iterator<char>(file)),{});
   auto entries=console::ConsoleDlcPack::read(package).entries;
   for(const auto& entry:entries)if(entry.type==7){auto rules=console::ConsoleGameRuleFile::read(entry.bytes).rules;
    std::function<void(const console::ConsoleGameRuleNode&,int)> print=[&](const auto& node,int depth){
     std::cout<<std::string(depth,' ')<<std::string(node.name.begin(),node.name.end());
     for(const auto& [key,value]:node.attributes)std::cout<<' '<<std::string(key.begin(),key.end())<<'='<<std::string(value.begin(),value.end());
     std::cout<<'\n';for(const auto& child:node.children)print(child,depth+1);
    };for(const auto& node:rules)print(node,0);
   }return 0;
  }
  if(mode!="--layout" && mode!="--digest")throw std::runtime_error("Unknown inspection option");
  for(const auto& p:tutorial.placements()){const auto& s=tutorial.schematic(p.filename);
   if(mode=="--layout")std::cout<<p.filename<<','<<p.x<<','<<p.y<<','<<p.z<<','<<s.width()<<','<<s.height()<<','<<s.depth()<<'\n';
   else {std::uint64_t hash=14695981039346656037ull;
    for(int x=0;x<s.width();++x)for(int z=0;z<s.depth();++z)for(int y=0;y<s.height();++y)
     for(int value:{s.block(x,y,z),s.data(x,y,z)})hash=(hash^value)*1099511628211ull;
    std::cout<<p.filename<<','<<hash<<'\n';}
  }
  return 0;
 }
 for(const auto& p:tutorial.placements()){const auto& s=tutorial.schematic(p.filename);
  for(int x=0;x<s.width();++x)for(int z=0;z<s.depth();++z)for(int y=0;y<s.height();++y)++blocks[s.block(x,y,z)];
 }
 for(auto [id,count]:blocks)std::cout<<id<<","<<count<<"\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
