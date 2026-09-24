#include "RichText.h"
#include "TutorialSession.h"
// _360_JOY_BUTTON_* bits from the PS3 4J_Input.h (tools/import_tutorial.py).
#include "../ported/tutorial/generated/JoyButtons.inc"

#include <algorithm>
#include <cwctype>

namespace console {
namespace {
// EControllerActions indices used by FormatHTMLString.
enum : int {
    MinecraftJump=21,MinecraftRight=25,MinecraftLookRight=27,MinecraftUse=30,MinecraftAction=31,
    MinecraftLeftScroll=32,MinecraftRightScroll=33,MinecraftInventory=34,MinecraftDrop=36,
    MinecraftSneakToggle=37,MinecraftCrafting=38,MinecraftThirdPerson=39,
    MinecraftDpadLeft=41,MinecraftDpadRight=42,MinecraftDpadUp=43,MinecraftDpadDown=44
};

// The Minecraft text palette documented in App_enums.h (eHTMLColor_0..f).
// T1-T3 come from the skin's colour table, which is not in the supplied
// media; the palette yellow stands in for these headings.
constexpr std::uint32_t kPalette[16]{0x000000,0x0000aa,0x109e10,0x109e9e,0xaa0000,0xaa00aa,0xffaa00,0xaaaaaa,
                                     0x555555,0x5555ff,0x55ff55,0x55ffff,0xff5555,0xff55ff,0xffff55,0xffffff};
constexpr std::uint32_t kTitle=0xffff55,kBlack=0x000000,kWhite=0xffffff;

PadGlyph glyphForButtons(unsigned input){
    // GetActionReplacement's non-Xbox branch: first matching button wins.
    if(input&_360_JOY_BUTTON_A)return PadGlyph::Cross;
    if(input&_360_JOY_BUTTON_B)return PadGlyph::Circle;
    if(input&_360_JOY_BUTTON_X)return PadGlyph::Square;
    if(input&_360_JOY_BUTTON_Y)return PadGlyph::Triangle;
    if(input&(_360_JOY_BUTTON_LSTICK_UP|_360_JOY_BUTTON_LSTICK_DOWN|_360_JOY_BUTTON_LSTICK_LEFT|_360_JOY_BUTTON_LSTICK_RIGHT))
        return PadGlyph::LeftStick;
    if(input&(_360_JOY_BUTTON_RSTICK_LEFT|_360_JOY_BUTTON_RSTICK_RIGHT|_360_JOY_BUTTON_RSTICK_UP|_360_JOY_BUTTON_RSTICK_DOWN))
        return PadGlyph::RightStick;
    if(input&_360_JOY_BUTTON_DPAD_LEFT)return PadGlyph::DpadLeft;
    if(input&_360_JOY_BUTTON_DPAD_RIGHT)return PadGlyph::DpadRight;
    if(input&_360_JOY_BUTTON_DPAD_UP)return PadGlyph::DpadUp;
    if(input&_360_JOY_BUTTON_DPAD_DOWN)return PadGlyph::DpadDown;
    if(input&_360_JOY_BUTTON_LT)return PadGlyph::L2;
    if(input&_360_JOY_BUTTON_RT)return PadGlyph::R2;
    if(input&_360_JOY_BUTTON_RB)return PadGlyph::R1;
    if(input&_360_JOY_BUTTON_LB)return PadGlyph::L1;
    if(input&_360_JOY_BUTTON_BACK)return PadGlyph::Select;
    if(input&_360_JOY_BUTTON_START)return PadGlyph::Start;
    if(input&_360_JOY_BUTTON_RTHUMB)return PadGlyph::R3;
    if(input&_360_JOY_BUTTON_LTHUMB)return PadGlyph::L3;
    return PadGlyph::None;
}

std::string utf8(const std::wstring& text){
    std::string out;
    for(wchar_t w:text){
        const auto c=static_cast<std::uint32_t>(w);
        if(c<0x80)out+=char(c);
        else if(c<0x800){out+=char(0xC0|(c>>6));out+=char(0x80|(c&0x3F));}
        else {out+=char(0xE0|(c>>12));out+=char(0x80|((c>>6)&0x3F));out+=char(0x80|(c&0x3F));}
    }
    return out;
}

std::wstring replaceAll(std::wstring text,const std::wstring& from,const std::wstring& to){
    for(std::size_t at=0;(at=text.find(from,at))!=std::wstring::npos;at+=to.size())text.replace(at,from.size(),to);
    return text;
}

// The {*...Icon*} tokens UIComponent_TutorialPopup::ParseDescription removes.
constexpr const wchar_t* kIconTokens[]{
    L"{*CraftingTableIcon*}",L"{*SticksIcon*}",L"{*PlanksIcon*}",L"{*WoodenShovelIcon*}",L"{*WoodenHatchetIcon*}",
    L"{*WoodenPickaxeIcon*}",L"{*FurnaceIcon*}",L"{*WoodenDoorIcon*}",L"{*TorchIcon*}",L"{*MinecartIcon*}",
    L"{*BoatIcon*}",L"{*FishingRodIcon*}",L"{*FishIcon*}",L"{*RailIcon*}",L"{*PoweredRailIcon*}",
    L"{*StructuresIcon*}",L"{*ToolsIcon*}",L"{*StoneIcon*}"};
}

PadGlyph consoleActionGlyph(int action,int layout){return glyphForButtons(consoleJoypadButtons(action,layout));}

bool consoleRichTextIcon(const std::wstring& text,int& id,int& aux){
    const std::wstring open=L"{*ICON*}",close=L"{*/ICON*}";
    const auto start=text.find(open);
    // _SetIcon only accepts a tag after the first character.
    if(start!=std::wstring::npos && start>0){
        const auto from=start+open.size(),end=text.find(close,from);
        if(end!=std::wstring::npos && end>from){
            const auto value=text.substr(from,end-from);
            const auto colon=value.find(L':');
            try{
                id=std::stoi(value.substr(0,colon));
                aux=colon==std::wstring::npos?0:std::stoi(value.substr(colon+1));
                return true;
            }catch(...){return false;}
        }
    }
    // Item::stick_Id 280, workBench 58, wood 5, shovel/hatchet/pickaxe wood
    // 269/271/270, furnace 61, door_wood 324, torch 50, boat 333,
    // fishingRod 346, fish_raw 349, minecart 328, rail 66, goldenRail 27, rock 1.
    static const std::pair<const wchar_t*,int> fixed[]{
        {L"{*CraftingTableIcon*}",58},{L"{*SticksIcon*}",280},{L"{*PlanksIcon*}",5},{L"{*WoodenShovelIcon*}",269},
        {L"{*WoodenHatchetIcon*}",271},{L"{*WoodenPickaxeIcon*}",270},{L"{*FurnaceIcon*}",61},{L"{*WoodenDoorIcon*}",324},
        {L"{*TorchIcon*}",50},{L"{*BoatIcon*}",333},{L"{*FishingRodIcon*}",346},{L"{*FishIcon*}",349},
        {L"{*MinecartIcon*}",328},{L"{*RailIcon*}",66},{L"{*PoweredRailIcon*}",27},{L"{*StructuresIcon*}",-1},
        {L"{*ToolsIcon*}",-2},{L"{*StoneIcon*}",1}};
    for(const auto& [token,item]:fixed)if(text.find(token)!=std::wstring::npos){id=item;aux=0;return true;}
    return false;
}

std::vector<RichSpan> parseConsoleRichText(const std::wstring& source,bool southpaw,std::uint32_t defaultColour,int layout){
    std::wstring text=source;
    // _SetIcon strips the {*ICON*}..{*/ICON*} tag it consumed.
    if(const auto start=text.find(L"{*ICON*}");start!=std::wstring::npos && start>0){
        const auto end=text.find(L"{*/ICON*}",start);
        if(end!=std::wstring::npos)text.erase(start,end+9-start);
    }
    for(const auto* token:kIconTokens)text=replaceAll(text,token,L"");
    text=replaceAll(text,L"{*EXIT_PICTURE*}",L"");
    // stripWhitespaceForHtml(text) (newlines kept by the popup's call).
    text.erase(std::remove(text.begin(),text.end(),L'\t'),text.end());
    text.erase(std::unique(text.begin(),text.end(),[](wchar_t a,wchar_t b){return a==L' ' && b==L' ';}),text.end());
    while(!text.empty() && std::iswspace(text.front()))text.erase(text.begin());
    while(!text.empty() && std::iswspace(text.back()))text.pop_back();

    std::vector<RichSpan> spans;
    std::vector<std::uint32_t> colours{defaultColour};
    std::wstring run;
    auto flush=[&]{if(!run.empty()){spans.push_back({utf8(run),colours.back()});run.clear();}};
    std::wstring currentToken;
    auto glyph=[&](PadGlyph g){flush();RichSpan s;s.glyph=g;s.colour=colours.back();s.token=utf8(currentToken);spans.push_back(s);};
    for(std::size_t i=0;i<text.size();){
        if(text.compare(i,2,L"{*")!=0){
            if(text[i]==L'\n' || text[i]==L'\r'){
                // Iggy HTML treats newlines as spaces.
                if(!run.empty() && run.back()!=L' ')run+=L' ';
            }else run+=text[i];
            ++i;continue;
        }
        const auto end=text.find(L"*}",i+2);
        if(end==std::wstring::npos){run+=text.substr(i);break;}
        const std::wstring token=text.substr(i+2,end-i-2);
        currentToken=token;
        i=end+2;
        if(token==L"B"){flush();RichSpan s;s.lineBreak=true;spans.push_back(s);continue;}
        if(token==L"T1" || token==L"T2" || token==L"T3"){flush();colours.push_back(kTitle);continue;}
        if(token==L"ETW" || token==L"ETB"){
            flush();if(colours.size()>1)colours.pop_back();
            colours.push_back(token==L"ETW"?kWhite:kBlack);continue;
        }
        if(token==L"EF"){flush();if(colours.size()>1)colours.pop_back();continue;}
        if(token.size()==2 && token[0]==L'C' && std::iswxdigit(token[1])){
            flush();colours.push_back(kPalette[std::stoi(token.substr(1),nullptr,16)]);continue;
        }
        if(token==L"CONTROLLER_ACTION_MOVE"){glyph(consoleActionGlyph(southpaw?MinecraftLookRight:MinecraftRight,layout));continue;}
        if(token==L"CONTROLLER_ACTION_LOOK"){glyph(consoleActionGlyph(southpaw?MinecraftRight:MinecraftLookRight,layout));continue;}
        if(token==L"CONTROLLER_MENU_NAVIGATE"){glyph(southpaw?PadGlyph::RightStick:PadGlyph::LeftStick);continue;}
        static const std::pair<const wchar_t*,int> actions[]{
            {L"CONTROLLER_ACTION_JUMP",MinecraftJump},{L"CONTROLLER_ACTION_SNEAK",MinecraftSneakToggle},
            {L"CONTROLLER_ACTION_USE",MinecraftUse},{L"CONTROLLER_ACTION_ACTION",MinecraftAction},
            {L"CONTROLLER_ACTION_LEFT_SCROLL",MinecraftLeftScroll},{L"CONTROLLER_ACTION_RIGHT_SCROLL",MinecraftRightScroll},
            {L"CONTROLLER_ACTION_INVENTORY",MinecraftInventory},{L"CONTROLLER_ACTION_CRAFTING",MinecraftCrafting},
            {L"CONTROLLER_ACTION_DROP",MinecraftDrop},{L"CONTROLLER_ACTION_CAMERA",MinecraftThirdPerson},
            {L"CONTROLLER_ACTION_DPAD_UP",MinecraftDpadUp},{L"CONTROLLER_ACTION_DPAD_DOWN",MinecraftDpadDown},
            {L"CONTROLLER_ACTION_DPAD_RIGHT",MinecraftDpadRight},{L"CONTROLLER_ACTION_DPAD_LEFT",MinecraftDpadLeft}};
        bool matched=false;
        for(const auto& [name,action]:actions)if(token==name){glyph(consoleActionGlyph(action,layout));matched=true;break;}
        if(matched)continue;
        // GetVKReplacement (circle/cross not swapped).
        static const std::pair<const wchar_t*,PadGlyph> keys[]{
            {L"CONTROLLER_VK_A",PadGlyph::Cross},{L"CONTROLLER_VK_B",PadGlyph::Circle},{L"CONTROLLER_VK_X",PadGlyph::Square},
            {L"CONTROLLER_VK_Y",PadGlyph::Triangle},{L"CONTROLLER_VK_LB",PadGlyph::L1},{L"CONTROLLER_VK_RB",PadGlyph::R1},
            {L"CONTROLLER_VK_LS",PadGlyph::LeftStick},{L"CONTROLLER_VK_RS",PadGlyph::RightStick},
            {L"CONTROLLER_VK_LT",PadGlyph::L2},{L"CONTROLLER_VK_RT",PadGlyph::R2}};
        for(const auto& [name,g]:keys)if(token==name){glyph(g);matched=true;break;}
        if(matched)continue;
        if(token==L"ICON_SHANK_01"){glyph(PadGlyph::Shank);continue;}
        if(token==L"ICON_SHANK_03"){glyph(PadGlyph::Shank);glyph(PadGlyph::Shank);glyph(PadGlyph::Shank);continue;}
        // FormatHTMLString leaves other tokens in place.
        run+=L"{*"+token+L"*}";
    }
    flush();
    return spans;
}

std::vector<RichLine> layoutRichText(const std::vector<RichSpan>& spans,float maxWidth,
                                     const std::function<float(const RichSpan&)>& glyphWidth,
                                     const std::function<float(const std::string&)>& measure){
    std::vector<RichLine> lines(1);
    float spaceWidth=measure(" ");
    auto place=[&](const RichSpan& span,float width){
        auto& line=lines.back();
        line.pieces.push_back({line.width,span});
        line.width+=width;
    };
    for(const auto& span:spans){
        if(span.lineBreak){lines.emplace_back();continue;}
        if(span.glyph!=PadGlyph::None){
            const float width=glyphWidth(span);
            if(lines.back().width>0 && lines.back().width+width>maxWidth)lines.emplace_back();
            place(span,width);continue;
        }
        // Words (keeping leading/trailing spaces as gaps).
        std::size_t at=0;
        const std::string& t=span.text;
        while(at<t.size()){
            if(t[at]==' '){
                if(lines.back().width>0)lines.back().width+=spaceWidth;
                ++at;continue;
            }
            std::size_t end=t.find(' ',at);if(end==std::string::npos)end=t.size();
            RichSpan word=span;word.text=t.substr(at,end-at);
            const float width=measure(word.text);
            if(lines.back().width>0 && lines.back().width+width>maxWidth){
                lines.emplace_back();
            }
            place(word,width);
            at=end;
        }
    }
    return lines;
}
}
