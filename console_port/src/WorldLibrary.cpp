#include "WorldLibrary.h"
#include "PS3WorldStorage.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>
namespace console {
std::vector<SavedWorld> listSavedWorlds(const std::filesystem::path& directory){
    std::vector<SavedWorld> result;
    auto add=[&](const std::filesystem::path& path){
        if(!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)))return;
        SavedWorld entry{path,"Legacy World",true};
        if(path.extension()==".inner")try{
            auto archive=PS3WorldStorage::readFile(path);auto metadata=archive->metadata();
            if(!metadata)throw std::runtime_error("Missing world metadata");
            entry.name.clear();for(auto c:metadata->getLevelName())entry.name+=c>=32 && c<127?char(c):'?';
            if(entry.name.empty())entry.name="World";
        }catch(const std::exception&){entry.name="Unreadable World";entry.readable=false;}
        result.push_back(std::move(entry));
    };
    if(std::filesystem::exists(directory/"world.inner"))add(directory/"world.inner");
    else if(std::filesystem::exists(directory/"world.mcp"))add(directory/"world.mcp");
    auto worlds=directory/"worlds";
    if(std::filesystem::is_directory(worlds))for(const auto& entry:std::filesystem::directory_iterator(worlds))
        if(std::filesystem::is_directory(entry.symlink_status()))add(entry.path()/"world.inner");
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){
        if(a.name!=b.name)return a.name<b.name;return a.path<b.path;
    });
    return result;
}
std::filesystem::path allocateWorldSave(const std::filesystem::path& directory){
    auto worlds=directory/"worlds";std::filesystem::create_directories(worlds);
    auto stamp=std::chrono::system_clock::now().time_since_epoch().count();
    for(int attempt=0;attempt<1000;++attempt){
        auto folder=worlds/("world-"+std::to_string(stamp)+"-"+std::to_string(attempt));
        if(std::filesystem::create_directory(folder))return folder/"world.inner";
    }
    throw std::runtime_error("Cannot allocate a new world save");
}
}
