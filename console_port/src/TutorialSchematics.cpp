#include "TutorialSchematics.h"
#include "ConsoleDlcPack.h"
#include <fstream>
#include <algorithm>
namespace console {
namespace {
std::vector<unsigned char> readPackage(const std::filesystem::path& root){
 const auto path=root/"Tutorial.pck";
 if(std::filesystem::file_size(path)>64*1024*1024)throw IoError("Tutorial package exceeds capacity");
 std::ifstream file(path,std::ios::binary);if(!file)throw IoError("Cannot open tutorial package");
 return {(std::istreambuf_iterator<char>(file)),{}};
}
}
TutorialSchematics::TutorialSchematics(const std::filesystem::path& root):TutorialSchematics(readPackage(root)){}
TutorialSchematics::TutorialSchematics(std::span<const unsigned char> bytes){
 auto pack=ConsoleDlcPack::read(bytes);bool found=false;
 for(const auto& entry:pack.entries)if(entry.name==u"languages.loc")strings_=readTutorialStrings(entry.bytes);
 for(const auto& entry:pack.entries)if(entry.type==7){
  if(found)throw IoError("Ambiguous tutorial rule files");found=true;
  auto content=ConsoleGameRuleFile::read(entry.bytes);rules_=std::move(content.rules);
  auto text=[](const std::wstring& wide){std::string result;for(auto c:wide){if(c<32 || c>126)throw IoError("Invalid tutorial asset name");result+=char(c);}return result;};
  for(const auto& [name,data]:content.files)files_.emplace(text(name),ConsoleSchematic::read(data));
  auto integer=[](const std::wstring& value){std::size_t used=0;long long n=std::stoll(value,&used);
   if(used!=value.size() || n < -30000000 || n>30000000)throw IoError("Invalid tutorial coordinate");return int(n);};
  for(const auto& rule:rules_)if(rule.name==L"MapOptions"){
   if(rule.attributes.at(L"seed")!=std::to_wstring(tutorialSeed))throw IoError("Tutorial seed does not match imported layout");
   for(const auto& child:rule.children)if(child.name==L"ApplySchematic"){
    if(integer(child.attributes.at(L"rot"))!=0)throw IoError("Rotated tutorial schematic requires rotation support");
    auto even=[&](const wchar_t* key){int n=integer(child.attributes.at(key));return n-(n%2!=0);};
    TutorialPlacement p{text(child.attributes.at(L"filename")),even(L"x"),std::max(0,even(L"y")),even(L"z")};
    if(!files_.contains(p.filename))throw IoError("Tutorial references a missing embedded schematic");
    placements_.push_back(std::move(p));
   }
  }
 }
 containers_=readTutorialContainers(rules_);
 spawners_=readTutorialSpawners(rules_);
 levelRules_=readTutorialLevelRules(rules_);
 if(!found || placements_.empty())throw IoError("Tutorial package has no structure layout");
}
std::unique_ptr<CompoundTag> TutorialSchematics::tagsForChunk(int cx,int cz)const{
 auto result=std::make_unique<CompoundTag>();auto tiles=std::make_unique<TagList>();auto entities=std::make_unique<TagList>();
 for(const auto& p:placements_){auto translated=files_.at(p.filename).tagsForChunk(cx,cz,p.x,p.y,p.z);
  for(const auto* key:{L"TileEntities",L"Entities"}){
   auto* source=translated->getList(key);auto* target=std::wstring(key)==L"TileEntities"?tiles.get():entities.get();
   for(int i=0;i<source->size();++i){
    auto* original=source->get(i);bool replaced=false;
    if(std::wstring(key)==L"TileEntities")if(auto* t=dynamic_cast<CompoundTag*>(original))
     for(const auto& c:containers_)if(c.dimension==0 && t->getInt(L"x")==c.x && t->getInt(L"y")==c.y && t->getInt(L"z")==c.z)replaced=true;
    if(std::wstring(key)==L"TileEntities")if(auto* t=dynamic_cast<CompoundTag*>(original))
     for(const auto& s:spawners_)if(s.dimension==0 && t->getInt(L"x")==s.x && t->getInt(L"y")==s.y && t->getInt(L"z")==s.z)replaced=true;
    if(replaced)continue;
    std::unique_ptr<Tag> copy(original->copy());target->add(copy.get());copy.release();
   }
  }
 }
 for(const auto& c:containers_)if(c.dimension==0 && c.x>=cx*16 && c.x<cx*16+16 && c.z>=cz*16 && c.z<cz*16+16){
  std::unique_ptr<Tag> tag(c.tag->copy());tiles->add(tag.get());tag.release();
 }
 for(const auto& s:spawners_)if(s.dimension==0 && s.x>=cx*16 && s.x<cx*16+16 && s.z>=cz*16 && s.z<cz*16+16){
  std::unique_ptr<Tag> tag(s.tag->copy());tiles->add(tag.get());tag.release();
 }
 result->put(L"TileEntities",tiles.get());tiles.release();result->put(L"Entities",entities.get());entities.release();return result;
}
bool TutorialSchematics::apply(ChunkStorage& chunk,int cx,int cz)const{
 if(cx < -1000000 || cx>1000000 || cz < -1000000 || cz>1000000)throw std::out_of_range("Tutorial chunk coordinate");
 bool changed=false;
 for(const auto& p:placements_){const auto& data=files_.at(p.filename);
  if(cx*16>=p.x+data.width() || cx*16+16<=p.x || cz*16>=p.z+data.depth() || cz*16+16<=p.z)continue;
  data.apply(chunk,cx,cz,p.x,p.y,p.z);changed=true;
 }
 for(const auto& c:containers_)if(c.dimension==0 && c.x>=cx*16 && c.x<cx*16+16 && c.z>=cz*16 && c.z<cz*16+16){
  const int x=c.x-cx*16,z=c.z-cz*16;chunk.blocks[(x*16+z)*256+c.y]=54;chunk.metadata.set(x,c.y,z,c.facing);chunk.unsaved=true;chunk.hasGapsToCheck=true;changed=true;
 }
 for(const auto& s:spawners_)if(s.dimension==0 && s.x>=cx*16 && s.x<cx*16+16 && s.z>=cz*16 && s.z<cz*16+16){
  const int x=s.x-cx*16,z=s.z-cz*16;chunk.blocks[(x*16+z)*256+s.y]=52;chunk.metadata.set(x,s.y,z,0);chunk.unsaved=true;chunk.hasGapsToCheck=true;changed=true;
 }
 return changed;
}
}
