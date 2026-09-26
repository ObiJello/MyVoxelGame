#pragma once

#include <algorithm>
#include <array>
#include <cmath>

// Timelines transcribed from the original 30 fps skin.swf and Panorama720.swf.
// Flash frame numbers below are one-based; elapsed time starts at the first
// rendered frame of the named animation.
namespace console_ui_animation {
inline constexpr double frameRate=30.0;

inline int panoramaFrame(double elapsed){
    if(!std::isfinite(elapsed) || elapsed<=0)return 0;
    // skin.swf Panorama / Panorama_Night: 4100 frames, translated one source
    // pixel per frame. Their bitmaps are scaled fivefold on the 720p stage.
    return int(std::fmod(std::floor(elapsed*frameRate),4100.0));
}

inline float panoramaU(double elapsed){return panoramaFrame(elapsed)/4100.f;}

inline float savePanelFadeIn(double elapsed){
    // FJ_PanelRecessFade frames 2..21, as entered by FJ_ButtonList::SetListFocus.
    // Alpha multiplication is in Flash's 0..256 range.
    constexpr std::array<int,20> alpha{
        133,141,146,154,159,166,172,179,184,192,
        197,205,210,218,223,230,236,243,248,256};
    const int frame=std::clamp(int(std::floor(std::max(0.0,elapsed)*frameRate)),0,19);
    return alpha[frame]/256.f;
}

inline constexpr float unfocusedPanelAlpha=128.f/256.f;

inline bool buttonPressed(double elapsed){
    // FJ_MenuButton_Normal / FJ_ListButton_Normal frames 18..27 show the
    // unhighlighted pressed state. Frame 28 returns to Active_Selected.
    return elapsed>=0 && elapsed<10.0/frameRate;
}

inline float scrollArrowAlpha(double elapsed,bool down){
    // FJ_ScrollArrow Animating_A frames 2..9 and Animating_B frames 11..19.
    constexpr std::array<int,8> up{256,207,159,110,64,128,192,256};
    constexpr std::array<int,9> downFrames{256,207,159,110,64,110,159,207,256};
    if(elapsed<0)return 1.f;
    const int frame=int(std::floor(elapsed*frameRate));
    if(down)return downFrames[std::clamp(frame,0,8)]/256.f;
    return up[std::clamp(frame,0,7)]/256.f;
}
}
