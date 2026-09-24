#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace console {
// The PS3 button images CMinecraftApp::GetActionReplacement/GetVKReplacement
// insert (ButtonA = Cross ... ButtonBack = Select) and the hunger shank.
enum class PadGlyph {
    None,Cross,Circle,Square,Triangle,L1,R1,L2,R2,L3,R3,LeftStick,RightStick,
    DpadUp,DpadDown,DpadLeft,DpadRight,Start,Select,Shank
};

struct RichSpan {
    std::string text;          // UTF-8, empty for glyphs and breaks
    std::uint32_t colour=0xffffff;
    PadGlyph glyph=PadGlyph::None;
    bool lineBreak=false;
    std::string token; // the {*...*} name a glyph came from
};

// UIComponent_TutorialPopup::ParseDescription + CMinecraftApp::FormatHTMLString
// + stripWhitespaceForHtml over a console string: {*B*} breaks, {*T1..3*},
// {*C0..F*}, {*ETW*}/{*ETB*}/{*EF*} colours, controller action/button images
// (the PS3 DefineActions map, southpaw swapping MOVE/LOOK) and shank icons.
// The icon tokens ({*...Icon*}, {*ICON*}id{*/ICON*}) are removed; read them
// first with consoleRichTextIcon.
std::vector<RichSpan> parseConsoleRichText(const std::wstring& text,bool southpaw=false,
                                           std::uint32_t defaultColour=0xffffff,int layout=0);

// UIComponent_TutorialPopup::_SetIcon: the item a description names with
// {*ICON*}id[:aux]{*/ICON*} or one of the fixed {*...Icon*} tokens.
// Returns false when the text has none. Structures/Tools use the crafting
// group icons: id -1 / -2.
bool consoleRichTextIcon(const std::wstring& text,int& id,int& aux);

// GetActionReplacement: the PS3 button bound to an EControllerActions value
// in controller layout 0-2.
PadGlyph consoleActionGlyph(int action,int layout=0);

// Word-wrapped layout of parsed spans. `measure` returns the width of UTF-8
// text at the layout scale and `glyphWidth` the width of a glyph span.
struct RichPiece { float x=0; RichSpan span; };
struct RichLine { std::vector<RichPiece> pieces; float width=0; };
std::vector<RichLine> layoutRichText(const std::vector<RichSpan>& spans,float maxWidth,
                                     const std::function<float(const RichSpan&)>& glyphWidth,
                                     const std::function<float(const std::string&)>& measure);
}
