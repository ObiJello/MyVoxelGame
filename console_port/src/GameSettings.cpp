#include "GameSettings.h"
#include <fstream>
#include <stdexcept>
#include <string>

namespace console {
namespace {
constexpr char kMagic[8]={'C','P','S','E','T','T','1','\0'};
}

void GameSettings::setDefaults(bool resetDifficulty){
    // CMinecraftApp::SetDefaultOptions (DEFAULT_VOLUME_LEVEL 100). The PS3
    // profile carries no system inversion/southpaw defaults, so both are off.
    set(GameSetting::MusicVolume,100);
    set(GameSetting::SoundFXVolume,100);
    set(GameSetting::Gamma,50);
    if(resetDifficulty)set(GameSetting::Difficulty,1);
    set(GameSetting::SensitivityInGame,100);
    set(GameSetting::ViewBob,1);
    set(GameSetting::ControlScheme,0);
    set(GameSetting::ControlInvertLook,0);
    set(GameSetting::ControlSouthPaw,0);
    set(GameSetting::SplitScreenVertical,0);
    set(GameSetting::GamertagsVisible,1);
    set(GameSetting::SensitivityInMenu,100);
    set(GameSetting::DisplaySplitscreenGamertags,1);
    set(GameSetting::Hints,1);
    set(GameSetting::Autosave,2);
    set(GameSetting::Tooltips,1);
    set(GameSetting::InterfaceOpacity,80);
    set(GameSetting::Clouds,1);
    set(GameSetting::Online,1);
    set(GameSetting::InviteOnly,0);
    set(GameSetting::FriendsOfFriends,1);
    set(GameSetting::BedrockFog,0);
    set(GameSetting::DisplayHUD,1);
    set(GameSetting::DisplayHand,1);
    set(GameSetting::CustomSkinAnim,1);
    set(GameSetting::DeathMessages,1);
    set(GameSetting::UISize,1);
    set(GameSetting::UISizeSplitscreen,2);
    set(GameSetting::AnimatedCharacter,1);
    set(GameSetting::PS3EulaRead,0);
}

unsigned char GameSettings::maximum(GameSetting setting){
    // Field widths from CMinecraftApp::GetGameSettings.
    switch(setting){
    case GameSetting::MusicVolume:case GameSetting::SoundFXVolume:case GameSetting::Gamma:
    case GameSetting::InterfaceOpacity:return 100;
    case GameSetting::SensitivityInGame:case GameSetting::SensitivityInMenu:return 200;
    case GameSetting::Difficulty:case GameSetting::ControlScheme:case GameSetting::UISize:
    case GameSetting::UISizeSplitscreen:case GameSetting::DisplayUpdateMessage:return 3;
    case GameSetting::Autosave:return 15;
    default:return 1;
    }
}

void GameSettings::set(GameSetting setting,unsigned char value){
    const int index=static_cast<int>(setting);
    if(index<0 || index>=static_cast<int>(GameSetting::Count))return;
    if(value>maximum(setting))value=maximum(setting);
    if(values_[index]!=value){values_[index]=value;changed_=true;}
}

void GameSettings::setSpecialTutorialCompletion(int index){
    if(index<0 || index>=32)return;
    const unsigned bit=1u<<index;
    if(!(special_&bit)){special_|=bit;changed_=true;}
}

void GameSettings::load(const std::filesystem::path& file){
    std::ifstream in(file,std::ios::binary);
    if(!in)return;
    char magic[8]{};
    unsigned char count=0;
    in.read(magic,8);
    in.read(reinterpret_cast<char*>(&count),1);
    if(!in || std::string(magic,8)!=std::string(kMagic,8))throw std::runtime_error("Unreadable settings file");
    GameSettings loaded;
    for(unsigned i=0;i<count;++i){
        unsigned char value=0;in.read(reinterpret_cast<char*>(&value),1);
        if(i<values_.size())loaded.set(static_cast<GameSetting>(i),value);
    }
    in.read(reinterpret_cast<char*>(loaded.tutorial_.data()),TutorialBytes);
    unsigned char special[4]{};in.read(reinterpret_cast<char*>(special),4);
    if(!in)throw std::runtime_error("Truncated settings file");
    loaded.special_=unsigned(special[0])|unsigned(special[1])<<8|unsigned(special[2])<<16|unsigned(special[3])<<24;
    *this=loaded;
    changed_=false;
}

void GameSettings::save(const std::filesystem::path& file){
    std::filesystem::create_directories(file.parent_path());
    const auto temporary=file.string()+".tmp";
    {
        std::ofstream out(temporary,std::ios::binary|std::ios::trunc);
        if(!out)throw std::runtime_error("Cannot write settings");
        out.write(kMagic,8);
        const unsigned char count=static_cast<unsigned char>(values_.size());
        out.write(reinterpret_cast<const char*>(&count),1);
        out.write(reinterpret_cast<const char*>(values_.data()),values_.size());
        out.write(reinterpret_cast<const char*>(tutorial_.data()),TutorialBytes);
        const unsigned char special[4]{static_cast<unsigned char>(special_),static_cast<unsigned char>(special_>>8),
                                       static_cast<unsigned char>(special_>>16),static_cast<unsigned char>(special_>>24)};
        out.write(reinterpret_cast<const char*>(special),4);
        if(!out)throw std::runtime_error("Cannot write settings");
    }
    std::filesystem::rename(temporary,file);
    changed_=false;
}
}
