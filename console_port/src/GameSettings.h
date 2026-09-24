#pragma once
#include <array>
#include <cstdint>
#include <filesystem>

namespace console {
// eGameSetting (App_enums.h), in source order.
enum class GameSetting : int {
    MusicVolume=0,SoundFXVolume,Gamma,Difficulty,SensitivityInGame,SensitivityInMenu,ViewBob,
    ControlScheme,ControlInvertLook,ControlSouthPaw,SplitScreenVertical,GamertagsVisible,Autosave,
    DisplaySplitscreenGamertags,Hints,InterfaceOpacity,Tooltips,Clouds,Online,InviteOnly,
    FriendsOfFriends,DisplayUpdateMessage,BedrockFog,DisplayHUD,DisplayHand,CustomSkinAnim,
    DeathMessages,UISize,UISizeSplitscreen,AnimatedCharacter,PS3EulaRead,PSVitaNetworkModeAdhoc,
    Count
};

// The player's profile data (GAME_SETTINGS): the values
// CMinecraftApp::GetGameSettings returns, the tutorial completion bits and the
// special (music disc) completion flags. Saved to one file in the data folder.
class GameSettings {
public:
    static constexpr std::size_t TutorialBytes=64; // TUTORIAL_PROFILE_STORAGE_BYTES
    GameSettings(){setDefaults(true);}
    // CMinecraftApp::SetDefaultOptions. Difficulty is only reset outside a game.
    void setDefaults(bool resetDifficulty);
    unsigned char get(GameSetting setting)const{return values_[static_cast<int>(setting)];}
    // SetGameSettings: stores the value clamped to the setting's range.
    void set(GameSetting setting,unsigned char value);
    // Largest value a setting holds (bit field width or 100 for the sliders).
    static unsigned char maximum(GameSetting setting);
    std::array<std::uint8_t,TutorialBytes>& tutorialCompletion(){changed_=true;return tutorial_;}
    const std::array<std::uint8_t,TutorialBytes>& tutorialCompletion()const{return tutorial_;}
    // CMinecraftApp::SetSpecialTutorialCompletionFlag (dataTag - 1).
    void setSpecialTutorialCompletion(int index);
    unsigned specialTutorialBitmask()const{return special_;}
    bool changed()const{return changed_;}
    // Throws IoError-style std::runtime_error on unreadable files; a missing
    // file keeps the defaults.
    void load(const std::filesystem::path& file);
    void save(const std::filesystem::path& file);
private:
    std::array<unsigned char,static_cast<int>(GameSetting::Count)> values_{};
    std::array<std::uint8_t,TutorialBytes> tutorial_{};
    unsigned special_=0;
    bool changed_=false;
};
}
