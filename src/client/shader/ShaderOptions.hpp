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

} // namespace Shaders
