// File: src/client/shader/ShaderOptions.hpp
//
// A shader pack's own settings: the `#define` lines its shaders declare as
// options, the way OptiFine and Iris read them.
//
//   #define SHADOWS                  a toggle, on by default
//   //#define SHADOWS                a toggle, off by default
//   #define SHADOW_RES 2048 // [1024 2048 4096]   a choice, with its values
//
// The player's choices are kept next to the pack as shaderpacks/<pack>.txt
// (`NAME=value` lines, Iris's file), and applied by rewriting those lines
// in the source before translation — the pack's `#ifdef`s then see them.
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace Shaders {

    struct Option {
        std::string name;
        bool        isToggle = false;
        bool        defaultOn = true;        // toggles: declared without the leading //
        std::string defaultValue;            // choices: the declared value
        std::vector<std::string> values;     // choices: the [...] list (includes the default)
        std::string comment;                 // trailing text before the list, if any
    };

    // Overrides: name -> "true"/"false" for a toggle, or a value.
    using Overrides = std::map<std::string, std::string>;

    // Every option declared under the pack's shaders directory
    // (Shaders::Prepare), by name, in the order found (files sorted). A
    // name declared in several files is one option.
    std::vector<Option> Discover(const std::string& shadersDir);

    // The saved choices for a pack (its list name, e.g. "BSL_v8.zip").
    Overrides LoadOverrides(const std::string& packName);
    void      SaveOverrides(const std::string& packName, const Overrides& overrides);

    // `source` with the overridden #define lines rewritten.
    std::string Apply(const std::string& source, const Overrides& overrides);

    // The value an option currently has (override or default), for the UI.
    std::string CurrentValue(const Option& option, const Overrides& overrides);

    // The pack's settings LAYOUT, as OptiFine and Iris read it from
    // shaders.properties and the pack's language file:
    //   screen=A B [SUB] <empty> <profile>      the main screen's items
    //   screen.SUB=...                          a sub-screen; [SUB] links to it
    //   screen.SUB.columns=N                    its column count (default 2)
    //   sliders=A B                             options shown as sliders
    //   profile.NAME=A=1 B !C profile.OTHER     a profile: option values
    //   lang/en_us.lang: option.A=Label, option.A.comment=Tooltip,
    //   value.A.1=Label, screen.SUB=Label, profile.NAME=Label.
    struct PackLayout {
        struct Profile {
            std::string name;
            Overrides   values;   // every option the profile sets (inheritance resolved)
        };
        // Items per screen, "" = the main screen; an item is an option name,
        // "[NAME]" (a link), "<empty>" (a gap) or "<profile>" (the profile
        // button). Empty when the pack declares no layout (every option is
        // then listed flat).
        std::map<std::string, std::vector<std::string>> screens;
        std::map<std::string, int> columns;
        std::set<std::string> sliders;
        std::vector<Profile> profiles;
        std::map<std::string, std::string> lang;

        bool HasLayout() const { return screens.count("") > 0; }
        // Labels from the language file, the raw name without one.
        std::string OptionLabel(const std::string& option) const;
        std::string OptionComment(const std::string& option) const;   // "" = none
        std::string ValueLabel(const std::string& option, const std::string& value) const;
        std::string ScreenLabel(const std::string& screen) const;
        std::string ScreenComment(const std::string& screen) const;
        std::string ProfileLabel(const std::string& profile) const;
        // The profile whose every value the overrides (over the options'
        // defaults) currently match, or -1 (Iris: "Custom").
        int MatchingProfile(const std::vector<Option>& options, const Overrides& overrides) const;
    };
    PackLayout LoadLayout(const std::string& shadersDir);

} // namespace Shaders
